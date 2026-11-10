// MLS's labeled crypto (RFC 9420 section 5) for ciphersuite 2
// (MLS_128_DHKEMP256_AES128GCM_SHA256_P256), DAVE's: every hash, key
// derivation, signature and public-key encryption in MLS goes through these,
// with "MLS 1.0 " and a label in front, so no two uses can be confused.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "hpke.h"
#include "mlswire.h"
#include "p256.h"
#include "sha256.h"

#define MLS_SUITE 2
#define MLS_NH 32  // hash and secret size
#define MLS_NK 16  // AEAD key
#define MLS_NN 12  // AEAD nonce

// ExpandWithLabel(secret, label, context, length)
bool mlsExpand(const uint8_t secret[MLS_NH], const char* label, const void* ctx, size_t ctx_n, uint8_t* out, size_t len);
// (with a label that's bytes, not text: the exporter's)
bool mlsExpandBytes(const uint8_t secret[MLS_NH], const void* label, size_t label_n, const void* ctx, size_t ctx_n, uint8_t* out,
                    size_t len);
// DeriveSecret(secret, label): ExpandWithLabel with no context, Nh bytes
bool mlsDeriveSecret(const uint8_t secret[MLS_NH], const char* label, uint8_t out[MLS_NH]);
// DeriveTreeSecret(secret, label, generation, length)
bool mlsDeriveTreeSecret(const uint8_t secret[MLS_NH], const char* label, uint32_t generation, uint8_t* out, size_t len);
// (from a secret of another length: DAVE's ratchets start from the exporter's 16 bytes)
bool mlsDeriveTreeSecretN(const uint8_t* secret, size_t secret_n, const char* label, uint32_t generation, uint8_t* out, size_t len);
// KDF.Extract(salt, ikm)
void mlsExtract(const uint8_t* salt, size_t salt_n, const uint8_t* ikm, size_t ikm_n, uint8_t out[MLS_NH]);
// RefHash(label, value): Hash of {label<V>, value<V>} (label in full, e.g. "MLS 1.0 KeyPackage Reference")
void mlsRefHash(const char* label, const void* value, size_t n, uint8_t out[MLS_NH]);

// SignWithLabel / VerifyWithLabel: ECDSA P-256 over {"MLS 1.0 " + label <V>, content<V>}
bool mlsSign(const uint8_t priv[P256_PRIV], const char* label, const void* content, size_t n, uint8_t sig[P256_SIG_MAX], size_t* sig_n);
bool mlsVerify(const uint8_t* pub, size_t pub_n, const char* label, const void* content, size_t n, const uint8_t* sig, size_t sig_n);

// EncryptWithLabel / DecryptWithLabel: HPKE base mode, info {"MLS 1.0 " + label <V>, context<V>}, no aad.
// ct gets n + 16 bytes; ikmE: 32 random bytes for the ephemeral key.
bool mlsEncrypt(const uint8_t pub[P256_PUB], const char* label, const void* ctx, size_t ctx_n, const void* pt, size_t n,
                const uint8_t ikmE[32], uint8_t kem_output[HPKE_ENC], uint8_t* ct);
bool mlsDecrypt(const uint8_t priv[P256_PRIV], const char* label, const void* ctx, size_t ctx_n, const uint8_t* kem_output, size_t kem_n,
                const uint8_t* ct, size_t ct_n, uint8_t* pt);
