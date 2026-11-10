// The MLS key schedule (RFC 9420 section 8): from the last epoch's
// init_secret, this epoch's commit_secret and the group context, everything
// the epoch's keys come from. DAVE needs few of them (the exporter, for its
// per-sender media keys; the confirmation and membership keys, to check
// commits; init, for the next epoch), but they're all derived so the
// official test vectors can check every step.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mlscrypto.h"

struct EpochKeys {
    uint8_t joiner[MLS_NH];
    uint8_t welcome[MLS_NH];
    uint8_t sender_data[MLS_NH];
    uint8_t encryption[MLS_NH];
    uint8_t exporter[MLS_NH];
    uint8_t external[MLS_NH];
    uint8_t confirmation_key[MLS_NH];
    uint8_t membership_key[MLS_NH];
    uint8_t resumption_psk[MLS_NH];
    uint8_t epoch_authenticator[MLS_NH];
    uint8_t init[MLS_NH];  // the next epoch's init_secret

    void wipeAll();
};

// A new epoch from a commit. psk_secret: null for none (all zeros, as DAVE always has).
bool mlsKeySchedule(const uint8_t init_prev[MLS_NH], const uint8_t commit_secret[MLS_NH], const uint8_t* psk_secret,
                    const Bytes& group_context, EpochKeys* out);
// The same from the joiner_secret a Welcome brings (welcome is filled in; joiner is copied)
bool mlsKeyScheduleFromJoiner(const uint8_t joiner[MLS_NH], const uint8_t* psk_secret, const Bytes& group_context, EpochKeys* out);
// welcome_secret alone, to open a Welcome's GroupInfo before the group context is known
bool mlsWelcomeSecret(const uint8_t joiner[MLS_NH], const uint8_t* psk_secret, uint8_t out[MLS_NH]);

// MLS-Exporter(label, context, length) with the epoch's exporter_secret
bool mlsExport(const uint8_t exporter_secret[MLS_NH], const void* label, size_t label_n, const void* ctx, size_t ctx_n, uint8_t* out,
               size_t len);
