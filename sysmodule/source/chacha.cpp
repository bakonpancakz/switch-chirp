#include "chacha.h"

#include <string.h>

#include "sha256.h"

namespace {

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

#define QR(a, b, c, d) \
    a += b, d ^= a, d = rol(d, 16), c += d, b ^= c, b = rol(b, 12), a += b, d ^= a, d = rol(d, 8), c += d, b ^= c, b = rol(b, 7)

void rounds(uint32_t x[16]) {
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]), QR(x[1], x[5], x[9], x[13]), QR(x[2], x[6], x[10], x[14]), QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]), QR(x[1], x[6], x[11], x[12]), QR(x[2], x[7], x[8], x[13]), QR(x[3], x[4], x[9], x[14]);
    }
}

void setup(uint32_t s[16], const uint8_t key[32], const uint8_t* tail16) {
    s[0] = 0x61707865, s[1] = 0x3320646e, s[2] = 0x79622d32, s[3] = 0x6b206574;  // "expand 32-byte k"
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + i * 4);
    for (int i = 0; i < 4; i++) s[12 + i] = le32(tail16 + i * 4);
}

}  // namespace

void chacha20(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, size_t n, uint8_t* out) {
    uint8_t tail[16];
    put32(tail, counter);
    memcpy(tail + 4, nonce, 12);
    uint32_t s[16], x[16];
    setup(s, key, tail);
    uint8_t ks[64];
    while (n) {
        memcpy(x, s, sizeof(x));
        rounds(x);
        for (int i = 0; i < 16; i++) put32(ks + i * 4, x[i] + s[i]);
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += take, out += take, n -= take;
        s[12]++;
    }
    wipe(s, sizeof(s));
    wipe(x, sizeof(x));
    wipe(ks, sizeof(ks));
}

void hchacha20(const uint8_t key[CHACHA_KEY], const uint8_t nonce[16], uint8_t out[32]) {
    uint32_t x[16];
    setup(x, key, nonce);
    rounds(x);
    for (int i = 0; i < 4; i++) put32(out + i * 4, x[i]), put32(out + 16 + i * 4, x[12 + i]);
    wipe(x, sizeof(x));
}

namespace {

// Poly1305 in 26-bit limbs (as poly1305-donna's 32-bit version), fed in parts
struct Poly {
    uint32_t r0, r1, r2, r3, r4, s1, s2, s3, s4;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    uint8_t key[32];
    uint8_t buf[16];
    size_t buf_n = 0;

    explicit Poly(const uint8_t k[32]) {
        memcpy(key, k, 32);
        r0 = le32(k) & 0x3ffffff, r1 = (le32(k + 3) >> 2) & 0x3ffff03, r2 = (le32(k + 6) >> 4) & 0x3ffc0ff;
        r3 = (le32(k + 9) >> 6) & 0x3f03fff, r4 = (le32(k + 12) >> 8) & 0x00fffff;
        s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    }
    ~Poly() { wipe(this, sizeof(*this)); }

    // One block of `take` bytes (16, but the last may be shorter)
    void block(const uint8_t* m, size_t take) {
        uint8_t b[17] = {0};
        memcpy(b, m, take);
        b[take] = 1;  // (the 2^(8*take) bit)
        h0 += le32(b) & 0x3ffffff;
        h1 += (le32(b + 3) >> 2) & 0x3ffffff;
        h2 += (le32(b + 6) >> 4) & 0x3ffffff;
        h3 += (le32(b + 9) >> 6) & 0x3ffffff;
        h4 += (le32(b + 12) >> 8) | ((uint32_t)b[16] << 24);
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c, c = (uint32_t)(d1 >> 26), h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c, c = (uint32_t)(d2 >> 26), h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c, c = (uint32_t)(d3 >> 26), h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c, c = (uint32_t)(d4 >> 26), h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
    }

    void update(const uint8_t* m, size_t n) {
        if (buf_n) {
            size_t take = 16 - buf_n < n ? 16 - buf_n : n;
            memcpy(buf + buf_n, m, take);
            buf_n += take, m += take, n -= take;
            if (buf_n < 16) return;
            block(buf, 16);
            buf_n = 0;
        }
        for (; n >= 16; m += 16, n -= 16) block(m, 16);
        memcpy(buf, m, n);
        buf_n = n;
    }

    void final(uint8_t tag[16]) {
        if (buf_n) block(buf, buf_n);
        // fully carry, then h - p if that's not negative
        uint32_t c = h1 >> 26;
        h1 &= 0x3ffffff;
        h2 += c, c = h2 >> 26, h2 &= 0x3ffffff;
        h3 += c, c = h3 >> 26, h3 &= 0x3ffffff;
        h4 += c, c = h4 >> 26, h4 &= 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
        uint32_t g0 = h0 + 5;
        c = g0 >> 26, g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c;
        c = g1 >> 26, g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c;
        c = g2 >> 26, g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c;
        c = g3 >> 26, g3 &= 0x3ffffff;
        uint32_t g4 = h4 + c - (1u << 26);
        uint32_t mask = (g4 >> 31) - 1;  // (all ones when h >= p)
        h0 = (h0 & ~mask) | (g0 & mask), h1 = (h1 & ~mask) | (g1 & mask), h2 = (h2 & ~mask) | (g2 & mask);
        h3 = (h3 & ~mask) | (g3 & mask), h4 = (h4 & ~mask) | (g4 & mask);
        // h + s, as 128 bits
        uint64_t f;
        f = (uint64_t)(h0 | h1 << 26) + le32(key + 16), put32(tag, (uint32_t)f);
        f = (uint64_t)(h1 >> 6 | h2 << 20) + le32(key + 20) + (f >> 32), put32(tag + 4, (uint32_t)f);
        f = (uint64_t)(h2 >> 12 | h3 << 14) + le32(key + 24) + (f >> 32), put32(tag + 8, (uint32_t)f);
        f = (uint64_t)(h3 >> 18 | h4 << 8) + le32(key + 28) + (f >> 32), put32(tag + 12, (uint32_t)f);
    }
};

}  // namespace

void poly1305(const uint8_t key[32], const uint8_t* msg, size_t n, uint8_t tag[POLY1305_TAG]) {
    Poly p(key);
    p.update(msg, n);
    p.final(tag);
}

namespace {

// The AEAD's MAC: aad, ct, each padded to 16, then their lengths (64-bit little-endian)
void aeadTag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n,
             uint8_t tag[16]) {
    uint8_t polykey[64] = {0};
    chacha20(key, nonce, 0, polykey, 64, polykey);
    static const uint8_t zeros[16] = {0};
    Poly p(polykey);
    p.update(aad, aad_n);
    p.update(zeros, (16 - aad_n % 16) % 16);
    p.update(ct, n);
    p.update(zeros, (16 - n % 16) % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)((uint64_t)aad_n >> (8 * i)), lens[8 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    p.update(lens, 16);
    p.final(tag);
    wipe(polykey, sizeof(polykey));
}

void xnonce(const uint8_t key[32], const uint8_t nonce[24], uint8_t subkey[32], uint8_t n12[12]) {
    hchacha20(key, nonce, subkey);
    memset(n12, 0, 4);
    memcpy(n12 + 4, nonce + 16, 8);
}

}  // namespace

void chachaSeal(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n,
                uint8_t* ct, uint8_t tag[POLY1305_TAG]) {
    chacha20(key, nonce, 1, pt, n, ct);
    aeadTag(key, nonce, aad, aad_n, ct, n, tag);
}

bool chachaOpen(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n,
                const uint8_t tag[POLY1305_TAG], uint8_t* pt) {
    uint8_t want[16];
    aeadTag(key, nonce, aad, aad_n, ct, n, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff) return false;
    chacha20(key, nonce, 1, ct, n, pt);
    return true;
}

void xchachaSeal(const uint8_t key[CHACHA_KEY], const uint8_t nonce[XCHACHA_NONCE], const uint8_t* aad, size_t aad_n, const uint8_t* pt,
                 size_t n, uint8_t* ct, uint8_t tag[POLY1305_TAG]) {
    uint8_t sub[32], n12[12];
    xnonce(key, nonce, sub, n12);
    chachaSeal(sub, n12, aad, aad_n, pt, n, ct, tag);
    wipe(sub, sizeof(sub));
}

bool xchachaOpen(const uint8_t key[CHACHA_KEY], const uint8_t nonce[XCHACHA_NONCE], const uint8_t* aad, size_t aad_n, const uint8_t* ct,
                 size_t n, const uint8_t tag[POLY1305_TAG], uint8_t* pt) {
    uint8_t sub[32], n12[12];
    xnonce(key, nonce, sub, n12);
    bool ok = chachaOpen(sub, n12, aad, aad_n, ct, n, tag, pt);
    wipe(sub, sizeof(sub));
    return ok;
}
