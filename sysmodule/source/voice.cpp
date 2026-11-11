#include "voice.h"
#include "cpushare.h"
#include "limits.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dave.h"
#include "chirp.h"
#include "json.h"
#include "mic.h"
#include "screen.h"
#include "share.h"
#include "mixer.h"
#include "rtp.h"
#include "tls.h"
#include "util.h"
#include "ws.h"

namespace voice {
namespace {

constexpr int WAIT_MS = 20;
constexpr uint64_t KEEPALIVE_MS = 5000, STATS_MS = 30000;
constexpr int SSRCS = 64;
constexpr size_t BINARY_MAX = RATCHET_ARENA + 16 * 1024;  // (also DAVE's scratch: our commit and Welcome)  // (a DAVE Welcome with the whole tree in it, or many proposals)

enum : int {
    Op_Identify = 0, Op_SelectProtocol = 1, Op_Ready = 2, Op_Heartbeat = 3, Op_SessionDescription = 4, Op_Speaking = 5,
    Op_HeartbeatAck = 6, Op_Hello = 8, Op_ClientsConnect = 11, Op_ClientDisconnect = 13
};

// --- what app.cpp asks for, under g_lock ---

struct Request {
    bool want;
    uint64_t guild, channel, me;
    char session[64], token[96], endpoint[128];
};
class Session;
// A connection and its thread: the call's own (voice: sound both ways), or Go Live's (stream: the
// screen and the game's sound, out only), each to the voice server Discord named for it
struct Link {
    bool stream;
    Request req;        // what app.cpp asks for (under g_lock)
    uint32_t serial;    // (goes up with each new request)
    volatile State state;
    Thread thread;
    Session* s;
    uint8_t* bin;
    volatile uint16_t ping;  // (ms: the last heartbeat there and back)
};
Mutex g_lock;
Link g_voice = {false, {}, 0, Voice_Off, {}, nullptr, nullptr, 0};
Link g_stream = {true, {}, 0, Voice_Off, {}, nullptr, nullptr, 0};
// (the MLS code's big buffers are shared: one connection's DAVE work at a time. Frames are encrypted
// and decrypted with each connection's own keys, outside it)
Mutex g_mls;
volatile bool g_run = false;
// (video is offered on every connection: the voice server gives a bot a video stream (Ready's
// streams), and takes its op 12)
constexpr bool g_video_probe = true;
LogFn g_log = nullptr;
RandomFn g_rnd = nullptr;

uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// share::Video's frames: the screen's queue (screen.h)
struct ScreenFrames : share::Frames {
    bool take(uint8_t** f, size_t* n, size_t* cap, uint64_t* tick, bool* gap) override { return screen::take(f, n, cap, tick, gap); }
    void done() override { screen::done(); }
    uint8_t* borrow(size_t* cap, bool flush) override { return screen::borrowRing(cap, flush); }
    bool fullRange() override { return screen::fullRange(); }
    void giveBack() override { screen::returnRing(); }
} g_frames;

// share::Video's loading screens: *.h264 in CHIRP_LOADING (scripts/loading.ps1 makes them)
struct CardLoading : share::Loading {
    // (one pass, each file taking the pick with a chance of 1 in how many so far: all alike in the end.
    // The SD card's directories can't be rewound (libnx's fsdev), so counting first, then reading again
    // for the one picked, found nothing the second time: no loading screen, and nothing in the log)
    bool pick(char* name, size_t cap, const char** why) override {
        name[0] = 0;
        DIR* dir = opendir(CHIRP_LOADING);
        if (!dir) {
            *why = "no " CHIRP_LOADING " folder";
            return false;
        }
        uint32_t count = 0;
        while (dirent* e = readdir(dir)) {
            size_t n = strlen(e->d_name);
            if (n <= 5 || strcmp(e->d_name + n - 5, ".h264") || n >= cap) continue;
            count++;
            uint32_t r = 0;
            g_rnd(&r, sizeof(r));
            if (r % count == 0) memcpy(name, e->d_name, n + 1);
        }
        closedir(dir);
        *why = "no loading screens in " CHIRP_LOADING;
        return name[0] != 0;
    }
    long read(const char* name, uint8_t* buf, size_t cap) override {
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", CHIRP_LOADING, name);
        FILE* f = fopen(path, "rb");
        if (!f) return -1;
        size_t n = fread(buf, 1, cap, f);
        fclose(f);
        return (long)n;
    }
} g_loading;

void say(const char* fmt, ...) {
    if (!g_log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // (the stream's connection says the same things, DAVE's too: told apart in the log)
    if (g_stream.thread.handle && threadGetCurHandle() == g_stream.thread.handle) {
        char st[264];
        snprintf(st, sizeof(st), "stream: %s", strncmp(line, "voice: ", 7) ? line : line + 7);
        g_log(st);
        return;
    }
    g_log(line);
}

long toLong(const char* s) { return strtol(s, nullptr, 10); }  // (a float's whole part too: "41250.0")

// --- one connection to the voice server ---

class Session : public WsHandler, public JsonHandler, public DaveOut, public share::Sink {
public:
    TlsStream tls;
    WebSocket ws;
    Json reader;
    DaveSession dave;
    Request req;
    Link* link = nullptr;
    bool stream = false;  // (Go Live's connection: see Link)
    uint32_t asent = 0, aunsent = 0;  // (the game's sound)
    // (for the Sender Reports, which line the sound up with the pictures for viewers: the sound's last RTP
    // timestamp and when it was recorded (system ns, 0: none yet); what each stream has sent; when next)
    uint32_t a_rtp_last = 0;
    uint64_t a_ns_last = 0, next_sr = 0;
    uint32_t a_pkts_total = 0, a_octets_total = 0, v_pkts_total = 0, v_octets_total = 0;
    void senderReports();
    // the screen's video: what to send (share.h); sendFrame() sends it
    share::Video video{&g_frames, &g_loading, this};

    // where the connection is
    bool identified = false, described = false, acked = true;
    int unacked = 0;  // (heartbeats in a row with no answer: two, and the connection's taken for dead)
    bool dead = false;
    uint32_t interval = 0;
    uint64_t next_beat = 0, next_keepalive = 0, next_stats = 0, next_limits = 0;
    int64_t seq = -1;
    uint64_t beat_sent = 0;  // (when the last heartbeat went: its ack gives the ping)
    uint64_t cpu_ticks = 0, idle_ticks = 0, cpu_at = 0;  // (this thread's CPU time, core 3's idle, and when they were read: the stats)
    int udp = -1;
    uint32_t ssrc = 0;
    uint32_t vssrc = 0, vrtx = 0;  // (Ready's video stream)
    uint16_t vseq = 0;
    uint32_t vts = 0;
    uint64_t vstart = 0;
    bool video_on = false;  // (announced as active: op 12)
    uint32_t vsent = 0, vbytes = 0;  // (video packets sent, and their bytes)
    // (since the stream began, for the overlay: frames sent, those over 25 ms after the one before, packets
    // viewers reported lost, the biggest frame)
    volatile uint32_t frames_total = 0, late_total = 0, lost_total = 0, frame_max_all = 0;
    // (viewers' RTCP: reports of lost packets and the packets they named; keyframes wanted; bandwidth
    // estimates; RTCP that didn't open. Counted only: Discord passed no lost-packet reports on, so
    // sending packets again (RTX) did nothing, and was taken out)
    uint32_t nacks = 0, lost = 0, plis = 0, rembs = 0, rtcp_unopened = 0;
    uint64_t last_out_ms = 0;                 // (when the last frame finished going out)
    uint32_t out_apart_max = 0, out_late = 0;  // (the longest between two; how many came over 25 ms after the last)
    uint64_t send_t0 = 0;
    // (the longest a frame took to send; the oldest one when its sending began; the biggest)
    uint32_t send_ms_max = 0, age_ms_max = 0, frame_max = 0;
    // (the slowest's time in DAVE's encryption, waiting for room in the socket, and its packets)
    uint32_t slow_dave_us = 0, slow_blocked_us = 0, slow_seal_us = 0, slow_packets = 0;
    RtpMode mode = Rtp_None;
    uint8_t key[RTP_KEY];
    uint32_t keepalive = 0;
    struct Map {
        uint32_t ssrc;
        uint64_t user;
    } map[SSRCS];
    size_t map_n = 0;
    uint32_t packets = 0, unopened = 0, unknown = 0, undecrypted = 0, rtcp = 0;
    int bad_pt = -1;  // (the last packet that didn't open: its payload type, for the log)
    bool first_audio = false;
    // what we send
    uint16_t out_seq = 0;
    uint32_t out_ts = 0, out_counter = 0;
    bool talking = false;
    uint32_t sent = 0, unsent = 0;

    // the message being read
    bool text = false, broken = false;
    int op = -1;
    long d_vssrc = 0, d_vrtx = 0;  // (Ready's video stream, if it gave one)
    long d_interval = 0, d_port = 0, d_ssrc = 0, d_dave = -1, d_transition = -1, d_protocol = -1, d_epoch = -1;
    bool d_aes = false, d_xchacha = false;
    char d_ip[64], d_mode[48];
    uint8_t d_key[RTP_KEY];
    size_t d_key_n = 0;
    uint64_t d_user = 0;
    uint64_t d_users[DAVE_USERS];
    size_t d_users_n = 0;
    uint8_t* bin = nullptr;
    size_t bin_n = 0;

    // DaveOut
    void binary(uint8_t o, const uint8_t* body, size_t n) override { ws.sendBinary(o, body, n); }
    uint8_t* scratch(size_t* cap) override;  // (g_bin: see DaveOut)
    void json(const char* t) override { ws.sendText(t, strlen(t)); }
    void log(const char* line) override { say("%s", line); }  // (DaveOut's and share::Sink's)
    // share::Sink
    bool send(uint8_t* f, size_t n, size_t cap, uint64_t tick) override { return sendFrame(f, n, cap, tick); }

    // WsHandler
    void messageStart(bool is_text) override {
        text = is_text;
        broken = false;
        op = -1;
        d_interval = d_port = d_ssrc = d_vssrc = d_vrtx = 0;
        d_dave = d_transition = d_protocol = d_epoch = -1;
        d_aes = d_xchacha = false;
        d_ip[0] = d_mode[0] = 0;
        d_key_n = 0;
        d_user = 0;
        d_users_n = 0;
        bin_n = 0;
        if (text) reader.reset(this);
    }
    void messageData(const char* p, size_t n) override {
        if (broken) return;
        if (text) {
            if (!reader.feed(p, n)) broken = true;
        } else if (bin_n + n <= BINARY_MAX) {
            memcpy(bin + bin_n, p, n);
            bin_n += n;
        } else {
            broken = true;
        }
    }
    void messageEnd() override {
        // (g_mls only for what hands work to DAVE: its binary messages, and the text ones about the
        // group. Held for every message, Ready's IP discovery (up to 5 s of waiting on UDP) kept the
        // other connection's DAVE waiting too, and a newcomer's sound couldn't be opened meanwhile)
        bool mls = !text || op == Op_SessionDescription || op == Op_ClientsConnect || op == Op_ClientDisconnect ||
                   op >= DaveOp_PrepareTransition;
        if (mls) mutexLock(&g_mls);
        handle();
        if (mls) mutexUnlock(&g_mls);
    }
    void handle();

    // JsonHandler
    void value(const char* path, JsonType type, const char* s, size_t n, bool) override {
        auto is = [&](const char* p) { return strcmp(path, p) == 0; };
        if (is("op") && type == Json_Number) op = (int)toLong(s);
        else if (is("seq") && type == Json_Number) seq = toLong(s);
        else if (is("d.heartbeat_interval") && type == Json_Number) d_interval = toLong(s);
        else if (is("d.ssrc") && type == Json_Number) d_ssrc = toLong(s);
        else if (is("d.streams[].ssrc") && type == Json_Number) d_vssrc = toLong(s);
        else if (is("d.streams[].rtx_ssrc") && type == Json_Number) d_vrtx = toLong(s);
        else if (is("d.port") && type == Json_Number) d_port = toLong(s);
        else if (is("d.ip") && type == Json_String) snprintf(d_ip, sizeof(d_ip), "%.*s", (int)n, s);
        else if (is("d.mode") && type == Json_String) snprintf(d_mode, sizeof(d_mode), "%.*s", (int)n, s);
        else if (is("d.modes[]") && type == Json_String) {
            if (n == 23 && !memcmp(s, "aead_aes256_gcm_rtpsize", 23)) d_aes = true;
            if (n == 31 && !memcmp(s, "aead_xchacha20_poly1305_rtpsize", 31)) d_xchacha = true;
        } else if (is("d.secret_key[]") && type == Json_Number && d_key_n < RTP_KEY) d_key[d_key_n++] = (uint8_t)toLong(s);
        else if (is("d.dave_protocol_version") && type == Json_Number) d_dave = toLong(s);
        else if (is("d.user_id") && type == Json_String) d_user = parseId(s, n);
        else if (is("d.user_ids[]") && type == Json_String && d_users_n < DAVE_USERS) d_users[d_users_n++] = parseId(s, n);
        else if (is("d.transition_id") && type == Json_Number) d_transition = toLong(s);
        else if (is("d.protocol_version") && type == Json_Number) d_protocol = toLong(s);
        else if (is("d.epoch") && type == Json_Number) d_epoch = toLong(s);
    }

    void sendJson(const char* fmt, ...) {
        char msg[768];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        if (n < 0 || n >= (int)sizeof(msg)) {  // (a message cut short would only confuse the server)
            say("voice: a message too long to send (%d bytes): not sent", n);
            return;
        }
        ws.sendText(msg, (size_t)n);
    }

    void identify();
    bool ready();
    void heartbeat(uint64_t now);
    void udpPackets(uint64_t now);
    void sendAudio();
    bool sendFrame(uint8_t* frame, size_t n, size_t cap, uint64_t tick);  // (false: it didn't all get out)
    void sendGameAudio();
    void announceVideo(bool on);
    void closeUdp() {
        if (udp >= 0) close(udp);
        udp = -1;
    }
};

uint8_t g_bin[BINARY_MAX], g_sbin[BINARY_MAX];

uint8_t* Session::scratch(size_t* cap) {
    *cap = BINARY_MAX;
    return bin;
}
Session g_s, g_ss;

void Session::identify() {
    char g[21], c[21], u[21];
    idString(req.guild, g);
    idString(req.channel, c);
    idString(req.me, u);
    char session[96], token[160];
    jsonEscape(session, sizeof(session), req.session);
    jsonEscape(token, sizeof(token), req.token);
    // (max_dave_protocol_version 1: what libdave and Chirp speak)
    // (a stream's connection: server_id is the stream's own (STREAM_CREATE's rtc_server_id), and the
    // channel's named too, as Discord's apps do; its video is a screen)
    if (stream)
        sendJson("{\"op\":0,\"d\":{\"server_id\":\"%s\",\"channel_id\":\"%s\",\"user_id\":\"%s\",\"session_id\":\"%s\",\"token\":\"%s\","
                 "\"max_dave_protocol_version\":1,\"video\":true,\"streams\":[{\"type\":\"screen\",\"rid\":\"100\",\"quality\":100}]}}",
                 g, c, u, session, token);
    else
        sendJson("{\"op\":0,\"d\":{\"server_id\":\"%s\",\"user_id\":\"%s\",\"session_id\":\"%s\",\"token\":\"%s\",\"max_dave_protocol_version\":1%s}}", g, u,
                 session, token, g_video_probe ? ",\"video\":true,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"quality\":100}]" : "");
    wipe(token, sizeof(token));
    identified = true;
}

// Ready: the UDP socket, IP discovery (where Discord sees us from), then Select Protocol
bool Session::ready() {
    ssrc = (uint32_t)d_ssrc;
    closeUdp();
    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp < 0) {
        say("voice: no UDP socket");
        return false;
    }
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)d_port);
    if (inet_pton(AF_INET, d_ip, &to.sin_addr) != 1 || connect(udp, (sockaddr*)&to, sizeof(to)) != 0) {
        say("voice: can't reach the voice server's UDP at %s:%ld", d_ip, d_port);
        return false;
    }
    // [type 1][length 70][ssrc][64 bytes][port]: the answer has our address (a string) and port (big-endian, at the end)
    uint8_t pkt[74] = {0, 1, 0, 70};
    for (int i = 0; i < 4; i++) pkt[4 + i] = (uint8_t)(ssrc >> (24 - 8 * i));
    char ip[64] = {0};
    long port = -1;
    for (int attempt = 0; attempt < 5 && port < 0; attempt++) {
        ::send(udp, pkt, sizeof(pkt), 0);
        pollfd p = {udp, POLLIN, 0};
        if (poll(&p, 1, 1000) != 1) continue;
        uint8_t ans[128];
        int n = (int)recv(udp, ans, sizeof(ans), 0);
        if (n < 74 || ans[0] != 0 || ans[1] != 2) continue;
        snprintf(ip, sizeof(ip), "%.*s", (int)sizeof(ip) - 1, (const char*)ans + 8);
        port = ans[n - 2] << 8 | ans[n - 1];
    }
    if (port < 0) {
        say("voice: no answer to IP discovery");
        return false;
    }
    int flags = fcntl(udp, F_GETFL, 0);
    fcntl(udp, F_SETFL, flags | O_NONBLOCK);
    mode = d_aes ? Rtp_Aes256Gcm : d_xchacha ? Rtp_XChaCha : Rtp_None;
    if (mode == Rtp_None) {
        say("voice: the voice server offers no encryption Chirp knows");
        return false;
    }
    sendJson("{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{\"address\":\"%s\",\"port\":%ld,\"mode\":\"%s\"}%s}}", ip, port,
         mode == Rtp_Aes256Gcm ? "aead_aes256_gcm_rtpsize" : "aead_xchacha20_poly1305_rtpsize",
         g_video_probe ? ",\"codecs\":[{\"name\":\"opus\",\"type\":\"audio\",\"priority\":1000,\"payload_type\":120},"
                         "{\"name\":\"H264\",\"type\":\"video\",\"priority\":1000,\"payload_type\":101,\"rtx_payload_type\":102,"
                         "\"encode\":true,\"decode\":true}]"
                       : "");
    say("voice: video stream ssrc %ld, rtx %ld (0: none given)", d_vssrc, d_vrtx);
    vssrc = (uint32_t)d_vssrc, vrtx = (uint32_t)d_vrtx;
    say("voice: UDP ready (ssrc %u, %s)", ssrc, mode == Rtp_Aes256Gcm ? "AES-256-GCM" : "XChaCha20-Poly1305");
    return true;
}

void Session::heartbeat(uint64_t now) {
    if (!acked) {
        say("voice: the last heartbeat wasn't answered");
        // (a connection gone quiet without closing: the call went silent and stayed so. Over: the thread
        // connects again, as the gateway does)
        if (++unacked >= 2) {
            say("voice: no answer to two heartbeats: connecting again");
            ws.sendClose(1000);
            dead = true;
            return;
        }
    } else {
        unacked = 0;
    }
    acked = false;
    beat_sent = now;
    sendJson("{\"op\":3,\"d\":{\"t\":%llu,\"seq_ack\":%lld}}", (unsigned long long)now, (long long)seq);
    next_beat = now + interval;
}

void Session::handle() {
    uint64_t now = nowMs();
    if (broken) {
        say("voice: a message that couldn't be read: skipped");
        return;
    }
    if (!text) {
        // [seq u16][opcode][body]: a DAVE message
        if (bin_n < 3) return;
        seq = bin[0] << 8 | bin[1];
        dave.onBinary(bin[2], bin + 3, bin_n - 3, now);
        return;
    }
    switch (op) {
        case Op_Hello: {
            interval = (uint32_t)d_interval;
            uint32_t r = 0;
            g_rnd(&r, sizeof(r));
            next_beat = now + (uint64_t)interval * (r % 1000) / 1000;
            if (!identified) identify();
            break;
        }
        case Op_Ready:
            if (!ready()) ws.sendClose(1000);
            break;
        case Op_SessionDescription:
            if (d_key_n != RTP_KEY) {
                say("voice: a session description without a key");
                break;
            }
            memcpy(key, d_key, RTP_KEY);
            wipe(d_key, sizeof(d_key));
            described = true;
            dave.onProtocol(d_dave < 0 ? 0 : (int)d_dave, now);
            say("voice: connected (DAVE protocol %ld)", d_dave);
            link->state = Voice_Connected;
            if (stream) mixer::setCushion(3);  // (60 ms: the call's voices share the Wi-Fi with the video now)
            if (!stream) {
                mixer::setVoice(true);
                mic::setCall(true);
            } else {
                if (vssrc) announceVideo(false);  // (there, not sending yet)
                // (the stream's sound: "soundshare", as Discord's apps say it, once)
                sendJson("{\"op\":5,\"d\":{\"speaking\":2,\"delay\":0,\"ssrc\":%u}}", ssrc);
            }
            break;
        case Op_HeartbeatAck:
            acked = true;
            if (beat_sent && now >= beat_sent) link->ping = (uint16_t)(now - beat_sent > 65535 ? 65535 : now - beat_sent);
            break;
        case Op_Speaking:
            // (who's behind an ssrc: their packets are known by it)
            if (d_user) {
                size_t i = 0;
                while (i < map_n && map[i].ssrc != (uint32_t)d_ssrc && map[i].user != d_user) i++;
                if (i == map_n && map_n < SSRCS) map_n++;
                if (i < SSRCS) map[i] = Map{(uint32_t)d_ssrc, d_user};
            }
            break;
        case Op_ClientsConnect:
            dave.onClientsConnect(d_users, d_users_n);
            break;
        case Op_ClientDisconnect:
            dave.onClientDisconnect(d_user);
            for (size_t i = 0; i < map_n; i++)
                if (map[i].user == d_user) map[i--] = map[--map_n];
            break;
        case DaveOp_PrepareTransition:
            dave.onPrepareTransition((int)d_protocol, (uint32_t)d_transition, now);
            break;
        case DaveOp_ExecuteTransition:
            dave.onExecuteTransition((uint32_t)d_transition, now);
            break;
        case DaveOp_PrepareEpoch:
            dave.onPrepareEpoch((int)d_protocol, (uint64_t)d_epoch, now);
            break;
        default:
            if (op != Op_HeartbeatAck) say("voice: opcode %ld ignored", (long)op);
            break;
    }
}

// What the mic has ready: DAVE-encrypted with our key, sealed in RTP, sent. Speaking (op 5) is
// said when talking starts, and unsaid with its last silence frame
void Session::sendAudio() {
    static mic::Packet pk;
    static uint8_t frame[MIC_PACKET_BYTES + DAVE_SUPPLEMENTAL_MAX];
    static uint8_t pkt[sizeof(frame) + RTP_OVERHEAD];
    while (mic::take(&pk)) {
        if (!talking && pk.last) continue;  // (silence after nothing was said)
        if (!talking) {
            sendJson("{\"op\":5,\"d\":{\"speaking\":1,\"delay\":0,\"ssrc\":%u}}", ssrc);
            talking = true;
        }
        size_t n = 0;
        if (dave.encrypt(pk.data, pk.n, frame, sizeof(frame), &n)) {
            // (the timestamp is the mic's clock, pauses included: run on through them, as it once did, a
            // receiver's jitter buffer took each new stretch of talk for packets arriving late, and grew)
            out_seq++;
            uint32_t ts = out_ts + pk.frame * 960;
            size_t m = rtpSeal(mode, key, &out_counter, out_seq, ts, ssrc, frame, n, pkt);
            if (::send(udp, pkt, m, 0) == (ssize_t)m) sent++;
            else unsent++;
        } else
            unsent++;
        if (pk.last) {
            sendJson("{\"op\":5,\"d\":{\"speaking\":0,\"delay\":0,\"ssrc\":%u}}", ssrc);
            talking = false;
        }
    }
}

// A Sender Report for the video and one for the game's sound, about once a second: each says which of its
// RTP timestamps is now, on one wall clock (the system's), so viewers' players can line the sound up with
// the pictures. (Without them each stream was played as it came, in step only by luck.) The video's
// timestamps are its frames' recorded times on the system's clock already; the sound's packets carry
// theirs (screen.h), from the same recorder clock
void Session::senderReports() {
    uint8_t sr[RTCP_SR_SIZE];
    uint64_t now_ns = armTicksToNs(armGetSystemTick());
    if (v_pkts_total) {
        uint32_t v_rtp = vts + (uint32_t)((now_ns - armTicksToNs(vstart)) * 9 / 100000);  // (90 kHz, as sendFrame's)
        size_t m = rtcpSenderReport(mode, key, &out_counter, vssrc, now_ns, v_rtp, v_pkts_total, v_octets_total, sr);
        ::send(udp, sr, m, 0);
    }
    if (a_ns_last && a_pkts_total) {
        int64_t since = (int64_t)(now_ns - a_ns_last);
        uint32_t a_rtp = a_rtp_last + (uint32_t)(since * 48000 / 1'000'000'000LL);  // (48 kHz)
        size_t m = rtcpSenderReport(mode, key, &out_counter, ssrc, now_ns, a_rtp, a_pkts_total, a_octets_total, sr);
        ::send(udp, sr, m, 0);
    }
}

// The game's sound (screen.h): DAVE-encrypted, sealed in RTP, sent; its timestamp follows the
// recorder's 20 ms frames
void Session::sendGameAudio() {
    static screen::AudioPacket ap;
    static uint8_t frame[SCREEN_AUDIO_BYTES + DAVE_SUPPLEMENTAL_MAX];
    static uint8_t pkt[sizeof(frame) + RTP_OVERHEAD];
    while (screen::takeAudio(&ap)) {
        size_t n = 0;
        if (dave.encrypt(ap.data, ap.n, frame, sizeof(frame), &n)) {
            out_seq++;
            uint32_t rtp_ts = out_ts + ap.frame * 960;
            size_t m = rtpSeal(mode, key, &out_counter, out_seq, rtp_ts, ssrc, frame, n, pkt);
            if (ap.ns) a_rtp_last = rtp_ts, a_ns_last = ap.ns;  // (where the sound is on the video's clock: the Sender Reports)
            // (the socket's buffer may be full of video: a moment's wait for room, not a packet lost)
            bool ok = false;
            for (int tries = 0; tries < 4; tries++) {
                if (::send(udp, pkt, m, 0) == (ssize_t)m) {
                    ok = true;
                    break;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
                pollfd w = {udp, POLLOUT, 0};
                poll(&w, 1, 5);
            }
            if (ok) asent++, a_pkts_total++, a_octets_total += (uint32_t)n;
            else aunsent++;
        } else
            aunsent++;
    }
}

// The video stream, active or not (op 12): 1280x720, as grc:d records; up to 60 a second and 10 Mbps
// (grc's own 30 and ~5, or 60 if Atmosphere's system_settings.ini asks it for that: these are only the most)
void Session::announceVideo(bool on) {
    sendJson("{\"op\":12,\"d\":{\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u,\"streams\":[{\"type\":\"video\","
             "\"rid\":\"100\",\"ssrc\":%u,\"active\":%s,\"quality\":100,\"rtx_ssrc\":%u,\"max_bitrate\":10000000,"
             "\"max_framerate\":60,\"max_resolution\":{\"type\":\"fixed\",\"width\":1280,\"height\":720}}]}}",
             ssrc, vssrc, vrtx, vssrc, on ? "true" : "false", vrtx);
    if (on != video_on) say("voice: screen sharing %s", on ? "on" : "off");
    video_on = on;
}

// A frame (Annex-B, in a buffer of cap bytes): DAVE-encrypted in place, cut into H.264 RTP payloads,
// each sealed and sent. (The socket's send buffer is small: when it's full, a moment's wait for room; a
// frame that can't get through is given up on: false)
bool Session::sendFrame(uint8_t* frame, size_t n, size_t cap, uint64_t tick) {
    send_t0 = nowMs();
    uint64_t now_t = armGetSystemTick();
    uint32_t age = now_t > tick ? (uint32_t)(armTicksToNs(now_t - tick) / 1000000) : 0;
    if (age > age_ms_max) age_ms_max = age;  // (since the recorder recorded it: its wait in grc and our queue)
    if (n > frame_max) frame_max = (uint32_t)n;
    size_t out_n = 0;
    uint64_t d0 = armGetSystemTick();
    if (!dave.encryptVideo(frame, n, cap, &out_n)) return false;
    uint64_t dave_ticks = armGetSystemTick() - d0, blocked_ticks = 0, seal_ticks = 0;
    uint32_t packets = 0;
    uint32_t ts = vts + (uint32_t)(armTicksToNs(tick - vstart) * 9 / 100000);  // (90 kHz)
    // (not paced: as fast as the socket takes them. Paced at 24 Mbps, then 40, a keyframe held the frames
    // behind it 60 ms, once a second, and viewers saw the frame rate judder though every frame was sent.
    // And not one send() a packet: each is a call into the system's sockets (bsd), ~0.3 ms, and a 200-packet
    // keyframe took 70 ms though the socket never once was full. Sealed into a batch of BATCH, sent with
    // one sendmmsg(): one call for them all)
    constexpr unsigned BATCH = 16;
    // (each packet as full as Discord takes: 1200 bytes of UDP payload, WebRTC's usual size, RTP_OVERHEAD
    // of it ours. Each packet costs the system's sockets the same ~50-100 us of core 3 whatever its size
    // (sockbench), so fewer, fuller ones: 1100 of video before. Filling a 1500-byte frame (1472) Discord
    // dropped: viewers saw nothing)
    constexpr size_t PAYLOAD = 1200 - RTP_OVERHEAD;
    constexpr size_t PKT = PAYLOAD + RTP_OVERHEAD;
    static uint8_t scratch[PAYLOAD], pkts[BATCH][PKT];
    static iovec iov[BATCH];
    static mmsghdr msgs[BATCH];
    static bool no_mmsg = false;  // (sendmmsg refused: one send() a packet, as before)
    unsigned queued = 0;
    bool gave_up = false;
    // The batch out. (The socket's send buffer (app.cpp's: 64 KB) full: a moment's wait for room, up to
    // SEND_PATIENCE_MS: capture's queue holds the frames meanwhile. Given up on: the whole frame, false)
    auto flush = [&]() {
        constexpr uint64_t SEND_PATIENCE_MS = 250;
        unsigned done = 0;
        uint64_t w0 = 0;
        while (done < queued) {
            int sent;
            if (!no_mmsg) {
                sent = sendmmsg(udp, msgs + done, queued - done, 0);
                if (sent < 0 && (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL) && !done) {
                    no_mmsg = true;
                    say("screen: sendmmsg isn't there (errno %d): a send() a packet", errno);
                    continue;
                }
            } else {
                sent = ::send(udp, iov[done].iov_base, iov[done].iov_len, 0) == (ssize_t)iov[done].iov_len ? 1 : -1;
            }
            if (sent > 0) {
                for (int i = 0; i < sent; i++) {
                    vbytes += (uint32_t)iov[done + i].iov_len;
                    v_octets_total += (uint32_t)(iov[done + i].iov_len - RTP_OVERHEAD);
                }
                done += (unsigned)sent, vsent += (uint32_t)sent, v_pkts_total += (uint32_t)sent;
                continue;
            }
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
                gave_up = true;
                return;
            }
            uint64_t t = armTicksToNs(armGetSystemTick());
            if (!w0) w0 = t;
            else if (t - w0 >= SEND_PATIENCE_MS * 1'000'000ULL) {
                gave_up = true;
                return;
            }
            pollfd w = {udp, POLLOUT, 0};
            if (poll(&w, 1, 10) <= 0) svcSleepThread(1'000'000LL);  // (ENOBUFS isn't polled for: a moment's rest)
        }
        if (w0) blocked_ticks += armGetSystemTick() - armNsToTicks(w0);
        queued = 0;
    };
    rtpH264Payloads(frame, out_n, sizeof(scratch), scratch, [&](const uint8_t* p, size_t k, bool last) {
        if (gave_up) return;
        uint64_t s0 = armGetSystemTick();
        size_t m = rtpSealAs(mode, key, &out_counter, RTP_H264, last, vseq++, ts, vssrc, p, k, pkts[queued]);
        seal_ticks += armGetSystemTick() - s0;
        iov[queued] = {pkts[queued], m};
        msgs[queued] = {};
        msgs[queued].msg_hdr.msg_iov = &iov[queued];
        msgs[queued].msg_hdr.msg_iovlen = 1;
        queued++, packets++;
        if (queued == BATCH || last) {
            flush();
            sendGameAudio();  // (the sound between batches: a keyframe takes a while to get out)
        }
    });
    if (queued && !gave_up) flush();
    if (!gave_up) {  // (how evenly frames leave: what viewers' "frame rate (network)" sees)
        uint64_t t = nowMs();
        if (t - send_t0 > send_ms_max) {  // (the slowest: where its time went)
            send_ms_max = (uint32_t)(t - send_t0);
            slow_dave_us = (uint32_t)(armTicksToNs(dave_ticks) / 1000);
            slow_blocked_us = (uint32_t)(armTicksToNs(blocked_ticks) / 1000);
            slow_seal_us = (uint32_t)(armTicksToNs(seal_ticks) / 1000);
            slow_packets = packets;
        }
        uint32_t apart = last_out_ms ? (uint32_t)(t - last_out_ms) : 0;
        last_out_ms = t;
        if (apart > out_apart_max) out_apart_max = apart;
        // (late: more than a frame and a half after the one before, at the rate the Switch records: 25 ms at
        // 60 fps, 50 at 30)
        unsigned fps = screen::recordFps();
        if (apart > (fps ? 1500 / fps : 25)) out_late++, late_total = late_total + 1;
        frames_total = frames_total + 1;
        if (n > frame_max_all) frame_max_all = (uint32_t)n;
    }
    return !gave_up;
}

void Session::udpPackets(uint64_t now) {
    uint8_t in[2048];  // (each connection's own: they run at once)
    static uint8_t open[2048], frame[2048];  // (only the call's: a stream's sound is ours alone)
    for (;;) {
        int n = (int)recv(udp, in, sizeof(in), 0);
        if (n <= 0) return;
        if (!described || n < 12) continue;
        if (in[1] >= 200 && in[1] <= 206) {  // (RTCP: reports, and viewers' feedback on our video; nothing to play)
            rtcp++;
            if (!stream) continue;
            // (sealed as RTP is, past its first 8 bytes; one that doesn't open is read as it came, in case
            // it was sent plain: rtcpFeedback stops at anything that doesn't parse)
            uint8_t opened[2048];
            size_t on = rtcpOpen(mode, key, in, (size_t)n, opened);
            if (!on) rtcp_unopened++;
            RtcpFeedback fb = {};
            rtcpFeedback(on ? opened : in, on ? on : (size_t)n, vssrc, nullptr, 0, &fb);
            nacks += fb.nacks, lost += fb.lost, plis += fb.keyframes, rembs += fb.remb;
            lost_total = lost_total + fb.lost;
            continue;
        }
        if (stream) continue;  // (nothing to hear on a stream)
        RtpPacket p;
        if (!rtpOpen(mode, key, in, (size_t)n, open, &p)) {
            unopened++;
            if (n >= 2) bad_pt = in[1] & 0x7f;
            continue;
        }
        uint64_t user = 0;
        for (size_t i = 0; i < map_n && !user; i++)
            if (map[i].ssrc == p.ssrc) user = map[i].user;
        if (!user) {
            unknown++;
            continue;
        }
        // (this connection's own keys, on its own thread, as its DAVE messages are handled: not under
        // g_mls, which is for the MLS work's shared buffers. Under it, every voice waited while the other
        // connection worked through a commit or a Welcome, and the mixer ran dry)
        size_t fn = 0;
        bool ok = dave.decrypt(user, p.payload, p.payload_n, frame, &fn, now);
        if (!ok) {
            undecrypted++;
            continue;
        }
        packets++;
        if (!first_audio) {
            first_audio = true;
            say("voice: sound is coming in");
        }
        mixer::voicePacket(user, p.seq, frame, fn);
    }
}

// The endpoint ("host" or "host:port") split
void splitEndpoint(const char* endpoint, char* host, size_t host_cap, char* port, size_t port_cap) {
    const char* colon = strrchr(endpoint, ':');
    if (colon) {
        snprintf(host, host_cap, "%.*s", (int)(colon - endpoint), endpoint);
        snprintf(port, port_cap, "%s", colon + 1);
    } else {
        snprintf(host, host_cap, "%s", endpoint);
        snprintf(port, port_cap, "443");
    }
}

// One connection, until it ends or app.cpp asks for another; the close code (0: none came)
uint16_t runSession(Link& L, uint32_t serial) {
    Session& s = *L.s;
    s.link = &L;
    s.stream = L.stream;
    s.bin = L.bin;
    s.asent = s.aunsent = 0;
    s.cpu_at = 0;
    s.video.reset(nowMs());  // (late frames are sent all the same: setStale() isn't used)
    s.identified = s.described = false;
    s.acked = true;
    s.unacked = 0, s.dead = false;
    s.seq = -1;
    s.interval = 0;
    s.map_n = 0;
    s.packets = s.unopened = s.unknown = s.undecrypted = s.rtcp = 0;
    s.first_audio = false;
    s.mode = Rtp_None;
    s.talking = false;
    s.sent = s.unsent = 0;
    g_rnd(&s.out_seq, sizeof(s.out_seq));  // (random starts, as RTP asks)
    g_rnd(&s.out_ts, sizeof(s.out_ts));
    mutexLock(&g_mls);
    s.dave.start(s.req.me, s.req.channel, &s, (MlsGroup::Random)g_rnd);
    mutexUnlock(&g_mls);
    s.vssrc = s.vrtx = 0;
    s.video_on = false;
    s.vsent = s.vbytes = s.nacks = s.lost = s.plis = s.rembs = s.rtcp_unopened = 0;
    s.last_out_ms = 0, s.out_apart_max = s.out_late = 0;
    s.a_rtp_last = 0, s.a_ns_last = 0, s.next_sr = 0;
    s.a_pkts_total = s.a_octets_total = s.v_pkts_total = s.v_octets_total = 0;
    s.frames_total = 0, s.late_total = 0, s.lost_total = 0, s.frame_max_all = 0;
    g_rnd(&s.vseq, sizeof(s.vseq));
    g_rnd(&s.vts, sizeof(s.vts));
    s.vstart = armGetSystemTick();
    char host[128], port[16], why[128];
    splitEndpoint(s.req.endpoint, host, sizeof(host), port, sizeof(port));
    L.state = Voice_Connecting;
    say("voice: connecting to %s", host);
    if (!s.tls.open(host, why, sizeof(why), port)) {
        say("voice: %s", why);
        return 0;
    }
    s.ws.reset(&s.tls, (WebSocket::RandomFn)g_rnd);
    char hosthdr[160];
    if (strcmp(port, "443") == 0) snprintf(hosthdr, sizeof(hosthdr), "%s", host);
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%s", host, port);
    if (!s.ws.handshake(hosthdr, "/?v=8", TLS_CONNECT_MS)) {
        say("voice: %s", s.ws.error());
        s.tls.close();
        return 0;
    }
    s.ws.poll(0, &s);  // (what came with the handshake: Hello, likely)
    uint64_t now = nowMs();
    s.next_keepalive = now + KEEPALIVE_MS;
    s.next_stats = now + 10000;  // (the first sooner: whether sound gets through shows early)
    bool alive = true;
    while (g_run && alive) {
        mutexLock(&g_lock);
        bool same = L.serial == serial && L.req.want;
        mutexUnlock(&g_lock);
        if (!same) break;
        // the websocket and the UDP socket, whichever has something first
        pollfd p[2] = {{s.tls.fd(), POLLIN, 0}, {s.udp, POLLIN, 0}};
        // (something already in the TLS layer: a moment only, but always a wait, as part of a record
        // reads as nothing yet and this loop would otherwise go round without letting anyone else run)
        // (while the mic records, a packet is ready every 20 ms: looked for every 5)
        // (and while the screen's shared, woken by capture itself when a frame or a sound packet's ready: a
        // frame waiting up to 5 ms here arrived as unevenly, and polling every 1 ms instead was a thousand
        // calls a second into the system's sockets, all on core 3. The sockets looked at without waiting
        // then, and at least every 5 ms)
        if (s.stream && screen::active() && !s.tls.pending()) {
            screen::waitWork(5'000'000ULL);
            poll(p, s.udp >= 0 ? 2 : 1, 0);
        } else {
            poll(p, s.udp >= 0 ? 2 : 1, s.tls.pending() ? 1 : (mic::active() || screen::active()) ? 5 : WAIT_MS);
        }
        now = nowMs();
        alive = s.ws.poll(0, &s) && !s.dead;
        if (s.udp >= 0) s.udpPackets(now);
        if (s.udp >= 0 && s.described && s.mode != Rtp_None) {
            if (!s.stream) {
                s.sendAudio();
            } else {
                // the screen: shared when wanted and the server gave a stream; announced active while it is
                screen::setCall(s.vssrc != 0);
                bool on = screen::active();
                if (on != s.video_on) s.announceVideo(on);
                // (frames queued behind a keyframe: a few at once, the sound between)
                for (int k = 0; on && k < 4 && s.video.step(now, armGetSystemTick()); k++) s.sendGameAudio();
                s.sendGameAudio();
                if (on && now >= s.next_sr) {
                    s.senderReports();
                    s.next_sr = now + 1000;
                }
            }
        }
        if (s.interval && now >= s.next_beat) s.heartbeat(now);
        if (s.udp >= 0 && now >= s.next_keepalive) {
            // (a keepalive: so the way back through NATs stays open)
            uint8_t ka[8] = {0};
            for (int i = 0; i < 4; i++) ka[i] = (uint8_t)(s.keepalive >> (8 * i));
            s.keepalive++;
            ::send(s.udp, ka, sizeof(ka), 0);
            s.next_keepalive = now + KEEPALIVE_MS;
        }
        if (s.stream && now >= s.next_stats) {
            // (this thread's share of a core since the last stats: encrypting and sending the video)
            // (and how much of core 3, everyone's, sat idle: little, and the sending waits on the others there)
            uint64_t cpu = 0, idle = 0, at = armGetSystemTick();
            svcGetInfo(&cpu, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total);
            svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, (u64)-1);
            if (s.cpu_at && at > s.cpu_at)
                say("stream: %u%% of a core; core 3 idle %u%%", (unsigned)((cpu - s.cpu_ticks) * 100 / (at - s.cpu_at)),
                    (unsigned)((idle - s.idle_ticks) * 100 / (at - s.cpu_at)));
            s.cpu_ticks = cpu, s.idle_ticks = idle, s.cpu_at = at;
            screen::Stats sc;
            screen::takeStats(&sc);
            say("screen: the recorder's frames %u skipped (we were too slow), %u dropped with no room to queue (the network's "
                "slower than the video), capture waited %u ms for room; "
                "their timestamps %llu..%llu apart", sc.skipped, sc.no_room, sc.waited_ms, (unsigned long long)sc.gap_min,
                (unsigned long long)sc.gap_max);
            // (in three lines: one, longer than a log line's 256 bytes, lost its end)
            share::Video::Stats vs;
            s.video.takeStats(&vs);
            say("screen: %s, %u frames captured, %u sent (%u keyframes) in %u packets (%u KB), %u not sent; %u loading screens "
                "or grey; %u too late (skipped), %u skipped waiting for a keyframe", sc.no_game ? "waiting for a title that records" : sc.open ? "capturing" : "not capturing",
                sc.captured, vs.sent, vs.keyframes, s.vsent, s.vbytes / 1024, vs.failed, vs.placeholders, vs.stale, vs.waited);
            say("screen: grc:d %u failures (0x%X); its longest wait for a frame %u ms (waiting now: %u ms), for sound %u ms (now: "
                "%u ms); %u sound chunks read", sc.failed, sc.last_rc, sc.video_longest_ms, sc.video_waiting_ms, sc.audio_longest_ms,
                sc.audio_waiting_ms, sc.audio_chunks);
            say("screen: viewers reported %u packets lost (in %u reports); %u keyframe requests, %u bandwidth estimates, "
                "%u RTCP didn't open", s.lost, s.nacks, s.plis, s.rembs, s.rtcp_unopened);
            say("screen: game sound: %u packets sent (%u not), %u dropped waiting, %u gaps", s.asent, s.aunsent,
                sc.audio_dropped, sc.audio_holes);
            say("screen: frames went out up to %u ms apart; %u late (over a frame and a half after the one before); the slowest took %u ms "
                "to send, the biggest %u KB; the oldest was %u ms old when its sending began", s.out_apart_max, s.out_late,
                s.send_ms_max, s.frame_max / 1024, s.age_ms_max);
            say("screen: the slowest frame: %u packets; %u us in DAVE's encryption, %u us sealing, %u us waiting for room "
                "in the socket, the rest sending", s.slow_packets, s.slow_dave_us, s.slow_seal_us, s.slow_blocked_us);
            s.vsent = s.vbytes = s.nacks = s.lost = s.plis = s.rembs = s.rtcp_unopened = 0;
            s.out_apart_max = s.out_late = s.send_ms_max = s.age_ms_max = s.frame_max = 0;
            s.slow_dave_us = s.slow_blocked_us = s.slow_seal_us = s.slow_packets = 0;
            s.asent = s.aunsent = 0;
            s.next_stats = now + STATS_MS;
        }
        if (!s.stream && now >= s.next_stats) {
            say("voice: %u packets in, %u didn't open (the last payload type %d), %u from an unknown ssrc, %u didn't decrypt (DAVE), "
                "%u RTCP", s.packets, s.unopened, s.bad_pt, s.unknown, s.undecrypted, s.rtcp);
            s.bad_pt = -1;
            uint32_t silence, plain, opened;
            s.dave.takeFrameStats(&silence, &plain, &opened);
            say("DAVE: %u frames Discord's silence, %u plain, %u decrypted", silence, plain, opened);
            mixer::Stats m;
            mixer::takeStats(&m);
            say("mixer: %u packets, decoders %u opened %u failed, %u decoded %u failed, %u frames mixed, loudest %s%u dB, "
                "gain %d%%, %s, audout %s, last error 0x%X; %u lost, %u late, waited %u frames, skipped %u to catch up, audout ran dry %u times",
                m.packets, m.dec_opened, m.dec_failed, m.decoded, m.decode_failed, m.mixed, m.loudest == MIXER_SILENT ? "none " : "-",
                m.loudest == MIXER_SILENT ? 0u : (unsigned)m.loudest, m.last_gain, m.deaf ? "DEAFENED" : "not deafened",
                m.out_open ? "open" : "CLOSED", m.last_rc, m.gaps, m.late, m.waited, m.trimmed, m.dry);
            mic::Stats mc;
            mic::takeStats(&mc);
            say("mic: %s, %u frames heard, %u talking, %u sent (%u not), %u dropped waiting; room %d dB, gain %+d dB, "
                "loudest sent %d dB; %u clicks kept out; %u.%u%% of a core (%u.%u%% noise suppression), %u frames lost (too slow); "
                "%u failures to open (0x%X)",
                mc.open ? "open" : "closed", mc.frames, mc.talking, s.sent, s.unsent, mc.dropped, mc.floor_db, mc.gain_db,
                mc.peak_db, mc.clicks, mc.frames ? mc.busy_us / (mc.frames * 200) : 0,
                mc.frames ? mc.busy_us / (mc.frames * 20) % 10 : 0, mc.frames ? mc.denoise_us / (mc.frames * 200) : 0,
                mc.frames ? mc.denoise_us / (mc.frames * 20) % 10 : 0, mc.lost, mc.open_failed, mc.last_rc);
            s.sent = s.unsent = 0;
            s.next_stats = now + STATS_MS;
        }
    }
    if (s.stream) screen::setCall(false), mixer::setCushion(MIXER_START_AT);
    else mic::setCall(false);
    uint16_t code = s.ws.closeCode();
    if (code) say("voice: the voice server closed it (%u%s%s)", code, s.ws.closeReason()[0] ? ": " : "", s.ws.closeReason());
    else if (s.ws.error()[0]) say("voice: %s", s.ws.error());
    s.ws.sendClose(1000);
    s.tls.close();
    s.closeUdp();
    wipe(s.key, sizeof(s.key));
    if (!s.stream) mixer::setVoice(false);
    return code;
}

constexpr int TRIES = 5;  // connections tried for one request before it's left (Join tries again)

void thread(void* arg) {
    Link& L = *(Link*)arg;
    uint32_t done = 0;  // (the last request a connection was made for)
    int retries = 0;
    int idle = 0;
    while (g_run) {
        mutexLock(&g_lock);
        bool want = L.req.want;
        uint32_t serial = L.serial;
        if (want) L.s->req = L.req;
        mutexUnlock(&g_lock);
        if (!want || (serial == done && retries >= TRIES)) {
            if (!want) L.state = Voice_Off, retries = 0;
            svcSleepThread(100'000'000LL);
            // (out of a call too: the system's limits each second, logged when they move)
            if (!L.stream && ++idle >= 10) {
                idle = 0;
                limits::watch(g_log);
            }
            continue;
        }
        if (serial != done) retries = 0;
        done = serial;
        uint16_t code = runSession(L, serial);
        mutexLock(&g_lock);
        bool still = L.req.want && L.serial == serial;
        mutexUnlock(&g_lock);
        if (!still) continue;
        // 4014 (kicked, or the main session's gone), 4021 (rate limited), 4022 (call over): not again
        if (code == 4014 || code == 4021 || code == 4022 || code == 4004) {
            L.state = Voice_Failed;
            retries = TRIES;
            continue;
        }
        retries++;
        L.state = retries >= TRIES ? Voice_Failed : Voice_Connecting;
        int wait_s = retries < 3 ? retries : 3;  // (1, 2, 3, 3 s: a lookup that failed may work in a moment)
        if (retries < TRIES) say("voice: trying again in %d s", wait_s);
        svcSleepThread(1'000'000'000LL * wait_s);
    }
}

}  // namespace

void start(LogFn log, RandomFn rnd) {
    if (g_run) return;
    g_log = log;
    g_rnd = rnd;
    mutexInit(&g_lock);
    mutexInit(&g_mls);
    g_voice.s = &g_s, g_voice.bin = g_bin;
    g_stream.s = &g_ss, g_stream.bin = g_sbin;
    g_run = true;
    // (all on core 3, lower numbers first: the system's sockets (bsd: 25 to 29), Chirp's mixer (30), the mic
    // (36), the call (37), the screen's capture (39), then the stream (40): the call's sound before the
    // video, and capture's quick reads never held up behind a keyframe's sending. Both above the gateway's
    // (44): their loops always wait, at least a millisecond, so the gateway runs)
    if (R_FAILED(threadCreate(&g_voice.thread, thread, &g_voice, nullptr, 0xA000, 0x25, -2))) {
        g_run = false;
        return;
    }
    if (R_FAILED(threadCreate(&g_stream.thread, thread, &g_stream, nullptr, 0xA000, 0x28, -2))) {
        g_run = false;
        threadClose(&g_voice.thread);
        g_voice.thread.handle = 0;
        return;
    }
    threadStart(&g_voice.thread);
    cpushare::track(g_voice.thread.handle);
    threadStart(&g_stream.thread);
    cpushare::track(g_stream.thread.handle);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    threadWaitForExit(&g_voice.thread);
    threadWaitForExit(&g_stream.thread);
    threadClose(&g_voice.thread);
    threadClose(&g_stream.thread);
}

static void connectLink(Link& L, uint64_t guild, uint64_t channel, uint64_t me, const char* session, const char* token, const char* endpoint) {
    mutexLock(&g_lock);
    Request& r = L.req;
    bool same = r.want && r.guild == guild && r.channel == channel && r.me == me && !strcmp(r.session, session) &&
                !strcmp(r.token, token) && !strcmp(r.endpoint, endpoint);
    if (!same) {
        r.want = true;
        r.guild = guild, r.channel = channel, r.me = me;
        snprintf(r.session, sizeof(r.session), "%s", session);
        snprintf(r.token, sizeof(r.token), "%s", token);
        snprintf(r.endpoint, sizeof(r.endpoint), "%s", endpoint);
        L.serial++;
    }
    mutexUnlock(&g_lock);
}

static void disconnectLink(Link& L) {
    mutexLock(&g_lock);
    if (L.req.want) {
        L.req.want = false;
        wipe(L.req.token, sizeof(L.req.token));
        L.serial++;
    }
    mutexUnlock(&g_lock);
}

void connect(uint64_t guild, uint64_t channel, uint64_t me, const char* session, const char* token, const char* endpoint) {
    connectLink(g_voice, guild, channel, me, session, token, endpoint);
}

void disconnect() { disconnectLink(g_voice); }

void retry() {
    mutexLock(&g_lock);
    if (g_voice.req.want && g_voice.state == Voice_Failed) {
        g_voice.serial++;
        g_voice.state = Voice_Connecting;
    }
    mutexUnlock(&g_lock);
}

State state() { return g_voice.state; }

void streamConnect(uint64_t server, uint64_t channel, uint64_t me, const char* session, const char* token, const char* endpoint) {
    connectLink(g_stream, server, channel, me, session, token, endpoint);
}

void streamDisconnect() { disconnectLink(g_stream); }

State streamState() { return g_stream.state; }

void info(char voice_server[64], uint16_t* voice_ping, char stream_server[64], uint16_t* stream_ping) {
    mutexLock(&g_lock);
    snprintf(voice_server, 64, "%s", g_voice.req.want ? g_voice.req.endpoint : "");
    snprintf(stream_server, 64, "%s", g_stream.req.want ? g_stream.req.endpoint : "");
    mutexUnlock(&g_lock);
    *voice_ping = g_voice.state == Voice_Connected ? g_voice.ping : 0;
    *stream_ping = g_stream.state == Voice_Connected ? g_stream.ping : 0;
}

void streamTotals(uint32_t* frames, uint32_t* kb, uint32_t* late, uint32_t* lost, uint32_t* frame_max_kb, int* range) {
    bool on = g_stream.state == Voice_Connected;
    *range = on ? g_ss.video.colorRange() : -1;
    *frames = on ? g_ss.frames_total : 0;
    *kb = on ? g_ss.v_octets_total / 1024 : 0;
    *late = on ? g_ss.late_total : 0;
    *lost = on ? g_ss.lost_total : 0;
    *frame_max_kb = on ? g_ss.frame_max_all / 1024 : 0;
}

}  // namespace voice
