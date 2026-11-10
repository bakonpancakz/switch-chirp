// ChaCha20 and Poly1305 (RFC 8439), HChaCha20 and XChaCha20-Poly1305
// (draft-irtf-cfrg-xchacha): Discord's other voice transport mode,
// aead_xchacha20_poly1305_rtpsize, which every client has to support.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CHACHA_KEY 32
#define XCHACHA_NONCE 24
#define POLY1305_TAG 16

// in XOR the ChaCha20 stream (key, 12-byte nonce, from block `counter`); out may be in
void chacha20(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, size_t n, uint8_t* out);
void hchacha20(const uint8_t key[CHACHA_KEY], const uint8_t nonce[16], uint8_t out[32]);
void poly1305(const uint8_t key[32], const uint8_t* msg, size_t n, uint8_t tag[POLY1305_TAG]);

// The AEAD (RFC 8439 section 2.8) with a 12-byte nonce, and with XChaCha's 24
void chachaSeal(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n,
                uint8_t* ct, uint8_t tag[POLY1305_TAG]);
bool chachaOpen(const uint8_t key[CHACHA_KEY], const uint8_t nonce[12], const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n,
                const uint8_t tag[POLY1305_TAG], uint8_t* pt);
void xchachaSeal(const uint8_t key[CHACHA_KEY], const uint8_t nonce[XCHACHA_NONCE], const uint8_t* aad, size_t aad_n, const uint8_t* pt,
                 size_t n, uint8_t* ct, uint8_t tag[POLY1305_TAG]);
bool xchachaOpen(const uint8_t key[CHACHA_KEY], const uint8_t nonce[XCHACHA_NONCE], const uint8_t* aad, size_t aad_n, const uint8_t* ct,
                 size_t n, const uint8_t tag[POLY1305_TAG], uint8_t* pt);
