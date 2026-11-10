// Noise suppression, by hand, with RNNoise's trained model (rnn_weights.h; Jean-Marc
// Valin's "A Hybrid DSP/Deep Learning Approach to Real-Time Full-Band Speech
// Enhancement", xiph rnnoise v0.1). Every 10 ms: the spectrum (a 960-point FFT
// over 20 ms, windowed), 22 bands' energies, their cepstrum and its deltas, the
// pitch and how well each band follows it: 42 numbers, into three GRUs. Out come
// a gain per band (the noise taken down, speech left) and how likely it is that
// someone's talking. The pitch harmonics are reinforced, the gains applied, and
// the spectrum turned back into sound. 10 ms of delay.
// (Chirp's noise suppression until v0.2's model fit, with Atmosphère 1.12.1's 40 MB for
// sysmodules. To go back: copy these three over sysmodule/source's denoise.h, denoise.cpp and
// rnn_weights2.h (deleted), and build test/denoise_test.cpp against rnnoise-ref/build/librnnoise.a.)
#pragma once
#include <stddef.h>
#include <stdint.h>

#define DENOISE_FRAME 480  // 10 ms at 48 kHz
#define DENOISE_BANDS 22
#define DENOISE_PITCH_BUF (768 + 960)
#define DENOISE_DELAY 1    // frames the output comes behind the input (the overlap)

struct Denoiser {
    float analysis[DENOISE_FRAME];  // the last frame (the window's first half)
    float synthesis[DENOISE_FRAME]; // the last output's overlap
    float pitch_buf[DENOISE_PITCH_BUF];
    float ceps[8][DENOISE_BANDS];   // the last 8 frames' cepstra
    int ceps_at;
    float last_gain;
    int last_period;
    float hp[2];                    // the high-pass filter's memory
    float vad_state[24], noise_state[48], denoise_state[96];
};

void denoiseInit(Denoiser* d);
// One 10 ms frame (s16 scale, as floats) -> out (may be in); how likely it's speech (0..1)
float denoiseFrame(Denoiser* d, float* out, const float* in);
