#include "mlskeys.h"

#include <string.h>

namespace {

const uint8_t ZEROS[MLS_NH] = {0};

// Everything from epoch_secret on (joiner and welcome are already in out)
bool derive(const uint8_t* psk_secret, const Bytes& group_context, EpochKeys* out) {
    uint8_t member[MLS_NH], epoch[MLS_NH];
    mlsExtract(out->joiner, MLS_NH, psk_secret ? psk_secret : ZEROS, MLS_NH, member);
    bool ok = mlsExpand(member, "epoch", group_context.p, group_context.n, epoch, MLS_NH) &&
              mlsDeriveSecret(epoch, "sender data", out->sender_data) && mlsDeriveSecret(epoch, "encryption", out->encryption) &&
              mlsDeriveSecret(epoch, "exporter", out->exporter) && mlsDeriveSecret(epoch, "external", out->external) &&
              mlsDeriveSecret(epoch, "confirm", out->confirmation_key) && mlsDeriveSecret(epoch, "membership", out->membership_key) &&
              mlsDeriveSecret(epoch, "resumption", out->resumption_psk) &&
              mlsDeriveSecret(epoch, "authentication", out->epoch_authenticator) && mlsDeriveSecret(epoch, "init", out->init);
    wipe(member, sizeof(member));
    wipe(epoch, sizeof(epoch));
    return ok;
}

}  // namespace

void EpochKeys::wipeAll() { wipe(this, sizeof(*this)); }

bool mlsWelcomeSecret(const uint8_t joiner[MLS_NH], const uint8_t* psk_secret, uint8_t out[MLS_NH]) {
    uint8_t member[MLS_NH];
    mlsExtract(joiner, MLS_NH, psk_secret ? psk_secret : ZEROS, MLS_NH, member);
    bool ok = mlsDeriveSecret(member, "welcome", out);
    wipe(member, sizeof(member));
    return ok;
}

bool mlsKeySchedule(const uint8_t init_prev[MLS_NH], const uint8_t commit_secret[MLS_NH], const uint8_t* psk_secret,
                    const Bytes& group_context, EpochKeys* out) {
    uint8_t prk[MLS_NH];
    mlsExtract(init_prev, MLS_NH, commit_secret, MLS_NH, prk);
    bool ok = mlsExpand(prk, "joiner", group_context.p, group_context.n, out->joiner, MLS_NH);
    wipe(prk, sizeof(prk));
    return ok && mlsKeyScheduleFromJoiner(out->joiner, psk_secret, group_context, out);
}

bool mlsKeyScheduleFromJoiner(const uint8_t joiner[MLS_NH], const uint8_t* psk_secret, const Bytes& group_context, EpochKeys* out) {
    if (joiner != out->joiner) memcpy(out->joiner, joiner, MLS_NH);
    return mlsWelcomeSecret(out->joiner, psk_secret, out->welcome) && derive(psk_secret, group_context, out);
}

bool mlsExport(const uint8_t exporter_secret[MLS_NH], const void* label, size_t label_n, const void* ctx, size_t ctx_n, uint8_t* out,
               size_t len) {
    uint8_t derived[MLS_NH], hash[MLS_NH];
    sha256(ctx, ctx_n, hash);
    bool ok = mlsExpandBytes(exporter_secret, label, label_n, nullptr, 0, derived, MLS_NH) &&
              mlsExpand(derived, "exported", hash, MLS_NH, out, len);
    wipe(derived, sizeof(derived));
    return ok;
}
