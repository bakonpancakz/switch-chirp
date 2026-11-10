#include "voice.h"
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

class Session : public WsHandler, public JsonHandler, public DaveOut {
public:
    TlsStream tls;
    WebSocket ws;
    Json reader;
    DaveSession dave;
    Request req;
    Link* link = nullptr;
    bool stream = false;  // (Go Live's connection: see Link)
    uint32_t asent = 0, aunsent = 0;  // (the game's sound)
    uint64_t last_frame_ms = 0, next_grey_ms = 0;  // (the grey placeholder: see sendVideo)
    bool need_idr = false;
    int grey_id = 0;
    uint32_t vgrey = 0, vwaited = 0;
    // (the recorder's blank picture: out of the game (the HOME menu), grc goes on sending a still grey one,
    // the same keyframe again and again, byte for byte. Three alike: the loading screen instead)
    size_t last_key_n = 0;
    int same_keys = 0;
    bool blank = false;
    void placeholder(uint64_t now, bool flush);
    char loading[160] = {0};  // (this time's loading screen: chosen when the game's picture goes)
    bool sendLoading(uint64_t tick, bool flush);
    void pickLoading();

    // where the connection is
    bool identified = false, described = false, acked = true;
    uint32_t interval = 0;
    uint64_t next_beat = 0, next_keepalive = 0, next_stats = 0, next_limits = 0;
    int64_t seq = -1;
    uint64_t beat_sent = 0;  // (when the last heartbeat went: its ack gives the ping)
    uint64_t cpu_ticks = 0, cpu_at = 0;  // (this thread's CPU time, and when it was last read: the stats)
    int udp = -1;
    uint32_t ssrc = 0;
    uint32_t vssrc = 0, vrtx = 0;  // (Ready's video stream)
    uint16_t vseq = 0;
    uint32_t vts = 0;
    uint64_t vstart = 0;
    bool video_on = false;  // (announced as active: op 12)
    uint32_t vsent = 0, vframes = 0, vdropped = 0, vbytes = 0, vkeys = 0;
    uint32_t nacks = 0, plis = 0;  // (viewers' RTCP: lost packets to send again, keyframes wanted)
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
    void log(const char* line) override { say("%s", line); }

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
        mutexLock(&g_mls);
        handle();
        mutexUnlock(&g_mls);
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
    bool sendVideo(uint64_t now);  // (true: a frame went, another may be waiting)
    void sendFrame(uint8_t* frame, size_t n, size_t cap, uint64_t tick);
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
    if (!acked) say("voice: the last heartbeat wasn't answered");
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
            size_t m = rtpSeal(mode, key, &out_counter, out_seq, out_ts + ap.frame * 960, ssrc, frame, n, pkt);
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
            if (ok) asent++;
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
// frame that can't get through is given up on)
void Session::sendFrame(uint8_t* frame, size_t n, size_t cap, uint64_t tick) {
    size_t out_n = 0;
    if (!dave.encryptVideo(frame, n, cap, &out_n)) {
        vdropped++;
        return;
    }
    uint32_t ts = vts + (uint32_t)(armTicksToNs(tick - vstart) * 9 / 100000);  // (90 kHz)
    static uint8_t scratch[1100], pkt[1100 + RTP_OVERHEAD];
    bool gave_up = false;
    rtpH264Payloads(frame, out_n, sizeof(scratch), scratch, [&](const uint8_t* p, size_t k, bool last) {
        if (gave_up) return;
        sendGameAudio();  // (the sound between video packets: a keyframe takes a while to get out)
        size_t m = rtpSealAs(mode, key, &out_counter, RTP_H264, last, vseq++, ts, vssrc, p, k, pkt);
        // (at a steady 24 Mbps at most, not as fast as the socket takes them: a keyframe in one burst kept
        // the Wi-Fi busy, and the call's voices came in late and in clumps meanwhile. 16 was too few: grc's
        // 10 Mbps peaks at 12-15 in busy scenes, keyframes on top, and the queue filled)
        constexpr uint64_t PACE_BYTES_PER_S = 3'000'000;
        static uint64_t pace_ns = 0;
        uint64_t now_ns = armTicksToNs(armGetSystemTick());
        if (pace_ns < now_ns) pace_ns = now_ns;
        pace_ns += (uint64_t)m * 1'000'000'000ULL / PACE_BYTES_PER_S;
        if (pace_ns > now_ns + 2'000'000) svcSleepThread((s64)(pace_ns - now_ns));
        for (int tries = 0;; tries++) {
            if (::send(udp, pkt, m, 0) == (ssize_t)m) break;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                gave_up = true;
                return;
            }
            pollfd w = {udp, POLLOUT, 0};
            if (tries >= 10 || poll(&w, 1, 5) <= 0) {
                gave_up = true;
                return;
            }
        }
        vsent++;
        vbytes += (uint32_t)m;
    });
    // (a frame only partly sent breaks the ones after it as a dropped one does: they wait for a keyframe)
    if (gave_up) vdropped++, need_idr = true;
    else vframes++;
}

// One of the loading screens (CHIRP_LOADING), at random, for this time the game's picture is gone
void Session::pickLoading() {
    loading[0] = 0;
    DIR* dir = opendir(CHIRP_LOADING);
    if (!dir) {
        say("screen: nothing to show, and no %s folder: grey", CHIRP_LOADING);
        return;
    }
    auto isFrame = [](const char* name) {
        size_t n = strlen(name);
        return n > 5 && !strcmp(name + n - 5, ".h264");
    };
    // (one pass, each file taking the pick with a chance of 1 in how many so far: all alike in the end.
    // The SD card's directories can't be rewound (libnx's fsdev), so counting first, then reading again
    // for the one picked, found nothing the second time: no loading screen, and nothing in the log)
    uint32_t count = 0;
    while (dirent* e = readdir(dir)) {
        if (!isFrame(e->d_name)) continue;
        count++;
        uint32_t r = 0;
        g_rnd(&r, sizeof(r));
        if (r % count == 0) snprintf(loading, sizeof(loading), "%s/%s", CHIRP_LOADING, e->d_name);
    }
    closedir(dir);
    if (!count) say("screen: nothing to show, and no loading screens in %s: grey", CHIRP_LOADING);
    if (loading[0]) say("screen: nothing to show: loading screen %s", loading + strlen(CHIRP_LOADING) + 1);
}

// The chosen loading screen, read into the (empty) send queue and sent from there; false: none, or it
// couldn't be (too big, unreadable, frames came meanwhile)
bool Session::sendLoading(uint64_t tick, bool flush) {
    if (!loading[0]) return false;
    size_t cap = 0;
    uint8_t* buf = screen::borrowRing(&cap, flush);
    if (!buf) return false;  // (frames came meanwhile: the game's back)
    const char* why = nullptr;
    constexpr size_t MAX = 200 * 1024;
    if (FILE* f = fopen(loading, "rb")) {
        size_t n = fread(buf, 1, MAX + 1, f);
        fclose(f);
        // (its SPS made as WebRTC wants it, as the recorder's are; its own ids (1), kept apart from those)
        static H264Params own;
        bool idr = false;
        if (n <= 4) why = "empty";
        else if (n > MAX) why = "over 200 KB";
        else if ((n = rtpH264WithParams(buf, n, cap - 1024, &own, &idr)), !idr) why = "not a keyframe";
        else sendFrame(buf, n, cap, tick);
    } else {
        why = "couldn't be opened";
    }
    screen::returnRing();
    if (why) {
        say("screen: loading screen %s %s: grey instead", loading + strlen(CHIRP_LOADING) + 1, why);
        loading[0] = 0;  // (grey from now on, this time)
    }
    return !why;
}

// Nothing of the game's to show: a loading screen (or grey), again every 2 s for viewers who come later.
// flush: the recorder's frames queued meanwhile are thrown away (its blank picture), so the queue's free
void Session::placeholder(uint64_t now, bool flush) {
    static H264Params own;  // (grey's: grc's SPS, as the frames it stands in for)
    static uint8_t grey[H264_PLACEHOLDER_MAX + 1024];  // (room for DAVE's trailer)
    if (now < next_grey_ms) return;
    own.full_range = screen::fullRange() ? 1 : 0;
    if (!next_grey_ms) pickLoading();  // (a new time without the game's picture)
    size_t g = 0;
    if (sendLoading(armGetSystemTick(), flush)) {
        vgrey++;
        need_idr = true;
    } else if ((g = h264Placeholder(&own, grey_id++, grey, H264_PLACEHOLDER_MAX)) != 0) {
        sendFrame(grey, g, sizeof(grey), armGetSystemTick());
        vgrey++;
        need_idr = true;
    }
    next_grey_ms = now + 2000;
}

// The recorder's next frame; with none for a while (no game, the HOME menu), a grey keyframe, again every
// 2 s for viewers who come later. After a grey one the recorder's frames wait for its next keyframe:
// the ones between refer to pictures the viewers no longer have
bool Session::sendVideo(uint64_t now) {
    static H264Params params;  // (keyframes carry the SPS and PPS: see rtp.h)
    uint8_t* frame;
    size_t n, cap;
    uint64_t tick;
    bool gap = false;
    if (!screen::take(&frame, &n, &cap, &tick, &gap)) {
        if (now - last_frame_ms >= 700) placeholder(now, false);
        return false;
    }
    last_frame_ms = now;
    bool idr = false;
    params.full_range = screen::fullRange() ? 1 : 0;
    n = rtpH264WithParams(frame, n, cap - 1024, &params, &idr);
    // (the recorder's blank picture: the same keyframe again and again, back to back, nothing between. A
    // game whose picture only holds still (a pause menu, a dialogue box) sends keyframes alike too, but
    // with ordinary frames between them: counted alike only one after another, it once took those for
    // blank and put the loading screen up after three alike (6-9 s with a 180-frame GOP). Out of it at the
    // first keyframe that differs, or the first ordinary frame: the game's running)
    if (idr) {
        same_keys = n == last_key_n ? same_keys + 1 : 0;
        last_key_n = n;
    } else {
        same_keys = 0, last_key_n = 0;
    }
    if (!blank && same_keys >= 2) {
        blank = true;
        say("screen: the recorder's sending a still, blank picture (out of the game?): the loading screen instead (%u-byte keyframes)",
            (unsigned)n);
    } else if (blank && same_keys == 0) {
        blank = false;
        need_idr = true;  // (an ordinary frame first: it refers to pictures the viewers never got)
        say("screen: the game's picture is back");
    }
    if (blank) {
        screen::done();
        placeholder(now, true);
        return true;
    }
    next_grey_ms = 0;
    if (gap) need_idr = true;  // (frames were dropped before this one: wait for a picture that stands alone)
    if (need_idr && !idr) {
        vwaited++;
        screen::done();
        return true;
    }
    need_idr = false;
    if (idr) vkeys++;
    sendFrame(frame, n, cap, tick);
    screen::done();
    return true;
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
            if (in[1] == 205) nacks++;       // (transport feedback: packets that didn't arrive)
            else if (in[1] == 206) plis++;   // (payload feedback: a keyframe wanted)
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
        size_t fn = 0;
        mutexLock(&g_mls);
        bool ok = dave.decrypt(user, p.payload, p.payload_n, frame, &fn, now);
        mutexUnlock(&g_mls);
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
    s.last_frame_ms = nowMs();
    s.next_grey_ms = 0;
    s.need_idr = false;
    s.vgrey = s.vwaited = 0;
    s.last_key_n = 0, s.same_keys = 0, s.blank = false;
    s.identified = s.described = false;
    s.acked = true;
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
    s.vsent = s.vframes = s.vdropped = s.vbytes = s.vkeys = s.nacks = s.plis = 0;
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
        poll(p, s.udp >= 0 ? 2 : 1, s.tls.pending() ? 1 : (mic::active() || screen::active()) ? 5 : WAIT_MS);
        now = nowMs();
        alive = s.ws.poll(0, &s);
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
                for (int k = 0; on && k < 4 && s.sendVideo(now); k++) s.sendGameAudio();
                s.sendGameAudio();
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
            uint64_t cpu = 0, at = armGetSystemTick();
            svcGetInfo(&cpu, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total);
            if (s.cpu_at && at > s.cpu_at) say("stream: %u%% of a core", (unsigned)((cpu - s.cpu_ticks) * 100 / (at - s.cpu_at)));
            s.cpu_ticks = cpu, s.cpu_at = at;
            screen::Stats sc;
            screen::takeStats(&sc);
            say("screen: the recorder's frames %u skipped (we were too slow), %u dropped with no room to queue (the network's "
                "slower than the video), capture waited %u ms for room; "
                "their timestamps %llu..%llu apart", sc.skipped, sc.no_room, sc.waited_ms, (unsigned long long)sc.gap_min,
                (unsigned long long)sc.gap_max);
            // (in three lines: one, longer than a log line's 256 bytes, lost its end)
            say("screen: %s, %u frames captured, %u sent (%u keyframes) in %u packets (%u KB), %u not sent; %u loading screens "
                "or grey; %u skipped waiting for a keyframe", sc.no_game ? "waiting for a game" : sc.open ? "capturing" : "not capturing", sc.captured, s.vframes,
                s.vkeys, s.vsent, s.vbytes / 1024, s.vdropped, s.vgrey, s.vwaited);
            say("screen: grc:d %u failures (0x%X); its longest wait for a frame %u ms (waiting now: %u ms), for sound %u ms (now: "
                "%u ms); %u sound chunks read", sc.failed, sc.last_rc, sc.video_longest_ms, sc.video_waiting_ms, sc.audio_longest_ms,
                sc.audio_waiting_ms, sc.audio_chunks);
            say("screen: viewers asked %u times for lost packets, %u for keyframes; game sound: %u packets sent (%u not), %u dropped "
                "waiting, %u gaps", s.nacks, s.plis, s.asent, s.aunsent, sc.audio_dropped, sc.audio_holes);
            s.vgrey = s.vwaited = 0;
            s.vsent = s.vframes = s.vdropped = s.vbytes = s.vkeys = s.nacks = s.plis = 0;
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
    // (just above the gateway's: its loop always waits, at least a millisecond, so the gateway runs.
    // The stream's just below the call's: the call's sound first)
    if (R_FAILED(threadCreate(&g_voice.thread, thread, &g_voice, nullptr, 0xA000, 0x28, -2))) {
        g_run = false;
        return;
    }
    if (R_FAILED(threadCreate(&g_stream.thread, thread, &g_stream, nullptr, 0xA000, 0x29, -2))) {
        g_run = false;
        threadClose(&g_voice.thread);
        g_voice.thread.handle = 0;
        return;
    }
    threadStart(&g_voice.thread);
    threadStart(&g_stream.thread);
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

}  // namespace voice
