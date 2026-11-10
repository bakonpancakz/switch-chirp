// AES-128/256 (FIPS 197) and AES-GCM (NIST SP 800-38D): Discord's voice
// transport (AES-256-GCM), DAVE's frames (AES-128-GCM, tag cut to 8 bytes)
// and MLS ciphersuite 2's AEAD (AES-128-GCM). Encryption only: GCM never
// needs the inverse cipher. Nothing allocates.
//
// Two ways, the same results: plain C, a byte at a time (the reference, and
// what the PC tests run), and on the Switch the CPU's own AES and carry-less
// multiply instructions (ARMv8's crypto extensions: AESE/AESMC, PMULL), many
// times faster, which video needs. aesSelfTest() checks the fast way against
// known answers and the plain one at startup; if they ever disagree, the
// plain one is used.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AES_BLOCK 16
#define GCM_IV 12
#define GCM_TAG 16

struct Aes {
    alignas(16) uint8_t rk[240];  // round keys (15 x 16 bytes for AES-256)
    int rounds;       // 10 or 14

    bool init(const uint8_t* key, size_t key_n);  // 16 or 32 bytes
    void encrypt(const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]) const;
    void encryptSoft(const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]) const;  // (the plain way, always)
};

// The fast way checked against the plain one; false: it disagreed, and the plain one's used from now on
// (or there's no fast way: on the PC). What it found, for the log
bool aesSelfTest(const char** what);

struct Gcm {
    Aes aes;
    uint8_t h[AES_BLOCK];  // the hash key, E(0)
    uint64_t hl[16], hh[16];  // its multiples, 4 bits at a time (GHASH by table: a tenth of the work bit by bit)
    uint64_t hk[2];           // H, each byte's bits reversed, as two little-endian words (GHASH by carry-less multiply)

    // y *= H in GF(2^128)
    void mulH(uint8_t y[AES_BLOCK]) const;
    void mulHTable(uint8_t y[AES_BLOCK]) const;  // (the plain way, always: the reference)
    void mulHClmulSoft(uint8_t y[AES_BLOCK]) const;  // (the carry-less way with a software multiply: for the PC tests)

    bool init(const uint8_t* key, size_t key_n);
    void wipeKey();
    // ct: n bytes (may be pt itself); tag: tag_n bytes (4..16) of the full tag
    void seal(const uint8_t iv[GCM_IV], const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n, uint8_t* ct, uint8_t* tag,
              size_t tag_n) const;
    // false (and pt untouched... or garbage: don't use it) when the tag doesn't match
    bool open(const uint8_t iv[GCM_IV], const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n, const uint8_t* tag, size_t tag_n,
              uint8_t* pt) const;

private:
    void ghash(const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n, uint8_t out[AES_BLOCK]) const;
    void ctr(const uint8_t j0[AES_BLOCK], const uint8_t* in, size_t n, uint8_t* out) const;
};

// The same AES-GCM, fed in pieces: all of the additional data first (any number of calls), then
// the plaintext, encrypted in place (any number of calls), then the tag. For DAVE's video frames,
// whose authenticated and encrypted parts take turns through the frame.
struct GcmStream {
    const Gcm* g;
    uint8_t j0[AES_BLOCK], c[AES_BLOCK], ks[AES_BLOCK], y[AES_BLOCK], part[AES_BLOCK];
    size_t ks_left, part_n;
    uint64_t aad_n, ct_n;
    bool in_ct;

    void start(const Gcm& gcm, const uint8_t iv[GCM_IV]);
    void aad(const uint8_t* p, size_t n);
    void encrypt(uint8_t* p, size_t n);
    void decrypt(uint8_t* p, size_t n);  // (the other way: hashed as it comes, then decrypted)
    void finish(uint8_t* tag, size_t tag_n);

private:
    void hash(const uint8_t* p, size_t n);
    void flushPart();
};
