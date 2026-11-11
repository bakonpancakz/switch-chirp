// Chirp's PC tests (test/run.sh): the parts that don't need a Switch.
// A pretend Discord on the other end of an in-memory Stream hands out its
// bytes in pieces of random sizes, the way TLS reads would.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gateway.h"
#include "json.h"
#include "mic.h"
#include "mixer.h"
#include "store.h"
#include "util.h"
#include "ws.h"
#include "framequeue.h"
#include "screenclock.h"
#include "share.h"
#include "sounds.h"
#include "aes.h"
#include "rtp.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (c) g_pass++;                                               \
        else {                                                         \
            g_fail++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
        }                                                              \
    } while (0)

static void rnd(void* out, size_t n) {
    for (size_t i = 0; i < n; i++) ((unsigned char*)out)[i] = (unsigned char)rand();
}

static std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) snprintf(b, sizeof(b), "%02x", p[i]), s += b;
    return s;
}

// --- util ---

static void testUtil() {
    uint8_t d[20];
    sha1("abc", 3, d);
    CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    std::string a(1000, 'a');
    sha1(a.data(), a.size(), d);
    CHECK(hex(d, 20) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    sha1("", 0, d);
    CHECK(hex(d, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    char b[64];
    base64((const uint8_t*)"foobar", 6, b);
    CHECK(strcmp(b, "Zm9vYmFy") == 0);
    base64((const uint8_t*)"fooba", 5, b);
    CHECK(strcmp(b, "Zm9vYmE=") == 0);
    base64((const uint8_t*)"foob", 4, b);
    CHECK(strcmp(b, "Zm9vYg==") == 0);
    // RFC 6455's own example of the handshake's key and answer
    const char* key = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1(key, strlen(key), d);
    base64(d, 20, b);
    CHECK(strcmp(b, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    CHECK(parseId("175928847299117063", 18) == 175928847299117063ULL);
    CHECK(parseId("18446744073709551616", 20) == 0);  // (too big)
    CHECK(parseId("12a", 3) == 0);
    char id[21];
    idString(175928847299117063ULL, id);
    CHECK(strcmp(id, "175928847299117063") == 0);

    char out[8];
    utf8Copy(out, sizeof(out), "abc", 3);
    CHECK(strcmp(out, "abc") == 0);
    utf8Copy(out, sizeof(out), "abcdefgh", 8);
    CHECK(strcmp(out, "abcd\xE2\x80\xA6") == 0);
    utf8Copy(out, sizeof(out), "abc\xC3\xA9xyz", 8);  // (é is two bytes: not cut in half)
    CHECK(strcmp(out, "abc\xE2\x80\xA6") == 0);
    utf8Copy(out, sizeof(out), "ab\xC3\xA9xyzw", 8);  // (a whole é fits before the "…")
    CHECK(strcmp(out, "ab\xC3\xA9\xE2\x80\xA6") == 0);

    char esc[32];
    CHECK(jsonEscape(esc, sizeof(esc), "a\"b\\c\n"));
    CHECK(strcmp(esc, "a\\\"b\\\\c\\u000a") == 0);
    CHECK(!jsonEscape(esc, 4, "abcd"));
}

// --- json ---

struct Recorder : JsonHandler {
    std::string log;
    void begin(const char* path, bool array) override { log += std::string(array ? "[" : "{") + path + "\n"; }
    void value(const char* path, JsonType type, const char* s, size_t n, bool cut) override {
        log += std::string(path) + "=" + std::to_string((int)type) + ":" + std::string(s, n) + (cut ? "+" : "") + "\n";
    }
    void end(const char* path, bool array) override { log += std::string(array ? "]" : "}") + path + "\n"; }
};

static std::string parse(const std::string& text, size_t piece, bool* ok) {
    Recorder r;
    Json j;
    j.reset(&r);
    *ok = true;
    for (size_t i = 0; i < text.size() && *ok; i += piece) *ok = j.feed(text.data() + i, std::min(piece, text.size() - i));
    *ok = *ok && j.done();
    return r.log;
}

static void testJson() {
    const std::string doc =
        "{\"t\":\"X\",\"s\":12,\"op\":0,\"d\":{\"a\":[1,-2.5e3,true,false,null,{\"b\":\"\\u00e9\\ud83d\\ude00\\n\"}],"
        "\"e\":{},\"f\":[]}}";
    bool ok;
    std::string whole = parse(doc, doc.size(), &ok);
    CHECK(ok);
    CHECK(whole.find("t=0:X\n") != std::string::npos);
    CHECK(whole.find("s=1:12\n") != std::string::npos);
    CHECK(whole.find("d.a[]=1:-2.5e3\n") != std::string::npos);
    CHECK(whole.find("d.a[]=2:true\n") != std::string::npos);
    CHECK(whole.find("d.a[]=4:null\n") != std::string::npos);
    CHECK(whole.find("d.a[].b=0:\xC3\xA9\xF0\x9F\x98\x80\n\n") != std::string::npos);  // é, 😀 and a newline
    CHECK(whole.find("}d.e\n") != std::string::npos);
    CHECK(whole.find("]d.f\n") != std::string::npos);
    // a byte at a time, and in odd pieces: the same events
    for (size_t piece : {1, 2, 3, 7, 13}) {
        std::string p = parse(doc, piece, &ok);
        CHECK(ok);
        CHECK(p == whole);
    }
    // not JSON
    parse("{\"a\":1,}", 1, &ok);
    CHECK(!ok);
    parse("{\"a\" 1}", 1, &ok);
    CHECK(!ok);
    parse("[1 2]", 1, &ok);
    CHECK(!ok);
    parse("{\"a\":tru}", 1, &ok);
    CHECK(!ok);
    // a long string is cut, and says so
    std::string longs = "{\"s\":\"" + std::string(300, 'x') + "\"}";
    std::string cut = parse(longs, 5, &ok);
    CHECK(ok);
    CHECK(cut.find("s=0:" + std::string(JSON_STRING_MAX, 'x') + "+\n") != std::string::npos);
    // too deep
    parse(std::string(40, '[') + std::string(40, ']'), 1, &ok);
    CHECK(!ok);
}

// --- the pretend Discord ---

struct FakeServer : Stream {
    std::string to_client;    // what it has said, not read yet
    std::string from_client;  // what the client sent, raw
    bool closed = false;
    int read(void* buf, int cap, int) override {
        if (to_client.empty()) return closed ? -1 : 0;
        int n = 1 + rand() % 700;
        if (n > cap) n = cap;
        if (n > (int)to_client.size()) n = (int)to_client.size();
        memcpy(buf, to_client.data(), n);
        to_client.erase(0, n);
        return n;
    }
    bool write(const void* data, int n) override {
        from_client.append((const char*)data, n);
        return true;
    }
    void frame(uint8_t opcode, const std::string& payload, bool fin = true) {
        std::string f;
        f += (char)((fin ? 0x80 : 0) | opcode);
        size_t n = payload.size();
        if (n < 126) {
            f += (char)n;
        } else if (n < 65536) {
            f += (char)126, f += (char)(n >> 8), f += (char)n;
        } else {
            f += (char)127;
            for (int i = 7; i >= 0; i--) f += (char)(n >> (i * 8));
        }
        to_client += f + payload;
    }
    // A message split over frames (continuations), to check they're joined
    void message(const std::string& json) {
        if (json.size() > 40) {
            size_t a = json.size() / 3;
            frame(0x1, json.substr(0, a), false);
            frame(0x0, json.substr(a, a), false);
            frame(0x0, json.substr(2 * a), true);
        } else {
            frame(0x1, json);
        }
    }
    // The client's frames since the handshake, unmasked
    std::vector<std::pair<int, std::string>> sent() {
        std::vector<std::pair<int, std::string>> out;
        size_t at = from_client.find("\r\n\r\n");
        at = at == std::string::npos ? 0 : at + 4;
        const std::string& s = from_client;
        while (at + 6 <= s.size()) {
            int op = s[at] & 0x0F;
            size_t n = s[at + 1] & 0x7F, h = 2;
            if (n == 126) n = (size_t)((unsigned char)s[at + 2] << 8 | (unsigned char)s[at + 3]), h = 4;
            const unsigned char* mask = (const unsigned char*)s.data() + at + h;
            std::string p;
            for (size_t i = 0; i < n; i++) p += (char)(s[at + h + 4 + i] ^ mask[i & 3]);
            out.push_back({op, p});
            at += h + 4 + n;
        }
        return out;
    }
};

static std::string acceptFor(const std::string& request) {
    size_t k = request.find("Sec-WebSocket-Key: ");
    std::string key = request.substr(k + 19, request.find("\r\n", k) - k - 19) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t d[20];
    sha1(key.data(), key.size(), d);
    char b[32];
    base64(d, 20, b);
    return b;
}

static std::vector<std::string> g_log;
static void logLine(const char* line) { g_log.push_back(line); }

static void pump(WebSocket& ws, Gateway& gw, FakeServer& srv, uint64_t now) {
    // (at least once: what came with the handshake is already in the websocket)
    int i = 0;
    do {
        if (!ws.poll(0, &gw)) break;
    } while (++i < 400 && !srv.to_client.empty());
    gw.tick(now);
}

static void testGateway() {
    FakeServer srv;
    WebSocket ws;
    ws.reset(&srv, rnd);
    // the handshake: a wrong answer is turned down
    srv.to_client = "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: nope\r\n\r\n";
    CHECK(!ws.handshake("gateway.discord.gg", "/?v=10&encoding=json", 1000));
    srv = FakeServer();
    ws.reset(&srv, rnd);
    // (the server answers once it has seen the request: write it first, then hand out the answer)
    struct Answering : Stream {
        FakeServer* s;
        bool answered = false;
        int read(void* b, int c, int t) override {
            if (!answered) {
                s->to_client = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "sec-websocket-accept: " + acceptFor(s->from_client) + "\r\n\r\n";
                answered = true;
                // Hello, in the same read as the header's end
                s->frame(0x1, "{\"t\":null,\"s\":null,\"op\":10,\"d\":{\"heartbeat_interval\":41250,\"_trace\":[\"x\"]}}");
            }
            return s->read(b, c, t);
        }
        bool write(const void* d, int n) override { return s->write(d, n); }
    } answering;
    answering.s = &srv;
    ws.reset(&answering, rnd);
    CHECK(ws.handshake("gateway.discord.gg", "/?v=10&encoding=json", 1000));
    CHECK(srv.from_client.find("GET /?v=10&encoding=json HTTP/1.1\r\n") == 0);
    CHECK(srv.from_client.find("Host: gateway.discord.gg\r\n") != std::string::npos);

    Gateway gw;
    uint64_t now = 1000;
    gw.start(&ws, "MTIz.abc.def", rnd, logLine, now);
    pump(ws, gw, srv, now);
    auto sent = srv.sent();
    CHECK(sent.size() == 1);
    CHECK(sent.size() >= 1 && sent[0].first == 1);
    CHECK(sent.size() >= 1 && sent[0].second.find("\"op\":2") != std::string::npos);
    CHECK(sent.size() >= 1 && sent[0].second.find("\"token\":\"MTIz.abc.def\"") != std::string::npos);
    CHECK(sent.size() >= 1 && sent[0].second.find("\"intents\":129") != std::string::npos);

    // a ping is answered with a pong
    srv.frame(0x9, "hi");
    // READY (a bot's: guild ids only) and a GUILD_CREATE, with members, as Discord sends them
    srv.message(
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"v\":10,\"user\":{\"id\":\"900\",\"username\":\"chirpbot\",\"global_name\":null,"
        "\"bot\":true},\"guilds\":[{\"id\":\"10\",\"unavailable\":true},{\"id\":\"20\",\"unavailable\":true}],"
        "\"session_id\":\"abc\"}}");
    srv.message(
        "{\"t\":\"GUILD_CREATE\",\"s\":2,\"op\":0,\"d\":{\"voice_states\":[{\"user_id\":\"501\",\"channel_id\":\"102\","
        "\"self_mute\":true,\"self_deaf\":false,\"mute\":false,\"deaf\":false,\"session_id\":\"q\"},"
        "{\"user_id\":\"502\",\"channel_id\":\"102\",\"self_mute\":false,\"self_deaf\":false,\"mute\":false,\"deaf\":false}],"
        "\"channels\":[{\"id\":\"100\",\"type\":4,\"name\":\"Voice Channels\",\"position\":1},"
        "{\"id\":\"101\",\"type\":0,\"name\":\"general\",\"position\":0},"
        "{\"id\":\"103\",\"type\":2,\"name\":\"Gaming\",\"position\":2,\"parent_id\":\"100\"},"
        "{\"id\":\"102\",\"type\":2,\"name\":\"Lounge \\ud83c\\udfae\",\"position\":1,\"parent_id\":\"100\"},"
        "{\"id\":\"104\",\"type\":2,\"name\":\"AFK\",\"position\":0,\"parent_id\":null}],"
        "\"members\":[{\"user\":{\"id\":\"501\",\"username\":\"kira\",\"global_name\":\"Kira\",\"avatar\":\"a_0123456789abcdef0123456789abcdef\"},"
        "\"nick\":\"kira-chan\",\"avatar\":null},"
        "{\"user\":{\"id\":\"900\",\"username\":\"chirpbot\",\"global_name\":null},\"nick\":null}],"
        "\"name\":\"Cozy Server\",\"id\":\"10\",\"unavailable\":false}}");
    srv.message("{\"t\":\"GUILD_CREATE\",\"s\":3,\"op\":0,\"d\":{\"id\":\"20\",\"name\":\"Second\",\"channels\":[],\"voice_states\":[]}}");
    now += 10;
    pump(ws, gw, srv, now);
    CHECK(gw.ready());
    CHECK(gw.me() == 900);
    CHECK(strcmp(gw.myName(), "chirpbot") == 0);
    sent = srv.sent();
    CHECK(sent.size() == 2 && sent[1].first == 0xA && sent[1].second == "hi");

    CHECK(store::guildCount() == 2);
    ChirpGuild guilds[4];
    CHECK(store::listGuilds(0, guilds, 4) == 2);
    CHECK(guilds[0].id == 10 && strcmp(guilds[0].name, "Cozy Server") == 0);
    CHECK(guilds[0].voice_channels == 3 && guilds[0].in_voice == 2);
    ChirpChannel ch[8];
    size_t nch = store::listChannels(10, ch, 8);
    // AFK (no category) first, then the category and its channels by position
    CHECK(nch == 4);
    CHECK(nch == 4 && ch[0].id == 104 && ch[1].id == 100 && ch[1].type == ChirpChannel_Category && ch[2].id == 102 && ch[3].id == 103);
    CHECK(nch == 4 && ch[2].members == 2 && ch[3].members == 0);
    CHECK(nch == 4 && strcmp(ch[2].name, "Lounge \xF0\x9F\x8E\xAE") == 0);
    ChirpMember m[8];
    size_t total = 0;
    CHECK(store::listMembers(102, 900, m, 8, &total) == 2 && total == 2);
    CHECK(strcmp(m[0].name, "kira-chan") == 0 && (m[0].flags & ChirpMember_SelfMute));
    CHECK(strcmp(m[1].name, "Unknown (502)") == 0 && (m[1].flags & ChirpMember_NoName));
    CHECK(strcmp(m[0].avatar, "a_0123456789abcdef0123456789abcdef") == 0 && m[1].avatar[0] == 0);

    // someone joins (their voice state brings their name), someone moves, a channel is renamed
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":4,\"op\":0,\"d\":{\"member\":{\"user\":{\"username\":\"momo\",\"id\":\"503\","
        "\"global_name\":\"Momo\",\"avatar\":\"ffffffffffffffffffffffffffffffff\"},\"nick\":null,\"avatar\":\"0123456789abcdef0123456789abcdef\"},\"user_id\":\"503\",\"suppress\":false,\"session_id\":\"z\",\"self_video\":false,"
        "\"self_mute\":false,\"self_deaf\":true,\"request_to_speak_timestamp\":null,\"mute\":false,\"guild_id\":\"10\","
        "\"deaf\":false,\"channel_id\":\"103\"}}");
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"member\":{\"user\":{\"username\":\"bob\",\"id\":\"502\","
        "\"global_name\":null,\"avatar\":\"../../../x\"},\"nick\":null},\"user_id\":\"502\",\"guild_id\":\"10\",\"channel_id\":null}}");
    srv.message("{\"t\":\"CHANNEL_UPDATE\",\"s\":6,\"op\":0,\"d\":{\"id\":\"103\",\"guild_id\":\"10\",\"type\":2,\"name\":\"Games\",\"position\":2,\"parent_id\":\"100\"}}");
    // an event whose name comes after its data can't be read as it streams: skipped
    srv.message("{\"op\":0,\"d\":{\"id\":\"10\",\"name\":\"Renamed\"},\"s\":7,\"t\":\"GUILD_UPDATE\"}");
    srv.message("{\"t\":\"PRESENCE_UPDATE\",\"s\":8,\"op\":0,\"d\":{\"user\":{\"id\":\"1\"}}}");
    now += 10;
    pump(ws, gw, srv, now);
    CHECK(store::listMembers(103, 900, m, 8, &total) == 1 && strcmp(m[0].name, "Momo") == 0 && (m[0].flags & ChirpMember_SelfDeaf));
    // (the server's own picture of them wins; it's a file name, so "g" in front)
    CHECK(strcmp(m[0].avatar, "g0123456789abcdef0123456789abcdef") == 0);
    // an avatar that isn't a hash never becomes a file name
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":6,\"op\":0,\"d\":{\"member\":{\"user\":{\"username\":\"eve\",\"id\":\"504\","
        "\"avatar\":\"../../../../atmosphere/x\"},\"nick\":null},\"user_id\":\"504\",\"guild_id\":\"10\",\"channel_id\":\"104\"}}");
    pump(ws, gw, srv, now);
    CHECK(store::listMembers(104, 900, m, 8, &total) == 1 && strcmp(m[0].name, "eve") == 0 && m[0].avatar[0] == 0);
    CHECK(store::membersIn(102) == 1);
    CHECK(strcmp(store::channelFind(103)->name, "Games") == 0);
    CHECK(store::listGuilds(0, guilds, 4) == 2 && strcmp(guilds[0].name, "Cozy Server") == 0);
    bool skippedLogged = false;
    for (auto& l : g_log) skippedLogged = skippedLogged || l.find("skipped") != std::string::npos;
    CHECK(skippedLogged);

    // joining: op 4 goes out; the bot's own voice state puts it in the call
    CHECK(gw.voiceState(10, 102, true, false));
    sent = srv.sent();
    CHECK(sent.back().second == "{\"op\":4,\"d\":{\"guild_id\":\"10\",\"channel_id\":\"102\",\"self_mute\":true,\"self_deaf\":false,\"self_video\":false}}");
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":9,\"op\":0,\"d\":{\"member\":{\"user\":{\"username\":\"chirpbot\",\"id\":\"900\"},"
        "\"nick\":null},\"user_id\":\"900\",\"guild_id\":\"10\",\"channel_id\":\"102\",\"self_mute\":true,\"self_deaf\":false}}");
    now += 5000;
    pump(ws, gw, srv, now);
    CHECK(gw.callGuild() == 10 && gw.callChannel() == 102 && gw.selfMute());
    CHECK(store::listMembers(102, gw.me(), m, 8, &total) == 2 && (m[1].flags & ChirpMember_Me));

    // heartbeats: one when due, with the last sequence; no ACK by the next one ends the session
    size_t before = srv.sent().size();
    now += 41250;
    gw.tick(now);
    sent = srv.sent();
    CHECK(sent.size() == before + 1 && sent.back().second == "{\"op\":1,\"d\":9}");
    srv.message("{\"t\":null,\"s\":null,\"op\":11,\"d\":null}");
    pump(ws, gw, srv, now);
    now += 41250;
    CHECK(gw.tick(now));
    now += 41250;
    CHECK(!gw.tick(now));
    CHECK(gw.over());
}

// A binary frame bigger than the send buffer (a DAVE commit with its Welcome can be tens of KB)
static void testBigBinary() {
    FakeServer srv;
    WebSocket ws;
    ws.reset(&srv, rnd);
    std::string body(3000, '\0');
    for (size_t i = 0; i < body.size(); i++) body[i] = (char)(i * 7);
    CHECK(ws.sendBinary(28, body.data(), body.size()));
    auto sent = srv.sent();
    CHECK(sent.size() == 1 && sent[0].first == 2 && sent[0].second.size() == 3001 && (uint8_t)sent[0].second[0] == 28 &&
          sent[0].second.substr(1) == body);
}

static void testClose() {
    FakeServer srv;
    WebSocket ws;
    ws.reset(&srv, rnd);
    Gateway gw;
    gw.start(&ws, "x", rnd, logLine, 0);
    srv.frame(0x8, std::string("\x0f\xa4", 2) + "Authentication failed.");  // 4004
    for (int i = 0; i < 20 && ws.poll(0, &gw); i++) {
    }
    CHECK(ws.closeCode() == 4004);
    CHECK(!ws.poll(0, &gw));
    // (it answered the close)
    auto sent = srv.sent();
    CHECK(!sent.empty() && sent.back().first == 0x8);

    // a frame the server masked is a broken server
    FakeServer srv2;
    WebSocket ws2;
    ws2.reset(&srv2, rnd);
    srv2.to_client = std::string("\x81\x82\x00\x00\x00\x00hi", 8);
    bool alive = true;
    for (int i = 0; i < 20 && alive; i++) alive = ws2.poll(0, nullptr);
    CHECK(!alive && strstr(ws2.error(), "masked"));
}

// A new session while Discord still has the bot in a voice channel (another
// device, or our last session): the bot's voice state comes in the
// GUILD_CREATE, before the guild's id, with another session's id. It's shown,
// but it isn't this session's call until this session joins
static void testInCallAlready() {
    FakeServer srv;
    WebSocket ws;
    ws.reset(&srv, rnd);
    Gateway gw;
    gw.start(&ws, "x", rnd, logLine, 0);
    srv.message("{\"t\":null,\"s\":null,\"op\":10,\"d\":{\"heartbeat_interval\":41250}}");
    srv.message("{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"user\":{\"id\":\"900\",\"username\":\"chirpbot\"},\"session_id\":\"mine\",\"guilds\":[{\"id\":\"10\"}]}}");
    srv.message(
        "{\"t\":\"GUILD_CREATE\",\"s\":2,\"op\":0,\"d\":{\"voice_states\":[{\"user_id\":\"900\",\"channel_id\":\"102\","
        "\"self_mute\":true,\"self_deaf\":false,\"session_id\":\"elsewhere\"}],\"channels\":[{\"id\":\"102\",\"type\":2,\"name\":\"Lounge\",\"position\":0}],"
        "\"name\":\"Cozy Server\",\"id\":\"10\"}}");
    pump(ws, gw, srv, 10);
    CHECK(gw.ready());
    CHECK(gw.callChannel() == 0 && gw.callGuild() == 0);  // (no call controls for someone else's call)
    ChirpMember m[4];
    size_t total = 0;
    CHECK(store::listMembers(102, 900, m, 4, &total) == 1 && (m[0].flags & ChirpMember_Me));  // (but the bot's shown there)
    // joining from here: Discord moves the voice state to this session
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":3,\"op\":0,\"d\":{\"user_id\":\"900\",\"guild_id\":\"10\",\"channel_id\":\"102\","
        "\"self_mute\":true,\"self_deaf\":false,\"session_id\":\"mine\"}}");
    pump(ws, gw, srv, 10);
    CHECK(gw.callChannel() == 102 && gw.callGuild() == 10 && gw.selfMute());
    CHECK(!strcmp(gw.voiceSession(), "mine"));
    // someone else into our channel and out of it: counted (the call's sounds); elsewhere: not
    CHECK(gw.othersJoined() == 0 && gw.othersLeft() == 0);
    srv.message("{\"t\":\"VOICE_STATE_UPDATE\",\"s\":4,\"op\":0,\"d\":{\"user_id\":\"501\",\"guild_id\":\"10\",\"channel_id\":\"102\",\"session_id\":\"x\"}}");
    srv.message("{\"t\":\"VOICE_STATE_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"user_id\":\"501\",\"guild_id\":\"10\",\"channel_id\":\"102\",\"self_mute\":true,\"session_id\":\"x\"}}");
    srv.message("{\"t\":\"VOICE_STATE_UPDATE\",\"s\":6,\"op\":0,\"d\":{\"user_id\":\"502\",\"guild_id\":\"10\",\"channel_id\":\"103\",\"session_id\":\"y\"}}");
    pump(ws, gw, srv, 10);
    CHECK(gw.othersJoined() == 1 && gw.othersLeft() == 0);  // (muting isn't joining again; another channel isn't ours)
    srv.message("{\"t\":\"VOICE_STATE_UPDATE\",\"s\":7,\"op\":0,\"d\":{\"user_id\":\"501\",\"guild_id\":\"10\",\"channel_id\":null,\"session_id\":\"x\"}}");
    pump(ws, gw, srv, 10);
    CHECK(gw.othersJoined() == 1 && gw.othersLeft() == 1);
    // and Leave can say which guild
    CHECK(gw.voiceState(gw.callGuild(), 0, false, false));
    CHECK(srv.sent().back().second == "{\"op\":4,\"d\":{\"guild_id\":\"10\",\"channel_id\":null,\"self_mute\":false,\"self_deaf\":false,\"self_video\":false}}");
    // sharing: the voice state says so (Discord's apps show the video, and ask for it, only then)
    CHECK(gw.voiceState(10, 102, false, false, true));
    CHECK(srv.sent().back().second == "{\"op\":4,\"d\":{\"guild_id\":\"10\",\"channel_id\":\"102\",\"self_mute\":false,\"self_deaf\":false,\"self_video\":true}}");
    // presence: invisible by default, said in Identify too; Rich Presence's game with its start; online when asked
    CHECK(gw.presence("Hatsune Miku", 1700000000000ULL));
    CHECK(srv.sent().back().second == "{\"op\":3,\"d\":{\"since\":null,\"activities\":[{\"name\":\"Hatsune Miku\",\"type\":0,"
                                       "\"timestamps\":{\"start\":1700000000000}}],\"status\":\"invisible\",\"afk\":false}}");
    gw.setInvisible(false);
    CHECK(gw.presence(nullptr, 0));
    CHECK(srv.sent().back().second == "{\"op\":3,\"d\":{\"since\":null,\"activities\":[],\"status\":\"online\",\"afk\":false}}");
    CHECK(srv.sent()[0].second.find("\"presence\":{\"since\":null,\"activities\":[],\"status\":\"invisible\"") != std::string::npos);
    // Go Live: create (op 18, then unpaused, op 22); Discord names the stream's server; someone else's is ignored
    CHECK(gw.streamCreate(10, 102));
    auto sent = srv.sent();
    CHECK(sent[sent.size() - 2].second == "{\"op\":18,\"d\":{\"type\":\"guild\",\"guild_id\":\"10\",\"channel_id\":\"102\",\"preferred_region\":null}}");
    CHECK(sent.back().second == "{\"op\":22,\"d\":{\"stream_key\":\"guild:10:102:900\",\"paused\":false}}");
    srv.message("{\"t\":\"STREAM_CREATE\",\"s\":3,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:901\",\"rtc_server_id\":\"77\",\"paused\":false}}");
    srv.message("{\"t\":\"STREAM_CREATE\",\"s\":4,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"rtc_server_id\":\"555\",\"viewer_ids\":[],\"paused\":false}}");
    srv.message("{\"t\":\"STREAM_SERVER_UPDATE\",\"s\":5,\"op\":0,\"d\":{\"token\":\"tok\",\"stream_key\":\"guild:10:102:900\",\"guild_id\":null,\"endpoint\":\"c-x.discord.media:2087\"}}");
    pump(ws, gw, srv, 20);
    CHECK(gw.streamServer() == 555 && !strcmp(gw.streamToken(), "tok") && !strcmp(gw.streamEndpoint(), "c-x.discord.media:2087"));
    // who's watching: STREAM_UPDATE's viewer_ids, each one coming and going counted; someone else's stream
    // isn't ours; an update without the list changes nothing; an empty one is everyone gone
    CHECK(gw.streamViewers() == 0 && gw.viewersJoined() == 0);
    srv.message("{\"t\":\"STREAM_UPDATE\",\"s\":51,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"viewer_ids\":[\"7\",\"8\"],\"paused\":false}}");
    srv.message("{\"t\":\"STREAM_UPDATE\",\"s\":52,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:901\",\"viewer_ids\":[\"9\"],\"paused\":false}}");
    pump(ws, gw, srv, 21);
    CHECK(gw.streamViewers() == 2 && gw.viewersJoined() == 2 && gw.viewersLeft() == 0);
    srv.message("{\"t\":\"STREAM_UPDATE\",\"s\":53,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"viewer_ids\":[\"8\"],\"paused\":false}}");
    srv.message("{\"t\":\"STREAM_UPDATE\",\"s\":54,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"paused\":true}}");
    pump(ws, gw, srv, 22);
    CHECK(gw.streamViewers() == 1 && gw.viewersJoined() == 2 && gw.viewersLeft() == 1);
    srv.message("{\"t\":\"STREAM_UPDATE\",\"s\":55,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"viewer_ids\":[],\"paused\":false}}");
    pump(ws, gw, srv, 23);
    CHECK(gw.streamViewers() == 0 && gw.viewersLeft() == 2);
    srv.message("{\"t\":\"STREAM_DELETE\",\"s\":6,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"reason\":\"user_requested\",\"unavailable\":false}}");
    pump(ws, gw, srv, 30);
    CHECK(gw.streamServer() == 0 && !gw.streamEndpoint()[0]);
    CHECK(gw.streamEnded() == 1);  // (Discord ended it, not us: Screenshare turns off)
    CHECK(gw.streamDelete(10, 102));
    CHECK(srv.sent().back().second == "{\"op\":19,\"d\":{\"stream_key\":\"guild:10:102:900\"}}");
    // our own ending, echoed back by Discord: not counted
    CHECK(gw.streamCreate(10, 102) && gw.streamDelete(10, 102));
    srv.message("{\"t\":\"STREAM_DELETE\",\"s\":7,\"op\":0,\"d\":{\"stream_key\":\"guild:10:102:900\",\"reason\":\"user_requested\",\"unavailable\":false}}");
    pump(ws, gw, srv, 40);
    CHECK(gw.streamEnded() == 1);
    // the bot in another guild's channel through another session: still our call here
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":8,\"op\":0,\"d\":{\"user_id\":\"900\",\"guild_id\":\"20\",\"channel_id\":\"201\","
        "\"session_id\":\"elsewhere\"}}");
    pump(ws, gw, srv, 50);
    CHECK(gw.callChannel() == 102 && gw.callGuild() == 10);
    // another session takes the bot's call in this guild over: the call's over here
    srv.message(
        "{\"t\":\"VOICE_STATE_UPDATE\",\"s\":9,\"op\":0,\"d\":{\"user_id\":\"900\",\"guild_id\":\"10\",\"channel_id\":\"102\","
        "\"session_id\":\"elsewhere\"}}");
    pump(ws, gw, srv, 60);
    CHECK(gw.callChannel() == 0 && gw.callGuild() == 0);
    // a voice server named; a new session forgets it (its token was the old session's: using it was turned down)
    srv.message("{\"t\":\"VOICE_SERVER_UPDATE\",\"s\":10,\"op\":0,\"d\":{\"token\":\"old\",\"guild_id\":\"10\",\"endpoint\":\"c-x.discord.media:443\"}}");
    pump(ws, gw, srv, 70);
    CHECK(!strcmp(gw.voiceEndpoint(), "c-x.discord.media:443") && gw.voiceServerGuild() == 10);
    gw.start(&ws, "x", rnd, logLine, 80);
    CHECK(!gw.voiceEndpoint()[0] && !gw.voiceToken()[0] && gw.voiceServerGuild() == 0 && !gw.voiceSession()[0]);
}

// Guilds in full inside READY (as a sharded bot may get them), and a stub that waits for its GUILD_CREATE
static void testReadyGuilds() {
    store::clear();
    FakeServer srv;
    WebSocket ws;
    ws.reset(&srv, rnd);
    Gateway gw;
    gw.start(&ws, "x", rnd, logLine, 0);
    srv.message("{\"t\":null,\"s\":null,\"op\":10,\"d\":{\"heartbeat_interval\":41250}}");
    srv.message(
        "{\"t\":\"READY\",\"s\":1,\"op\":0,\"d\":{\"user\":{\"id\":\"900\",\"username\":\"chirpbot\",\"avatar\":\"0123456789abcdef\"},"
        "\"guilds\":[{\"id\":\"20\",\"name\":\"Ready Server\",\"icon\":\"a_0123456789abcdef\",\"channels\":[{\"id\":\"201\",\"type\":2,"
        "\"name\":\"Hangout\",\"position\":0}],\"voice_states\":[{\"user_id\":\"7\",\"channel_id\":\"201\"}]},"
        "{\"id\":\"30\",\"unavailable\":true}]}}");
    pump(ws, gw, srv, 10);
    CHECK(gw.ready());
    int g = store::guildFind(20);
    CHECK(g >= 0);
    const store::Guild* guild = store::guildAt(g);
    CHECK(guild && !strcmp(guild->name, "Ready Server") && !strcmp(guild->icon, "ia_0123456789abcdef"));
    CHECK(store::channelFind(201) != nullptr);
    CHECK(store::guildFind(30) < 0);  // (a stub: not until its GUILD_CREATE)
    char me[CHIRP_AVATAR];
    gw.myAvatar(me);
    CHECK(!strcmp(me, "0123456789abcdef"));
    // the stub's GUILD_CREATE: now it's there too, and the first one still is
    srv.message("{\"t\":\"GUILD_CREATE\",\"s\":2,\"op\":0,\"d\":{\"id\":\"30\",\"name\":\"Later Server\",\"channels\":[]}}");
    pump(ws, gw, srv, 10);
    CHECK(store::guildFind(30) >= 0 && store::guildFind(20) >= 0);
}

static void testMixer() {
    static int16_t a[MIXER_FRAME], b[MIXER_FRAME], out[MIXER_FRAME * 2];
    for (size_t i = 0; i < MIXER_FRAME; i++) a[i] = 20000, b[i] = (int16_t)(i & 1 ? -10000 : 10000);
    const int16_t* frames[2] = {a, b};
    const int U = MIXER_UNITY;
    int gains[2] = {U, U};
    mixer::mix(frames, gains, 2, out);
    CHECK(out[0] == 30000 && out[1] == 30000 && out[2] == 10000 && out[3] == 10000);
    gains[0] = 2 * U;  // 40000 + 10000: clamped
    mixer::mix(frames, gains, 2, out);
    CHECK(out[0] == 32767 && out[2] == 30000);
    gains[0] = 0, gains[1] = U / 2;  // muted, and half
    mixer::mix(frames, gains, 2, out);
    CHECK(out[0] == 5000 && out[2] == -5000);
    for (size_t i = 0; i < MIXER_FRAME; i++) a[i] = -30000;
    gains[0] = 2 * U, gains[1] = 0;
    mixer::mix(frames, gains, 2, out);
    CHECK(out[0] == -32768);
    // the sliders' curve: squared up to 100% (50%: -12 dB), linear to x2 above
    CHECK(mixer::gainQ12(0) == 0 && mixer::gainQ12(-5) == 0 && mixer::gainQ12(100) == U);
    CHECK(mixer::gainQ12(50) == U / 4 && mixer::gainQ12(10) == U / 100);
    CHECK(mixer::gainQ12(150) == U * 3 / 2 && mixer::gainQ12(200) == 2 * U && mixer::gainQ12(250) == 2 * U);
    int last = -1;
    bool rising = true;
    for (int v = 0; v <= 200; v += 5) rising = rising && mixer::gainQ12(v) > last, last = mixer::gainQ12(v);
    CHECK(rising);
}

// Whether the mic hears talk: frames' levels in dBFS. (feed() decides the frame before the one fed)
static void testDetector() {
    mic::Detector d;
    int on = 0;
    for (int i = 0; i < 200; i++) on += d.feed(-62 + (i % 5));  // (a quiet room, wavering)
    CHECK(on == 0 && d.floor_db < -55);
    // a word: two loud frames in a row open it (the first of them is the one decided)
    CHECK(!d.feed(-30) && d.feed(-30));
    int held = 0;
    while (d.feed(-62)) held++;
    CHECK(held == 15);  // (300 ms after: the last loud frame, then 14 more)
    // a click or a button: one loud frame alone never opens it, and is counted
    for (int i = 0; i < 20; i++) d.feed(-62);
    uint32_t clicks = d.clicks;
    CHECK(!d.feed(-20) && !d.feed(-62) && !d.feed(-62));
    CHECK(d.clicks == clicks + 1);
    // ten seconds of talk, syllables with short gaps: never cut off once it's going
    d.feed(-28);
    int cut = 0;
    for (int i = 0; i < 500; i++) cut += !d.feed(i % 10 < 7 ? -28 : -60);
    CHECK(cut == 0);
    // a fan turns on (-40): heard as talk at first, but the floor catches up within ~7 s
    int talk = 0;
    for (int i = 0; i < 1000; i++) talk += d.feed(-40);
    CHECK(talk < 400 && !d.on);
    // and talk over the fan still comes through
    d.feed(-22);
    CHECK(d.feed(-22));
    // a fixed sensitivity: -35 dB is the line whatever the room does
    mic::Detector f;
    f.fixed_db = -35;
    for (int i = 0; i < 100; i++) f.feed(-80);
    CHECK(f.threshold() == -35.f);
    f.feed(-38);
    CHECK(!f.feed(-38));  // (under it)
    f.feed(-30);
    CHECK(f.feed(-30));
    // the input volume: 100% is as it is, 50% a quarter (-12 dB), 200% double
    CHECK(mic::volumeGain(100) == 1.f && fabsf(mic::volumeGain(50) - 0.25f) < 1e-6f && mic::volumeGain(200) == 2.f);
    // the high-pass: a 30 Hz rumble mostly gone, 1 kHz untouched
    auto through = [](float hz) {
        mic::HighPass hp;
        static float s[48000];
        for (int i = 0; i < 48000; i++) s[i] = sinf(2 * 3.14159265f * hz * i / 48000);
        hp.run(s, 48000);
        double e = 0;
        for (int i = 24000; i < 48000; i++) e += (double)s[i] * s[i];
        return 10 * log10(e / 12000);  // (a unit sine's energy over 24000 samples is 12000)
    };
    CHECK(through(30) < -20 && fabs(through(1000)) < 0.5);
    // the automatic gain: a quiet talker (EarPods: speech around -45 dBFS) is brought up to about -20
    // within ten seconds; silence leaves it alone; someone loud is turned down within a second
    mic::Agc agc;
    for (int i = 0; i < 500; i++) agc.feed(i % 10 < 7 ? -45 : -60, i % 10 < 7);
    float out = -45 + agc.gain_db;
    CHECK(out > -24 && out < -16);
    float kept = agc.gain_db;
    for (int i = 0; i < 500; i++) agc.feed(-70, false);
    CHECK(agc.gain_db == kept);
    for (int i = 0; i < 50; i++) agc.feed(-8, true);
    CHECK(-8 + agc.gain_db < -16 && agc.gain_db >= MIC_AGC_MIN);
    for (int i = 0; i < 5000; i++) agc.feed(-90, true);  // (never past the most it gives)
    CHECK(agc.gain_db <= MIC_AGC_MAX);
    // levels
    int16_t full[960], none[960];
    for (int i = 0; i < 960; i++) full[i] = i & 1 ? 32767 : -32767, none[i] = 0;
    CHECK(fabsf(mic::levelDb(full, 960)) < 0.01f && mic::levelDb(none, 960) == -96.f);
}

// The jitter buffer, fed as the Switch log showed: a packet every 20 ms, but the voice thread
// now and then held up for 100-200 ms and handing everything in at once
static void testJitter() {
    static mixer::Jitter j;
    mixer::Stats st = {};
    uint8_t p[4] = {1, 2, 3, 4};
    uint16_t seq = 65500;  // (wraps partway)
    j = mixer::Jitter{};
    std::vector<uint16_t> held;
    int played = 0, silent = 0, worst_lag = 0;
    bool in_order = true;
    uint16_t last = 0;
    for (int t = 0; t < 3000; t++) {
        held.push_back(seq++);
        bool stalled = (t % 250) >= 240 || (t % 400) >= 390;  // (10 frames: 200 ms)
        if (!stalled) {
            for (uint16_t s : held) j.push(s, p, 4, &st);
            held.clear();
        }
        const mixer::Jitter::Slot* sl = j.pop(&st);
        if (sl) {
            if (played && sl->seq != (uint16_t)(last + 1) && (int16_t)(sl->seq - last) <= 0) in_order = false;
            last = sl->seq, played++;
        } else silent++;
        int lag = (int16_t)(seq - 1 - j.next);
        if (t > 100 && !stalled && lag > worst_lag) worst_lag = lag;
    }
    CHECK(in_order && st.late == 0 && st.gaps == 0);
    CHECK(worst_lag <= MIXER_TRIM_AT);  // (the lag doesn't grow: the old buffer's did, until it overflowed)
    CHECK(st.waited > 0 && st.trimmed > 0 && played + silent == 3000);
    CHECK((int)st.trimmed + played >= 3000 - 30);  // (each packet played or skipped: none overwritten)

    // a lost one is skipped over; one after its turn is dropped
    j = mixer::Jitter{}, st = mixer::Stats{};
    for (uint16_t s = 0; s < 4; s++)
        if (s != 2) j.push(s, p, 4, &st);
    CHECK(j.pop(&st) && j.pop(&st) && st.trimmed == 0);
    CHECK(!j.pop(&st) && st.gaps == 1 && j.pop(&st) && j.next == 4);
    j.push(2, p, 4, &st);
    CHECK(st.late == 1);
    // too far ahead to fit: the oldest go, the newest stays
    for (uint16_t s = 4; s < 8; s++) j.push(s, p, 4, &st);
    j.push(4 + MIXER_JITTER + 3, p, 4, &st);
    CHECK(j.next == 8 && st.trimmed == 4);
    // too far behind (one comes in TRIM_AT ahead): back to START_AT's cushion
    j = mixer::Jitter{}, st = mixer::Stats{};
    for (uint16_t s = 0; s < MIXER_START_AT; s++) j.push(s, p, 4, &st);
    CHECK(j.pop(&st) != nullptr);
    for (uint16_t s = MIXER_START_AT; s <= MIXER_TRIM_AT + 1; s++) j.push(s, p, 4, &st);
    const mixer::Jitter::Slot* sl = j.pop(&st);
    CHECK(sl && (int)sl->seq == MIXER_TRIM_AT + 1 - (MIXER_START_AT - 1) && st.trimmed > 0);
    // nothing for 200 ms: stops, and starts again with the cushion
    j = mixer::Jitter{}, st = mixer::Stats{};
    for (uint16_t s = 0; s < MIXER_START_AT; s++) j.push(s, p, 4, &st);
    for (int k = 0; k < MIXER_START_AT; k++) CHECK(j.pop(&st) != nullptr);
    for (int k = 0; k < 11; k++) CHECK(!j.pop(&st));
    CHECK(!j.playing && st.waited == 0);
    j.push(MIXER_START_AT, p, 4, &st);
    CHECK(!j.pop(&st) || MIXER_START_AT == 1);
}

static void testStore() {
    store::clear();
    int s = store::guildBegin();
    store::channelSet(s, 1, 0, 0, ChirpChannel_Voice, "v", 1);
    store::guildCommit(s, 77, "G", 1);
    store::memberSet(s, 5, 1, 0, nullptr, 0, nullptr);
    CHECK(store::membersIn(1) == 1);
    // the same guild again (a new GUILD_CREATE): it replaces the first, channels and all
    int s2 = store::guildBegin();
    CHECK(s2 != s);
    store::guildCommit(s2, 77, "G2", 2);
    CHECK(store::guildCount() == 1);
    CHECK(store::channelFind(1) == nullptr && store::membersIn(1) == 0);
    // a voice channel that becomes a text channel goes
    store::channelSet(s2, 2, 0, 0, ChirpChannel_Voice, "v", 1);
    store::channelSet(s2, 2, 0, 0, 0, "t", 1);
    CHECK(store::channelFind(2) == nullptr);
    // prefs
    CHECK(store::prefVolume(5) == 100 && !store::prefMute(5));
    store::prefSetVolume(5, 250);
    CHECK(store::prefVolume(5) == 200);
    store::prefSetMute(5, true);
    store::prefSetVolume(5, 100);
    CHECK(store::prefMute(5) && store::prefVolume(5) == 100);
    // people.ini: written, then read back into an empty table
    store::prefSetVolume(7, 150);
    store::prefSetMute(8, true);
    char ini[1024];
    size_t n = store::prefsWrite(ini, sizeof(ini));
    std::string text(ini, n);
    CHECK(text.find("id=5;volume=100;muted=true;\n") != std::string::npos &&
          text.find("id=7;volume=150;muted=false;\n") != std::string::npos &&
          text.find("id=8;volume=100;muted=true;\n") != std::string::npos);
    store::prefSetMute(5, false), store::prefSetVolume(7, 100), store::prefSetMute(8, false);
    CHECK(store::prefVolume(7) == 100 && !store::prefMute(8));
    // (with a line it can't read, and spaces where they may be)
    std::string more = text + "garbage\nmuted=false; volume=40;id=9\r\n";
    store::prefsRead(more.data(), more.size());
    CHECK(store::prefMute(5) && store::prefVolume(7) == 150 && store::prefMute(8) && store::prefVolume(9) == 40);
    // the master volume: "id=everyone", written and read back; an id that can't be read isn't it
    CHECK(store::prefVolume(STORE_EVERYONE) == 100);
    store::prefSetVolume(STORE_EVERYONE, 80);
    n = store::prefsWrite(ini, sizeof(ini));
    CHECK(std::string(ini, n).find("id=everyone;volume=80;muted=false;\n") != std::string::npos);
    store::prefSetVolume(STORE_EVERYONE, 100);
    std::string master = "id=everyone;volume=60;\nid=nobody;volume=10;\n";
    store::prefsRead(master.data(), master.size());
    CHECK(store::prefVolume(STORE_EVERYONE) == 60 && store::prefVolume(0) == 100);
    // a full guild table drops (and counts) the rest
    store::clear();
    for (int i = 0; i < STORE_GUILDS; i++) store::guildCommit(store::guildBegin(), 1000 + i, "g", 1);
    CHECK(store::guildBegin() == -1 && store::dropped().guilds == 1);
}

// The screen share's send queue (framequeue.h): frames whole in the ring, room after the newest or back
// at the start, one held at a time, one of its own memory blocking room until it's gone
static void testFrameQueue() {
    static uint8_t ring[1000], own[50];
    FrameQueue q;
    q.init(ring, sizeof(ring));
    FrameQueue::Frame f;
    CHECK(q.empty() && !q.take(&f));
    CHECK(q.room(1001) == nullptr);  // (bigger than the ring: never)
    uint8_t* a = q.room(400);
    CHECK(a == ring);
    q.push(a, 300, 400, 11, false);
    uint8_t* b = q.room(400);
    CHECK(b == ring + 400);
    q.push(b, 350, 400, 22, true);
    CHECK(q.room(400) == nullptr);  // (200 left after, none before the first)
    CHECK(q.take(&f) && f.p == ring && f.n == 300 && f.tick == 11 && !f.gap);
    CHECK(!q.take(&f));  // (one at a time)
    q.done();
    CHECK(q.room(400) == ring);  // (the first's gone: room at the start again)
    q.push(ring, 10, 400, 33, false);
    CHECK(q.take(&f) && f.p == ring + 400 && f.gap);
    q.done();
    CHECK(q.take(&f) && f.tick == 33);
    q.done();
    CHECK(q.empty());
    // one of its own memory: no room in the ring until it's sent
    q.push(own, 50, sizeof(own), 44, false);
    CHECK(q.room(10) == nullptr);
    CHECK(q.take(&f) && f.p == own);
    q.done();
    CHECK(q.room(10) == ring);
    // dropped: what's queued goes, but not one the reader holds
    q.push(q.room(100), 100, 100, 1, false);
    q.push(q.room(100), 100, 100, 2, false);
    CHECK(q.take(&f));
    q.drop();
    CHECK(!q.empty());
    q.done();
    q.drop();
    CHECK(q.empty());
    // all the slots full: no room, however much ring is left
    for (uint32_t i = 0; i < FrameQueue::SLOTS; i++) q.push(q.room(10), 10, 10, i, false);
    CHECK(q.room(10) == nullptr);
}

// The recorder's clocks (screenclock.h)
static void testScreenClocks() {
    VideoClock v;
    uint64_t gap = 0;
    // the first frame: set on our clock at once; then frames 16.7 ms apart follow the recorder's own times
    CHECK(v.recorded(1'000'000, 5'000'000'000ULL) == 5'000'000'000ULL);
    CHECK(v.missed(1'000'000, &gap) == 0 && gap == 0);
    CHECK(v.recorded(1'016'667, 5'020'000'000ULL) == 5'016'667'000ULL);  // (read 3 ms late: still its own time)
    CHECK(v.missed(1'016'667, &gap) == 0 && gap == 16667);
    CHECK(v.missed(1'033'334, &gap) == 0);
    // three frames' time: two missed
    CHECK(v.missed(1'083'335, &gap) == 2 && gap == 50001);
    // a step shorter than a frame (the recorder catching up) isn't taken as the frame time
    CHECK(v.missed(1'083'335 + 5000, &gap) == 0);
    CHECK(v.missed(1'083'335 + 5000 + 16667, &gap) == 0);
    // drifted over half a second: set again from now
    CHECK(v.recorded(1'200'000, 9'000'000'000ULL) == 9'000'000'000ULL);
    // after a reset, the next is the first again
    v.reset();
    CHECK(v.missed(2'000'000, &gap) == 0 && gap == 0);

    AudioClock a;
    // chunks of 1024 samples, 21333 us apart: the timestamps are microseconds; nothing to fill
    uint64_t ts = 10'000'000;
    for (int i = 0; i < 4; i++, ts += 21333) {
        AudioClock::Place p = a.place(ts, 1024);
        CHECK(!p.hole && !p.silence && !p.skip);
    }
    // one chunk missed (read too late): its samples are filled with silence
    ts += 21333;
    AudioClock::Place p = a.place(ts, 1024);
    CHECK(p.hole && p.silence >= 1020 && p.silence <= 1030 && !p.skip);
    // a long hole (the HOME menu, 2 s): the packets move on instead
    ts += 21333 + 2'000'000;
    p = a.place(ts, 1024);
    CHECK(p.hole && !p.silence && p.skip > 90000);
    // the clock going back over a second: begun again, no hole
    p = a.place(ts - 3'000'000, 1024);
    CHECK(!p.hole);
    p = a.place(ts - 3'000'000 + 21333, 1024);
    CHECK(!p.hole && !p.silence);
}

// share::Video's stand-ins: frames handed out in order, loading screens, and a sink that records
struct FakeFrames : share::Frames {
    struct F {
        std::vector<uint8_t> bytes;
        bool gap;
        uint64_t tick = 0;
    };
    std::vector<F> waiting;
    uint8_t buf[8192];
    uint8_t ring[8192];
    bool lent = false, full = false;
    bool take(uint8_t** f, size_t* n, size_t* cap, uint64_t* tick, bool* gap) override {
        if (waiting.empty()) return false;
        memcpy(buf, waiting[0].bytes.data(), waiting[0].bytes.size());
        *f = buf, *n = waiting[0].bytes.size(), *cap = sizeof(buf), *tick = waiting[0].tick, *gap = waiting[0].gap;
        return true;
    }
    void done() override { waiting.erase(waiting.begin()); }
    uint8_t* borrow(size_t* cap, bool flush) override {
        if (flush) waiting.clear();
        if (!waiting.empty()) return nullptr;
        lent = true;
        *cap = sizeof(ring);
        return ring;
    }
    void giveBack() override { lent = false; }
    bool fullRange() override { return full; }
};

struct FakeLoading : share::Loading {
    bool have = true;
    std::vector<uint8_t> file;
    bool pick(char* name, size_t cap, const char** why) override {
        if (!have) {
            *why = "none here";
            return false;
        }
        snprintf(name, cap, "miku.h264");
        return true;
    }
    long read(const char*, uint8_t* buf, size_t cap) override {
        size_t n = file.size() < cap ? file.size() : cap;
        memcpy(buf, file.data(), n);
        return (long)n;
    }
};

struct FakeSink : share::Sink {
    struct Sent {
        int nal;  // (the type of the last NAL: 5 a keyframe's slice, 1 another's)
        bool keyframe;
        size_t n;
    };
    std::vector<Sent> sent;
    std::vector<std::string> lines;
    bool fail_next = false;
    bool send(uint8_t* f, size_t n, size_t, uint64_t) override {
        Sent s = {0, false, n};
        for (size_t i = 0; i + 3 < n; i++)
            if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1) {
                s.nal = f[i + 3] & 0x1f;
                if (s.nal == 5) s.keyframe = true;
            }
        sent.push_back(s);
        if (fail_next) {
            fail_next = false;
            return false;
        }
        return true;
    }
    void log(const char* line) override { lines.push_back(line); }
    bool said(const char* what) const {
        for (auto& l : lines)
            if (l.find(what) != std::string::npos) return true;
        return false;
    }
};

static std::vector<uint8_t> h264Frame(bool idr, uint8_t fill, size_t body = 40) {
    std::vector<uint8_t> f = {0, 0, 0, 1, (uint8_t)(idr ? 0x65 : 0x41), 0x88};
    for (size_t i = 0; i < body; i++) f.push_back((uint8_t)(fill + i) | 0x10);  // (no start codes inside)
    return f;
}

static void testShareVideo() {
    FakeFrames frames;
    FakeLoading loading;
    FakeSink sink;
    share::Video v(&frames, &loading, &sink);
    uint64_t now = 1000;
    v.reset(now);
    auto push = [&](bool idr, uint8_t fill, bool gap = false, size_t body = 40) { frames.waiting.push_back({h264Frame(idr, fill, body), gap}); };
    auto run = [&]() {
        while (v.step(now, 0)) {
        }
    };

    // the game's frames go as they come; a keyframe with the SPS and PPS put in front
    push(true, 1), push(false, 2), push(false, 3);
    run();
    CHECK(sink.sent.size() == 3 && sink.sent[0].keyframe && !sink.sent[1].keyframe);
    CHECK(sink.sent[0].n > h264Frame(true, 1).size());  // (the parameter sets went in front)
    share::Video::Stats st;
    v.takeStats(&st);
    CHECK(st.sent == 3 && st.keyframes == 1 && st.waited == 0);

    // frames lost before one: the ones after wait for a keyframe
    sink.sent.clear();
    push(false, 4, true), push(false, 5), push(true, 6), push(false, 7);
    run();
    CHECK(sink.sent.size() == 2 && sink.sent[0].keyframe);
    v.takeStats(&st);
    CHECK(st.waited == 2);

    // one that didn't all get out breaks the ones after it the same way
    sink.sent.clear();
    sink.fail_next = true;
    push(false, 8), push(false, 9), push(true, 10);
    run();
    CHECK(sink.sent.size() == 2 && sink.sent[1].keyframe);
    v.takeStats(&st);
    CHECK(st.failed == 1 && st.waited == 1);

    // frames too late (they waited behind a slow network) aren't sent; the ones after wait for a keyframe,
    // which goes even late; in time again, they go
    sink.sent.clear();
    v.setStale(300);
    frames.waiting.push_back({h264Frame(false, 13), false, 1000});  // (1000 ticks late: skipped)
    frames.waiting.push_back({h264Frame(false, 14), false, 1990});  // (in time, but after a skipped one)
    frames.waiting.push_back({h264Frame(true, 15), false, 1000});   // (late, but a keyframe)
    frames.waiting.push_back({h264Frame(false, 16), false, 1995});
    while (v.step(now, 2000)) {
    }
    CHECK(sink.sent.size() == 2 && sink.sent[0].keyframe && !sink.sent[1].keyframe);
    v.takeStats(&st);
    CHECK(st.stale == 1 && st.waited == 1);
    v.setStale(0);

    // no frames for a while: the loading screen, again every 2 s, not in between
    sink.sent.clear();
    loading.file = h264Frame(true, 0x40, 300);
    now += 699;
    v.step(now, 0);
    CHECK(sink.sent.empty());
    now += 1;
    v.step(now, 0);
    CHECK(sink.sent.size() == 1 && sink.sent[0].keyframe && sink.said("loading screen miku.h264"));
    now += 1000;
    v.step(now, 0);
    CHECK(sink.sent.size() == 1);
    now += 1000;
    v.step(now, 0);
    CHECK(sink.sent.size() == 2 && !frames.lent);
    // the game's back: its frames wait for its keyframe (viewers have the loading screen's picture)
    sink.sent.clear();
    push(false, 11), push(true, 12);
    run();
    CHECK(sink.sent.size() == 1 && sink.sent[0].keyframe);

    // a loading screen that isn't a keyframe: grey instead, and grey for the rest of this time
    sink.sent.clear();
    sink.lines.clear();
    loading.file = h264Frame(false, 0x40, 300);
    now += 5000;
    v.step(now, 0);
    CHECK(sink.sent.size() == 1 && sink.sent[0].keyframe && sink.said("isn't a keyframe: grey instead"));
    // none on the card: grey
    push(true, 13);
    run();
    sink.sent.clear();
    sink.lines.clear();
    loading.have = false;
    now += 5000;
    v.step(now, 0);
    CHECK(sink.sent.size() == 1 && sink.sent[0].keyframe && sink.said("none here: grey"));
    loading.have = true;
    loading.file = h264Frame(true, 0x40, 300);

    // a game holding still: keyframes alike, but ordinary frames between: never taken for blank
    sink.sent.clear();
    sink.lines.clear();
    for (int i = 0; i < 5; i++) push(true, 20), push(false, (uint8_t)(21 + i));
    run();
    // (keyframes alike either side of a time with no frames (the loading screen meanwhile) aren't back to
    // back either: the keyframe before it, 13, is the size of these)
    CHECK(sink.sent.size() == 10 && !sink.said("blank picture"));

    // the recorder's blank picture: the same keyframe back to back. The third: the loading screen, the
    // queued frames thrown away
    sink.sent.clear();
    push(true, 30), push(true, 30), push(true, 30);
    run();
    CHECK(sink.said("blank picture"));
    CHECK(sink.sent.size() == 3 && sink.sent[2].n != sink.sent[0].n);  // (two keyframes, then the loading screen in place of the third)
    // still blank: nothing of the recorder's goes
    push(true, 30), push(true, 30);
    run();
    CHECK(sink.sent.size() == 3);
    // an ordinary frame: the game's back; it waits for a keyframe, which goes
    push(false, 31), push(true, 32), push(false, 33);
    run();
    CHECK(sink.said("picture is back") && sink.sent.size() == 5 && sink.sent[3].keyframe && !sink.sent[4].keyframe);
}

// The call's sounds (sounds.h): WAV headers read, stereo mixed down, a cut one faded; and the mixer's
// resampling to 48 kHz
static std::vector<uint8_t> wavBytes(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits, const std::vector<int16_t>& samples,
                                     bool extra_chunk = false) {
    std::vector<uint8_t> w;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) w.push_back((uint8_t)(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { w.push_back((uint8_t)v), w.push_back((uint8_t)(v >> 8)); };
    auto tag = [&](const char* t) { w.insert(w.end(), t, t + 4); };
    tag("RIFF"), u32(0), tag("WAVE");
    if (extra_chunk) tag("LIST"), u32(3), w.push_back('a'), w.push_back('b'), w.push_back('c'), w.push_back(0);  // (odd: padded)
    tag("fmt ");
    if (format == 0xFFFE) {
        u32(40), u16(format), u16(channels), u32(rate), u32(rate * channels * 2), u16((uint16_t)(channels * 2)), u16(bits);
        u16(22), u16(bits), u32(0), u16(1);  // (cbSize, valid bits, channel mask, the sub-format's first two bytes: PCM)
        for (int i = 0; i < 14; i++) w.push_back(0);
    } else {
        u32(16), u16(format), u16(channels), u32(rate), u32(rate * channels * 2), u16((uint16_t)(channels * 2)), u16(bits);
    }
    tag("data"), u32((uint32_t)(samples.size() * 2));
    for (int16_t v : samples) u16((uint16_t)v);
    return w;
}

static void testSounds() {
    sounds::WavInfo w;
    // the default chime, as it ships
    FILE* f = fopen("../sounds/voice_join.wav", "rb");
    CHECK(f != nullptr);
    if (f) {
        uint8_t head[4096];
        size_t n = fread(head, 1, sizeof(head), f);
        fclose(f);
        CHECK(sounds::wavInfo(head, n, &w) && w.rate == 24000 && w.channels == 1 && w.data_at == 44 && w.data_bytes / 2 <= SOUNDS_MAX);
    }
    // stereo, with a chunk before its samples (padded to an even size): found, mixed to mono
    auto st = wavBytes(1, 2, 48000, 16, {1000, 3000, -2000, -4000, 32767, 32767}, true);
    CHECK(sounds::wavInfo(st.data(), st.size(), &w) && w.channels == 2 && w.rate == 48000 && w.data_bytes == 12);
    int16_t mono[3];
    sounds::toMono(st.data() + w.data_at, 3, 2, mono);
    CHECK(mono[0] == 2000 && mono[1] == -3000 && mono[2] == 32767);
    // "extensible" PCM is PCM; 8-bit, float, three channels and 96 kHz aren't played
    auto ext = wavBytes(0xFFFE, 1, 16000, 16, {1, 2});
    CHECK(sounds::wavInfo(ext.data(), ext.size(), &w) && w.rate == 16000 && w.data_bytes == 4);
    auto b8 = wavBytes(1, 1, 24000, 8, {1});
    CHECK(!sounds::wavInfo(b8.data(), b8.size(), &w));
    auto fl = wavBytes(3, 1, 24000, 16, {1});
    CHECK(!sounds::wavInfo(fl.data(), fl.size(), &w));
    auto three = wavBytes(1, 3, 24000, 16, {1, 2, 3});
    CHECK(!sounds::wavInfo(three.data(), three.size(), &w));
    auto fast = wavBytes(1, 1, 96000, 16, {1});
    CHECK(!sounds::wavInfo(fast.data(), fast.size(), &w));
    const uint8_t junk[] = "RIFX....WAVEfmt ";
    CHECK(!sounds::wavInfo(junk, sizeof(junk), &w));
    // a cut sound's end faded to nothing, the rest as it was
    std::vector<int16_t> loud(1000, 10000);
    sounds::fadeOut(loud.data(), loud.size(), 24000, 10);  // (10 ms: 240 samples)
    CHECK(loud[759] == 10000 && loud[760] < 10000 && loud[999] == 0 && loud[880] > 4000 && loud[880] < 6000);

    // 24 kHz to 48: every other output sample between two inputs; then it runs out
    std::vector<int16_t> ramp(600);
    for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (int16_t)(i * 10);
    int16_t out[MIXER_FRAME];
    uint64_t pos = 0;
    CHECK(mixer::soundFrame(ramp.data(), ramp.size(), 24000, &pos, out));
    CHECK(out[0] == 0 && out[1] == 5 && out[2] == 10 && out[3] == 15 && out[958] == 4790);
    CHECK(!mixer::soundFrame(ramp.data(), ramp.size(), 24000, &pos, out));  // (600 at 24 kHz: 1200 at 48, 1.25 frames)
    CHECK(out[0] == 4800 && out[239] != 0 && out[240] == 0 && out[959] == 0);
    // 48 kHz plays as it is
    pos = 0;
    std::vector<int16_t> same(2000, 123);
    CHECK(mixer::soundFrame(same.data(), same.size(), 48000, &pos, out) && out[0] == 123 && out[959] == 123);
}

// Viewers' RTCP: opened as Discord seals it, and its feedback read
static void testRtcp() {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)(x >> 8)), v.push_back((uint8_t)x); };
    auto put32 = [&](std::vector<uint8_t>& v, uint32_t x) { put16(v, (uint16_t)(x >> 16)), put16(v, (uint16_t)x); };
    const uint32_t VSSRC = 0x11223344, OTHER = 0x55667788;
    // a receiver report (header only), a NACK for our video (packet 1000, and 1001 and 1004 by the bitmask,
    // then 2000 alone), a PLI, a REMB, and a NACK for another stream
    std::vector<uint8_t> c;
    c = {0x80, 201, 0, 1}, put32(c, 0xAAAA0001);
    c.push_back(0x81), c.push_back(205), put16(c, 4), put32(c, 0xAAAA0001), put32(c, VSSRC);
    put16(c, 1000), put16(c, 0x0009), put16(c, 2000), put16(c, 0);
    c.push_back(0x81), c.push_back(206), put16(c, 2), put32(c, 0xAAAA0001), put32(c, VSSRC);
    c.push_back(0x8F), c.push_back(206), put16(c, 5), put32(c, 0xAAAA0001), put32(c, 0);
    c.insert(c.end(), {'R', 'E', 'M', 'B', 1, 0, 0, 0}), put32(c, VSSRC);
    c.push_back(0x81), c.push_back(205), put16(c, 3), put32(c, 0xAAAA0001), put32(c, OTHER), put16(c, 7), put16(c, 0);
    uint16_t lost[8];
    RtcpFeedback fb = {};
    size_t k = rtcpFeedback(c.data(), c.size(), VSSRC, lost, 8, &fb);
    CHECK(k == 4 && lost[0] == 1000 && lost[1] == 1001 && lost[2] == 1004 && lost[3] == 2000);
    CHECK(fb.nacks == 1 && fb.lost == 4 && fb.keyframes == 1 && fb.remb == 1 && fb.other == 2);
    // more named than fit: counted, not written past the end
    fb = {};
    k = rtcpFeedback(c.data(), c.size(), VSSRC, lost, 2, &fb);
    CHECK(k == 2 && fb.lost == 4);
    // a length past the end: read up to it, no further
    std::vector<uint8_t> bad = c;
    bad[2] = 0x7f;
    fb = {};
    CHECK(rtcpFeedback(bad.data(), bad.size(), VSSRC, lost, 8, &fb) == 0 && fb.nacks == 0);

    // sealed as Discord does (the first 8 bytes the AAD, the rest AES-256-GCM, tag, 4-byte nonce): opens
    uint8_t key[RTP_KEY];
    for (int i = 0; i < RTP_KEY; i++) key[i] = (uint8_t)(i * 7 + 1);
    std::vector<uint8_t> pkt(c.begin(), c.begin() + 8);
    pkt.resize(c.size() + 16 + 4);
    uint8_t iv[GCM_IV] = {0, 0, 0, 42};
    Gcm g;
    g.init(key, RTP_KEY);
    g.seal(iv, c.data(), 8, c.data() + 8, c.size() - 8, pkt.data() + 8, pkt.data() + c.size(), GCM_TAG);
    memcpy(pkt.data() + c.size() + 16, iv, 4);
    std::vector<uint8_t> out(pkt.size());
    size_t on = rtcpOpen(Rtp_Aes256Gcm, key, pkt.data(), pkt.size(), out.data());
    CHECK(on == c.size() && memcmp(out.data(), c.data(), c.size()) == 0);
    pkt[12] ^= 1;  // (tampered: doesn't open)
    CHECK(rtcpOpen(Rtp_Aes256Gcm, key, pkt.data(), pkt.size(), out.data()) == 0);
    // only counted (no room for the numbers): nothing written
    fb = {};
    CHECK(rtcpFeedback(c.data(), c.size(), VSSRC, nullptr, 0, &fb) == 0 && fb.lost == 4 && fb.remb == 1);

    // a Sender Report of ours: sealed as viewers' reports are, opens to the right fields
    for (RtpMode mode : {Rtp_Aes256Gcm, Rtp_XChaCha}) {
        uint8_t sr[RTCP_SR_SIZE], op[RTCP_SR_SIZE];
        uint32_t counter = 41;
        size_t m = rtcpSenderReport(mode, key, &counter, VSSRC, 3'500'000'000ULL, 0xABCDEF01, 1234, 567890, sr);
        CHECK(m == RTCP_SR_SIZE && counter == 42 && sr[1] == 200 && sr[3] == 6);
        size_t on2 = rtcpOpen(mode, key, sr, m, op);
        auto be32at = [&](size_t o) { return (uint32_t)op[o] << 24 | (uint32_t)op[o + 1] << 16 | (uint32_t)op[o + 2] << 8 | op[o + 3]; };
        CHECK(on2 == 28 && be32at(4) == VSSRC && be32at(8) == 3 + 2208988800u);
        CHECK(be32at(12) == 0x80000000u && be32at(16) == 0xABCDEF01 && be32at(20) == 1234 && be32at(24) == 567890);
        fb = {};
        rtcpFeedback(op, on2, VSSRC, nullptr, 0, &fb);
        CHECK(fb.other == 1 && fb.nacks == 0);
    }

    // the sound's times put on the video's clock: none before the video's first frame, then by its offset
    VideoClock vc;
    uint64_t ns = 0;
    CHECK(!vc.toLocal(1000, &ns));
    vc.recorded(1000, 5'000'000'000ULL);
    CHECK(vc.toLocal(1000, &ns) && ns == 5'000'000'000ULL);
    CHECK(vc.toLocal(21333, &ns) && ns == 5'000'000'000ULL + 20'333'000);
}

int main() {
    srand(1234);
    testRtcp();
    testUtil();
    testJson();
    testGateway();
    testClose();
    testBigBinary();
    testInCallAlready();
    testReadyGuilds();
    testMixer();
    testJitter();
    testDetector();
    testStore();
    testFrameQueue();
    testScreenClocks();
    testShareVideo();
    testSounds();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
