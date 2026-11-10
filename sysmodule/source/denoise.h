// Noise suppression, by hand, with RNNoise v0.2's trained model (rnn_weights2.h;
// xiph rnnoise 0.2, Jean-Marc Valin). Every 10 ms: the spectrum (a 960-point
// FFT over 20 ms, windowed), 32 ERB bands' energies and their cepstrum, the
// pitch and how well each band follows it: 65 numbers, through two 1-D
// convolutions (the last 3 frames) and three 384-wide GRUs (int8 weights,
// block-sparse). Out come a gain per band and how likely it is that someone's
// talking. The pitch harmonics are reinforced, the gains applied, and the
// spectrum turned back into sound, a frame later than it came (the model looks
// one frame ahead): 20 ms of delay in all.
// The matrix products are written with GCC's vector types: NEON on the Switch,
// SSE on the PC, the same code either way, so the PC tests check what runs on
// the Switch (test/denoise_test.cpp, against xiph's own C code).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define DENOISE_FRAME 480  // 10 ms at 48 kHz
#define DENOISE_BANDS 32
#define DENOISE_PITCH_BUF (768 + 960)
#define DENOISE_DELAY 2    // frames the output comes behind the input (the overlap, and the look-ahead)

struct Denoiser {
    float analysis[DENOISE_FRAME];   // the last frame (the window's first half)
    float synthesis[DENOISE_FRAME];  // the last output's overlap
    float pitch_buf[DENOISE_PITCH_BUF];
    float last_gain;
    int last_period;
    float hp[2];  // the high-pass filter's memory
    float lastg[DENOISE_BANDS];
    float conv1[65 * 2], conv2[128 * 2];  // the convolutions' last two frames
    float gru1[384], gru2[384], gru3[384];
    // the last frame's spectrum, waiting for this frame's gains (the model's look-ahead)
    float dX[2 * 481], dP[2 * 481];
    float dEx[DENOISE_BANDS], dEp[DENOISE_BANDS], dExp[DENOISE_BANDS];
};

void denoiseInit(Denoiser* d);
// One 10 ms frame (s16 scale, as floats) -> out (may be in); how likely it's speech (0..1)
float denoiseFrame(Denoiser* d, float* out, const float* in);
