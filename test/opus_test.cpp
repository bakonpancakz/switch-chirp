// Chirp's Opus encoder against libopus (test/opus-ref: built from the 1.5.2 source):
// its MDCT against libopus's, then whole packets through libopus's decoder. A packet
// decodes in step only if the decoder's range coder ends where ours did (the final
// range), and what comes out has to sound like what went in (SNR, with libopus's own
// CELT encoder at the same size alongside for scale).
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "opus.h"
#include "opusenc.h"

extern "C" void ref_mdct(const float* in, float* out);

static int g_pass = 0, g_fail = 0;
#define CHECK(c) ((c) ? (void)g_pass++ : (void)(g_fail++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c)))

static uint32_t g_seed = 1;
static float rnd() {  // -1..1
    g_seed = g_seed * 1664525 + 1013904223;
    return (float)(int32_t)g_seed / 2147483648.f;
}

static void testMdct() {
    float in[1080], a[960], b[960];
    double worst = 0;
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 1080; i++) in[i] = rnd() * (round < 10 ? 30000 : 300);
        opusencMdct(in, a);
        ref_mdct(in, b);
        double err = 0, mag = 0;
        for (int i = 0; i < 960; i++) err += (a[i] - b[i]) * (a[i] - b[i]), mag += b[i] * b[i];
        double rel = sqrt(err / mag);
        if (rel > worst) worst = rel;
    }
    printf("  MDCT: worst relative error %.2e\n", worst);
    CHECK(worst < 1e-4);
}

// Test signals, 48 kHz mono
static std::vector<int16_t> signal(int kind, int frames) {
    std::vector<int16_t> s(frames * 960);
    double ph = 0, ph2 = 0;
    for (size_t i = 0; i < s.size(); i++) {
        double t = i / 48000.0, v = 0;
        switch (kind) {
        case 0: v = 0.5 * sin(2 * M_PI * 440 * t); break;  // a tone
        case 1:  // a sweep, 100 Hz to 12 kHz
            ph += 2 * M_PI * (100 * pow(120.0, t / (s.size() / 48000.0))) / 48000;
            v = 0.4 * sin(ph);
            break;
        case 2:  // something like a voice: a buzzy 140 Hz source through a moving resonance, in syllables
        {
            ph2 += 2 * M_PI * 140 / 48000;
            double buzz = 0;
            for (int h = 1; h < 40; h++) buzz += sin(h * ph2) / h * (1 + sin(t * 3 + h) * 0.5) * exp(-h * (0.08 + 0.05 * sin(t * 2)));
            double env = pow(fabs(sin(M_PI * t * 3.3)), 0.5);
            v = 0.15 * buzz * env + 0.01 * rnd() * env;
            break;
        }
        case 3: v = 0.3 * rnd(); break;  // noise
        case 4: v = 0; break;            // silence
        case 5: v = 0.9 * sin(2 * M_PI * 1000 * t) + 0.2 * sin(2 * M_PI * 3500 * t); break;  // loud
        }
        s[i] = (int16_t)lrint(v * 32767);
    }
    return s;
}

// How well b (decoded) matches a (sent), at the best delay: SNR in dB
static double snr(const std::vector<int16_t>& a, const std::vector<int16_t>& b, int* delay) {
    double best = -1e9;
    size_t from = 4800, to = a.size() - 4800;  // (past the start)
    for (int d = 0; d < 1200; d++) {
        double sig = 0, err = 0;
        for (size_t i = from; i < to; i++) {
            double x = a[i], y = b[i + d];
            sig += x * x, err += (x - y) * (x - y);
        }
        double s = sig > 0 ? 10 * log10(sig / (err + 1e-9)) : (err == 0 ? 99 : -99);
        if (s > best) best = s, *delay = d;
    }
    return best;
}

static void testRoundTrip() {
    const char* NAMES[] = {"tone", "sweep", "voice-ish", "noise", "silence", "loud"};
    const int SIZES[] = {40, 80, 120, 160, 240};
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    CHECK(dec && err == OPUS_OK);
    for (int kind = 0; kind < 6; kind++) {
        int frames = 150;
        std::vector<int16_t> in = signal(kind, frames);
        for (int bytes : SIZES) {
            opus_decoder_ctl(dec, OPUS_RESET_STATE);
            OpusEnc enc;
            opusencInit(&enc);
            std::vector<int16_t> out(in.size() + 2000, 0);
            uint8_t pkt[OPUSENC_MAX_BYTES];
            int range_ok = 0, decoded_ok = 0;
            for (int f = 0; f < frames; f++) {
                opusencFrame(&enc, &in[f * 960], pkt, bytes);
                int n = opus_decode(dec, pkt, bytes, &out[f * 960], 960, 0);
                decoded_ok += n == 960;
                uint32_t rng = 0;
                opus_decoder_ctl(dec, OPUS_GET_FINAL_RANGE(&rng));
                range_ok += rng == enc.final_range;
            }
            // libopus's own CELT at the same size, for scale
            OpusEncoder* ref = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
            opus_encoder_ctl(ref, OPUS_SET_BITRATE(bytes * 8 * 50));
            opus_encoder_ctl(ref, OPUS_SET_VBR(0));
            opus_encoder_ctl(ref, OPUS_SET_COMPLEXITY(10));
            OpusDecoder* dec2 = opus_decoder_create(48000, 1, &err);
            std::vector<int16_t> out2(in.size() + 2000, 0);
            for (int f = 0; f < frames; f++) {
                int n = opus_encode(ref, &in[f * 960], 960, pkt, sizeof(pkt));
                if (opus_decode(dec2, pkt, n, &out2[f * 960], 960, 0) != 960) printf("  (libopus's own packet didn't decode)\n");
            }
            opus_encoder_destroy(ref);
            opus_decoder_destroy(dec2);
            int d1 = 0, d2 = 0;
            double s1 = snr(in, out, &d1), s2 = snr(in, out2, &d2);
            printf("  %-9s %3d bytes (%3d kbps): decoded %3d/%d, range matched %3d/%d, SNR %5.1f dB at delay %d (libopus %5.1f dB)\n",
                   NAMES[kind], bytes, bytes * 8 * 50 / 1000, decoded_ok, frames, range_ok, frames, s1, d1, s2);
            CHECK(decoded_ok == frames);
            CHECK(range_ok == frames);
            // voice: as good as libopus at its best, give or take. Tones and sweeps trail it (it boosts the
            // bands a tone is in, dynalloc: an encoder choice left out here) but must still come through
            if (kind == 2) CHECK(s1 > s2 - 3);
            if ((kind == 0 || kind == 1 || kind == 5) && bytes >= 80) CHECK(s1 > 7);
            if (kind == 4) {
                int loud = 0;
                for (int16_t v : out) loud = abs(v) > loud ? abs(v) : loud;
                CHECK(loud < 64);
            }
        }
    }
    opus_decoder_destroy(dec);
}

static void testSpeed() {
    std::vector<int16_t> in = signal(2, 500);
    OpusEnc enc;
    opusencInit(&enc);
    uint8_t pkt[OPUSENC_MAX_BYTES];
    clock_t t0 = clock();
    for (int f = 0; f < 500; f++) opusencFrame(&enc, &in[f * 960], pkt, 160);
    double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  speed: 10 s of audio in %.0f ms on this PC\n", ms);
}

int main() {
    testMdct();
    testRoundTrip();
    testSpeed();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
