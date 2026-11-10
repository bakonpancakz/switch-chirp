// HPKE (RFC 9180), base mode, for the one suite MLS ciphersuite 2 uses:
// DHKEM(P-256, HKDF-SHA256), HKDF-SHA256, AES-128-GCM. MLS seals group
// secrets to a member's public key with it (and derives its tree's keys with
// DeriveKeyPair).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "aes.h"
#include "p256.h"

#define HPKE_SECRET 32
#define HPKE_KEY 16
#define HPKE_NONCE 12
#define HPKE_ENC P256_PUB

// sk, pk from input keying material (RFC 9180 section 7.1.3), as MLS derives its tree's keys
bool hpkeDeriveKeyPair(const uint8_t* ikm, size_t ikm_n, uint8_t sk[P256_PRIV], uint8_t pk[P256_PUB]);

struct HpkeContext {
    Gcm gcm;
    uint8_t key[HPKE_KEY];
    uint8_t base_nonce[HPKE_NONCE];
    uint8_t exporter_secret[HPKE_SECRET];
    uint64_t seq;

    void seal(const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n, uint8_t* ct_and_tag);  // n + 16 bytes
    bool open(const uint8_t* aad, size_t aad_n, const uint8_t* ct_and_tag, size_t n, uint8_t* pt);  // n includes the tag
    bool exportSecret(const uint8_t* ctx, size_t ctx_n, uint8_t* out, size_t l);
    void wipeAll();
};

// The sender's side: ephemeral key from ikmE (32 random bytes; the RFC's vectors give theirs), enc to send
// (info2: more info, after info, when it comes in two parts)
bool hpkeSetupBaseS(const uint8_t pkR[P256_PUB], const uint8_t* info, size_t info_n, const uint8_t ikmE[32], uint8_t enc[HPKE_ENC],
                    HpkeContext* ctx, uint8_t shared_secret_out[HPKE_SECRET] = nullptr, const uint8_t* info2 = nullptr,
                    size_t info2_n = 0);
// The receiver's side
bool hpkeSetupBaseR(const uint8_t enc[HPKE_ENC], const uint8_t skR[P256_PRIV], const uint8_t* info, size_t info_n, HpkeContext* ctx,
                    uint8_t shared_secret_out[HPKE_SECRET] = nullptr, const uint8_t* info2 = nullptr, size_t info2_n = 0);
