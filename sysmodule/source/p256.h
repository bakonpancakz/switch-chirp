// NIST P-256 (secp256r1): ECDH and ECDSA with SHA-256, what MLS ciphersuite 2
// (DHKEM(P-256) and ecdsa_secp256r1_sha256) and so DAVE need. Keys travel as
// 32 big-endian bytes (private) and 65 uncompressed bytes 0x04||x||y
// (public); signatures as DER, as MLS sends them.
//
// Scalar multiplication is a Montgomery ladder (the same steps whatever the
// key's bits, past its leading zeros). Signing uses RFC 6979's deterministic
// nonces, so it needs no randomness of its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define P256_PRIV 32
#define P256_PUB 65
#define P256_SIG_MAX 72  // DER of two 32-byte integers

// A private key is valid when it's 1..n-1; false when it isn't
bool p256Public(const uint8_t priv[P256_PRIV], uint8_t pub[P256_PUB]);
bool p256ValidPrivate(const uint8_t priv[P256_PRIV]);
bool p256ValidPublic(const uint8_t pub[P256_PUB]);  // on the curve, not the point at infinity
// The x coordinate of priv * pub (ECDH's shared secret); false for a bad key
bool p256Ecdh(const uint8_t priv[P256_PRIV], const uint8_t pub[P256_PUB], uint8_t secret[32]);

// ECDSA over SHA-256 of msg; sig gets DER, *sig_n its length
bool p256Sign(const uint8_t priv[P256_PRIV], const void* msg, size_t n, uint8_t sig[P256_SIG_MAX], size_t* sig_n);
bool p256Verify(const uint8_t pub[P256_PUB], const void* msg, size_t n, const uint8_t* sig, size_t sig_n);

// (for tests: the raw r and s, and signing with a given nonce k)
bool p256SignRaw(const uint8_t priv[P256_PRIV], const uint8_t hash[32], uint8_t r[32], uint8_t s[32]);
