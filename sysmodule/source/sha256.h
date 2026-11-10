// SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and HKDF-SHA256 (RFC 5869):
// what MLS's ciphersuite 2 (and so DAVE) hashes, MACs and derives keys with.
// Nothing allocates; the incremental forms are for messages that come in parts.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE 32
#define SHA256_BLOCK 64

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[SHA256_BLOCK];
    size_t buf_len;
    uint64_t total;

    void init();
    void update(const void* data, size_t n);
    void final(uint8_t out[SHA256_SIZE]);
};

void sha256(const void* data, size_t n, uint8_t out[SHA256_SIZE]);

struct HmacSha256 {
    Sha256 inner, outer;

    void init(const void* key, size_t key_n);
    void update(const void* data, size_t n);
    void final(uint8_t out[SHA256_SIZE]);
};

void hmacSha256(const void* key, size_t key_n, const void* data, size_t n, uint8_t out[SHA256_SIZE]);

// HKDF-Extract: prk = HMAC(salt, ikm) (an empty salt is 32 zero bytes, as the RFC says)
void hkdfExtract(const void* salt, size_t salt_n, const void* ikm, size_t ikm_n, uint8_t prk[SHA256_SIZE]);
// HKDF-Expand: n bytes (at most 255 * 32) of output keying material
bool hkdfExpand(const uint8_t prk[SHA256_SIZE], const void* info, size_t info_n, uint8_t* out, size_t n);
// (with a pseudorandom key of another length: DAVE's ratchets start from a 16-byte one)
bool hkdfExpandKey(const uint8_t* prk, size_t prk_n, const void* info, size_t info_n, uint8_t* out, size_t n);

// Wipes memory in a way the compiler won't leave out (keys, once they're done with)
void wipe(void* p, size_t n);
