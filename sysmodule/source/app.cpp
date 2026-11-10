#include "app.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"
#include "gateway.h"
#include "game.h"
#include "mic.h"
#include "screen.h"
#include "mixer.h"
#include "aes.h"
#include "avatars.h"
#include "voice.h"
#include "limits.h"
#include "store.h"
#include "tls.h"
#include "util.h"
#include "ws.h"

namespace app {
namespace {

const char* const HOST = "gateway.discord.gg";
const char* const PATH = "/?v=10&encoding=json";
constexpr size_t LOG_MAX = 64 * 1024;
constexpr int WAIT_MS = 50;  // the longest a command waits for the network thread
const uint32_t RETRY_S[] = {1, 2, 3, 3, 3};  // (seconds before trying again: 3 at most, waiting longer only annoys)

// --- the log (no token in it, ever) ---

s64 wallClock() {
    u64 t = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &t))) return 0;
    return (s64)t;
}

u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

void random(void* out, size_t n) { randomGet(out, n); }

// --- shared with the IPC thread, under g_lock ---

Mutex g_lock;
Thread g_thread;
volatile bool g_run = false;
bool g_sockets = false, g_ssl = false;

u8 g_conn = ChirpConn_NoToken;
u16 g_last_close = 0;
u64 g_retry_at_ms = 0;
bool g_want_mute = false, g_want_deaf = false;  // ours, kept when not in a call
// The call to go back to when a session that was in one drops (the network, Discord asking for a
// reconnect): the new session asks for it again once READY. Not after Reload (another login, maybe),
// nor when it's been too long. (The old session's voice state isn't ours to use: gateway.h)
struct Rejoin {
    uint64_t guild, channel, at_ms;
};
Rejoin g_rejoin = {};
constexpr u64 REJOIN_MS = 120'000;
// Go Live: the stream we asked for (its call's guild and channel; 0: none)
uint64_t g_stream_guild = 0, g_stream_channel = 0;

struct Pending {
    bool join, leave, voice, reload;
    ChirpJoin j;
};
Pending g_pending;

TlsStream g_tls;
WebSocket g_ws;
Gateway g_gw;
bool g_session = false;  // g_gw is talking (READY or not)

// --- settings.ini: which login, and Rich Presence ---

char g_login[CHIRP_NAME] = "";         // the one chosen (settings.ini)
char g_login_active[CHIRP_NAME] = "";  // the one connected as (empty: token.txt)
// the mic's settings (settings.ini): input volume (percent), sensitivity (0 automatic, else dBFS), automatic gain
int g_mic_volume = 100, g_mic_sensitivity = MIC_SENSITIVITY_AUTO;
bool g_mic_agc = true, g_mic_denoise = true;
bool g_full_range = false;  // (the screen's video labelled full range)// the bot's presence (settings.ini): invisible (shows offline, the default: not online whenever the
// Switch is on), and Rich Presence ("Playing <game>"); dirty: say it again
bool g_invisible = true, g_rpc = false, g_rpc_dirty = true;
char g_game[CHIRP_NAME] = "";  // (what Rich Presence says: empty, nothing)

void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// Past leading blanks and a UTF-8 BOM (Notepad's)
char* skipLead(char* s) {
    while (*s == ' ' || *s == '\t' || (unsigned char)*s == 0xEF || (unsigned char)*s == 0xBB || (unsigned char)*s == 0xBF) s++;
    return s;
}

void makeDir() {
    mkdir("sdmc:/config", 0777);
    mkdir(CHIRP_DIR, 0777);
}

// A file written whole through a .part file, so a write cut short leaves the last one there
// (the card must be open: a Card in scope)
bool writeFile(const char* path, const void* data, size_t n) {
    char part[96];
    snprintf(part, sizeof(part), "%s.part", path);
    makeDir();
    FILE* f = fopen(part, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok) return false;
    remove(path);
    return rename(part, path) == 0;
}

void loadSettings() {
    Card c;
    if (!c.ok) return;
    FILE* f = fopen(CHIRP_SETTINGS_INI, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "login=", 6)) snprintf(g_login, sizeof(g_login), "%s", line + 6);
        else if (!strncmp(line, "input_volume=", 13)) g_mic_volume = atoi(line + 13);
        else if (!strncmp(line, "input_sensitivity=", 18)) g_mic_sensitivity = !strcmp(line + 18, "auto") ? 0 : atoi(line + 18);
        else if (!strncmp(line, "automatic_gain=", 15)) g_mic_agc = !strcmp(line + 15, "true");
        else if (!strncmp(line, "noise_suppression=", 18)) g_mic_denoise = !strcmp(line + 18, "true");
        else if (!strncmp(line, "full_range_video=", 17)) g_full_range = !strcmp(line + 17, "true");
        else if (!strncmp(line, "appear_invisible=", 17)) g_invisible = !strcmp(line + 17, "true");
        else if (!strncmp(line, "rich_presence=", 14)) g_rpc = !strcmp(line + 14, "true");    }
    fclose(f);
}

void saveSettings() {
    char text[512], sens[8];
    if (g_mic_sensitivity < 0) snprintf(sens, sizeof(sens), "%d", g_mic_sensitivity);
    else snprintf(sens, sizeof(sens), "auto");
    int n = snprintf(text, sizeof(text),
                     "# Chirp's settings (the overlay changes these)\nlogin=%s\n"
                     "# the mic: volume in percent (0..200); sensitivity auto, or the level talk must be over (dBFS, e.g. -45)\n"
                     "input_volume=%d\ninput_sensitivity=%s\nautomatic_gain=%s\nnoise_suppression=%s\n"
                     "# Screenshare: label the video full range (0..255) rather than grc's limited (16..235)\nfull_range_video=%s\n"
                     "# the bot: offline to everyone while connected; \"Playing <game>\"\nappear_invisible=%s\nrich_presence=%s\n",
                     g_login, g_mic_volume, sens, g_mic_agc ? "true" : "false", g_mic_denoise ? "true" : "false",
                     g_full_range ? "true" : "false", g_invisible ? "true" : "false", g_rpc ? "true" : "false");
    Card c;
    if (!c.ok) return;
    // (snprintf says what it would have written: past the buffer, fwrite would have read past it too)
    if (n <= 0 || (size_t)n >= sizeof(text) || !writeFile(CHIRP_SETTINGS_INI, text, (size_t)n)) log("couldn't save settings.ini");
}

// token.ini: "Name=token" a line ('#' or ';' starts a comment). Each (name, token) in turn to fn;
// false: there's no token.ini
template <class Fn>
bool eachLogin(Fn fn) {
    Card c;
    if (!c.ok) return false;
    FILE* f = fopen(CHIRP_TOKEN_INI, "rb");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* s = skipLead(line);
        trim(s);
        if (!*s || *s == '#' || *s == ';') continue;
        char* eq = strchr(s, '=');
        if (!eq || eq == s) continue;
        *eq = 0;
        trim(s);
        if (fn(s, eq + 1)) break;
    }
    memset(line, 0, sizeof(line));
    fclose(f);
    return true;
}

// A token as pasted: trimmed, without "Bot "; false if it can't be one
bool cleanToken(const char* in, char* out, size_t cap, const char* where) {
    out[0] = 0;
    char line[192];
    snprintf(line, sizeof(line), "%s", in);
    char* s = skipLead(line);
    if (strncmp(s, "Bot ", 4) == 0) s += 4;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    s[n] = 0;
    bool ok = n > 0 && n < cap;
    for (size_t i = 0; ok && i < n; i++) {
        char ch = s[i];
        ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    }
    if (ok) memcpy(out, s, n + 1);
    memset(line, 0, sizeof(line));
    if (!ok && n > 0) {
        char why[96];
        snprintf(why, sizeof(why), "%s doesn't look like a bot token (letters, digits, '.', '_' and '-' only)", where);
        log(why);
    }
    return ok;
}

// The token to connect with: token.ini's chosen login (or its first), else token.txt's first line
bool loadToken(char* out, size_t cap) {
    out[0] = 0;
    g_login_active[0] = 0;
    bool found = false;
    char first[CHIRP_NAME] = "";
    char first_token[192] = "";
    bool have_ini = eachLogin([&](const char* name, const char* token) {
        if (!first[0]) {
            snprintf(first, sizeof(first), "%s", name);
            snprintf(first_token, sizeof(first_token), "%s", token);
        }
        if (g_login[0] && !strcmp(name, g_login)) {
            found = cleanToken(token, out, cap, "token.ini");
            if (found) snprintf(g_login_active, sizeof(g_login_active), "%s", name);
            return true;
        }
        return false;
    });
    if (have_ini && !found && first[0]) {
        found = cleanToken(first_token, out, cap, "token.ini");
        if (found) snprintf(g_login_active, sizeof(g_login_active), "%s", first);
    }
    memset(first_token, 0, sizeof(first_token));
    if (found) return true;
    Card c;
    if (!c.ok) return false;
    FILE* f = fopen(CHIRP_TOKEN_TXT, "rb");
    if (!f) return false;
    char line[192] = {0};
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    bool ok = cleanToken(line, out, cap, "token.txt");
    memset(line, 0, sizeof(line));
    return ok;
}

// The bot's presence: every few seconds, what's running (Rich Presence); when that or a setting changes, said
// again (op 3). The game's name is read once per game (game.h: into the send queue's buffer while it's idle)
void rpcTick(uint64_t now) {
    static uint64_t next = 0, shown = ~0ULL, start_ms = 0;
    if (!g_gw.ready()) {
        shown = ~0ULL;
        return;
    }
    if (now < next && !g_rpc_dirty) return;
    next = now + 5000;
    uint64_t tid = g_rpc ? game::running() : 0;
    // (real games only: Nintendo's application ids all start 01, homebrew installed as an application
    // (sphaira, forwarders) takes others: it isn't announced)
    if ((tid >> 56) != 0x01) tid = 0;
    if (tid == shown && !g_rpc_dirty) return;
    if (tid) {
        char name[CHIRP_NAME];
        if (!game::name(tid, name, sizeof(name))) return;  // (no buffer or no data just now: in 5 s again)
        // (the Switch's clock, as the log's: libc's time() isn't set up in a sysmodule, and gave 1970)
        if (tid != shown) start_ms = (uint64_t)wallClock() * 1000;
        snprintf(g_game, sizeof(g_game), "%s", name);
    } else {
        g_game[0] = 0;
    }
    if (g_gw.presence(g_game, start_ms)) {
        char line[128];
        snprintf(line, sizeof(line), "presence: %s%s%s", g_invisible ? "invisible" : "online", g_game[0] ? ", playing " : "", g_game);
        log(line);
        shown = tid;
        g_rpc_dirty = false;
    }
}

void logFn(const char* line) { log(line); }

bool reloadAsked() {
    mutexLock(&g_lock);
    bool reload = g_pending.reload;
    mutexUnlock(&g_lock);
    return reload;
}

// Waits ms (or less, when Reload is pressed)
void restFor(u64 ms) {
    u64 until = nowMs() + ms;
    while (g_run && nowMs() < until && !reloadAsked()) svcSleepThread(100'000'000LL);
}

// Until Reload is pressed (or Chirp stops)
void waitForReload() {
    while (g_run && !reloadAsked()) restFor(60'000);
}

void setConn(u8 conn) {
    mutexLock(&g_lock);
    g_conn = conn;
    mutexUnlock(&g_lock);
}

// The guild of the call: the gateway's, or (should it not know) its channel's (under g_lock)
uint64_t callGuild() {
    if (g_gw.callGuild()) return g_gw.callGuild();
    if (const store::Channel* c = store::channelFind(g_gw.callChannel()))
        if (const store::Guild* g = store::guildAt(c->guild)) return g->id;
    return 0;
}

// The voice connection follows the bot: in a call with a voice server Discord named, connected to it;
// out of one, not (under g_lock)
void syncVoice() {
    uint64_t channel = g_gw.ready() ? g_gw.callChannel() : 0;
    uint64_t guild = channel ? callGuild() : 0;
    if (channel && guild && g_gw.voiceSession()[0] && g_gw.voiceEndpoint()[0] && g_gw.voiceServerGuild() == guild)
        voice::connect(guild, channel, g_gw.me(), g_gw.voiceSession(), g_gw.voiceToken(), g_gw.voiceEndpoint());
    else if (!channel)
        voice::disconnect();
    // Screenshare turns itself off when the call ends (as in Discord's apps) or when Discord ends the stream
    // itself: a bot not allowed to Go Live was removed from the call each time, and rejoined with it still
    // on, it asked again, and was removed again, for ever
    static uint64_t was_channel = 0;
    static uint32_t ended_seen = 0;
    if (g_gw.streamEnded() != ended_seen) {
        ended_seen = g_gw.streamEnded();
        if (screen::wanted()) {
            screen::setWanted(false);
            log("Screenshare off: Discord ended the stream (the reason's above; a bot may not be allowed to Go Live)");
        }
    }
    if (was_channel && !channel && screen::wanted()) {
        screen::setWanted(false);
        log("Screenshare off: out of the call");
    }
    was_channel = channel;
    // Share Screen is Go Live: once the call's connection is up, a stream is asked for; Discord names
    // its voice server, and the stream's own connection goes there. Off, or out of the call: ended
    bool want = screen::wanted() && channel && guild && voice::state() == voice::Voice_Connected;
    if (want && (g_stream_guild != guild || g_stream_channel != channel)) {
        if (g_stream_channel) g_gw.streamDelete(g_stream_guild, g_stream_channel);
        if (g_gw.streamCreate(guild, channel)) g_stream_guild = guild, g_stream_channel = channel;
    } else if (!want && g_stream_channel) {
        if (channel == g_stream_channel) g_gw.streamDelete(g_stream_guild, g_stream_channel);  // (left the call: it's over anyway)
        g_stream_guild = g_stream_channel = 0;
    }
    if (want && g_stream_channel && g_gw.streamServer() && g_gw.streamEndpoint()[0])
        voice::streamConnect(g_gw.streamServer(), g_gw.streamChannel() ? g_gw.streamChannel() : channel, g_gw.me(), g_gw.voiceSession(),
                             g_gw.streamToken(), g_gw.streamEndpoint());
    else
        voice::streamDisconnect();
}

// Each speaker's volume for the mixer (its thread asks, every frame): percent, 0 when muted for us
int gainFor(uint64_t user) {
    mutexLock(&g_lock);
    int v = store::prefMute(user) ? 0 : store::prefVolume(user);
    mutexUnlock(&g_lock);
    return mixer::gainQ12(v);
}

// What the IPC thread queued, run here (under g_lock)
void runPending() {
    Pending p = g_pending;
    g_pending.join = g_pending.leave = g_pending.voice = false;
    if (!g_gw.ready()) return;
    uint64_t guild = g_gw.callChannel() ? callGuild() : 0;
    if (p.join) {
        g_want_mute = p.j.muted != 0;
        g_gw.voiceState(p.j.guild, p.j.channel, g_want_mute || g_want_deaf, g_want_deaf);
        voice::retry();  // (already in that channel, Discord sends nothing new: a failed connection would stay failed)
    } else if (p.leave) {
        if (guild) g_gw.voiceState(guild, 0, false, false);
        else log("can't leave: which server the call is in isn't known");
    } else if (p.voice && guild) {
        g_gw.voiceState(guild, g_gw.callChannel(), g_want_mute || g_want_deaf, g_want_deaf);  // (deafened is muted too, as in Discord)
    }
    mic::setMuted(g_want_mute || g_want_deaf);  // (the mic is only open unmuted)
}

// One session, until it ends. True if it got as far as READY.
bool session(const char* token) {
    char why[128] = {0};
    setConn(ChirpConn_Connecting);
    if (!g_tls.open(HOST, why, sizeof(why))) {
        log(why);
        return false;
    }
    g_ws.reset(&g_tls, random);
    if (!g_ws.handshake(HOST, PATH, TLS_CONNECT_MS)) {
        log(g_ws.error());
        g_tls.close();
        return false;
    }
    mutexLock(&g_lock);
    g_gw.start(&g_ws, token, random, logFn, nowMs());
    g_gw.setInvisible(g_invisible);  // (in Identify: never online first)
    g_rpc_dirty = true;
    g_session = true;
    mutexUnlock(&g_lock);
    log("connected; waiting for Hello");

    bool was_ready = false;
    bool alive = true;
    while (g_run && alive) {
        u64 now = nowMs();
        int wait = g_gw.waitMs(now);
        if (wait > WAIT_MS) wait = WAIT_MS;
        g_tls.wait(wait);  // (unlocked: the IPC thread gets in meanwhile)
        mutexLock(&g_lock);
        if (g_pending.reload) g_gw.close();
        alive = g_ws.poll(0, &g_gw) && g_gw.tick(nowMs());
        if (alive) runPending();
        alive = alive && !g_gw.over();
        if (g_gw.ready() && !was_ready) {
            was_ready = true;
            g_conn = ChirpConn_Online;
            if (g_rejoin.channel && nowMs() - g_rejoin.at_ms < REJOIN_MS && !g_gw.callChannel()) {
                log("back to the call the last session was in");
                g_gw.voiceState(g_rejoin.guild, g_rejoin.channel, g_want_mute || g_want_deaf, g_want_deaf);
            }
            g_rejoin = {};
        }
        syncVoice();
        rpcTick(nowMs());
        mutexUnlock(&g_lock);
        // (the system's limits each second, logged when they move: here, not on the call's thread, so a
        // write to the card never holds up the call's sound)
        static u64 next_limits = 0;
        if (nowMs() >= next_limits) {
            limits::watch(logFn);
            next_limits = nowMs() + 1000;
            // (the application running (in front or suspended behind the HOME menu), logged when it changes, homebrew too: Rich Presence names real games
            // only, and the screen's recorder follows whatever this is)
            static uint64_t shown_app = ~0ULL;
            uint64_t app = game::running();
            if (app != shown_app) {
                shown_app = app;
                char line[96];
                if (app) snprintf(line, sizeof(line), "application running: %016llX%s", (unsigned long long)app,
                                  (app >> 56) == 0x01 ? "" : " (not a game: homebrew?)");
                else snprintf(line, sizeof(line), "application running: none (nothing open, the HOME menu alone)");
                log(line);
            }
        }
    }

    mutexLock(&g_lock);
    g_session = false;
    // (dropped while in a call: the next session goes back to it; Reload is a fresh start)
    uint64_t call_guild = g_gw.callChannel() ? callGuild() : 0;
    if (call_guild && !g_pending.reload) g_rejoin = {call_guild, g_gw.callChannel(), nowMs()};
    else g_rejoin = {};
    voice::disconnect();
    voice::streamDisconnect();
    g_stream_guild = g_stream_channel = 0;
    g_last_close = g_ws.closeCode();
    const char* gw_why = g_gw.error();
    const char* ws_why = g_ws.error();
    if (g_last_close)
        snprintf(why, sizeof(why), "session over: Discord closed it (%u%s%s)", g_last_close, g_ws.closeReason()[0] ? ": " : "",
                 g_ws.closeReason());
    else snprintf(why, sizeof(why), "session over: %s", gw_why[0] ? gw_why : ws_why[0] ? ws_why : "stopped");
    store::clear();
    mutexUnlock(&g_lock);
    g_ws.sendClose(1000);
    g_tls.close();
    log(why);
    return was_ready;
}

void netThread(void*) {
    // the network may not be up this early in the boot: sockets until they come
    while (g_run && !g_sockets) {
        static const SocketInitConfig cfg = {
            .tcp_tx_buf_size = 0x1000,
            .tcp_rx_buf_size = 0x2000,  // (a GUILD_CREATE can be tens of KB: it's read as it comes, a window at a time)
            .tcp_tx_buf_max_size = 0,
            .tcp_rx_buf_max_size = 0,
            .udp_tx_buf_size = 0x2400,
            .udp_rx_buf_size = 0x4000,  // (voice: read every 5-20 ms; 16 KB holds ~100 packets, for clumps while the stream's busy)
            // (each socket takes its buffers from the pool when it opens: 6 at most, the gateway's, an avatar's,
            // and the call's and the stream's websocket and UDP; 2 was too few once the stream came. The pool
            // is (tx + rx, TCP and UDP) x 3: 87 KB of the heap, down from 159 with 16 KB to receive)
            .sb_efficiency = 3,
            .num_bsd_sessions = 5,  // (calls at once: the gateway, the call, the stream, an avatar; a blocked poll holds one)
            .bsd_service_type = BsdServiceType_Auto,
        };
        g_sockets = R_SUCCEEDED(socketInitialize(&cfg));
        if (!g_sockets) svcSleepThread(5'000'000'000LL);
    }
    while (g_run && !g_ssl) {
        g_ssl = R_SUCCEEDED(sslInitialize(3));
        if (!g_ssl) svcSleepThread(5'000'000'000LL);
    }
    if (g_ssl) {
        avatars::start();
        voice::start(log, random);
        mic::start();
        if (!screen::start()) log("screen sharing can't start: no room for its threads (the heap is full)");
    }
    {
        char line[256];
        limits::describe(line, sizeof(line));
        log(line);
    }

    loadSettings();
    mic::setSettings(g_mic_volume, g_mic_sensitivity, g_mic_agc, g_mic_denoise);
    screen::setFullRange(g_full_range);    size_t failures = 0;
    char token[128];
    while (g_run) {
        mutexLock(&g_lock);
        g_pending.reload = false;
        mutexUnlock(&g_lock);
        if (!loadToken(token, sizeof(token))) {
            setConn(ChirpConn_NoToken);
            waitForReload();  // (then token.txt is read again)
            continue;
        }
        bool was_ready = session(token);
        memset(token, 0, sizeof(token));
        if (!g_run) break;
        mutexLock(&g_lock);
        bool reload = g_pending.reload;
        u16 code = g_last_close;
        mutexUnlock(&g_lock);
        if (reload) {
            failures = 0;
            continue;
        }
        if (code == 4004 || (code >= 4010 && code <= 4014)) {
            // (trying again won't help: wait for Reload)
            setConn(code == 4004 ? ChirpConn_BadToken : ChirpConn_Refused);
            log(code == 4004 ? "Discord turned the token down: check token.txt, then Reload"
                             : "Discord won't take this session as it is: see the close code");
            waitForReload();
            continue;
        }
        if (was_ready) failures = 0;
        uint32_t wait_s = RETRY_S[failures < 5 ? failures : 4];
        failures++;
        mutexLock(&g_lock);
        g_conn = ChirpConn_Offline;
        g_retry_at_ms = nowMs() + wait_s * 1000ULL;
        mutexUnlock(&g_lock);
        restFor(wait_s * 1000ULL);
    }
}

}  // namespace

void log(const char* line) {
    Card c;
    if (!c.ok) return;
    makeDir();
    struct stat st;
    if (stat(CHIRP_LOG, &st) == 0 && (size_t)st.st_size > LOG_MAX) {
        remove(CHIRP_DIR "/chirp.old.log");
        rename(CHIRP_LOG, CHIRP_DIR "/chirp.old.log");
    }
    // (opened and closed for every line: FTP can copy it while Chirp runs)
    if (FILE* f = fopen(CHIRP_LOG, "ab")) {
        TimeCalendarTime cal = {};
        TimeCalendarAdditionalInfo info = {};
        timeToCalendarTimeWithMyRule((u64)wallClock(), &cal, &info);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\n", cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second, line);
        fclose(f);
    }
}

static void loadPrefs();  // (people.ini, further down with its saving)

void start() {
    mutexInit(&g_lock);
    g_run = true;
    log("Chirp started");
    const char* crypto = nullptr;
    aesSelfTest(&crypto);
    log(crypto);
    loadPrefs();
    mixer::setGainSource(gainFor);
    mixer::setMaster(mixer::gainQ12(store::prefVolume(STORE_EVERYONE)));
    if (R_FAILED(threadCreate(&g_thread, netThread, nullptr, nullptr, 0x8000, 0x2C, -2))) {
        g_run = false;
        return;
    }
    threadStart(&g_thread);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    avatars::stop();
    mic::stop();
    screen::stop();
    if (g_ssl) sslExit();
    if (g_sockets) socketExit();
    g_ssl = g_sockets = false;
}

void status(ChirpStatus* s) {
    memset(s, 0, sizeof(*s));
    mutexLock(&g_lock);
    s->conn = g_conn;
    s->last_close = g_last_close;
    s->serial = store::serial();
    u64 now = nowMs();
    s->retry_in_s = g_conn == ChirpConn_Offline && g_retry_at_ms > now ? (u32)((g_retry_at_ms - now + 999) / 1000) : 0;
    s->guilds = (u32)store::guildCount();
    s->sound_check = mixer::soundCheck() ? 1 : 0;
    s->volume = (u16)store::prefVolume(STORE_EVERYONE);
    s->screen_share = screen::wanted();
    s->mic_volume = (uint16_t)g_mic_volume;
    s->mic_sensitivity = (int8_t)g_mic_sensitivity;
    s->mic_agc = g_mic_agc;
    s->mic_denoise = g_mic_denoise;
    s->full_range = g_full_range;
    s->invisible = g_invisible;
    s->rich_presence = g_rpc;
    snprintf(s->game, sizeof(s->game), "%s", g_game);
    snprintf(s->login, sizeof(s->login), "%s", g_login_active);
    if (g_session) g_gw.myAvatar(s->avatar);
    if (s->avatar[0] && g_session) avatars::want(g_gw.me(), 0, s->avatar);  // (the home screen shows it)
    s->screen_live = screen::active() && voice::streamState() == voice::Voice_Connected;
    if (g_session && g_gw.ready()) {
        snprintf(s->user, sizeof(s->user), "%s", g_gw.myName());
        if (g_gw.callChannel()) {
            s->in_call = 1;
            s->call_guild = callGuild();
            s->call_channel = g_gw.callChannel();
            s->call_seconds = (u32)((now - g_gw.callSinceMs()) / 1000);
            s->self_mute = g_gw.selfMute();
            s->self_deaf = g_gw.selfDeaf();
            int slot = store::guildFind(s->call_guild);
            if (const store::Guild* g = store::guildAt(slot)) snprintf(s->call_guild_name, sizeof(s->call_guild_name), "%s", g->name);
            if (const store::Channel* c = store::channelFind(s->call_channel))
                snprintf(s->call_channel_name, sizeof(s->call_channel_name), "%s", c->name);
        }
    }
    if (!s->in_call) {
        s->self_mute = g_want_mute;
        s->self_deaf = g_want_deaf;
    }
    mutexUnlock(&g_lock);
}

size_t listGuilds(size_t first, ChirpGuild* out, size_t cap, size_t* total) {
    mutexLock(&g_lock);
    size_t n = store::listGuilds(first, out, cap);
    *total = store::guildCount();
    mutexUnlock(&g_lock);
    for (size_t i = 0; i < n; i++)  // (the overlay's about to show them: their icons, if not on the card yet)
        if (out[i].icon[0]) avatars::want(0, out[i].id, out[i].icon);
    return n;
}

size_t listChannels(uint64_t guild, ChirpChannel* out, size_t cap) {
    mutexLock(&g_lock);
    size_t n = store::listChannels(guild, out, cap);
    mutexUnlock(&g_lock);
    return n;
}

size_t listMembers(uint64_t channel, ChirpMember* out, size_t cap, size_t* total) {
    mutexLock(&g_lock);
    size_t n = store::listMembers(channel, g_session ? g_gw.me() : 0, out, cap, total);
    uint64_t guild = 0;
    if (const store::Channel* c = store::channelFind(channel))
        if (const store::Guild* g = store::guildAt(c->guild)) guild = g->id;
    mutexUnlock(&g_lock);
    // (the overlay is about to show them: their pictures, if they're not on the card yet)
    for (size_t i = 0; i < n; i++) {
        avatars::want(out[i].user_id, guild, out[i].avatar);
        // (who's talking, and how loud, from what the mixer's playing)
        bool speaking = false;
        uint8_t level = MIXER_SILENT;
        if (out[i].flags & ChirpMember_Me) {  // (us: from the mic)
            if (mic::speaking()) out[i].flags |= ChirpMember_Speaking;
            out[i].level = mic::level();
        } else if (mixer::speaker(out[i].user_id, &speaking, &level)) {
            if (speaking) out[i].flags |= ChirpMember_Speaking;
            out[i].level = level;
        }
    }
    return n;
}

void join(const ChirpJoin& j) {
    mutexLock(&g_lock);
    g_pending.join = true;
    g_pending.j = j;
    mutexUnlock(&g_lock);
}

void leave() {
    mutexLock(&g_lock);
    g_pending.leave = true;
    mutexUnlock(&g_lock);
}

// In a call, what Discord says we are is where a change starts from. (While
// deafened Discord says muted too; g_want_mute keeps whether we were before,
// so undeafening puts that back, as Discord's own app does.)
static void syncWanted() {
    if (g_session && g_gw.callChannel()) {
        g_want_deaf = g_gw.selfDeaf();
        if (!g_want_deaf) g_want_mute = g_gw.selfMute();
    }
}

void setSelfMute(bool on) {
    mutexLock(&g_lock);
    syncWanted();
    g_want_mute = on;
    bool undeafen = !on && g_want_deaf;  // (unmuting undeafens too, as in Discord)
    if (undeafen) g_want_deaf = false;
    g_pending.voice = true;
    mutexUnlock(&g_lock);
    if (undeafen) mixer::setDeaf(false);
}

void setSelfDeaf(bool on) {
    mutexLock(&g_lock);
    syncWanted();
    g_want_deaf = on;
    g_pending.voice = true;
    mutexUnlock(&g_lock);
    mixer::setDeaf(on);
}

// people.ini, written again whenever a mute or volume changes (only the IPC
// thread does: one write at a time)
static char g_prefs_text[STORE_PREFS * 40 + 256];

static void savePrefs() {
    mutexLock(&g_lock);
    size_t n = store::prefsWrite(g_prefs_text, sizeof(g_prefs_text));
    mutexUnlock(&g_lock);
    Card c;
    if (!c.ok) return;
    if (!writeFile(CHIRP_PEOPLE_INI, g_prefs_text, n)) log("couldn't save people.ini");
}

static void loadPrefs() {
    Card c;
    if (!c.ok) return;
    FILE* f = fopen(CHIRP_PEOPLE_INI, "rb");
    if (!f) return;
    size_t n = fread(g_prefs_text, 1, sizeof(g_prefs_text), f);
    fclose(f);
    mutexLock(&g_lock);
    store::prefsRead(g_prefs_text, n);
    mutexUnlock(&g_lock);
}

void setUserMute(uint64_t user, bool on) {
    mutexLock(&g_lock);
    store::prefSetMute(user, on);
    mutexUnlock(&g_lock);
    savePrefs();
}

void setUserVolume(uint64_t user, int volume) {
    mutexLock(&g_lock);
    store::prefSetVolume(user, volume);
    mutexUnlock(&g_lock);
    savePrefs();
}

void setVolume(int volume) {
    mutexLock(&g_lock);
    store::prefSetVolume(STORE_EVERYONE, volume);
    int v = store::prefVolume(STORE_EVERYONE);
    mutexUnlock(&g_lock);
    mixer::setMaster(mixer::gainQ12(v));
    savePrefs();
}

void soundCheck(bool on) { mixer::setSoundCheck(on); }
void setScreenShare(bool on) {
    screen::setWanted(on);  // (syncVoice starts or ends the stream)
}

void setMic(int volume, int sensitivity, bool agc, bool denoise) {
    mutexLock(&g_lock);
    g_mic_volume = volume < 0 ? 0 : volume > 200 ? 200 : volume;
    g_mic_sensitivity = sensitivity >= 0 ? MIC_SENSITIVITY_AUTO : sensitivity < -96 ? -96 : sensitivity;
    g_mic_agc = agc;
    g_mic_denoise = denoise;
    mic::setSettings(g_mic_volume, g_mic_sensitivity, g_mic_agc, g_mic_denoise);
    mutexUnlock(&g_lock);
    saveSettings();
}

void setInvisible(bool on) {
    mutexLock(&g_lock);
    g_invisible = on;
    g_gw.setInvisible(on);
    g_rpc_dirty = true;
    mutexUnlock(&g_lock);
    saveSettings();
}

void setRichPresence(bool on) {
    mutexLock(&g_lock);
    g_rpc = on;
    g_rpc_dirty = true;
    mutexUnlock(&g_lock);
    saveSettings();
}

void setFullRange(bool on) {
    mutexLock(&g_lock);
    g_full_range = on;
    screen::setFullRange(on);
    mutexUnlock(&g_lock);
    saveSettings();
}

void debug(ChirpDebug* d) {
    *d = ChirpDebug{};
    voice::info(d->voice_server, &d->voice_ping, d->stream_server, &d->stream_ping);
    d->voice_state = (uint8_t)voice::state();
    d->stream_state = (uint8_t)voice::streamState();
    int level = 0, threshold = 0, gain = 0;
    d->mic_open = mic::meter(&level, &threshold, &gain);
    d->mic_level = (int16_t)level, d->mic_threshold = (int16_t)threshold, d->mic_gain = (int16_t)gain;
    int sp = 0, dc = 0, sp_max = 0, dc_max = 0;
    mixer::busy(&sp, &dc, &sp_max, &dc_max);
    d->speakers = (uint8_t)sp, d->decoders = (uint8_t)dc, d->speakers_max = (uint8_t)sp_max, d->decoders_max = (uint8_t)dc_max;
    ChirpStatus st;
    status(&st);
    d->call_seconds = st.in_call ? st.call_seconds : 0;
    unsigned free_kb = 0, chirp_kb = 0, heap_used = 0, heap = 0;
    limits::memory(&free_kb, &chirp_kb, &heap_used, &heap);
    d->system_free_kb = free_kb, d->chirp_kb = chirp_kb, d->heap_used_kb = heap_used, d->heap_kb = heap;
}

size_t listLogins(ChirpLogin* out, size_t cap) {
    size_t n = 0;
    eachLogin([&](const char* name, const char*) {
        if (n == cap) return true;
        memset(&out[n], 0, sizeof(out[n]));
        utf8Copy(out[n].name, sizeof(out[n].name), name, strlen(name));
        n++;
        return false;
    });
    return n;
}

void setLogin(const char* name) {
    mutexLock(&g_lock);
    snprintf(g_login, sizeof(g_login), "%s", name);
    mutexUnlock(&g_lock);
    saveSettings();
    reload();  // (a new session, as that bot)
}

void reload() {
    mutexLock(&g_lock);
    g_pending.reload = true;
    mutexUnlock(&g_lock);
}

}  // namespace app
