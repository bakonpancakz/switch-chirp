#include "util.h"

#include <string.h>

namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1Block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

}  // namespace

void sha1(const void* data, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint8_t* p = (const uint8_t*)data;
    size_t left = n;
    while (left >= 64) {
        sha1Block(h, p);
        p += 64, left -= 64;
    }
    // the rest, a 1 bit, zeros, and the length in bits (one or two blocks)
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t blocks = left + 9 <= 64 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (size_t i = 0; i < blocks; i++) sha1Block(h, tail + i * 64);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8), out[i * 4 + 3] = (uint8_t)h[i];
    }
}

void base64(const uint8_t* in, size_t n, char* out) {
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = abc[(v >> 18) & 63];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = i + 1 < n ? abc[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? abc[v & 63] : '=';
    }
    out[o] = 0;
}

uint64_t parseId(const char* s, size_t n) {
    if (n == 0 || n > 20) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    return v;
}

void idString(uint64_t id, char out[21]) {
    char tmp[21];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + id % 10);
        id /= 10;
    } while (id);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

void utf8Copy(char* dst, size_t cap, const char* src, size_t n) {
    if (cap == 0) return;
    if (n < cap) {
        memcpy(dst, src, n);
        dst[n] = 0;
        return;
    }
    // room for the text, "…" (3 bytes) and the NUL; back off to a character's start
    size_t keep = cap >= 4 ? cap - 4 : 0;
    while (keep > 0 && ((unsigned char)src[keep] & 0xC0) == 0x80) keep--;
    memcpy(dst, src, keep);
    if (cap >= 4) {
        memcpy(dst + keep, "\xE2\x80\xA6", 3);
        dst[keep + 3] = 0;
    } else {
        dst[keep] = 0;
    }
}

bool jsonEscape(char* out, size_t cap, const char* s) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char* add;
        char esc[7];
        if (c == '"') add = "\\\"";
        else if (c == '\\') add = "\\\\";
        else if (c < 0x20) {
            esc[0] = '\\', esc[1] = 'u', esc[2] = '0', esc[3] = '0', esc[4] = hex[c >> 4], esc[5] = hex[c & 15], esc[6] = 0;
            add = esc;
        } else {
            esc[0] = (char)c, esc[1] = 0;
            add = esc;
        }
        size_t len = strlen(add);
        if (o + len >= cap) return false;
        memcpy(out + o, add, len);
        o += len;
    }
    out[o] = 0;
    return true;
}
