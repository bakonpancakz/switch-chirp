// An Opus encoder, by hand: CELT-only (the part of Opus made for music and
// low delay), mono, 48 kHz, 20 ms frames, a fixed size per packet. Every
// choice the format leaves to the encoder is made the simplest way: no pitch
// pre-filter, long blocks only, no per-band boosts, the default allocation
// trim. What remains has to match the decoder bit for bit (RFC 6716 4.3): the
// range coder, energy quantization, bit allocation, band splitting and PVQ.
// Checked on the PC by decoding with libopus (test/opus_test.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define OPUSENC_FRAME 960     // samples (20 ms at 48 kHz)
#define OPUSENC_OVERLAP 120   // the MDCT window's overlap with the next frame
#define OPUSENC_BANDS 21
#define OPUSENC_MAX_BYTES 400

struct OpusEnc {
    float overlap[OPUSENC_OVERLAP];  // the last frame's tail (pre-emphasized)
    float preemph;                   // the pre-emphasis filter's memory
    float old_e[OPUSENC_BANDS];      // each band's quantized energy, as the decoder has it (log2)
    int last_coded;                  // bands coded last frame (keeps the band count from flickering)
    bool started;                    // (the first frame's energy is coded on its own: intra)
    uint32_t final_range;            // the range coder's state at the end (the decoder's must match: tests)
};

void opusencInit(OpusEnc* e);
// One 20 ms frame of mono s16 into exactly `bytes` bytes (the TOC byte included; 3..OPUSENC_MAX_BYTES)
void opusencFrame(OpusEnc* e, const int16_t* pcm, uint8_t* out, size_t bytes);

// The parts, for the PC tests
void opusencMdct(const float* in, float* out);  // 1080 samples (overlap + frame) -> 960 coefficients
