#include "aes.h"

#include <string.h>

#include "sha256.h"

#if defined(__aarch64__) && (defined(__ARM_FEATURE_CRYPTO) || defined(__ARM_FEATURE_AES))
#include <arm_neon.h>
#define AES_HW 1
#else
#define AES_HW 0
#endif

namespace {
bool g_hw = AES_HW;  // (the CPU's instructions: until the self-test says otherwise)
}

namespace {

uint8_t g_sbox[256];
bool g_ready = false;

uint8_t xtime(uint8_t v) { return (uint8_t)((v << 1) ^ ((v & 0x80) ? 0x1b : 0)); }

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) r ^= a;
    return r;
}

// The S-box from its definition: the inverse in GF(2^8) (0 for 0), then the affine map
void makeSbox() {
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        for (int y = 1; y < 256 && x; y++) {
            if (gmul((uint8_t)x, (uint8_t)y) == 1) {
                inv = (uint8_t)y;
                break;
            }
        }
        uint8_t s = inv;
        for (int i = 1; i < 5; i++) s ^= (uint8_t)((inv << i) | (inv >> (8 - i)));
        g_sbox[x] = (uint8_t)(s ^ 0x63);
    }
    g_ready = true;
}

}  // namespace

bool Aes::init(const uint8_t* key, size_t key_n) {
    if (key_n != 16 && key_n != 32) return false;
    if (!g_ready) makeSbox();
    int nk = (int)key_n / 4;
    rounds = nk + 6;
    int words = 4 * (rounds + 1);
    memcpy(rk, key, key_n);
    uint8_t rcon = 1;
    for (int i = nk; i < words; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % nk == 0) {
            uint8_t first = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rcon), t[1] = g_sbox[t[2]], t[2] = g_sbox[t[3]], t[3] = g_sbox[first];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = g_sbox[t[k]];
        }
        for (int k = 0; k < 4; k++) rk[i * 4 + k] = (uint8_t)(rk[(i - nk) * 4 + k] ^ t[k]);
    }
    return true;
}

void Aes::encrypt(const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]) const {
#if AES_HW
    if (g_hw) {
        // AESE: AddRoundKey, SubBytes, ShiftRows; AESMC: MixColumns. The last round has no MixColumns,
        // and the last round key is added on its own
        uint8x16_t b = vld1q_u8(in);
        int r = 0;
        for (; r < rounds - 1; r++) b = vaesmcq_u8(vaeseq_u8(b, vld1q_u8(rk + 16 * r)));
        b = vaeseq_u8(b, vld1q_u8(rk + 16 * r));
        b = veorq_u8(b, vld1q_u8(rk + 16 * rounds));
        vst1q_u8(out, b);
        return;
    }
#endif
    encryptSoft(in, out);
}

void Aes::encryptSoft(const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int r = 1; r <= rounds; r++) {
        // SubBytes and ShiftRows together (the state is column after column)
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[c * 4 + row] = g_sbox[s[((c + row) % 4) * 4 + row]];
        if (r < rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[r * 16 + i]);
    }
    memcpy(out, s, 16);
}

// --- GCM ---

namespace {

#if AES_HW
void ghashHw(uint8_t y[16], const uint64_t hk[2], const uint8_t* p, size_t blocks);
void ctrHw(const Aes& aes, uint8_t c[16], const uint8_t* in, uint8_t* out, size_t blocks);
#endif

void ghashBlocks(uint8_t y[16], const Gcm& g, const uint8_t* p, size_t n) {
#if AES_HW
    if (g_hw && n >= 16) {
        ghashHw(y, g.hk, p, n / 16);
        p += n & ~(size_t)15, n &= 15;
    }
#endif
    while (n) {
        size_t take = n < 16 ? n : 16;
        for (size_t i = 0; i < take; i++) y[i] ^= p[i];
        g.mulH(y);
        p += take, n -= take;
    }
}

// (the reduction of the 4 bits shifted out, as GCM's polynomial folds them back in)
const uint64_t LAST4[16] = {0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                            0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

// GHASH by carry-less multiply. GCM keeps a block's bits backwards (byte 0's top bit is x^0): with
// each byte's bits reversed, the block read as a little-endian 128-bit number has x^i at bit i, and
// y*H is a plain carry-less 128x128 multiply (Karatsuba: three 64x64s), then the top 128 bits folded
// back in by GCM's polynomial, x^128 = x^7 + x^2 + x + 1 (0x87): twice, as the first fold spills over.
struct U128 {
    uint64_t lo, hi;
};

U128 clmulSoft(uint64_t a, uint64_t b) {
    U128 r = {0, 0};
    for (int i = 0; i < 64; i++)
        if ((b >> i) & 1) r.lo ^= a << i, r.hi ^= i ? a >> (64 - i) : 0;
    return r;
}

uint8_t rbit8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

// (a0, a1: y's words; the product reduced: r0, r1)
template <class Clmul>
void ghashMul(uint64_t a0, uint64_t a1, const uint64_t hk[2], uint64_t* r0, uint64_t* r1, Clmul clmul) {
    U128 z0 = clmul(a0, hk[0]), z2 = clmul(a1, hk[1]), zm = clmul(a0 ^ a1, hk[0] ^ hk[1]);
    uint64_t m0 = zm.lo ^ z0.lo ^ z2.lo, m1 = zm.hi ^ z0.hi ^ z2.hi;
    uint64_t p0 = z0.lo, p1 = z0.hi ^ m0, p2 = z2.lo ^ m1, p3 = z2.hi;
    U128 t = clmul(p2, 0x87), u = clmul(p3, 0x87), v = clmul(u.hi, 0x87);
    *r0 = p0 ^ t.lo ^ v.lo;
    *r1 = p1 ^ t.hi ^ u.lo;
}

#if AES_HW
inline U128 clmulHw(uint64_t a, uint64_t b) {
    uint64x2_t v = vreinterpretq_u64_p128(vmull_p64((poly64_t)a, (poly64_t)b));
    return U128{vgetq_lane_u64(v, 0), vgetq_lane_u64(v, 1)};
}
#endif

void inc32(uint8_t c[16]) {
    for (int i = 15; i >= 12; i--)
        if (++c[i]) break;
}

#if AES_HW
// The fast ways, for whole blocks (what video's frames are nearly all of): AES-CTR four blocks at a time
// (each AESE waits on the one before it in its block: four blocks side by side keep the unit busy), the
// round keys held in registers; and GHASH kept in vector registers from block to block, its multiply and
// reduction there too (ghashMul's, the same steps), not through memory and general registers each block.

// c: the counter block, incremented before each block's keystream (as inc32's); its last 4 bytes, big-endian
void ctrHw(const Aes& aes, uint8_t c[16], const uint8_t* in, uint8_t* out, size_t blocks) {
    uint8x16_t k[15];
    for (int r = 0; r <= aes.rounds; r++) k[r] = vld1q_u8(aes.rk + 16 * r);
    const int last = aes.rounds - 1;
    uint32x4_t base = vreinterpretq_u32_u8(vld1q_u8(c));
    uint32_t ctr = (uint32_t)c[12] << 24 | (uint32_t)c[13] << 16 | (uint32_t)c[14] << 8 | c[15];
    while (blocks >= 4) {
        uint8x16_t b0 = vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(ctr + 1), base, 3));
        uint8x16_t b1 = vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(ctr + 2), base, 3));
        uint8x16_t b2 = vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(ctr + 3), base, 3));
        uint8x16_t b3 = vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(ctr + 4), base, 3));
        ctr += 4;
        for (int r = 0; r < last; r++) {
            b0 = vaesmcq_u8(vaeseq_u8(b0, k[r]));
            b1 = vaesmcq_u8(vaeseq_u8(b1, k[r]));
            b2 = vaesmcq_u8(vaeseq_u8(b2, k[r]));
            b3 = vaesmcq_u8(vaeseq_u8(b3, k[r]));
        }
        b0 = veorq_u8(vaeseq_u8(b0, k[last]), k[aes.rounds]);
        b1 = veorq_u8(vaeseq_u8(b1, k[last]), k[aes.rounds]);
        b2 = veorq_u8(vaeseq_u8(b2, k[last]), k[aes.rounds]);
        b3 = veorq_u8(vaeseq_u8(b3, k[last]), k[aes.rounds]);
        vst1q_u8(out, veorq_u8(vld1q_u8(in), b0));
        vst1q_u8(out + 16, veorq_u8(vld1q_u8(in + 16), b1));
        vst1q_u8(out + 32, veorq_u8(vld1q_u8(in + 32), b2));
        vst1q_u8(out + 48, veorq_u8(vld1q_u8(in + 48), b3));
        in += 64, out += 64, blocks -= 4;
    }
    for (; blocks; blocks--, in += 16, out += 16) {
        uint8x16_t b = vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(++ctr), base, 3));
        for (int r = 0; r < last; r++) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
        b = veorq_u8(vaeseq_u8(b, k[last]), k[aes.rounds]);
        vst1q_u8(out, veorq_u8(vld1q_u8(in), b));
    }
    c[12] = (uint8_t)(ctr >> 24), c[13] = (uint8_t)(ctr >> 16), c[14] = (uint8_t)(ctr >> 8), c[15] = (uint8_t)ctr;
}

inline uint64x2_t pmullLo(uint64x2_t a, uint64x2_t b) {
    return vreinterpretq_u64_p128(
        vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u64(a), 0), vgetq_lane_p64(vreinterpretq_p64_u64(b), 0)));
}
inline uint64x2_t pmullHi(uint64x2_t a, uint64x2_t b) {
    return vreinterpretq_u64_p128(vmull_high_p64(vreinterpretq_p64_u64(a), vreinterpretq_p64_u64(b)));
}

// a * H, reduced: ghashMul's steps (Karatsuba; then the top 128 bits folded back in by x^128 = x^7+x^2+x+1).
// a, h: each byte's bits reversed, as two little-endian words; hx's low word: H's two words XORed
inline uint64x2_t gfmulHw(uint64x2_t a, uint64x2_t h, uint64x2_t hx) {
    const uint64x2_t zero = vdupq_n_u64(0), poly = vdupq_n_u64(0x87);
    uint64x2_t z0 = pmullLo(a, h), z2 = pmullHi(a, h);
    uint64x2_t zm = veorq_u64(pmullLo(veorq_u64(a, vextq_u64(a, a, 1)), hx), veorq_u64(z0, z2));
    uint64x2_t lo = veorq_u64(z0, vextq_u64(zero, zm, 1));  // (p0, p1)
    uint64x2_t hi = veorq_u64(z2, vextq_u64(zm, zero, 1));  // (p2, p3)
    uint64x2_t t = pmullLo(hi, poly), u = pmullHi(hi, poly), v = pmullHi(u, poly);
    return veorq_u64(veorq_u64(lo, t), veorq_u64(vextq_u64(zero, u, 1), v));
}

// y (16 bytes, as GCM keeps it) through `blocks` whole blocks of p
void ghashHw(uint8_t y[16], const uint64_t hk[2], const uint8_t* p, size_t blocks) {
    uint64x2_t h = vld1q_u64(hk), hx = vdupq_n_u64(hk[0] ^ hk[1]);
    uint64x2_t a = vreinterpretq_u64_u8(vrbitq_u8(vld1q_u8(y)));
    for (; blocks; blocks--, p += 16) a = gfmulHw(veorq_u64(a, vreinterpretq_u64_u8(vrbitq_u8(vld1q_u8(p)))), h, hx);
    vst1q_u8(y, vrbitq_u8(vreinterpretq_u8_u64(a)));
}
#endif

}  // namespace

bool Gcm::init(const uint8_t* key, size_t key_n) {
    if (!aes.init(key, key_n)) return false;
    uint8_t zero[16] = {0};
    aes.encrypt(zero, h);
    // H's multiples for every 4-bit value (Shoup's method, as mbedTLS's gcm.c)
    uint64_t vh = be64(h), vl = be64(h + 8);
    hl[8] = vl, hh[8] = vh;
    hl[0] = hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (uint32_t)(vl & 1) * 0xe1000000u;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)t << 32);
        hl[i] = vl, hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2) {
        vh = hh[i], vl = hl[i];
        for (int j = 1; j < i; j++) hh[i + j] = vh ^ hh[j], hl[i + j] = vl ^ hl[j];
    }
    uint8_t r[16];
    for (int i = 0; i < 16; i++) r[i] = rbit8(h[i]);
    hk[0] = le64(r), hk[1] = le64(r + 8);
    return true;
}

void Gcm::mulH(uint8_t y[AES_BLOCK]) const {
#if AES_HW
    if (g_hw) {
        uint64x2_t a = vreinterpretq_u64_u8(vrbitq_u8(vld1q_u8(y)));
        uint64_t r0, r1;
        ghashMul(vgetq_lane_u64(a, 0), vgetq_lane_u64(a, 1), hk, &r0, &r1, clmulHw);
        uint64x2_t r = vcombine_u64(vcreate_u64(r0), vcreate_u64(r1));
        vst1q_u8(y, vrbitq_u8(vreinterpretq_u8_u64(r)));
        return;
    }
#endif
    mulHTable(y);
}

void Gcm::mulHClmulSoft(uint8_t y[AES_BLOCK]) const {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = rbit8(y[i]);
    uint64_t r0, r1;
    ghashMul(le64(t), le64(t + 8), hk, &r0, &r1, clmulSoft);
    for (int i = 0; i < 8; i++) y[i] = rbit8((uint8_t)(r0 >> (8 * i))), y[8 + i] = rbit8((uint8_t)(r1 >> (8 * i)));
}

void Gcm::mulHTable(uint8_t y[AES_BLOCK]) const {
    int lo = y[15] & 0xf;
    uint64_t zh = hh[lo], zl = hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = y[i] & 0xf;
        int hi = (y[i] >> 4) & 0xf;
        if (i != 15) {
            int rem = (int)(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ (LAST4[rem] << 48);
            zh ^= hh[lo], zl ^= hl[lo];
        }
        int rem = (int)(zl & 0xf);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4) ^ (LAST4[rem] << 48);
        zh ^= hh[hi], zl ^= hl[hi];
    }
    for (int i = 0; i < 8; i++) y[i] = (uint8_t)(zh >> (56 - 8 * i)), y[8 + i] = (uint8_t)(zl >> (56 - 8 * i));
}

void Gcm::wipeKey() { wipe(this, sizeof(*this)); }

void Gcm::ghash(const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n, uint8_t out[AES_BLOCK]) const {
    uint8_t y[16] = {0};
    ghashBlocks(y, *this, aad, aad_n);
    ghashBlocks(y, *this, ct, n);
    uint8_t len[16];
    uint64_t abits = (uint64_t)aad_n * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(abits >> (56 - 8 * i)), len[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    ghashBlocks(y, *this, len, 16);
    memcpy(out, y, 16);
}

void Gcm::ctr(const uint8_t j0[AES_BLOCK], const uint8_t* in, size_t n, uint8_t* out) const {
    uint8_t c[16], ks[16];
    memcpy(c, j0, 16);
#if AES_HW
    if (g_hw && n >= 16) {
        ctrHw(aes, c, in, out, n / 16);
        size_t done = n & ~(size_t)15;
        in += done, out += done, n -= done;
    }
#endif
    while (n) {
        inc32(c);
        aes.encrypt(c, ks);
        size_t take = n < 16 ? n : 16;
        for (size_t i = 0; i < take; i++) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += take, out += take, n -= take;
    }
    wipe(ks, sizeof(ks));
}

void Gcm::seal(const uint8_t iv[GCM_IV], const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n, uint8_t* ct, uint8_t* tag,
               size_t tag_n) const {
    uint8_t j0[16] = {0};
    memcpy(j0, iv, GCM_IV);
    j0[15] = 1;
    ctr(j0, pt, n, ct);
    uint8_t s[16], ek[16];
    ghash(aad, aad_n, ct, n, s);
    aes.encrypt(j0, ek);
    for (size_t i = 0; i < tag_n && i < 16; i++) tag[i] = (uint8_t)(s[i] ^ ek[i]);
}

bool Gcm::open(const uint8_t iv[GCM_IV], const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n, const uint8_t* tag, size_t tag_n,
               uint8_t* pt) const {
    if (tag_n < 4 || tag_n > 16) return false;
    uint8_t j0[16] = {0};
    memcpy(j0, iv, GCM_IV);
    j0[15] = 1;
    uint8_t s[16], ek[16];
    ghash(aad, aad_n, ct, n, s);
    aes.encrypt(j0, ek);
    uint8_t diff = 0;
    for (size_t i = 0; i < tag_n; i++) diff |= (uint8_t)(s[i] ^ ek[i] ^ tag[i]);
    if (diff) return false;
    ctr(j0, ct, n, pt);
    return true;
}

// --- GCM in pieces ---

void GcmStream::start(const Gcm& gcm, const uint8_t iv[GCM_IV]) {
    g = &gcm;
    memset(j0, 0, sizeof(j0));
    memcpy(j0, iv, GCM_IV);
    j0[15] = 1;
    memcpy(c, j0, sizeof(c));
    memset(y, 0, sizeof(y));
    ks_left = part_n = 0;
    aad_n = ct_n = 0;
    in_ct = false;
}

// (GHASH takes whole blocks: the AAD's last partial block is padded with zeros before the ciphertext's start)
void GcmStream::hash(const uint8_t* p, size_t n) {
    // a block begun by an earlier piece finished first (DAVE's pieces are any length: without this, once
    // one ended mid-block every block after it went the slow way, copied through part)
    if (part_n) {
        size_t take = 16 - part_n < n ? 16 - part_n : n;
        memcpy(part + part_n, p, take);
        part_n += take, p += take, n -= take;
        if (part_n < 16) return;
        for (int i = 0; i < 16; i++) y[i] ^= part[i];
        g->mulH(y);
        part_n = 0;
    }
    // whole blocks straight from p
#if AES_HW
    if (g_hw && n >= 16) {
        ghashHw(y, g->hk, p, n / 16);
        p += n & ~(size_t)15, n &= 15;
    }
#endif
    for (; n >= 16; p += 16, n -= 16) {
        for (int i = 0; i < 16; i++) y[i] ^= p[i];
        g->mulH(y);
    }
    memcpy(part, p, n);  // (the rest: the start of the next block)
    part_n = n;
}

void GcmStream::flushPart() {
    if (!part_n) return;
    for (size_t i = 0; i < part_n; i++) y[i] ^= part[i];
    g->mulH(y);
    part_n = 0;
}

void GcmStream::aad(const uint8_t* p, size_t n) {
    hash(p, n);
    aad_n += n;
}

void GcmStream::encrypt(uint8_t* p, size_t n) {
    if (!in_ct) {
        flushPart();
        in_ct = true;
    }
    keystream(p, n);
    hash(p, n);
    ct_n += n;
}

// (the keystream XORed in a block's worth at a time, not a byte and a test at a time: video's whole frames
// go through here)
void GcmStream::keystream(uint8_t* p, size_t n) {
    while (n) {
#if AES_HW
        if (!ks_left && g_hw && n >= 16) {
            ctrHw(g->aes, c, p, p, n / 16);
            size_t done = n & ~(size_t)15;
            p += done, n -= done;
            continue;
        }
#endif
        if (!ks_left) {
            inc32(c);
            g->aes.encrypt(c, ks);
            ks_left = 16;
        }
        size_t take = ks_left < n ? ks_left : n;
        const uint8_t* k = ks + (16 - ks_left);
        for (size_t i = 0; i < take; i++) p[i] ^= k[i];
        p += take, n -= take, ks_left -= take;
    }
}

void GcmStream::decrypt(uint8_t* p, size_t n) {
    if (!in_ct) {
        flushPart();
        in_ct = true;
    }
    hash(p, n);
    ct_n += n;
    keystream(p, n);
}

void GcmStream::finish(uint8_t* tag, size_t tag_n) {
    flushPart();
    uint8_t len[16];
    uint64_t abits = aad_n * 8, cbits = ct_n * 8;
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(abits >> (56 - 8 * i)), len[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    for (int i = 0; i < 16; i++) y[i] ^= len[i];
    g->mulH(y);
    uint8_t ek[16];
    g->aes.encrypt(j0, ek);
    for (size_t i = 0; i < tag_n && i < 16; i++) tag[i] = (uint8_t)(y[i] ^ ek[i]);
    wipe(ks, sizeof(ks));
}

bool aesSelfTest(const char** what) {
#if AES_HW
    // FIPS 197's examples (C.1, C.3), then GHASH both ways on a few blocks
    static const uint8_t key[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t ct128[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    static const uint8_t ct256[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    bool ok = true;
    for (int k = 0; k < 2 && ok; k++) {
        Gcm g;
        g.init(key, k ? 32 : 16);
        uint8_t out[16], soft[16];
        g.aes.encrypt(pt, out);
        g.aes.encryptSoft(pt, soft);
        ok = !memcmp(out, k ? ct256 : ct128, 16) && !memcmp(soft, out, 16);
        uint8_t y[16], z[16];
        for (int i = 0; i < 16; i++) y[i] = (uint8_t)(i * 37 + k * 101 + 5);
        for (int round = 0; round < 8 && ok; round++) {
            memcpy(z, y, 16);
            g.mulH(y);
            g.mulHTable(z);
            ok = !memcmp(y, z, 16);
            y[round] ^= 0xA5;
        }
        // whole messages, and DAVE's way in pieces, the fast way against the plain one (g_hw off): lengths
        // around the 16-byte blocks and the 4-block batches
        static uint8_t msg[300], ct_hw[300], ct_soft[300], piece[300];
        uint8_t aad[20], iv[12], tag_hw[16], tag_soft[16], tag_st[16];
        for (int i = 0; i < 300; i++) msg[i] = (uint8_t)(i * 7 + k * 13 + 3);
        for (int i = 0; i < 20; i++) aad[i] = (uint8_t)(i * 11 + 1);
        for (int i = 0; i < 12; i++) iv[i] = (uint8_t)(i * 5 + k);
        static const size_t lens[] = {0, 1, 15, 16, 17, 47, 63, 64, 65, 80, 129, 300};
        for (size_t len : lens) {
            if (!ok) break;
            g.seal(iv, aad, sizeof(aad), msg, len, ct_hw, tag_hw, 16);
            g_hw = false;
            g.seal(iv, aad, sizeof(aad), msg, len, ct_soft, tag_soft, 16);
            g_hw = true;
            ok = !memcmp(ct_hw, ct_soft, len) && !memcmp(tag_hw, tag_soft, 16);
            // in pieces: odd sizes (the keystream's leftovers), then 48 at a time (whole blocks, the fast way)
            for (int way = 0; way < 2 && ok; way++) {
                GcmStream st;
                st.start(g, iv);
                st.aad(aad, 7);
                st.aad(aad + 7, sizeof(aad) - 7);
                memcpy(piece, msg, len);
                for (size_t at = 0, step = way ? 48 : 5; at < len; at += step, step = way ? 48 : step * 3 % 97 + 1)
                    st.encrypt(piece + at, step < len - at ? step : len - at);
                st.finish(tag_st, 16);
                ok = !memcmp(piece, ct_soft, len) && !memcmp(tag_st, tag_soft, 16);
            }
        }
        g.wipeKey();
    }
    g_hw = ok;
    *what = ok ? "crypto: the CPU's AES and carry-less multiply, checked" : "crypto: the CPU's AES disagreed with the plain one: plain C from now on (slow)";
    return ok;
#else
    *what = "crypto: plain C (no crypto instructions here)";
    return false;
#endif
}
