// The call's sounds: voice_* as people (us too) join the call and leave it, screenshare_* as they start
// watching our screen share and stop, as Discord plays them. Each is a
// WAV on the card (CHIRP_SOUNDS/<name>.wav: 16-bit PCM, mono or stereo, 8..48 kHz), read when it's
// wanted into the one buffer there is (SOUNDS_MAX samples, mixed to mono: 1 s at 24 kHz, 0.5 s at
// 48; longer is cut, faded) and played by the mixer (mixer.h), resampled to 48 kHz. A missing file is
// silence. Read on the card's own thread (app.cpp), never the mixer's.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SOUNDS_MAX 24000  // samples (48 KB)

namespace sounds {

enum Kind : uint8_t { VoiceJoin, VoiceLeave, ScreenshareJoin, ScreenshareLeave, KINDS };
const char* name(Kind k);  // its file's name, without ".wav"

// Read it and play it (the card's thread; it takes a moment)
void load(Kind k);

// --- the WAV itself (apart for the PC tests) ---
struct WavInfo {
    int rate, channels;
    size_t data_at, data_bytes;  // where the samples begin in the file, and how many bytes of them
};
// From the file's first n bytes (its header, and the "data" chunk's header within them): false if it
// isn't a WAV this can play (16-bit PCM, 1 or 2 channels, 8000..48000 Hz)
bool wavInfo(const uint8_t* head, size_t n, WavInfo* out);
// frames of s16 (channels interleaved, little-endian bytes) into mono at out
void toMono(const uint8_t* in, size_t frames, int channels, int16_t* out);
// The last ms of n samples faded to nothing (a sound cut short doesn't click)
void fadeOut(int16_t* pcm, size_t n, int rate, int ms);

}  // namespace sounds
