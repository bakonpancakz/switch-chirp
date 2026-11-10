// Chirp's noise suppression against xiph's RNNoise v0.2 (test/rnnoise-ref, built from its source):
// the same frames through both should come out the same, near enough (floating point, and the
// exact tanh where RNNoise uses a table); the voice probability too. And it has to do its job:
// noise alone comes out much quieter, speech with noise comes out closer to the speech.
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "denoise.h"

extern "C" {
typedef struct DenoiseState DenoiseState;
DenoiseState* rnnoise_create(void* model);  // (v0.2: null, its own model)
float rnnoise_process_frame(DenoiseState* st, float* out, const float* in);
void rnnoise_destroy(DenoiseState* st);
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c) ((c) ? (void)g_pass++ : (void)(g_fail++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c)))

static uint32_t g_seed = 7;
static float rnd() {
    g_seed = g_seed * 1664525 + 1013904223;
    return (float)(int32_t)g_seed / 2147483648.f;
}

// Something like a voice (a buzzy pitch through a moving resonance, in syllables), and fan-like noise
static void voice(std::vector<float>& s, float amp) {
    double ph = 0;
    for (size_t i = 0; i < s.size(); i++) {
        double t = i / 48000.0;
        double f0 = 120 + 30 * sin(t * 1.7);
        ph += 2 * M_PI * f0 / 48000;
        double buzz = 0;
        for (int h = 1; h < 30; h++) buzz += sin(h * ph) / h * exp(-h * (0.06 + 0.04 * sin(t * 2 + h * 0.3)));
        double env = pow(fabs(sin(M_PI * t * 2.5)), 0.6);
        s[i] = (float)(amp * buzz * env);
    }
}
static void noise(std::vector<float>& s, float amp) {
    float lp = 0;
    for (size_t i = 0; i < s.size(); i++) {
        lp = 0.9f * lp + 0.1f * rnd();  // (a low, broad hum of air, as fans and rooms are)
        s[i] = amp * (lp * 3 + 0.3f * rnd());
    }
}

static double energy(const std::vector<float>& s, size_t from) {
    double e = 0;
    for (size_t i = from; i < s.size(); i++) e += (double)s[i] * s[i];
    return e;
}

static void run(const std::vector<float>& in, std::vector<float>& mine, std::vector<float>& ref, double* vad_diff, float* vad_mean) {
    static Denoiser d;
    denoiseInit(&d);
    DenoiseState* r = rnnoise_create(nullptr);
    mine.assign(in.size(), 0);
    ref.assign(in.size(), 0);
    double diff = 0, sum = 0;
    int frames = (int)in.size() / DENOISE_FRAME;
    for (int f = 0; f < frames; f++) {
        float a = denoiseFrame(&d, &mine[f * DENOISE_FRAME], &in[f * DENOISE_FRAME]);
        float b = rnnoise_process_frame(r, &ref[f * DENOISE_FRAME], &in[f * DENOISE_FRAME]);
        diff += fabs(a - b);
        sum += a;
    }
    rnnoise_destroy(r);
    *vad_diff = diff / frames;
    *vad_mean = (float)(sum / frames);
}

static double snrDb(const std::vector<float>& want, const std::vector<float>& got, size_t from, int delay) {
    double s = 0, e = 0;
    for (size_t i = from; i + delay < got.size(); i++) {
        double d = want[i] - got[i + delay];
        s += (double)want[i] * want[i], e += d * d;
    }
    return 10 * log10(s / (e + 1e-9));
}

int main() {
    const size_t N = 48000 * 8;  // 8 s
    const size_t SKIP = 48000;   // (a second to settle)
    std::vector<float> v(N), n(N), mix(N), mine, ref;
    voice(v, 6000);
    noise(n, 1500);
    for (size_t i = 0; i < N; i++) mix[i] = v[i] + n[i];
    double vd;
    float vm;

    // the same as xiph's: speech with noise, noise alone, speech alone, silence
    struct Case {
        const char* name;
        const std::vector<float>* in;
    } cases[] = {{"speech+noise", &mix}, {"noise", &n}, {"speech", &v}};
    for (const Case& c : cases) {
        run(*c.in, mine, ref, &vd, &vm);
        // (how far apart the two outputs are, against what went in: noise comes out nearly silent from
        // both, and against that near-silence any difference at all would look large)
        double diff = 0;
        for (size_t i = SKIP; i < N; i++) diff += (double)(ref[i] - mine[i]) * (ref[i] - mine[i]);
        double agree = 10 * log10(energy(*c.in, SKIP) / (diff + 1e-9));
        printf("  %-13s matches xiph's RNNoise to %.1f dB below the input; voice probability off by %.3f on average (mean %.2f)\n",
               c.name, agree, vd, vm);
        CHECK(agree > 40);
        CHECK(vd < 0.05);
    }
    // it does what it's for
    run(n, mine, ref, &vd, &vm);
    double cut = 10 * log10(energy(n, SKIP) / (energy(mine, SKIP) + 1e-9));
    printf("  noise alone comes out %.1f dB quieter\n", cut);
    CHECK(cut > 10);
    // talk and pauses, a second each, over the noise: the pauses come out much quieter, the talk about as loud
    std::vector<float> talk(N);
    for (size_t i = 0; i < N; i++) talk[i] = ((i / 48000) % 2 ? v[i] : 0.f) + n[i];
    run(talk, mine, ref, &vd, &vm);
    double pause_in = 0, pause_out = 0, talk_v = 0, talk_out = 0;
    for (size_t i = SKIP + 4800; i + DENOISE_DELAY * DENOISE_FRAME < N; i++) {
        if (i % 48000 < 4800) continue;  // (a moment after each change)
        double o = mine[i + DENOISE_DELAY * DENOISE_FRAME];  // (its delay: the overlap, and any look-ahead)
        if ((i / 48000) % 2) talk_v += (double)v[i] * v[i], talk_out += o * o;
        else pause_in += (double)n[i] * n[i], pause_out += o * o;
    }
    double pause_cut = 10 * log10(pause_in / (pause_out + 1e-9)), talk_kept = 10 * log10(talk_out / talk_v);
    printf("  talk and pauses in noise: pauses %.1f dB quieter, talk %+.1f dB from the voice alone\n", pause_cut, talk_kept);
    CHECK(pause_cut > 10);
    CHECK(fabs(talk_kept) < 4);
    (void)snrDb;
    std::vector<float> zero(N, 0.f);
    run(zero, mine, ref, &vd, &vm);
    CHECK(energy(mine, 0) == 0);
    // speed
    static Denoiser d;
    denoiseInit(&d);
    std::vector<float> out(N);
    clock_t t0 = clock();
    for (size_t f = 0; f < N / DENOISE_FRAME; f++) denoiseFrame(&d, &out[f * DENOISE_FRAME], &mix[f * DENOISE_FRAME]);
    printf("  speed: 8 s of audio in %.0f ms on this PC\n", (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
