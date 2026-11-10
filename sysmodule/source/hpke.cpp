#include "hpke.h"

#include <string.h>

#include "sha256.h"

namespace {

const uint8_t KEM_SUITE[5] = {'K', 'E', 'M', 0x00, 0x10};
const uint8_t HPKE_SUITE[10] = {'H', 'P', 'K', 'E', 0x00, 0x10, 0x00, 0x01, 0x00, 0x01};

struct Suite {
    const uint8_t* id;
    size_t n;
};
const Suite KEM = {KEM_SUITE, sizeof(KEM_SUITE)};
const Suite HPKE = {HPKE_SUITE, sizeof(HPKE_SUITE)};

// LabeledExtract(salt, label, ikm) = Extract(salt, "HPKE-v1" || suite_id || label || ikm)
// (ikm in two parts, one after the other: MLS's info is a label then a context of any size)
void labeledExtract(const Suite& s, const uint8_t* salt, size_t salt_n, const char* label, const uint8_t* ikm, size_t ikm_n,
                    uint8_t prk[32], const uint8_t* ikm2 = nullptr, size_t ikm2_n = 0) {
    static const uint8_t zeros[32] = {0};
    HmacSha256 m;
    if (salt_n == 0) salt = zeros, salt_n = 32;
    m.init(salt, salt_n);
    m.update("HPKE-v1", 7);
    m.update(s.id, s.n);
    m.update(label, strlen(label));
    m.update(ikm, ikm_n);
    m.update(ikm2, ikm2_n);
    m.final(prk);
}

// LabeledExpand(prk, label, info, L) = Expand(prk, I2OSP(L, 2) || "HPKE-v1" || suite_id || label || info, L)
bool labeledExpand(const Suite& s, const uint8_t prk[32], const char* label, const uint8_t* info, size_t info_n, uint8_t* out, size_t l) {
    uint8_t buf[256];
    size_t label_n = strlen(label), at = 0;
    if (2 + 7 + s.n + label_n + info_n > sizeof(buf)) return false;
    buf[at++] = (uint8_t)(l >> 8), buf[at++] = (uint8_t)l;
    memcpy(buf + at, "HPKE-v1", 7), at += 7;
    memcpy(buf + at, s.id, s.n), at += s.n;
    memcpy(buf + at, label, label_n), at += label_n;
    memcpy(buf + at, info, info_n), at += info_n;
    return hkdfExpand(prk, buf, at, out, l);
}

// ExtractAndExpand: the KEM's shared secret from the DH output and kem_context (enc || pkR)
void extractAndExpand(const uint8_t dh[32], const uint8_t* kem_context, size_t n, uint8_t ss[HPKE_SECRET]) {
    uint8_t prk[32];
    labeledExtract(KEM, nullptr, 0, "eae_prk", dh, 32, prk);
    labeledExpand(KEM, prk, "shared_secret", kem_context, n, ss, HPKE_SECRET);
    wipe(prk, sizeof(prk));
}

// The key schedule, base mode (no PSK)
bool keySchedule(const uint8_t ss[HPKE_SECRET], const uint8_t* info, size_t info_n, const uint8_t* info2, size_t info2_n,
                 HpkeContext* ctx) {
    uint8_t ksc[1 + 32 + 32], secret[32];
    ksc[0] = 0;  // mode_base
    labeledExtract(HPKE, nullptr, 0, "psk_id_hash", nullptr, 0, ksc + 1);
    labeledExtract(HPKE, nullptr, 0, "info_hash", info, info_n, ksc + 33, info2, info2_n);
    labeledExtract(HPKE, ss, HPKE_SECRET, "secret", nullptr, 0, secret);
    bool ok = labeledExpand(HPKE, secret, "key", ksc, sizeof(ksc), ctx->key, HPKE_KEY) &&
              labeledExpand(HPKE, secret, "base_nonce", ksc, sizeof(ksc), ctx->base_nonce, HPKE_NONCE) &&
              labeledExpand(HPKE, secret, "exp", ksc, sizeof(ksc), ctx->exporter_secret, HPKE_SECRET) &&
              ctx->gcm.init(ctx->key, HPKE_KEY);
    ctx->seq = 0;
    wipe(secret, sizeof(secret));
    return ok;
}

void nonceFor(const HpkeContext* ctx, uint8_t nonce[HPKE_NONCE]) {
    memcpy(nonce, ctx->base_nonce, HPKE_NONCE);
    for (int i = 0; i < 8; i++) nonce[HPKE_NONCE - 1 - i] ^= (uint8_t)(ctx->seq >> (8 * i));
}

}  // namespace

bool hpkeDeriveKeyPair(const uint8_t* ikm, size_t ikm_n, uint8_t sk[P256_PRIV], uint8_t pk[P256_PUB]) {
    uint8_t prk[32];
    labeledExtract(KEM, nullptr, 0, "dkp_prk", ikm, ikm_n, prk);
    bool ok = false;
    for (int counter = 0; counter < 256 && !ok; counter++) {
        uint8_t c = (uint8_t)counter;
        labeledExpand(KEM, prk, "candidate", &c, 1, sk, P256_PRIV);
        // (P-256's bitmask is 0xff: nothing to mask) 0 or >= n: the next candidate
        ok = p256ValidPrivate(sk);
    }
    wipe(prk, sizeof(prk));
    return ok && p256Public(sk, pk);
}

bool hpkeSetupBaseS(const uint8_t pkR[P256_PUB], const uint8_t* info, size_t info_n, const uint8_t ikmE[32], uint8_t enc[HPKE_ENC],
                    HpkeContext* ctx, uint8_t shared_secret_out[HPKE_SECRET], const uint8_t* info2, size_t info2_n) {
    uint8_t skE[P256_PRIV], dh[32], kem_context[2 * P256_PUB], ss[HPKE_SECRET];
    bool ok = hpkeDeriveKeyPair(ikmE, 32, skE, enc) && p256Ecdh(skE, pkR, dh);
    if (ok) {
        memcpy(kem_context, enc, P256_PUB);
        memcpy(kem_context + P256_PUB, pkR, P256_PUB);
        extractAndExpand(dh, kem_context, sizeof(kem_context), ss);
        ok = keySchedule(ss, info, info_n, info2, info2_n, ctx);
        if (shared_secret_out) memcpy(shared_secret_out, ss, HPKE_SECRET);
    }
    wipe(skE, sizeof(skE));
    wipe(dh, sizeof(dh));
    wipe(ss, sizeof(ss));
    return ok;
}

bool hpkeSetupBaseR(const uint8_t enc[HPKE_ENC], const uint8_t skR[P256_PRIV], const uint8_t* info, size_t info_n, HpkeContext* ctx,
                    uint8_t shared_secret_out[HPKE_SECRET], const uint8_t* info2, size_t info2_n) {
    uint8_t pkR[P256_PUB], dh[32], kem_context[2 * P256_PUB], ss[HPKE_SECRET];
    bool ok = p256Public(skR, pkR) && p256Ecdh(skR, enc, dh);
    if (ok) {
        memcpy(kem_context, enc, P256_PUB);
        memcpy(kem_context + P256_PUB, pkR, P256_PUB);
        extractAndExpand(dh, kem_context, sizeof(kem_context), ss);
        ok = keySchedule(ss, info, info_n, info2, info2_n, ctx);
        if (shared_secret_out) memcpy(shared_secret_out, ss, HPKE_SECRET);
    }
    wipe(dh, sizeof(dh));
    wipe(ss, sizeof(ss));
    return ok;
}

void HpkeContext::seal(const uint8_t* aad, size_t aad_n, const uint8_t* pt, size_t n, uint8_t* ct_and_tag) {
    uint8_t nonce[HPKE_NONCE];
    nonceFor(this, nonce);
    gcm.seal(nonce, aad, aad_n, pt, n, ct_and_tag, ct_and_tag + n, GCM_TAG);
    seq++;
}

bool HpkeContext::open(const uint8_t* aad, size_t aad_n, const uint8_t* ct_and_tag, size_t n, uint8_t* pt) {
    if (n < GCM_TAG) return false;
    uint8_t nonce[HPKE_NONCE];
    nonceFor(this, nonce);
    if (!gcm.open(nonce, aad, aad_n, ct_and_tag, n - GCM_TAG, ct_and_tag + n - GCM_TAG, GCM_TAG, pt)) return false;
    seq++;
    return true;
}

bool HpkeContext::exportSecret(const uint8_t* ctx, size_t ctx_n, uint8_t* out, size_t l) {
    return labeledExpand(HPKE, exporter_secret, "sec", ctx, ctx_n, out, l);
}

void HpkeContext::wipeAll() { wipe(this, sizeof(*this)); }
