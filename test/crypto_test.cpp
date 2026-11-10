// Chirp's crypto against published vectors (the standards' own), then against
// OpenSSL on random inputs (test/crypto_vectors.js writes those first).
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "aes.h"
#include "chacha.h"
#include "daveframe.h"
#include "hpke.h"
#include "p256.h"
#include "rtp.h"
#include "sha256.h"

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
    return s;
}
static std::string hex(const Buf& b) { return hex(b.data(), b.size()); }

static void published() {
    uint8_t d[32];
    // FIPS 180-2 examples
    sha256("abc", 3, d);
    CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("", 0, d);
    CHECK(hex(d, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d);
    CHECK(hex(d, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 s;
        s.init();
        std::string a(1000, 'a');
        for (int i = 0; i < 1000; i++) s.update(a.data(), a.size());  // (a million a's, in parts)
        s.final(d);
        CHECK(hex(d, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    // RFC 4231 test cases 1 and 2
    Buf k1(20, 0x0b);
    hmacSha256(k1.data(), k1.size(), "Hi There", 8, d);
    CHECK(hex(d, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmacSha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    CHECK(hex(d, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 5869 test case 1
    {
        Buf ikm(22, 0x0b), salt = unhex("000102030405060708090a0b0c"), info = unhex("f0f1f2f3f4f5f6f7f8f9");
        uint8_t prk[32], okm[42];
        hkdfExtract(salt.data(), salt.size(), ikm.data(), ikm.size(), prk);
        CHECK(hex(prk, 32) == "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
        CHECK(hkdfExpand(prk, info.data(), info.size(), okm, sizeof(okm)));
        CHECK(hex(okm, 42) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    }
    // FIPS 197 appendix C.1 and C.3
    {
        Buf pt = unhex("00112233445566778899aabbccddeeff"), k = unhex("000102030405060708090a0b0c0d0e0f");
        Aes a;
        uint8_t ct[16];
        CHECK(a.init(k.data(), 16));
        a.encrypt(pt.data(), ct);
        CHECK(hex(ct, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
        Buf k256 = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        CHECK(a.init(k256.data(), 32));
        a.encrypt(pt.data(), ct);
        CHECK(hex(ct, 16) == "8ea2b7ca516745bfeafc49904b496089");
    }
    // The GCM spec's test cases 2 and 14 (all-zero key, IV and block; AES-128 and AES-256)
    {
        uint8_t z[32] = {0}, ct[16], tag[16];
        Gcm g;
        g.init(z, 16);
        g.seal(z, nullptr, 0, z, 16, ct, tag, 16);
        CHECK(hex(ct, 16) == "0388dace60b6a392f328c2b971b2fe78" && hex(tag, 16) == "ab6e47d42cec13bdf53a67b21257bddf");
        g.init(z, 32);
        g.seal(z, nullptr, 0, z, 16, ct, tag, 16);
        CHECK(hex(ct, 16) == "cea7403d4d606b6e074ec5d3baf39d18" && hex(tag, 16) == "d0d1c8a799996bf0265b98b5d48ab919");
        // a wrong tag, or a flipped bit, doesn't open; a cut tag (as DAVE's 8 bytes) does
        uint8_t back[16];
        CHECK(g.open(z, nullptr, 0, ct, 16, tag, 16, back) && !memcmp(back, z, 16));
        CHECK(g.open(z, nullptr, 0, ct, 16, tag, 8, back));
        tag[3] ^= 1;
        CHECK(!g.open(z, nullptr, 0, ct, 16, tag, 16, back));
    }
    // RFC 8439 2.5.2 (Poly1305) and the XChaCha draft's HChaCha20 (2.2.1) and AEAD (A.3.1)
    {
        Buf k = unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        uint8_t tag[16];
        poly1305(k.data(), (const uint8_t*)"Cryptographic Forum Research Group", 34, tag);
        CHECK(hex(tag, 16) == "a8061dc1305136c6c22b8baf0c0127a9");
        Buf hk = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"), hn = unhex("000000090000004a0000000031415927");
        uint8_t sub[32];
        hchacha20(hk.data(), hn.data(), sub);
        CHECK(hex(sub, 32) == "82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc");
        Buf xk = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        Buf xn = unhex("404142434445464748494a4b4c4d4e4f5051525354555657"), aad = unhex("50515253c0c1c2c3c4c5c6c7");
        const char* text =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        size_t n = strlen(text);
        std::vector<uint8_t> ct(n);
        xchachaSeal(xk.data(), xn.data(), aad.data(), aad.size(), (const uint8_t*)text, n, ct.data(), tag);
        CHECK(hex(ct.data(), 16) == "bd6d179d3e83d43b9576579493c0e939");
        CHECK(hex(tag, 16) == "c0875924c1c7987947deafd8780acf49");
    }
}

// RFC 6979 A.2.5: P-256, SHA-256, the message "sample" (the nonce is the RFC's, so r and s are exact)
static void rfc6979() {
    Buf d = unhex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
    uint8_t pub[P256_PUB], hash[32], r[32], s[32];
    CHECK(p256Public(d.data(), pub));
    CHECK(hex(pub + 1, 32) == "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6");
    CHECK(hex(pub + 33, 32) == "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
    sha256("sample", 6, hash);
    CHECK(p256SignRaw(d.data(), hash, r, s));
    CHECK(hex(r, 32) == "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716");
    CHECK(hex(s, 32) == "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");
    uint8_t sig[P256_SIG_MAX];
    size_t n = 0;
    CHECK(p256Sign(d.data(), "sample", 6, sig, &n));
    CHECK(p256Verify(pub, "sample", 6, sig, n));
    CHECK(!p256Verify(pub, "samplf", 6, sig, n));
    // keys that aren't: zero, n itself, a point off the curve
    uint8_t zero[32] = {0};
    CHECK(!p256ValidPrivate(zero));
    Buf order = unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    CHECK(!p256ValidPrivate(order.data()));
    pub[64] ^= 1;
    CHECK(!p256ValidPublic(pub));
}

// RFC 9180 A.3.1: DHKEM(P-256, HKDF-SHA256), HKDF-SHA256, AES-128-GCM, base mode
static void rfc9180() {
    Buf info = unhex("4f6465206f6e2061204772656369616e2055726e");
    Buf ikmE = unhex("4270e54ffd08d79d5928020af4686d8f6b7d35dbe470265f1f5aa22816ce860e");
    Buf ikmR = unhex("668b37171f1072f3cf12ea8a236a45df23fc13b82af3609ad1e354f6ef817550");
    uint8_t sk[P256_PRIV], pk[P256_PUB];
    CHECK(hpkeDeriveKeyPair(ikmE.data(), ikmE.size(), sk, pk));
    CHECK(hex(sk, 32) == "4995788ef4b9d6132b249ce59a77281493eb39af373d236a1fe415cb0c2d7beb");
    CHECK(hex(pk, 65) ==
          "04a92719c6195d5085104f469a8b9814d5838ff72b60501e2c4466e5e67b325ac98536d7b61a1af4b78e5b7f951c0900be863c403ce65c9bfcb9382657222d18c4");
    uint8_t skR[P256_PRIV], pkR[P256_PUB];
    CHECK(hpkeDeriveKeyPair(ikmR.data(), ikmR.size(), skR, pkR));
    CHECK(hex(skR, 32) == "f3ce7fdae57e1a310d87f1ebbde6f328be0a99cdbcadf4d6589cf29de4b8ffd2");
    CHECK(hex(pkR, 65) ==
          "04fe8c19ce0905191ebc298a9245792531f26f0cece2460639e8bc39cb7f706a826a779b4cf969b8a0e539c7f62fb3d30ad6aa8f80e30f1d128aafd68a2ce72ea0");

    HpkeContext s, r;
    uint8_t enc[HPKE_ENC], ss[32], ssR[32];
    CHECK(hpkeSetupBaseS(pkR, info.data(), info.size(), ikmE.data(), enc, &s, ss));
    CHECK(hex(enc, 65) == hex(pk, 65));
    CHECK(hex(ss, 32) == "c0d26aeab536609a572b07695d933b589dcf363ff9d93c93adea537aeabb8cb8");
    CHECK(hex(s.key, 16) == "868c066ef58aae6dc589b6cfdd18f97e");
    CHECK(hex(s.base_nonce, 12) == "4e0bc5018beba4bf004cca59");
    CHECK(hex(s.exporter_secret, 32) == "14ad94af484a7ad3ef40e9f3be99ecc6fa9036df9d4920548424df127ee0d99f");
    CHECK(hpkeSetupBaseR(enc, skR, info.data(), info.size(), &r, ssR));
    CHECK(!memcmp(ss, ssR, 32));

    Buf pt = unhex("4265617574792069732074727574682c20747275746820626561757479");
    const char* want[2] = {"5ad590bb8baa577f8619db35a36311226a896e7342a6d836d8b7bcd2f20b6c7f9076ac232e3ab2523f39513434",
                           "fa6f037b47fc21826b610172ca9637e82d6e5801eb31cbd3748271affd4ecb06646e0329cbdf3c3cd655b28e82"};
    for (int seq = 0; seq < 2; seq++) {
        Buf aad = unhex(seq == 0 ? "436f756e742d30" : "436f756e742d31");
        Buf ct(pt.size() + 16), back(pt.size());
        s.seal(aad.data(), aad.size(), pt.data(), pt.size(), ct.data());
        CHECK(hex(ct) == want[seq]);
        CHECK(r.open(aad.data(), aad.size(), ct.data(), ct.size(), back.data()) && back == pt);
    }
    uint8_t exported[32];
    CHECK(s.exportSecret(nullptr, 0, exported, 32));
    CHECK(hex(exported, 32) == "5e9bc3d236e1911d95e65b576a8a86d478fb827e8bdfe77b741b289890490d4d");
}

static FILE* g_sigs;  // Chirp's signatures, for OpenSSL to check after (crypto_vectors.js verify)

static void againstOpenssl(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("FAIL: no %s (run test/crypto_vectors.js first)\n", path);
        g_fail++;
        return;
    }
    char line[20000];
    int cases = 0;
    while (fgets(line, sizeof(line), f)) {
        std::vector<std::string> w;
        for (char* t = strtok(line, " \r\n"); t; t = strtok(nullptr, " \r\n")) w.push_back(t);
        if (w.empty()) continue;
        cases++;
        const std::string& kind = w[0];
        if (kind == "sha256") {
            Buf m = unhex(w[1]);
            uint8_t d[32];
            sha256(m.data(), m.size(), d);
            CHECK(hex(d, 32) == w[2]);
        } else if (kind == "hmac") {
            Buf k = unhex(w[1]), m = unhex(w[2]);
            uint8_t d[32];
            hmacSha256(k.data(), k.size(), m.data(), m.size(), d);
            CHECK(hex(d, 32) == w[3]);
        } else if (kind == "hkdf") {
            Buf salt = unhex(w[1]), ikm = unhex(w[2]), info = unhex(w[3]);
            size_t len = (size_t)std::stoi(w[4]);
            uint8_t prk[32];
            Buf okm(len);
            hkdfExtract(salt.data(), salt.size(), ikm.data(), ikm.size(), prk);
            hkdfExpand(prk, info.data(), info.size(), okm.data(), len);
            CHECK(hex(okm) == w[5]);
        } else if (kind == "gcm" || kind == "chachapoly" || kind == "xchachapoly") {
            Buf k = unhex(w[1]), iv = unhex(w[2]), aad = unhex(w[3]), pt = unhex(w[4]), want = unhex(w[5]);
            Buf ct(pt.size()), back(pt.size());
            uint8_t tag[16];
            bool opened;
            if (kind == "gcm") {
                Gcm g;
                g.init(k.data(), k.size());
                g.seal(iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), ct.data(), tag, 16);
                opened = g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), tag, 16, back.data());
            } else if (kind == "chachapoly") {
                chachaSeal(k.data(), iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), ct.data(), tag);
                opened = chachaOpen(k.data(), iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), tag, back.data());
            } else {
                xchachaSeal(k.data(), iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), ct.data(), tag);
                opened = xchachaOpen(k.data(), iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), tag, back.data());
            }
            CHECK(hex(ct) == w[5] || (ct.empty() && w[5] == "-"));
            CHECK(hex(tag, 16) == w[6]);
            CHECK(opened && back == pt);
        } else if (kind == "p256pub") {
            Buf d = unhex(w[1]);
            uint8_t pub[P256_PUB];
            CHECK(p256Public(d.data(), pub) && hex(pub, P256_PUB) == w[2]);
        } else if (kind == "ecdh") {
            Buf d = unhex(w[1]), pub = unhex(w[2]);
            uint8_t secret[32];
            CHECK(p256Ecdh(d.data(), pub.data(), secret) && hex(secret, 32) == w[3]);
            // and Chirp's own signature with that private key (over the other key's bytes), for OpenSSL
            uint8_t mine[P256_PUB], sig[P256_SIG_MAX];
            size_t n = 0;
            p256Public(d.data(), mine);
            CHECK(p256Sign(d.data(), pub.data(), pub.size(), sig, &n));
            if (g_sigs) fprintf(g_sigs, "%s %s %s\n", hex(mine, P256_PUB).c_str(), hex(pub).c_str(), hex(sig, n).c_str());
        } else if (kind == "ecdsa") {
            Buf pub = unhex(w[1]), msg = unhex(w[2]), sig = unhex(w[3]);
            CHECK(p256Verify(pub.data(), msg.data(), msg.size(), sig.data(), sig.size()));
            Buf bad = msg;
            bad.push_back(0);
            CHECK(!p256Verify(pub.data(), bad.data(), bad.size(), sig.data(), sig.size()));
            Buf badSig = sig;
            badSig[badSig.size() - 1] ^= 0x40;
            CHECK(!p256Verify(pub.data(), msg.data(), msg.size(), badSig.data(), badSig.size()));
        } else if (kind == "rtp") {
            RtpMode mode = w[1] == "aes" ? Rtp_Aes256Gcm : Rtp_XChaCha;
            Buf key = unhex(w[2]), opus = unhex(w[6]), pkt = unhex(w[7]);
            Buf out(pkt.size());
            RtpPacket p;
            CHECK(rtpOpen(mode, key.data(), pkt.data(), pkt.size(), out.data(), &p));
            CHECK(p.seq == std::stoul(w[3]) && p.timestamp == std::stoul(w[4]) && p.ssrc == std::stoul(w[5]));
            CHECK(p.payload_n == opus.size() && !memcmp(p.payload, opus.data(), opus.size()));
            // (a plain header: ours, sealed with the same counter, is the same packet)
            uint32_t counter = (uint32_t)std::stoul(w[8]);
            if (counter) {
                Buf mine(opus.size() + RTP_OVERHEAD);
                uint32_t c = counter - 1;
                size_t n = rtpSeal(mode, key.data(), &c, p.seq, p.timestamp, p.ssrc, opus.data(), opus.size(), mine.data());
                CHECK(n == pkt.size() && !memcmp(mine.data(), pkt.data(), n));
            }
            pkt[pkt.size() - 10] ^= 1;  // (in the tag)
            CHECK(!rtpOpen(mode, key.data(), pkt.data(), pkt.size(), out.data(), &p));
        } else if (kind == "dave") {
            // a frame sealed as libdave does: it opens, and ours sealed with the same counter is the same bytes
            Buf base = unhex(w[1]), opus = unhex(w[3]), frame = unhex(w[4]);
            uint32_t counter = (uint32_t)std::stoul(w[2]);
            static DaveRatchet r;
            r.init(7, base.data());
            Buf back(frame.size());
            size_t back_n = 0;
            CHECK(daveDecrypt(r, frame.data(), frame.size(), back.data(), &back_n) && back_n == opus.size() &&
                  !memcmp(back.data(), opus.data(), back_n));
            r.init(7, base.data());
            uint32_t c = counter - 1;
            Buf mine(opus.size() + DAVE_SUPPLEMENTAL_MAX);
            size_t mine_n = 0;
            CHECK(daveEncrypt(r, &c, opus.data(), opus.size(), mine.data(), mine.size(), &mine_n) && mine_n == frame.size() &&
                  !memcmp(mine.data(), frame.data(), mine_n));
            frame[0] ^= 1;
            r.init(7, base.data());
            CHECK(!daveDecrypt(r, frame.data(), frame.size(), back.data(), &back_n));
        } else {
            printf("FAIL: unknown case %s\n", kind.c_str());
            g_fail++;
        }
    }
    fclose(f);
    printf("%d cases from OpenSSL\n", cases);
}

// GCM in pieces against GCM in one go: random keys, data and splits; and back again
static void gcmStream() {
    uint32_t s = 99;
    auto rnd = [&]() { return s = s * 1664525 + 1013904223; };
    int same = 0, opened = 0, rounds = 200;
    for (int r = 0; r < rounds; r++) {
        uint8_t key[16], iv[12], aad[300], pt[700], ct1[700], buf[700], tag1[16], tag2[16];
        for (auto& b : key) b = (uint8_t)rnd();
        for (auto& b : iv) b = (uint8_t)rnd();
        size_t an = rnd() % 300, n = rnd() % 700;
        for (size_t i = 0; i < an; i++) aad[i] = (uint8_t)rnd();
        for (size_t i = 0; i < n; i++) pt[i] = (uint8_t)rnd();
        Gcm g;
        g.init(key, 16);
        g.seal(iv, aad, an, pt, n, ct1, tag1, 16);
        // the same, in pieces of random sizes
        GcmStream st;
        st.start(g, iv);
        for (size_t at = 0; at < an;) {
            size_t k = 1 + rnd() % 40;
            k = k < an - at ? k : an - at;
            st.aad(aad + at, k);
            at += k;
        }
        memcpy(buf, pt, n);
        for (size_t at = 0; at < n;) {
            size_t k = 1 + rnd() % 60;
            k = k < n - at ? k : n - at;
            st.encrypt(buf + at, k);
            at += k;
        }
        st.finish(tag2, 16);
        same += !memcmp(buf, ct1, n) && !memcmp(tag1, tag2, 16);
        // and back
        st.start(g, iv);
        st.aad(aad, an);
        st.decrypt(buf, n);
        st.finish(tag2, 16);
        opened += !memcmp(buf, pt, n) && !memcmp(tag1, tag2, 16);
    }
    CHECK(same == rounds);
    CHECK(opened == rounds);
}

// DAVE's H.264 frames: what stays readable, the trailer, and decryption from the trailer alone
static void daveH264() {
    uint32_t s = 7;
    auto rnd = [&]() { return s = s * 1664525 + 1013904223; };
    auto body = [&](std::vector<uint8_t>& v, size_t n) {  // (no zero bytes: no start code by chance)
        for (size_t i = 0; i < n; i++) v.push_back((uint8_t)(1 + rnd() % 255));
    };
    std::vector<uint8_t> f;
    f.insert(f.end(), {0, 0, 0, 1, 0x67});  // SPS (4-byte start code)
    body(f, 20);
    f.insert(f.end(), {0, 0, 1, 0x68});  // PPS (3-byte start code: made 4)
    body(f, 6);
    // an IDR slice: first_mb_in_slice 0 ("1"), slice_type 7 ("0001000"), pps_id 0 ("1"): 9 bits, 2 bytes
    f.insert(f.end(), {0, 0, 0, 1, 0x65, 0x88, 0x80 | 0x7f});
    body(f, 3000);
    f.insert(f.end(), {0, 0, 1, 0x41, 0x9a});  // a P slice (3-byte start code), its header: first_mb "1", type "1": in byte 1
    body(f, 1500);
    std::vector<uint8_t> want;  // (the frame with 4-byte start codes, as it should decrypt to)
    for (size_t i = 0; i < f.size(); i++) {
        if (i + 2 < f.size() && f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1 && (i == 0 || f[i - 1] != 0)) want.push_back(0);
        want.push_back(f[i]);
    }
    uint8_t base[DAVE_KEY];
    for (auto& b : base) b = (uint8_t)rnd();
    static DaveRatchet r;
    r.init(1234, base);
    uint32_t counter = 0;
    std::vector<uint8_t> buf(f.size() + 1024);
    memcpy(buf.data(), f.data(), f.size());
    size_t n = 0;
    CHECK(daveEncryptH264(r, &counter, buf.data(), f.size(), buf.size(), &n));
    buf.resize(n);
    // the trailer: [frame][tag 8][ULEB nonce][ULEB pairs][size][FA FA]
    CHECK(n > 3 && buf[n - 1] == 0xFA && buf[n - 2] == 0xFA);
    size_t supp = buf[n - 3], frame_n = n - supp;
    CHECK(frame_n == want.size());
    size_t p = frame_n + DAVE_TAG;
    auto uleb = [&]() {
        uint32_t v = 0;
        for (int sh = 0;; sh += 7) {
            uint8_t b = buf[p++];
            v |= (uint32_t)(b & 0x7f) << sh;
            if (!(b & 0x80)) return v;
        }
    };
    uint32_t nonce = uleb();
    std::vector<std::pair<size_t, size_t>> ranges;
    while (p < n - 3) {
        size_t o = uleb(), z = uleb();
        ranges.push_back({o, z});
    }
    CHECK(nonce == counter);
    // SPS and PPS whole and joined to the IDR's start code, header and 2 slice-header bytes: one range;
    // then the P slice's start code, header, and its one byte
    size_t idr = 4 + 1 + 20 + 4 + 1 + 6;
    size_t pslice = idr + 4 + 1 + 2 + 3000;
    CHECK(ranges.size() == 2);
    CHECK(ranges.size() == 2 && ranges[0].first == 0 && ranges[0].second == idr + 4 + 1 + 2);
    CHECK(ranges.size() == 2 && ranges[1].first == pslice && ranges[1].second == 4 + 1 + 1);
    // decrypt from the trailer's ranges alone
    uint8_t iv[12] = {0};
    for (int i = 0; i < 4; i++) iv[8 + i] = (uint8_t)(nonce >> (8 * i));
    GcmStream st;
    st.start(*r.forGeneration(nonce >> 24), iv);
    for (auto& rg : ranges) st.aad(buf.data() + rg.first, rg.second);
    size_t pos = 0;
    for (size_t i = 0; i <= ranges.size(); i++) {
        size_t stop = i < ranges.size() ? ranges[i].first : frame_n;
        if (stop > pos) st.decrypt(buf.data() + pos, stop - pos);
        if (i < ranges.size()) pos = ranges[i].first + ranges[i].second;
    }
    uint8_t tag[DAVE_TAG];
    st.finish(tag, DAVE_TAG);
    CHECK(!memcmp(tag, buf.data() + frame_n, DAVE_TAG));
    CHECK(!memcmp(buf.data(), want.data(), frame_n));
    // not a frame (no start code first): refused
    uint8_t junk[16] = {1, 2, 3};
    CHECK(!daveEncryptH264(r, &counter, junk, 3, sizeof(junk), &n));
}

// The H.264 packetizer: what a receiver rebuilds from its payloads is the frame, NAL by NAL
static void h264Packets() {
    uint32_t s = 11;
    auto rnd = [&]() { return s = s * 1664525 + 1013904223; };
    std::vector<uint8_t> f;
    std::vector<std::vector<uint8_t>> nals;
    const size_t sizes[] = {12, 5, 2600, 1100, 1101, 7};
    for (size_t sz : sizes) {
        std::vector<uint8_t> nal = {(uint8_t)(0x60 | (1 + rnd() % 8))};
        for (size_t i = 1; i < sz; i++) nal.push_back((uint8_t)(1 + rnd() % 255));
        f.insert(f.end(), {0, 0, 0, 1});
        f.insert(f.end(), nal.begin(), nal.end());
        nals.push_back(nal);
    }
    const size_t MAX = 1100;
    uint8_t scratch[MAX];
    std::vector<std::vector<uint8_t>> got;
    std::vector<uint8_t> fu;
    int packets = 0, marked = 0, too_big = 0;
    bool last_marked = false;
    rtpH264Payloads(f.data(), f.size(), MAX, scratch, [&](const uint8_t* p, size_t n, bool last) {
        packets++;
        marked += last;
        last_marked = last;
        too_big += n > MAX;
        if ((p[0] & 0x1f) == 28) {  // FU-A: the NAL header from the indicator's F/NRI and the FU header's type
            if (p[1] & 0x80) fu = {(uint8_t)((p[0] & 0xE0) | (p[1] & 0x1F))};
            fu.insert(fu.end(), p + 2, p + n);
            if (p[1] & 0x40) got.push_back(fu);
        } else
            got.push_back(std::vector<uint8_t>(p, p + n));
    });
    CHECK(got == nals);
    CHECK(marked == 1 && last_marked && too_big == 0);
    CHECK(packets == 1 + 1 + 3 + 1 + 2 + 1);  // (2600: 3 fragments; 1100 fits; 1101: 2)
}

// A keyframe without SPS and PPS gets the last ones seen in front; other frames are left as they are
static void h264Params() {
    H264Params p;
    bool idr = false;
    uint8_t buf[256];
    {  // (before any are seen: grc's own)
        H264Params fresh;
        const uint8_t key[] = {0, 0, 0, 1, 0x65, 0x88};
        memcpy(buf, key, sizeof(key));
        size_t n = rtpH264WithParams(buf, sizeof(key), sizeof(buf), &fresh, &idr);
        CHECK(idr && n > sizeof(key) + 4 + 16 + 4 + 4 && buf[4] == 0x67);
    }
    {  // grc's SPS gets the restriction WebRTC would otherwise put in; then it's left alone
        const uint8_t grc[] = {0x67, 0x64, 0x0c, 0x20, 0xac, 0x2b, 0x40, 0x28, 0x02, 0xdd, 0x35, 0x01, 0x0d, 0x01, 0xe0, 0x80};
        uint8_t a[64], b2[64];
        size_t an = h264FixSps(grc, sizeof(grc), a, sizeof(a));
        CHECK(an > sizeof(grc) && !memcmp(a, grc, 14));
        size_t bn = h264FixSps(a, an, b2, sizeof(b2));
        CHECK(bn == an && !memcmp(a, b2, an));
        // the full-range flag: set, it differs from the fixed SPS in that one bit (grc's VUI has a video signal
        // type: its flag is bit 4 of the video signal type's first byte... found by comparing); set back, as before
        uint8_t full[64], back[64];
        size_t fn = h264FixSps(a, an, full, sizeof(full), 1);
        int bits = 0;
        for (size_t i = 0; i < an && fn == an; i++) bits += __builtin_popcount(a[i] ^ full[i]);
        CHECK(fn == an && bits == 1);
        size_t kn = h264FixSps(full, fn, back, sizeof(back), 0);
        CHECK(kn == an && !memcmp(back, a, an));
        CHECK(h264FixSps(grc, sizeof(grc), back, sizeof(back), 1) == an && !memcmp(back, full, an));  // (raw grc: both at once)
        printf("  grc SPS fixed:");
        for (size_t i = 0; i < an; i++) printf(" %02x", a[i]);
        printf("\n");
        const uint8_t novui[] = {0x67, 0x64, 0x0c, 0x20, 0xac, 0x2b, 0x40, 0x28, 0x02, 0xdc, 0x80};  // (grc's without its VUI)
        size_t cn = h264FixSps(novui, sizeof(novui), a, sizeof(a));
        CHECK(cn > sizeof(novui));
        CHECK(h264FixSps(a, cn, b2, sizeof(b2)) == cn);
        printf("  no-VUI SPS fixed:");
        for (size_t i = 0; i < cn; i++) printf(" %02x", a[i]);
        printf("\n");
    }
    {  // the grey placeholder: SPS, PPS 1, an IDR slice; written out for test/check_grey.py
        H264Params fresh;
        static uint8_t g[H264_PLACEHOLDER_MAX];
        size_t gn = h264Placeholder(&fresh, 0, g, sizeof(g));
        CHECK(gn > 3600 && gn < sizeof(g));
        if (FILE* f = fopen("out/grey.h264", "wb")) {
            fwrite(g, 1, gn, f);
            fclose(f);
        }
    }
    const uint8_t first[] = {0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    memcpy(buf, first, sizeof(first));
    CHECK(rtpH264WithParams(buf, sizeof(first), sizeof(buf), &p, &idr) == sizeof(first) && idr);  // (it has its own)
    CHECK(p.sps_n == 4 && p.pps_n == 4 && p.sps[0] == 0x67 && p.pps[0] == 0x68);
    const uint8_t slice[] = {0, 0, 0, 1, 0x41, 0x9a, 0x11, 0x22};
    memcpy(buf, slice, sizeof(slice));
    CHECK(rtpH264WithParams(buf, sizeof(slice), sizeof(buf), &p, &idr) == sizeof(slice) && !idr);  // (not a keyframe)
    const uint8_t key[] = {0, 0, 1, 0x65, 0x88, 0x84, 0x99};  // (bare, a 3-byte start code)
    memcpy(buf, key, sizeof(key));
    size_t n = rtpH264WithParams(buf, sizeof(key), sizeof(buf), &p, &idr);
    const uint8_t want[] = {0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 1, 0x65, 0x88, 0x84, 0x99};
    CHECK(idr && n == sizeof(want) && !memcmp(buf, want, n));
    // and the result still packetizes into the three NALs
    uint8_t scratch[64];
    int nals = 0;
    rtpH264Payloads(buf, n, sizeof(scratch), scratch, [&](const uint8_t*, size_t, bool) { nals++; });
    CHECK(nals == 3);
}

// GHASH by carry-less multiply (the Switch's PMULL way, here with a software multiply) against the table
// way, for random keys and blocks; and the self-test says there's no fast way on the PC
static void ghashClmul() {
    uint32_t seed = 12345;
    auto rnd = [&]() { return (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16); };
    int same = 0;
    for (int k = 0; k < 200; k++) {
        uint8_t key[32];
        for (uint8_t& b : key) b = rnd();
        Gcm g;
        g.init(key, k & 1 ? 32 : 16);
        for (int n = 0; n < 10; n++) {
            uint8_t y[16], z[16];
            for (uint8_t& b : y) b = rnd();
            if (n == 0) memset(y, 0, 16), y[0] = 0x80;  // (x^0: y*H is H)
            memcpy(z, y, 16);
            g.mulHClmulSoft(y);
            g.mulHTable(z);
            same += !memcmp(y, z, 16);
        }
    }
    CHECK(same == 2000);
    const char* what = nullptr;
    CHECK(!aesSelfTest(&what) && what);
}

int main(int argc, char** argv) {
    ghashClmul();
    h264Params();
    gcmStream();
    h264Packets();
    daveH264();
    published();
    rfc6979();
    rfc9180();
    g_sigs = fopen("out/chirp_sigs.txt", "w");
    againstOpenssl(argc > 1 ? argv[1] : "out/crypto_vectors.txt");
    if (g_sigs) fclose(g_sigs);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
