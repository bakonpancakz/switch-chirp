#include "mlswire.h"

void Writer::raw(const void* p, size_t len) {
    if (!ok || len > cap - n) {
        ok = false;
        return;
    }
    if (len) memcpy(buf + n, p, len);
    n += len;
}

void Writer::u16(uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    raw(b, 2);
}

void Writer::u32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    raw(b, 4);
}

void Writer::u64(uint64_t v) {
    u32((uint32_t)(v >> 32));
    u32((uint32_t)v);
}

void Writer::varint(uint32_t v) {
    if (v < 0x40) {
        u8((uint8_t)v);
    } else if (v < 0x4000) {
        u16((uint16_t)(0x4000 | v));
    } else if (v < 0x40000000) {
        u32(0x80000000u | v);
    } else {
        ok = false;
    }
}

void Writer::vec(const void* p, size_t len) {
    varint((uint32_t)len);
    raw(p, len);
}

// (the length is written as 4 bytes at first, then moved down to its minimal size)
size_t Writer::begin() {
    size_t at = n;
    u32(0);
    return at;
}

void Writer::end(size_t at) {
    if (!ok) return;
    size_t len = n - at - 4;
    uint8_t hdr[4];
    Writer h(hdr, sizeof(hdr));
    h.varint((uint32_t)len);
    if (!h.ok) {
        ok = false;
        return;
    }
    memmove(buf + at + h.n, buf + at + 4, len);
    memcpy(buf + at, hdr, h.n);
    n = at + h.n + len;
}

bool Reader::raw(void* out, size_t len) {
    if (!ok || len > n) {
        ok = false;
        return false;
    }
    if (len) memcpy(out, p, len);
    p += len, n -= len;
    return true;
}

Bytes Reader::take(size_t len) {
    if (!ok || len > n) {
        ok = false;
        return Bytes{};
    }
    Bytes b{p, len};
    p += len, n -= len;
    return b;
}

uint8_t Reader::u8() {
    uint8_t v = 0;
    raw(&v, 1);
    return v;
}

uint16_t Reader::u16() {
    uint8_t b[2] = {0, 0};
    raw(b, 2);
    return (uint16_t)(b[0] << 8 | b[1]);
}

uint32_t Reader::u32() {
    uint8_t b[4] = {0, 0, 0, 0};
    raw(b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t Reader::u64() {
    uint64_t hi = u32();
    return hi << 32 | u32();
}

uint32_t Reader::varint() {
    if (!ok || n == 0) {
        ok = false;
        return 0;
    }
    uint8_t prefix = p[0] >> 6;
    uint32_t v;
    if (prefix == 0) {
        v = u8();
    } else if (prefix == 1) {
        v = u16() & 0x3fff;
        if (v < 0x40) ok = false;  // (not minimal)
    } else if (prefix == 2) {
        v = u32() & 0x3fffffff;
        if (v < 0x4000) ok = false;
    } else {
        ok = false;  // (8-byte lengths aren't allowed in MLS)
        v = 0;
    }
    return ok ? v : 0;
}

Bytes Reader::vec() {
    uint32_t len = varint();
    return take(len);
}
