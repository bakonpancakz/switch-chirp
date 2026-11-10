// MLS's wire format (RFC 9420 section 2.1): TLS presentation language, with
// vectors' lengths as QUIC-style variable-length integers (1, 2 or 4 bytes,
// minimal). A Writer fills a fixed buffer; a Reader walks one. Neither
// allocates: a read vector is a view into the bytes it came from.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool operator==(const Bytes& o) const { return n == o.n && (n == 0 || memcmp(p, o.p, n) == 0); }
};

struct Writer {
    uint8_t* buf;
    size_t cap, n = 0;
    bool ok = true;  // false once something didn't fit

    Writer(uint8_t* b, size_t c) : buf(b), cap(c) {}
    void raw(const void* p, size_t len);
    void u8(uint8_t v) { raw(&v, 1); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void varint(uint32_t v);                  // (lengths up to 2^30 - 1)
    void vec(const void* p, size_t len);      // opaque x<V>: length, then the bytes
    void vec(const Bytes& b) { vec(b.p, b.n); }
    // A vector whose contents are written after: begin() returns where its length goes, end() fills it in
    size_t begin();
    void end(size_t at);
    Bytes bytes() const { return Bytes{buf, n}; }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    bool ok = true;  // false once something was missing or malformed

    Reader() : p(nullptr), n(0) {}
    Reader(const uint8_t* b, size_t len) : p(b), n(len) {}
    explicit Reader(const Bytes& b) : p(b.p), n(b.n) {}
    bool raw(void* out, size_t len);
    Bytes take(size_t len);
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    uint32_t varint();
    Bytes vec();                         // opaque x<V>
    // A vector of structs, read on its own (failed if this one has)
    Reader sub() {
        Reader r(vec());
        r.ok = ok;
        return r;
    }
    bool done() const { return ok && n == 0; }
};
