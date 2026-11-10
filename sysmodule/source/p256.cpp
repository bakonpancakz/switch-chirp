#include "p256.h"

#include <string.h>

#include "sha256.h"

namespace {

// --- 256-bit numbers: 8 x 32-bit limbs, least significant first ---

struct U256 {
    uint32_t v[8];
};

void fromBytes(U256& a, const uint8_t b[32]) {
    for (int i = 0; i < 8; i++)
        a.v[i] = (uint32_t)b[31 - i * 4] | (uint32_t)b[30 - i * 4] << 8 | (uint32_t)b[29 - i * 4] << 16 | (uint32_t)b[28 - i * 4] << 24;
}

void toBytes(uint8_t b[32], const U256& a) {
    for (int i = 0; i < 8; i++)
        b[31 - i * 4] = (uint8_t)a.v[i], b[30 - i * 4] = (uint8_t)(a.v[i] >> 8), b[29 - i * 4] = (uint8_t)(a.v[i] >> 16),
                   b[28 - i * 4] = (uint8_t)(a.v[i] >> 24);
}

U256 fromHex(const char* h) {
    uint8_t b[32];
    for (int i = 0; i < 32; i++) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        b[i] = (uint8_t)(nib(h[i * 2]) << 4 | nib(h[i * 2 + 1]));
    }
    U256 a;
    fromBytes(a, b);
    return a;
}

// r = a - b; the borrow out (1 when a < b)
uint32_t sub(U256& r, const U256& a, const U256& b) {
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t d = (uint64_t)a.v[i] - b.v[i] - borrow;
        r.v[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

uint32_t add(U256& r, const U256& a, const U256& b) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t s = (uint64_t)a.v[i] + b.v[i] + carry;
        r.v[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}

// r = mask ? a : b (mask all ones or zero)
void select(U256& r, uint32_t mask, const U256& a, const U256& b) {
    for (int i = 0; i < 8; i++) r.v[i] = (a.v[i] & mask) | (b.v[i] & ~mask);
}

bool isZero(const U256& a) {
    uint32_t x = 0;
    for (int i = 0; i < 8; i++) x |= a.v[i];
    return x == 0;
}

bool equal(const U256& a, const U256& b) {
    uint32_t x = 0;
    for (int i = 0; i < 8; i++) x |= a.v[i] ^ b.v[i];
    return x == 0;
}

// --- arithmetic modulo m, in Montgomery form (x * 2^256 mod m) ---

struct Mod {
    U256 m;
    uint32_t m0inv;  // -m^-1 mod 2^32
    U256 rr;         // 2^512 mod m (to convert into the form)
    U256 one;        // 1 in the form (2^256 mod m)

    void init(const char* hex) {
        m = fromHex(hex);
        uint32_t inv = 1;  // (Newton: m^-1 mod 2^32)
        for (int i = 0; i < 5; i++) inv *= 2 - m.v[0] * inv;
        m0inv = 0 - inv;
        // 2^256 mod m, then doubled 256 more times
        U256 x = {{1}};
        for (int i = 0; i < 512; i++) {
            if (i == 256) one = x;
            addm(x, x, x);
        }
        rr = x;
    }

    void addm(U256& r, const U256& a, const U256& b) const {
        U256 s, t;
        uint32_t carry = add(s, a, b);
        uint32_t borrow = sub(t, s, m);
        // (s >= m, or it overflowed: the subtracted one)
        select(r, 0 - (carry | (borrow ^ 1)), t, s);
    }

    void subm(U256& r, const U256& a, const U256& b) const {
        U256 d, t;
        uint32_t borrow = sub(d, a, b);
        add(t, d, m);
        select(r, 0 - borrow, t, d);
    }

    // r = a * b / 2^256 mod m (CIOS)
    void mul(U256& r, const U256& a, const U256& b) const {
        uint32_t t[10] = {0};
        for (int i = 0; i < 8; i++) {
            uint64_t c = 0;
            for (int j = 0; j < 8; j++) {
                uint64_t s = (uint64_t)t[j] + (uint64_t)a.v[j] * b.v[i] + c;
                t[j] = (uint32_t)s;
                c = s >> 32;
            }
            uint64_t s = (uint64_t)t[8] + c;
            t[8] = (uint32_t)s;
            t[9] = (uint32_t)(s >> 32);
            uint32_t k = t[0] * m0inv;
            s = (uint64_t)t[0] + (uint64_t)k * m.v[0];
            c = s >> 32;
            for (int j = 1; j < 8; j++) {
                s = (uint64_t)t[j] + (uint64_t)k * m.v[j] + c;
                t[j - 1] = (uint32_t)s;
                c = s >> 32;
            }
            s = (uint64_t)t[8] + c;
            t[7] = (uint32_t)s;
            t[8] = t[9] + (uint32_t)(s >> 32);
        }
        U256 x, y;
        memcpy(x.v, t, 32);
        uint32_t borrow = sub(y, x, m);
        select(r, 0 - (t[8] | (borrow ^ 1)), y, x);
    }

    void sqr(U256& r, const U256& a) const { mul(r, a, a); }
    void to(U256& r, const U256& a) const { mul(r, a, rr); }
    void from(U256& r, const U256& a) const {
        U256 o = {{1}};
        mul(r, a, o);
    }

    // a^(m-2): the inverse (m is prime), in the form
    void inv(U256& r, const U256& a) const {
        U256 e = m, two = {{2}};
        sub(e, e, two);
        U256 x = one;
        for (int i = 255; i >= 0; i--) {
            sqr(x, x);
            if ((e.v[i / 32] >> (i % 32)) & 1) mul(x, x, a);  // (the exponent is public)
        }
        r = x;
    }

    // a mod m for a < 2m (a 256-bit value: hashes, x coordinates)
    void reduce(U256& r, const U256& a) const {
        U256 t;
        uint32_t borrow = sub(t, a, m);
        select(r, 0 - (borrow ^ 1), t, a);
    }
};

Mod g_p, g_n;
U256 g_b, g_gx, g_gy;  // (b and G in the field's form)
bool g_ready = false;

void setup() {
    if (g_ready) return;
    g_p.init("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    g_n.init("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    g_p.to(g_b, fromHex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"));
    g_p.to(g_gx, fromHex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"));
    g_p.to(g_gy, fromHex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"));
    g_ready = true;
}

// --- points, Jacobian (X/Z^2, Y/Z^3), Z = 0 the point at infinity ---

struct Pt {
    U256 x, y, z;
};

void dbl(Pt& r, const Pt& a) {
    // dbl-2001-b (a = -3)
    const Mod& f = g_p;
    U256 delta, gamma, beta, alpha, t1, t2;
    f.sqr(delta, a.z);
    f.sqr(gamma, a.y);
    f.mul(beta, a.x, gamma);
    f.subm(t1, a.x, delta);
    f.addm(t2, a.x, delta);
    f.mul(alpha, t1, t2);
    f.addm(t1, alpha, alpha);
    f.addm(alpha, t1, alpha);  // 3 (x - delta)(x + delta)
    Pt o;
    f.addm(t1, beta, beta);
    f.addm(t1, t1, t1);  // 4 beta
    f.addm(t2, t1, t1);  // 8 beta
    f.sqr(o.x, alpha);
    f.subm(o.x, o.x, t2);
    f.addm(o.z, a.y, a.z);
    f.sqr(o.z, o.z);
    f.subm(o.z, o.z, gamma);
    f.subm(o.z, o.z, delta);
    f.subm(t1, t1, o.x);
    f.mul(o.y, alpha, t1);
    f.sqr(t2, gamma);
    f.addm(t2, t2, t2);
    f.addm(t2, t2, t2);
    f.addm(t2, t2, t2);  // 8 gamma^2
    f.subm(o.y, o.y, t2);
    r = o;
}

void addPt(Pt& r, const Pt& a, const Pt& b) {
    if (isZero(a.z)) {
        r = b;
        return;
    }
    if (isZero(b.z)) {
        r = a;
        return;
    }
    // add-2007-bl
    const Mod& f = g_p;
    U256 z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    f.sqr(z1z1, a.z);
    f.sqr(z2z2, b.z);
    f.mul(u1, a.x, z2z2);
    f.mul(u2, b.x, z1z1);
    f.mul(t, b.z, z2z2);
    f.mul(s1, a.y, t);
    f.mul(t, a.z, z1z1);
    f.mul(s2, b.y, t);
    f.subm(h, u2, u1);
    f.subm(rr, s2, s1);
    if (isZero(h)) {
        if (isZero(rr)) {  // (the same point: double it)
            dbl(r, a);
        } else {  // (P + -P)
            memset(&r, 0, sizeof(r));
        }
        return;
    }
    f.addm(rr, rr, rr);
    f.addm(t, h, h);
    f.sqr(i, t);
    f.mul(j, h, i);
    f.mul(v, u1, i);
    Pt o;
    f.sqr(o.x, rr);
    f.subm(o.x, o.x, j);
    f.subm(o.x, o.x, v);
    f.subm(o.x, o.x, v);
    f.subm(t, v, o.x);
    f.mul(o.y, rr, t);
    f.mul(t, s1, j);
    f.addm(t, t, t);
    f.subm(o.y, o.y, t);
    f.addm(t, a.z, b.z);
    f.sqr(t, t);
    f.subm(t, t, z1z1);
    f.subm(t, t, z2z2);
    f.mul(o.z, t, h);
    r = o;
}

void swapPt(Pt& a, Pt& b, uint32_t mask) {
    uint32_t* x = (uint32_t*)&a;
    uint32_t* y = (uint32_t*)&b;
    for (size_t i = 0; i < sizeof(Pt) / 4; i++) {
        uint32_t t = (x[i] ^ y[i]) & mask;
        x[i] ^= t, y[i] ^= t;
    }
}

// k * p, k a plain (not Montgomery) scalar
void mulPt(Pt& r, const U256& k, const Pt& p) {
    Pt r0, r1 = p;
    memset(&r0, 0, sizeof(r0));
    for (int i = 255; i >= 0; i--) {
        uint32_t bit = 0 - ((k.v[i / 32] >> (i % 32)) & 1);
        swapPt(r0, r1, bit);
        addPt(r1, r0, r1);
        dbl(r0, r0);
        swapPt(r0, r1, bit);
    }
    r = r0;
}

// Affine x, y (plain, not Montgomery); false for the point at infinity
bool affine(const Pt& p, U256& x, U256& y) {
    if (isZero(p.z)) return false;
    U256 zi, zi2, zi3, t;
    g_p.inv(zi, p.z);
    g_p.sqr(zi2, zi);
    g_p.mul(zi3, zi2, zi);
    g_p.mul(t, p.x, zi2);
    g_p.from(x, t);
    g_p.mul(t, p.y, zi3);
    g_p.from(y, t);
    return true;
}

Pt generator() {
    Pt g;
    g.x = g_gx, g.y = g_gy, g.z = g_p.one;
    return g;
}

bool scalarOk(const U256& k) {
    U256 t;
    return !isZero(k) && sub(t, k, g_n.m) == 1;  // 1 <= k < n
}

bool decodePub(const uint8_t pub[P256_PUB], Pt& p) {
    if (pub[0] != 0x04) return false;
    U256 x, y, t;
    fromBytes(x, pub + 1);
    fromBytes(y, pub + 33);
    if (sub(t, x, g_p.m) != 1 || sub(t, y, g_p.m) != 1) return false;  // (each below p)
    g_p.to(p.x, x);
    g_p.to(p.y, y);
    p.z = g_p.one;
    // y^2 = x^3 - 3x + b
    U256 lhs, rhs, x3;
    g_p.sqr(lhs, p.y);
    g_p.sqr(x3, p.x);
    g_p.mul(x3, x3, p.x);
    g_p.addm(t, p.x, p.x);
    g_p.addm(t, t, p.x);
    g_p.subm(rhs, x3, t);
    g_p.addm(rhs, rhs, g_b);
    return equal(lhs, rhs);
}

void encodePub(uint8_t pub[P256_PUB], const U256& x, const U256& y) {
    pub[0] = 0x04;
    toBytes(pub + 1, x);
    toBytes(pub + 33, y);
}

// RFC 6979 section 3.2: the nonce k for (priv, hash), with HMAC-SHA256
void rfc6979(const U256& d, const uint8_t hash[32], U256& k) {
    uint8_t x[32], h1[32], v[32], kk[32];
    toBytes(x, d);
    U256 e;
    fromBytes(e, hash);
    g_n.reduce(e, e);
    toBytes(h1, e);  // (bits2octets: the hash mod n)
    memset(v, 0x01, 32);
    memset(kk, 0x00, 32);
    for (uint8_t round = 0; round < 2; round++) {
        HmacSha256 m;
        m.init(kk, 32);
        m.update(v, 32);
        m.update(&round, 1);
        m.update(x, 32);
        m.update(h1, 32);
        m.final(kk);
        hmacSha256(kk, 32, v, 32, v);
    }
    for (;;) {
        hmacSha256(kk, 32, v, 32, v);
        fromBytes(k, v);
        if (scalarOk(k)) break;
        uint8_t zero = 0;
        HmacSha256 m;
        m.init(kk, 32);
        m.update(v, 32);
        m.update(&zero, 1);
        m.final(kk);
        hmacSha256(kk, 32, v, 32, v);
    }
    wipe(x, sizeof(x));
    wipe(kk, sizeof(kk));
    wipe(v, sizeof(v));
}

// An unsigned big-endian integer as DER (minimal, a 0 in front when the top bit is set)
size_t derInt(uint8_t* out, const uint8_t v[32]) {
    int i = 0;
    while (i < 31 && v[i] == 0) i++;
    size_t n = 32 - i;
    bool pad = v[i] & 0x80;
    out[0] = 0x02;
    out[1] = (uint8_t)(n + pad);
    size_t at = 2;
    if (pad) out[at++] = 0;
    memcpy(out + at, v + i, n);
    return at + n;
}

// One DER INTEGER at *p (left in 32 bytes, big-endian); false if it isn't one that fits
bool derReadInt(const uint8_t*& p, const uint8_t* end, uint8_t v[32]) {
    if (end - p < 2 || p[0] != 0x02) return false;
    size_t n = p[1];
    p += 2;
    if (n == 0 || n > 33 || (size_t)(end - p) < n) return false;
    if (p[0] & 0x80) return false;  // (negative)
    if (n > 1 && p[0] == 0 && !(p[1] & 0x80)) return false;  // (not minimal)
    if (n == 33) {
        if (p[0] != 0) return false;
        p++, n--;
    }
    memset(v, 0, 32);
    memcpy(v + 32 - n, p, n);
    p += n;
    return true;
}

}  // namespace

bool p256ValidPrivate(const uint8_t priv[P256_PRIV]) {
    setup();
    U256 d;
    fromBytes(d, priv);
    return scalarOk(d);
}

bool p256ValidPublic(const uint8_t pub[P256_PUB]) {
    setup();
    Pt p;
    return decodePub(pub, p);
}

bool p256Public(const uint8_t priv[P256_PRIV], uint8_t pub[P256_PUB]) {
    setup();
    U256 d, x, y;
    fromBytes(d, priv);
    if (!scalarOk(d)) return false;
    Pt q;
    mulPt(q, d, generator());
    bool ok = affine(q, x, y);
    if (ok) encodePub(pub, x, y);
    wipe(&d, sizeof(d));
    return ok;
}

bool p256Ecdh(const uint8_t priv[P256_PRIV], const uint8_t pub[P256_PUB], uint8_t secret[32]) {
    setup();
    U256 d, x, y;
    Pt q, s;
    fromBytes(d, priv);
    if (!scalarOk(d) || !decodePub(pub, q)) return false;
    mulPt(s, d, q);
    bool ok = affine(s, x, y);
    if (ok) toBytes(secret, x);
    wipe(&d, sizeof(d));
    wipe(&s, sizeof(s));
    return ok;
}

bool p256SignRaw(const uint8_t priv[P256_PRIV], const uint8_t hash[32], uint8_t r_out[32], uint8_t s_out[32]) {
    setup();
    U256 d, k, e, x, y, r, s, t, km, kinv, rm, dm, em;
    fromBytes(d, priv);
    if (!scalarOk(d)) return false;
    fromBytes(e, hash);
    g_n.reduce(e, e);
    rfc6979(d, hash, k);
    Pt kg;
    mulPt(kg, k, generator());
    affine(kg, x, y);
    g_n.reduce(r, x);
    if (isZero(r)) return false;  // (k was chosen by RFC 6979: this can't happen in practice)
    // s = k^-1 (e + r d) mod n, in the order's Montgomery form
    g_n.to(km, k);
    g_n.inv(kinv, km);
    g_n.to(rm, r);
    g_n.to(dm, d);
    g_n.to(em, e);
    g_n.mul(t, rm, dm);
    g_n.addm(t, t, em);
    g_n.mul(t, t, kinv);
    g_n.from(s, t);
    if (isZero(s)) return false;
    toBytes(r_out, r);
    toBytes(s_out, s);
    wipe(&d, sizeof(d));
    wipe(&k, sizeof(k));
    wipe(&km, sizeof(km));
    wipe(&kinv, sizeof(kinv));
    wipe(&dm, sizeof(dm));
    return true;
}

bool p256Sign(const uint8_t priv[P256_PRIV], const void* msg, size_t n, uint8_t sig[P256_SIG_MAX], size_t* sig_n) {
    uint8_t hash[32], r[32], s[32];
    sha256(msg, n, hash);
    if (!p256SignRaw(priv, hash, r, s)) return false;
    uint8_t body[70];
    size_t at = derInt(body, r);
    at += derInt(body + at, s);
    sig[0] = 0x30;
    sig[1] = (uint8_t)at;
    memcpy(sig + 2, body, at);
    *sig_n = at + 2;
    return true;
}

bool p256Verify(const uint8_t pub[P256_PUB], const void* msg, size_t n, const uint8_t* sig, size_t sig_n) {
    setup();
    Pt q;
    if (!decodePub(pub, q)) return false;
    if (sig_n < 8 || sig[0] != 0x30 || sig[1] != sig_n - 2) return false;
    const uint8_t* p = sig + 2;
    const uint8_t* end = sig + sig_n;
    uint8_t rb[32], sb[32];
    if (!derReadInt(p, end, rb) || !derReadInt(p, end, sb) || p != end) return false;
    U256 r, s, e, w, u1, u2, t, x, y, xr;
    fromBytes(r, rb);
    fromBytes(s, sb);
    if (!scalarOk(r) || !scalarOk(s)) return false;
    uint8_t hash[32];
    sha256(msg, n, hash);
    fromBytes(e, hash);
    g_n.reduce(e, e);
    // w = s^-1, u1 = e w, u2 = r w (mod n)
    U256 sm, em, rm;
    g_n.to(sm, s);
    g_n.inv(w, sm);
    g_n.to(em, e);
    g_n.to(rm, r);
    g_n.mul(t, em, w);
    g_n.from(u1, t);
    g_n.mul(t, rm, w);
    g_n.from(u2, t);
    Pt a, b, c;
    mulPt(a, u1, generator());
    mulPt(b, u2, q);
    addPt(c, a, b);
    if (!affine(c, x, y)) return false;
    g_n.reduce(xr, x);
    return equal(xr, r);
}
