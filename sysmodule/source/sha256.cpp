#include "sha256.h"

#include <string.h>

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

void block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

}  // namespace

void Sha256::init() {
    static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, IV, sizeof(h));
    buf_len = 0;
    total = 0;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    total += n;
    if (buf_len) {
        size_t take = SHA256_BLOCK - buf_len < n ? SHA256_BLOCK - buf_len : n;
        memcpy(buf + buf_len, p, take);
        buf_len += take, p += take, n -= take;
        if (buf_len < SHA256_BLOCK) return;
        block(h, buf);
        buf_len = 0;
    }
    for (; n >= SHA256_BLOCK; p += SHA256_BLOCK, n -= SHA256_BLOCK) block(h, p);
    memcpy(buf, p, n);
    buf_len = n;
}

void Sha256::final(uint8_t out[SHA256_SIZE]) {
    uint64_t bits = total * 8;
    uint8_t pad[SHA256_BLOCK + 8] = {0x80};
    size_t pad_n = (buf_len < 56 ? 56 : 120) - buf_len;
    for (int i = 0; i < 8; i++) pad[pad_n + i] = (uint8_t)(bits >> (56 - 8 * i));
    update(pad, pad_n + 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8), out[i * 4 + 3] = (uint8_t)h[i];
    }
    wipe(this, sizeof(*this));
}

void sha256(const void* data, size_t n, uint8_t out[SHA256_SIZE]) {
    Sha256 s;
    s.init();
    s.update(data, n);
    s.final(out);
}

void HmacSha256::init(const void* key, size_t key_n) {
    uint8_t k[SHA256_BLOCK] = {0};
    if (key_n > SHA256_BLOCK) sha256(key, key_n, k);
    else memcpy(k, key, key_n);
    uint8_t pad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++) pad[i] = k[i] ^ 0x36;
    inner.init();
    inner.update(pad, sizeof(pad));
    for (int i = 0; i < SHA256_BLOCK; i++) pad[i] = k[i] ^ 0x5c;
    outer.init();
    outer.update(pad, sizeof(pad));
    wipe(k, sizeof(k));
    wipe(pad, sizeof(pad));
}

void HmacSha256::update(const void* data, size_t n) { inner.update(data, n); }

void HmacSha256::final(uint8_t out[SHA256_SIZE]) {
    uint8_t ih[SHA256_SIZE];
    inner.final(ih);
    outer.update(ih, sizeof(ih));
    outer.final(out);
    wipe(ih, sizeof(ih));
}

void hmacSha256(const void* key, size_t key_n, const void* data, size_t n, uint8_t out[SHA256_SIZE]) {
    HmacSha256 m;
    m.init(key, key_n);
    m.update(data, n);
    m.final(out);
}

void hkdfExtract(const void* salt, size_t salt_n, const void* ikm, size_t ikm_n, uint8_t prk[SHA256_SIZE]) {
    static const uint8_t zeros[SHA256_SIZE] = {0};
    if (salt_n == 0) salt = zeros, salt_n = sizeof(zeros);
    hmacSha256(salt, salt_n, ikm, ikm_n, prk);
}

bool hkdfExpand(const uint8_t prk[SHA256_SIZE], const void* info, size_t info_n, uint8_t* out, size_t n) {
    return hkdfExpandKey(prk, SHA256_SIZE, info, info_n, out, n);
}

bool hkdfExpandKey(const uint8_t* prk, size_t prk_n, const void* info, size_t info_n, uint8_t* out, size_t n) {
    if (n > 255 * SHA256_SIZE) return false;
    uint8_t t[SHA256_SIZE];
    size_t t_n = 0, done = 0;
    for (uint8_t i = 1; done < n; i++) {
        HmacSha256 m;
        m.init(prk, prk_n);
        m.update(t, t_n);
        m.update(info, info_n);
        m.update(&i, 1);
        m.final(t);
        t_n = SHA256_SIZE;
        size_t take = n - done < SHA256_SIZE ? n - done : SHA256_SIZE;
        memcpy(out + done, t, take);
        done += take;
    }
    wipe(t, sizeof(t));
    return true;
}

void wipe(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}
