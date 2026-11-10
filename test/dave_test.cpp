// DAVE sessions (dave.cpp) against a pretend voice gateway that acts as
// Discord's does (protocol.md): key packages in, external proposals out, the
// first commit for an epoch announced and its Welcome sent, transitions
// executed once everyone's ready. Then each member's frames, opened by the others.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "dave.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                \
    do {                                                        \
        if (c) g_pass++;                                        \
        else {                                                  \
            g_fail++;                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                       \
    } while (0)

typedef std::vector<uint8_t> Buf;

static void testRandom(void* out, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t*)out)[i] = (uint8_t)rand();
}

static const uint64_t CHANNEL = 555;
static uint8_t g_gw_priv[P256_PRIV], g_gw_pub[P256_PUB];
static uint64_t g_now = 1000;

// One member: its session, and what it has sent the gateway
struct Member : DaveOut {
    uint64_t id;
    DaveSession dave;
    std::vector<std::pair<int, Buf>> sent;  // (op, body): binary ones, and JSON ones as text with op -1
    void binary(uint8_t op, const uint8_t* body, size_t n) override { sent.push_back({op, Buf(body, body + n)}); }
    void json(const char* text) override { sent.push_back({-1, Buf(text, text + strlen(text))}); }
    void log(const char* line) override { printf("    [%llu] %s\n", (unsigned long long)id, line); }
    // (as voice.cpp: the messages come in through the same buffer DAVE writes its commit into)
    uint8_t box[RATCHET_ARENA + 16 * 1024];
    uint8_t* scratch(size_t* cap) override {
        *cap = sizeof(box);
        return box;
    }
    void feed(uint8_t op, const uint8_t* body, size_t n, uint64_t now) {
        memcpy(box, body, n);
        dave.onBinary(op, box, n, now);
    }
};

static Member g_m[3];

// The length of one MLSMessage commit at the front of b (the gateway splits commit from Welcome so)
static size_t commitLength(const Buf& b) {
    Reader r(b.data(), b.size());
    r.u16(), r.u16();
    r.vec(), r.u64(), r.u8(), r.u32(), r.vec(), r.u8();
    r.sub();
    if (r.u8() == 1) {
        LeafView l;
        readLeafNode(r, &l);
        r.vec();
    }
    r.vec(), r.vec(), r.vec();
    return r.ok ? b.size() - r.n : 0;
}

// An external Add proposal for a key package (op 26's body: an MLSMessage), at an epoch
// (kp: as a member sends it in op 26, the bare KeyPackage, as libdave and davey do)
static Buf addProposal(const Buf& kp, uint64_t epoch) {
    uint8_t gid[8];
    for (int i = 0; i < 8; i++) gid[i] = (uint8_t)(CHANNEL >> (56 - 8 * i));
    static uint8_t c[8192], tbs[8192];
    Writer w(c, sizeof(c));
    w.vec(gid, 8);
    w.u64(epoch);
    w.u8(2), w.u32(0), w.vec(nullptr, 0), w.u8(2);
    w.u16(Prop_Add);
    CHECK(kp.size() > 4 && !(kp[0] == 0 && kp[1] == 1 && kp[2] == 0 && kp[3] == 5));  // (no MLSMessage header)
    w.raw(kp.data(), kp.size());
    Writer t(tbs, sizeof(tbs));
    t.u16(1), t.u16(Wire_Public), t.raw(c, w.n);
    uint8_t sig[P256_SIG_MAX];
    size_t sig_n = 0;
    mlsSign(g_gw_priv, "FramedContentTBS", tbs, t.n, sig, &sig_n);
    Buf m = {0, 1, 0, Wire_Public};
    m.insert(m.end(), c, c + w.n);
    uint8_t len[4];
    Writer lw(len, sizeof(len));
    lw.varint((uint32_t)sig_n);
    m.insert(m.end(), len, len + lw.n);
    m.insert(m.end(), sig, sig + sig_n);
    return m;
}

// op 27, append: proposal_messages<V>
static Buf proposalsBody(const Buf& msg) {
    Buf b = {0};
    uint8_t len[4];
    Writer lw(len, sizeof(len));
    lw.varint((uint32_t)msg.size());
    b.insert(b.end(), len, len + lw.n);
    b.insert(b.end(), msg.begin(), msg.end());
    return b;
}

static Buf withTransition(uint16_t t, const uint8_t* p, size_t n) {
    Buf b = {(uint8_t)(t >> 8), (uint8_t)t};
    b.insert(b.end(), p, p + n);
    return b;
}

// The member's last sent message with this op (and forget what it sent)
static Buf takeSent(Member& m, int op) {
    Buf found;
    for (auto& s : m.sent)
        if (s.first == op) found = s.second;
    m.sent.clear();
    return found;
}

// A member joins: the gateway proposes adding it to those already in, they all commit, the
// first commit is announced (and its Welcome sent to the new one), then the transition is executed
static void joins(int who, std::vector<int> in, uint16_t transition, int committer) {
    Member& m = g_m[who];
    Buf kp = takeSent(m, DaveOp_KeyPackage);
    CHECK(!kp.empty());
    uint64_t epoch = in.empty() ? 0 : g_m[in[0]].dave.group().epoch();
    Buf body = proposalsBody(addProposal(kp, epoch));
    for (int i : in) g_m[i].feed(DaveOp_Proposals, body.data(), body.size(), g_now);
    // (everyone commits; the gateway takes the committer's, the first it got)
    Buf cw;
    for (int i : in) {
        Buf mine = takeSent(g_m[i], DaveOp_CommitWelcome);
        CHECK(!mine.empty());
        if (i == committer) cw = mine;
    }
    size_t commit_n = commitLength(cw);
    CHECK(commit_n > 0 && commit_n < cw.size());
    Buf announce = withTransition(transition, cw.data(), commit_n);
    Buf welcome = withTransition(transition, cw.data() + commit_n, cw.size() - commit_n);
    for (int i : in) g_m[i].feed(DaveOp_AnnounceCommit, announce.data(), announce.size(), g_now);
    m.feed(DaveOp_Welcome, welcome.data(), welcome.size(), g_now);
    // each says it's ready (op 23); then the gateway executes it
    in.push_back(who);
    for (int i : in) {
        Buf ready = takeSent(g_m[i], -1);
        std::string text(ready.begin(), ready.end());
        CHECK(text.find("\"op\":23") != std::string::npos && text.find("\"transition_id\":" + std::to_string(transition)) != std::string::npos);
        g_m[i].dave.onExecuteTransition(transition, g_now);
    }
}

// a's frame (sealed with a's own key for this epoch) opens for b
static bool hears(int a, int b, const Buf* frame_in = nullptr) {
    Buf frame;
    if (frame_in) {
        frame = *frame_in;
    } else {
        uint8_t base[DAVE_KEY];
        static DaveRatchet r;
        daveBaseSecret(g_m[a].dave.group().exporterSecret(), g_m[a].id, base);
        r.init(g_m[a].id, base);
        uint32_t counter = 0;
        uint8_t opus[60];
        testRandom(opus, sizeof(opus));
        frame.resize(sizeof(opus) + DAVE_SUPPLEMENTAL_MAX);
        size_t n = 0;
        daveEncrypt(r, &counter, opus, sizeof(opus), frame.data(), frame.size(), &n);
        frame.resize(n);
    }
    Buf out(frame.size());
    size_t n = 0;
    return g_m[b].dave.decrypt(g_m[a].id, frame.data(), frame.size(), out.data(), &n, g_now);
}

static Buf frameFrom(int a) {
    uint8_t base[DAVE_KEY];
    static DaveRatchet r;
    daveBaseSecret(g_m[a].dave.group().exporterSecret(), g_m[a].id, base);
    r.init(g_m[a].id, base);
    uint32_t counter = 41;
    uint8_t opus[40] = {1, 2, 3};
    Buf frame(sizeof(opus) + DAVE_SUPPLEMENTAL_MAX);
    size_t n = 0;
    daveEncrypt(r, &counter, opus, sizeof(opus), frame.data(), frame.size(), &n);
    frame.resize(n);
    return frame;
}

int main() {
    srand(7);
    do testRandom(g_gw_priv, sizeof(g_gw_priv));
    while (!p256ValidPrivate(g_gw_priv));
    p256Public(g_gw_priv, g_gw_pub);
    // op 25's body: ExternalSender {signature_key<V>, credential}
    uint8_t ext[128];
    Writer e(ext, sizeof(ext));
    e.vec(g_gw_pub, P256_PUB);
    e.u16(1);
    e.vec("gateway", 7);
    const uint64_t ids[3] = {1001, 1002, 1003};
    for (int i = 0; i < 3; i++) {
        g_m[i].id = ids[i];
        g_m[i].dave.start(ids[i], CHANNEL, &g_m[i], testRandom);
        g_m[i].feed(DaveOp_ExternalSender, ext, e.n, g_now);
        g_m[i].dave.onClientsConnect(ids, 3);
        g_m[i].dave.onProtocol(1, g_now);  // (Session Description: DAVE 1; each sends a key package)
    }
    // before any group is real, frames sent plain still play (passthrough), sealed ones don't open
    uint8_t plain[3] = {0x78, 0x01, 0x02}, out[64];
    size_t n = 0;
    CHECK(g_m[1].dave.decrypt(1001, plain, 3, out, &n, g_now) && n == 3);

    // Alice is first: Bob joins, Alice's commit wins
    takeSent(g_m[0], DaveOp_KeyPackage);  // (Alice's own key package: nobody adds her, her group is hers)
    joins(1, {0}, 1, 0);
    CHECK(g_m[0].dave.group().epoch() == 1 && g_m[1].dave.group().epoch() == 1);
    CHECK(hears(0, 1) && hears(1, 0));
    Buf old = frameFrom(0);  // (sealed in epoch 1, to open after the next transition)

    // Carol joins: both commit, Bob's is taken
    joins(2, {0, 1}, 2, 1);
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            if (a != b) CHECK(hears(a, b));
    CHECK(g_m[0].dave.group().epoch() == 2 && g_m[2].dave.group().epoch() == 2);
    // the frame from the last epoch: Bob still opens it for 10 seconds, then not
    CHECK(hears(0, 1, &old));
    g_now += 11000;
    CHECK(!hears(0, 1, &old));
    CHECK(hears(0, 1));
    // silence passes; a frame from someone not in the group doesn't open
    uint8_t silence[3] = {0xF8, 0xFF, 0xFE};
    CHECK(g_m[2].dave.decrypt(1001, silence, 3, out, &n, g_now) && n == 3);
    Buf f = frameFrom(0);
    CHECK(!g_m[2].dave.decrypt(9999, f.data(), f.size(), out, &n, g_now));

    // what the session itself sends (DaveSession::encrypt): each member's frames open for the others, in
    // order, many in a row; and silence goes as it is
    uint8_t opus[160], sealed[160 + DAVE_SUPPLEMENTAL_MAX], back[sizeof(sealed)];
    int opened = 0;
    for (int k = 0; k < 50; k++) {
        int a = k % 3, b = (k + 1) % 3;
        testRandom(opus, sizeof(opus));
        size_t sn = 0, bn = 0;
        CHECK(g_m[a].dave.encrypt(opus, sizeof(opus), sealed, sizeof(sealed), &sn) && sn > sizeof(opus));
        if (g_m[b].dave.decrypt(g_m[a].id, sealed, sn, back, &bn, g_now) && bn == sizeof(opus) && !memcmp(back, opus, bn)) opened++;
    }
    CHECK(opened == 50);
    size_t sn = 0;
    CHECK(g_m[0].dave.encrypt(silence, 3, sealed, sizeof(sealed), &sn) && sn == 3 && !memcmp(sealed, silence, 3));
    // before there's a group in a DAVE call, nothing can go (no key anyone could open it with)
    DaveSession lone;
    Member nobody;
    lone.start(4242, CHANNEL, &nobody, testRandom);
    lone.onProtocol(1, g_now);
    CHECK(!lone.encrypt(opus, sizeof(opus), sealed, sizeof(sealed), &sn));
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
