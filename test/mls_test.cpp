// Chirp's MLS against the MLS working group's test vectors (ciphersuite 2),
// flattened by test/mls_vectors.js.
#include <stdio.h>
#include <string.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mlscrypto.h"
#include "mlskeys.h"
#include "mlstree.h"
#include "mlswire.h"
#include "mlsgroup.h"
#include "ratchet.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (c) g_pass++;                                                                               \
        else {                                                                                         \
            g_fail++;                                                                                  \
            printf("FAIL %s:%d (%s): %s\n", __FILE__, __LINE__, g_case.c_str(), #c);                 \
        }                                                                                              \
    } while (0)

typedef std::vector<uint8_t> Buf;
typedef std::map<std::string, std::string> Case;
static std::string g_case;

static Buf unhex(const std::string& s) {
    Buf b;
    if (s == "-") return b;
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    return b;
}

static std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) snprintf(b, sizeof(b), "%02x", p[i]), s += b;
    return s.empty() ? "-" : s;
}

static std::vector<long long> nums(const std::string& s) {
    std::vector<long long> v;
    std::istringstream in(s);
    long long x;
    while (in >> x) v.push_back(x);
    return v;
}

static void wire() {
    g_case = "wire";
    uint8_t buf[64];
    // RFC 9420 2.1.2's examples of variable-length integers
    Writer w(buf, sizeof(buf));
    w.varint(37), w.varint(15293), w.varint(494878333);
    CHECK(w.ok && hex(buf, w.n) == "257bbd9d7f3e7d");
    Reader r(buf, w.n);
    CHECK(r.varint() == 37 && r.varint() == 15293 && r.varint() == 494878333 && r.done());
    uint8_t notMinimal[2] = {0x40, 0x25};
    Reader r2(notMinimal, 2);
    r2.varint();
    CHECK(!r2.ok);
    // a vector written before its contents are known
    Writer v(buf, sizeof(buf));
    size_t at = v.begin();
    for (int i = 0; i < 50; i++) v.u8(0xee);  // (50 needs only a 1-byte length: the contents move down 3)
    v.end(at);
    CHECK(v.ok && v.n == 51 && buf[0] == 50 && buf[1] == 0xee && buf[50] == 0xee);
    Writer small(buf, 40);
    size_t at2 = small.begin();
    small.u16(0xabcd);
    small.end(at2);
    CHECK(small.ok && hex(buf, small.n) == "02abcd");
}

static void cryptoBasics(Case& c) {
    uint8_t out[64];
    Buf secret = unhex(c["expand_with_label.secret"]), ctx = unhex(c["expand_with_label.context"]);
    size_t len = (size_t)std::stoi(c["expand_with_label.length"]);
    CHECK(mlsExpand(secret.data(), c["expand_with_label.label"].c_str(), ctx.data(), ctx.size(), out, len) &&
          hex(out, len) == c["expand_with_label.out"]);
    secret = unhex(c["derive_secret.secret"]);
    CHECK(mlsDeriveSecret(secret.data(), c["derive_secret.label"].c_str(), out) && hex(out, 32) == c["derive_secret.out"]);
    secret = unhex(c["derive_tree_secret.secret"]);
    len = (size_t)std::stoi(c["derive_tree_secret.length"]);
    CHECK(mlsDeriveTreeSecret(secret.data(), c["derive_tree_secret.label"].c_str(), (uint32_t)std::stoul(c["derive_tree_secret.generation"]),
                              out, len) &&
          hex(out, len) == c["derive_tree_secret.out"]);
    Buf value = unhex(c["ref_hash.value"]);
    mlsRefHash(c["ref_hash.label"].c_str(), value.data(), value.size(), out);
    CHECK(hex(out, 32) == c["ref_hash.out"]);
    // their signature checks out; ours does too; a changed one doesn't
    Buf priv = unhex(c["sign_with_label.priv"]), pub = unhex(c["sign_with_label.pub"]), content = unhex(c["sign_with_label.content"]);
    Buf sig = unhex(c["sign_with_label.signature"]);
    const char* label = c["sign_with_label.label"].c_str();
    CHECK(mlsVerify(pub.data(), pub.size(), label, content.data(), content.size(), sig.data(), sig.size()));
    uint8_t mine[P256_SIG_MAX];
    size_t mine_n = 0;
    CHECK(mlsSign(priv.data(), label, content.data(), content.size(), mine, &mine_n));
    CHECK(mlsVerify(pub.data(), pub.size(), label, content.data(), content.size(), mine, mine_n));
    CHECK(!mlsVerify(pub.data(), pub.size(), "another label", content.data(), content.size(), sig.data(), sig.size()));
    // their encryption opens
    priv = unhex(c["encrypt_with_label.priv"]);
    Buf ectx = unhex(c["encrypt_with_label.context"]), kem = unhex(c["encrypt_with_label.kem_output"]);
    Buf ct = unhex(c["encrypt_with_label.ciphertext"]), pt = unhex(c["encrypt_with_label.plaintext"]);
    Buf back(ct.size());
    CHECK(mlsDecrypt(priv.data(), c["encrypt_with_label.label"].c_str(), ectx.data(), ectx.size(), kem.data(), kem.size(), ct.data(),
                     ct.size(), back.data()) &&
          hex(back.data(), pt.size()) == hex(pt.data(), pt.size()));
    // and ours, to their key
    pub = unhex(c["encrypt_with_label.pub"]);
    uint8_t ikm[32] = {7}, kem2[HPKE_ENC];
    Buf ct2(pt.size() + 16), back2(pt.size() + 1);
    CHECK(mlsEncrypt(pub.data(), "test", ectx.data(), ectx.size(), pt.data(), pt.size(), ikm, kem2, ct2.data()));
    CHECK(mlsDecrypt(priv.data(), "test", ectx.data(), ectx.size(), kem2, sizeof(kem2), ct2.data(), ct2.size(), back2.data()) &&
          hex(back2.data(), pt.size()) == hex(pt.data(), pt.size()));
}

static void treeMath(Case& c) {
    uint32_t leaves = (uint32_t)std::stoul(c["n_leaves"]), nodes = (uint32_t)std::stoul(c["n_nodes"]);
    CHECK(mlstree::nodeWidth(leaves) == nodes);
    CHECK(mlstree::root(leaves) == (uint32_t)std::stoul(c["root"]));
    auto left = nums(c["left"]), right = nums(c["right"]), parent = nums(c["parent"]), sibling = nums(c["sibling"]);
    CHECK(left.size() == nodes && right.size() == nodes && parent.size() == nodes && sibling.size() == nodes);
    uint32_t root = mlstree::root(leaves);
    for (uint32_t x = 0; x < nodes && x < left.size(); x++) {
        if (mlstree::level(x) > 0) {
            CHECK(mlstree::left(x) == left[x] && mlstree::right(x) == right[x]);
        } else {
            CHECK(left[x] == -1 && right[x] == -1);
        }
        if (x != root) {
            CHECK(mlstree::parent(x) == parent[x] && mlstree::sibling(x) == sibling[x]);
        } else {
            CHECK(parent[x] == -1 && sibling[x] == -1);
        }
    }
}

static void keySchedule(Case& c) {
    Buf init = unhex(c["initial_init_secret"]);
    for (int e = 0;; e++) {
        std::string p = "epochs." + std::to_string(e) + ".";
        if (!c.count(p + "commit_secret")) break;
        Buf commit = unhex(c[p + "commit_secret"]), psk = unhex(c[p + "psk_secret"]), gc = unhex(c[p + "group_context"]);
        EpochKeys k;
        CHECK(mlsKeySchedule(init.data(), commit.data(), psk.data(), Bytes{gc.data(), gc.size()}, &k));
        CHECK(hex(k.joiner, 32) == c[p + "joiner_secret"]);
        CHECK(hex(k.welcome, 32) == c[p + "welcome_secret"]);
        CHECK(hex(k.sender_data, 32) == c[p + "sender_data_secret"]);
        CHECK(hex(k.encryption, 32) == c[p + "encryption_secret"]);
        CHECK(hex(k.exporter, 32) == c[p + "exporter_secret"]);
        CHECK(hex(k.external, 32) == c[p + "external_secret"]);
        CHECK(hex(k.confirmation_key, 32) == c[p + "confirmation_key"]);
        CHECK(hex(k.membership_key, 32) == c[p + "membership_key"]);
        CHECK(hex(k.resumption_psk, 32) == c[p + "resumption_psk"]);
        CHECK(hex(k.epoch_authenticator, 32) == c[p + "epoch_authenticator"]);
        CHECK(hex(k.init, 32) == c[p + "init_secret"]);
        // the external key pair comes from external_secret
        uint8_t sk[P256_PRIV], pk[P256_PUB];
        CHECK(hpkeDeriveKeyPair(k.external, 32, sk, pk) && hex(pk, P256_PUB) == c[p + "external_pub"]);
        // the exporter, as DAVE will use it. (The vectors' label is text that looks like hex, and is
        // used as that text, as the reference implementations do; DAVE's is "Discord Secure Frames v0".)
        const std::string& label = c[p + "exporter.label"];
        Buf ctx = unhex(c[p + "exporter.context"]);
        size_t len = (size_t)std::stoi(c[p + "exporter.length"]);
        uint8_t out[64];
        CHECK(mlsExport(k.exporter, label.data(), label.size(), ctx.data(), ctx.size(), out, len) && hex(out, len) == c[p + "exporter.secret"]);
        // (the group context is what the vector gives; checking it's made of its parts comes with the group code)
        init.assign(k.init, k.init + 32);
    }
}

static RatchetTree g_tree;  // (64 KB: not on the stack)

static void treeValidation(Case& c) {
    // (the vector is the whole ratchet_tree<V>, its length in front, as it comes in a message)
    Buf tree = unhex(c["tree"]), group = unhex(c["group_id"]);
    Reader r(tree.data(), tree.size());
    Bytes contents = r.vec();
    CHECK(r.done() && g_tree.decode(contents));
    // written back the same
    static uint8_t buf[64 * 1024];
    Writer w(buf, sizeof(buf));
    size_t at = w.begin();
    CHECK(g_tree.encode(w));
    w.end(at);
    CHECK(w.ok && hex(buf, w.n) == c["tree"]);
    for (uint32_t node = 0; node < g_tree.nodes(); node++) {
        std::string i = std::to_string(node);
        uint8_t h[MLS_NH];
        CHECK(g_tree.treeHash(node, h) && hex(h, MLS_NH) == c["tree_hashes." + i]);
        uint32_t res[RATCHET_NODES];
        int n = g_tree.resolution(node, res, RATCHET_NODES);
        auto want = nums(c["resolutions." + i]);
        bool same = n == (int)want.size();
        for (int k = 0; same && k < n; k++) same = res[k] == want[k];
        CHECK(same);
    }
    // every parent chained back to a leaf by parent hashes (the vectors' trees are all valid)
    CHECK(g_tree.verifyParentHashes());
    // every member's leaf is signed by them
    for (uint32_t li = 0; li < g_tree.leaves(); li++) {
        LeafView leaf;
        if (!g_tree.leaf(li, &leaf)) continue;
        CHECK(verifyLeafSignature(leaf, Bytes{group.data(), group.size()}, li));
    }
}

static MlsGroup g_group;
static const char* RESULT[] = {"Ok", "Malformed", "WrongGroup", "BadSignature", "BadTree", "NotForUs", "Unsupported", "UnknownProposal", "Removed", "TooBig"};

// An MLSMessage's body, after its version and wire format (which must be `wire`)
static Bytes body(const Buf& m, uint16_t wire) {
    Reader r(m.data(), m.size());
    if (r.u16() != 1 || r.u16() != wire) return Bytes{};
    return Bytes{r.p, r.n};
}

// The passive client tests: join from a Welcome, then follow every proposal
// and commit, the epoch authenticator checked at each epoch
static int g_passive_ok = 0, g_passive_skipped = 0;
static void passiveClient(Case& c) {
    g_group.reset();
    Buf kp = unhex(c["key_package"]), init = unhex(c["init_priv"]), enc = unhex(c["encryption_priv"]), sig = unhex(c["signature_priv"]);
    CHECK(g_group.setKeyPackage(body(kp, Wire_KeyPackage), init.data(), enc.data(), sig.data()));
    for (int k = 0;; k++) {
        std::string p = "external_psks." + std::to_string(k) + ".";
        if (!c.count(p + "psk_id")) break;
        Buf id = unhex(c[p + "psk_id"]), psk = unhex(c[p + "psk"]);
        CHECK(g_group.addExternalPsk(Bytes{id.data(), id.size()}, Bytes{psk.data(), psk.size()}));
    }
    Buf welcome = unhex(c["welcome"]);
    Buf tree = c["ratchet_tree"] == "-1" ? Buf() : unhex(c["ratchet_tree"]);
    MlsResult r = g_group.join(body(welcome, Wire_Welcome), Bytes{tree.data(), tree.size()});
    if (r == Mls_Unsupported) {
        g_passive_skipped++;  // (pre-shared keys: DAVE doesn't use them)
        printf("  %s: join stops at mlsgroup.cpp:%d (unsupported)\n", g_case.c_str(), g_group.whyLine());
        return;
    }
    if (r != Mls_Ok) printf("  join: %s at mlsgroup.cpp:%d\n", RESULT[r], g_group.whyLine());
    CHECK(r == Mls_Ok);
    if (r != Mls_Ok) return;
    CHECK(hex(g_group.epochAuthenticator(), MLS_NH) == c["initial_epoch_authenticator"]);
    for (int e = 0;; e++) {
        std::string p = "epochs." + std::to_string(e) + ".";
        if (!c.count(p + "commit")) break;
        for (int k = 0;; k++) {
            std::string key = p + "proposals." + std::to_string(k);
            if (!c.count(key)) break;
            Buf m = unhex(c[key]);
            r = g_group.handle(Bytes{m.data(), m.size()});
            if (r != Mls_Ok) printf("  epoch %d proposal %d: %s\n", e, k, RESULT[r]);
            CHECK(r == Mls_Ok);
        }
        Buf m = unhex(c[p + "commit"]);
        r = g_group.handle(Bytes{m.data(), m.size()});
        if (r == Mls_Unsupported) {
            g_passive_skipped++;
            printf("  %s: epoch %d's commit stops at mlsgroup.cpp:%d (unsupported)\n", g_case.c_str(), e, g_group.whyLine());
            return;
        }
        if (r != Mls_Ok) printf("  epoch %d commit: %s\n", e, RESULT[r]);
        CHECK(r == Mls_Ok);
        if (r != Mls_Ok) return;
        CHECK(hex(g_group.epochAuthenticator(), MLS_NH) == c[p + "epoch_authenticator"]);
    }
    g_passive_ok++;
}

// --- our own commits: a DAVE-like session between our own clients ---

static void testRandom(void* out, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t*)out)[i] = (uint8_t)rand();
}

// The voice gateway, as the group's external sender: its key, and its signed proposals
static uint8_t g_gw_priv[P256_PRIV], g_gw_pub[P256_PUB];

// An Add (a key package, as an MLSMessage) or a Remove (a leaf), proposed by the gateway
static Buf gatewayProposal(MlsGroup& g, const Buf& group_id, const Buf* key_package, uint32_t remove) {
    static uint8_t buf[8192];
    Writer c(buf, sizeof(buf));
    c.vec(group_id.data(), group_id.size());
    c.u64(g.epoch());
    c.u8(2);   // external
    c.u32(0);  // (the first external sender)
    c.vec(nullptr, 0);
    c.u8(2);   // proposal
    if (key_package) {
        c.u16(Prop_Add);
        c.raw(key_package->data() + 4, key_package->size() - 4);  // (the KeyPackage, out of its MLSMessage)
    } else {
        c.u16(Prop_Remove);
        c.u32(remove);
    }
    static uint8_t tbs[8192];
    Writer t(tbs, sizeof(tbs));
    t.u16(1);
    t.u16(Wire_Public);
    t.raw(buf, c.n);  // (no group context: external senders don't have it)
    uint8_t sig[P256_SIG_MAX];
    size_t sig_n = 0;
    mlsSign(g_gw_priv, "FramedContentTBS", tbs, t.n, sig, &sig_n);
    Buf m = {0, 1, 0, Wire_Public};
    m.insert(m.end(), buf, buf + c.n);
    uint8_t len[4];
    Writer lw(len, sizeof(len));
    lw.varint((uint32_t)sig_n);
    m.insert(m.end(), len, len + lw.n);
    m.insert(m.end(), sig, sig + sig_n);
    return m;
}

static Buf makeKp(MlsGroup& g, uint64_t user) {
    uint8_t id[8];
    for (int i = 0; i < 8; i++) id[i] = (uint8_t)(user >> (56 - 8 * i));  // (DAVE: the user id, big-endian)
    static uint8_t buf[2048];
    Writer w(buf, sizeof(buf));
    CHECK(g.makeKeyPackage(id, 8, testRandom, w));
    return Buf(buf, buf + w.n);
}

// One member commits what it has; the commit comes back to everyone still in (as DAVE's gateway announces it)
static Buf commitAndAnnounce(MlsGroup& by, std::vector<MlsGroup*> others) {
    static uint8_t commit[64 * 1024], welcome[64 * 1024];
    Writer c(commit, sizeof(commit)), w(welcome, sizeof(welcome));
    bool has_welcome = false;
    MlsResult r = by.commit(testRandom, c, w, &has_welcome);
    if (r != Mls_Ok) printf("  commit: %s at mlsgroup.cpp:%d\n", RESULT[r], by.whyLine());
    CHECK(r == Mls_Ok);
    for (MlsGroup* o : others) {
        r = o->handle(Bytes{commit, c.n});
        if (r != Mls_Ok && r != Mls_Removed) printf("  someone's handling: %s at mlsgroup.cpp:%d\n", RESULT[r], o->whyLine());
    }
    r = by.handle(Bytes{commit, c.n});
    if (r != Mls_Ok) printf("  own commit back: %s at mlsgroup.cpp:%d\n", RESULT[r], by.whyLine());
    CHECK(r == Mls_Ok);
    return has_welcome ? Buf(welcome, welcome + w.n) : Buf();
}

// Everyone in the group agrees on the epoch, its authenticator, and every speaker's DAVE key
static void sameKeys(std::vector<MlsGroup*> members, std::vector<uint64_t> users) {
    for (MlsGroup* m : members) {
        CHECK(m->joined() && m->epoch() == members[0]->epoch());
        CHECK(!memcmp(m->epochAuthenticator(), members[0]->epochAuthenticator(), MLS_NH));
        for (uint64_t u : users) {
            uint8_t ctx[8], a[16], b[16];
            for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(u >> (8 * i));  // (little-endian, as DAVE's exporter context)
            const char* label = "Discord Secure Frames v0";
            CHECK(m->exporter(label, strlen(label), ctx, 8, a, 16) && members[0]->exporter(label, strlen(label), ctx, 8, b, 16) &&
                  !memcmp(a, b, 16));
        }
    }
}

static MlsGroup g_alice, g_bob, g_carol;

static void ownCommits() {
    g_case = "own commits";
    srand(42);
    do testRandom(g_gw_priv, sizeof(g_gw_priv));
    while (!p256ValidPrivate(g_gw_priv));
    p256Public(g_gw_priv, g_gw_pub);
    // the group context's external_senders extension: the gateway's key and a basic credential
    static uint8_t ext[256];
    Writer e(ext, sizeof(ext));
    e.u16(5);
    size_t data = e.begin();
    size_t list = e.begin();
    e.vec(g_gw_pub, P256_PUB);
    e.u16(1);
    e.vec("gateway", 7);
    e.end(list);
    e.end(data);
    Buf group_id = {'c', 'h', 'a', 'n', 'n', 'e', 'l'};

    // Alice alone; the gateway adds Bob; Alice commits; Bob joins from her Welcome
    g_alice.reset(), g_bob.reset(), g_carol.reset();
    makeKp(g_alice, 1001);
    CHECK(g_alice.create(Bytes{group_id.data(), group_id.size()}, Bytes{ext, e.n}, testRandom));
    Buf bob_kp = makeKp(g_bob, 1002);
    Buf p = gatewayProposal(g_alice, group_id, &bob_kp, 0);
    CHECK(g_alice.handle(Bytes{p.data(), p.size()}) == Mls_Ok);
    Buf welcome = commitAndAnnounce(g_alice, {});
    CHECK(!welcome.empty());
    MlsResult r = g_bob.join(Bytes{welcome.data(), welcome.size()});
    if (r != Mls_Ok) printf("  bob joins: %s at mlsgroup.cpp:%d\n", RESULT[r], g_bob.whyLine());
    CHECK(r == Mls_Ok && g_alice.epoch() == 1);
    sameKeys({&g_alice, &g_bob}, {1001, 1002});

    // the gateway adds Carol; this time Bob commits (Alice has to open his path)
    Buf carol_kp = makeKp(g_carol, 1003);
    p = gatewayProposal(g_alice, group_id, &carol_kp, 0);
    CHECK(g_alice.handle(Bytes{p.data(), p.size()}) == Mls_Ok && g_bob.handle(Bytes{p.data(), p.size()}) == Mls_Ok);
    welcome = commitAndAnnounce(g_bob, {&g_alice});
    r = g_carol.join(Bytes{welcome.data(), welcome.size()});
    if (r != Mls_Ok) printf("  carol joins: %s at mlsgroup.cpp:%d\n", RESULT[r], g_carol.whyLine());
    CHECK(r == Mls_Ok);
    sameKeys({&g_alice, &g_bob, &g_carol}, {1001, 1002, 1003});

    // Alice leaves (the gateway removes her leaf, 0); Carol commits; Bob follows, Alice is out
    p = gatewayProposal(g_bob, group_id, nullptr, 0);
    for (MlsGroup* g : {&g_alice, &g_bob, &g_carol}) CHECK(g->handle(Bytes{p.data(), p.size()}) == Mls_Ok);
    static uint8_t commit[64 * 1024], wel[64 * 1024];
    Writer c(commit, sizeof(commit)), w(wel, sizeof(wel));
    bool has_welcome = true;
    CHECK(g_carol.commit(testRandom, c, w, &has_welcome) == Mls_Ok && !has_welcome);
    CHECK(g_alice.handle(Bytes{commit, c.n}) == Mls_Removed);
    CHECK(g_bob.handle(Bytes{commit, c.n}) == Mls_Ok);
    CHECK(g_carol.handle(Bytes{commit, c.n}) == Mls_Ok);
    sameKeys({&g_bob, &g_carol}, {1002, 1003});
    CHECK(g_bob.epoch() == 3);

    // a commit that isn't ours, with our leaf's name on it, is refused; a stale one too
    CHECK(g_bob.handle(Bytes{commit, c.n}) == Mls_WrongGroup);
}

int main(int argc, char** argv) {
    wire();
    ownCommits();
    FILE* f = fopen(argc > 1 ? argv[1] : "out/mls_vectors.txt", "r");
    if (!f) {
        printf("FAIL: no vectors (run test/mls_vectors.js first)\n");
        return 1;
    }
    std::vector<Case> cases;
    std::vector<std::string> kinds;
    static char line[1 << 20];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("=== ", 0) == 0) {
            cases.emplace_back();
            kinds.push_back(s.substr(4));
            continue;
        }
        size_t sp = s.find(' ');
        if (sp != std::string::npos && !cases.empty()) cases.back()[s.substr(0, sp)] = s.substr(sp + 1);
    }
    fclose(f);
    int counts[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < cases.size(); i++) {
        g_case = kinds[i];
        if (kinds[i].rfind("crypto-basics", 0) == 0) cryptoBasics(cases[i]), counts[0]++;
        else if (kinds[i].rfind("tree-math", 0) == 0) treeMath(cases[i]), counts[1]++;
        else if (kinds[i].rfind("key-schedule", 0) == 0) keySchedule(cases[i]), counts[2]++;
        else if (kinds[i].rfind("tree-validation", 0) == 0) treeValidation(cases[i]), counts[3]++;
        else if (kinds[i].rfind("passive-client", 0) == 0) passiveClient(cases[i]);
    }
    printf("passive client: %d followed to the end, %d stopped at what DAVE doesn't use (PSKs)\n", g_passive_ok, g_passive_skipped);
    printf("crypto-basics %d, tree-math %d, key-schedule %d, tree-validation %d cases\n", counts[0], counts[1], counts[2], counts[3]);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
