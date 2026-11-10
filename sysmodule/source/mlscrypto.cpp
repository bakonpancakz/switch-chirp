#include "mlscrypto.h"

#include <string.h>

namespace {

// "MLS 1.0 " + label, as a vector, into w
void labelVec(Writer& w, const void* label, size_t n) {
    w.varint((uint32_t)(8 + n));
    w.raw("MLS 1.0 ", 8);
    w.raw(label, n);
}
void labelVec(Writer& w, const char* label) { labelVec(w, label, strlen(label)); }

}  // namespace

bool mlsExpandBytes(const uint8_t secret[MLS_NH], const void* label, size_t label_n, const void* ctx, size_t ctx_n, uint8_t* out,
                    size_t len) {
    uint8_t buf[512];
    Writer w(buf, sizeof(buf));
    w.u16((uint16_t)len);
    labelVec(w, label, label_n);
    w.vec(ctx, ctx_n);
    return w.ok && hkdfExpand(secret, buf, w.n, out, len);
}

bool mlsExpand(const uint8_t secret[MLS_NH], const char* label, const void* ctx, size_t ctx_n, uint8_t* out, size_t len) {
    return mlsExpandBytes(secret, label, strlen(label), ctx, ctx_n, out, len);
}

bool mlsDeriveSecret(const uint8_t secret[MLS_NH], const char* label, uint8_t out[MLS_NH]) {
    return mlsExpand(secret, label, nullptr, 0, out, MLS_NH);
}

bool mlsDeriveTreeSecret(const uint8_t secret[MLS_NH], const char* label, uint32_t generation, uint8_t* out, size_t len) {
    return mlsDeriveTreeSecretN(secret, MLS_NH, label, generation, out, len);
}

bool mlsDeriveTreeSecretN(const uint8_t* secret, size_t secret_n, const char* label, uint32_t generation, uint8_t* out, size_t len) {
    uint8_t g[4] = {(uint8_t)(generation >> 24), (uint8_t)(generation >> 16), (uint8_t)(generation >> 8), (uint8_t)generation};
    uint8_t buf[128];
    Writer w(buf, sizeof(buf));
    w.u16((uint16_t)len);
    labelVec(w, label);
    w.vec(g, 4);
    return w.ok && hkdfExpandKey(secret, secret_n, buf, w.n, out, len);
}

void mlsExtract(const uint8_t* salt, size_t salt_n, const uint8_t* ikm, size_t ikm_n, uint8_t out[MLS_NH]) {
    // (KDF.Extract is HMAC(salt, ikm) as is: an empty salt is an empty HMAC key, which HMAC pads to zeros anyway)
    hmacSha256(salt, salt_n, ikm, ikm_n, out);
}

void mlsRefHash(const char* label, const void* value, size_t n, uint8_t out[MLS_NH]) {
    Sha256 h;
    h.init();
    uint8_t len[4];
    Writer w(len, sizeof(len));
    w.varint((uint32_t)strlen(label));
    h.update(len, w.n);
    h.update(label, strlen(label));
    Writer v(len, sizeof(len));
    v.varint((uint32_t)n);
    h.update(len, v.n);
    h.update(value, n);
    h.final(out);
}

// SignContent {label<V>, content<V>} for content of any size: written into a buffer
// (MLS signs whole structs; the biggest here are commits and key packages, a few KB)
static uint8_t g_sign_buf[16 * 1024];

bool mlsSign(const uint8_t priv[P256_PRIV], const char* label, const void* content, size_t n, uint8_t sig[P256_SIG_MAX], size_t* sig_n) {
    Writer w(g_sign_buf, sizeof(g_sign_buf));
    labelVec(w, label);
    w.vec(content, n);
    return w.ok && p256Sign(priv, w.buf, w.n, sig, sig_n);
}

bool mlsVerify(const uint8_t* pub, size_t pub_n, const char* label, const void* content, size_t n, const uint8_t* sig, size_t sig_n) {
    if (pub_n != P256_PUB) return false;
    Writer w(g_sign_buf, sizeof(g_sign_buf));
    labelVec(w, label);
    w.vec(content, n);
    return w.ok && p256Verify(pub, w.buf, w.n, sig, sig_n);
}

// EncryptContext {label<V>, context<V>}: the part before the context (which may be big:
// a Welcome's is the whole encrypted GroupInfo), for HPKE to hash with the context after it
static bool encryptContextHead(Writer& w, const char* label, size_t ctx_n) {
    labelVec(w, label);
    w.varint((uint32_t)ctx_n);
    return w.ok;
}

bool mlsEncrypt(const uint8_t pub[P256_PUB], const char* label, const void* ctx, size_t ctx_n, const void* pt, size_t n,
                const uint8_t ikmE[32], uint8_t kem_output[HPKE_ENC], uint8_t* ct) {
    uint8_t info[128];
    Writer w(info, sizeof(info));
    if (!encryptContextHead(w, label, ctx_n)) return false;
    HpkeContext h;
    if (!hpkeSetupBaseS(pub, info, w.n, ikmE, kem_output, &h, nullptr, (const uint8_t*)ctx, ctx_n)) return false;
    h.seal(nullptr, 0, (const uint8_t*)pt, n, ct);
    h.wipeAll();
    return true;
}

bool mlsDecrypt(const uint8_t priv[P256_PRIV], const char* label, const void* ctx, size_t ctx_n, const uint8_t* kem_output, size_t kem_n,
                const uint8_t* ct, size_t ct_n, uint8_t* pt) {
    if (kem_n != HPKE_ENC) return false;
    uint8_t info[128];
    Writer w(info, sizeof(info));
    if (!encryptContextHead(w, label, ctx_n)) return false;
    HpkeContext h;
    if (!hpkeSetupBaseR(kem_output, priv, info, w.n, &h, nullptr, (const uint8_t*)ctx, ctx_n)) return false;
    bool ok = h.open(nullptr, 0, ct, ct_n, pt);
    h.wipeAll();
    return ok;
}
