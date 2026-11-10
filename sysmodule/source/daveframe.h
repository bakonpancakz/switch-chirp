// DAVE's media frames (Discord's protocol.md and libdave): each speaker's key
// for an epoch comes from the MLS exporter ("Discord Secure Frames v0", the
// user id as 8 little-endian bytes, 16 bytes), then a hash ratchet per
// generation. An Opus frame is encrypted whole with AES-128-GCM:
//   [ciphertext][tag: 8 bytes][nonce: ULEB128][ranges: none][size: 1 byte][0xFA 0xFA]
// The 12-byte nonce is 8 zero bytes then the 32-bit counter, little-endian;
// the generation is the counter's top byte. Silence (F8 FF FE) passes as is.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "aes.h"
#include "mlscrypto.h"

#define DAVE_LABEL "Discord Secure Frames v0"
#define DAVE_KEY 16
#define DAVE_TAG 8
#define DAVE_SUPPLEMENTAL_MAX (DAVE_TAG + 5 + 1 + 2)  // (a 32-bit ULEB128 is 5 bytes at most)

// One speaker's keys for one epoch: the ratchet from their base secret
struct DaveRatchet {
    uint64_t user;
    uint8_t secret[MLS_NH];   // the next generation's ratchet secret
    size_t secret_n;          // (16 at first: the base secret; 32 after)
    uint32_t next_gen;        // which generation `secret` is for
    uint32_t key_gen;         // which generation `key` is (valid when has_key)
    bool has_key;
    Gcm key;                  // AES-128-GCM with that generation's key

    bool init(uint64_t user_id, const uint8_t base[DAVE_KEY]);
    // The cipher for a generation (ratcheting forward as needed; an older one than we've passed is gone)
    const Gcm* forGeneration(uint32_t gen);
    void wipeAll();
};

// The base secret for a speaker, from the epoch's MLS exporter secret
bool daveBaseSecret(const uint8_t exporter_secret[MLS_NH], uint64_t user, uint8_t out[DAVE_KEY]);

bool daveIsSilence(const uint8_t* p, size_t n);

// Decrypts a DAVE frame into out (at least n bytes); false if it isn't one, or doesn't open
bool daveDecrypt(DaveRatchet& r, const uint8_t* frame, size_t n, uint8_t* out, size_t* out_n);
// Encrypts an Opus frame (our own, to send): counter is our running nonce, pre-incremented
bool daveEncrypt(DaveRatchet& r, uint32_t* counter, const uint8_t* opus, size_t n, uint8_t* out, size_t cap, size_t* out_n);
// An H.264 frame (Annex-B: start codes, one access unit), encrypted in place as DAVE has it (libdave's
// codec_utils): start codes made 4 bytes and left readable, as are each slice's NAL header and its
// slice header up to pic_parameter_set_id, and every non-slice NAL whole; the rest encrypted, the
// readable bytes authenticated; the trailer appended with the readable ranges. A nonce that puts a
// start code in the ciphertext is passed over for the next. buf: cap bytes (the frame grows: 3-byte
// start codes, and the trailer); false: dropped (not a frame, or too many ranges)
bool daveEncryptH264(DaveRatchet& r, uint32_t* counter, uint8_t* buf, size_t n, size_t cap, size_t* out_n);
