#include "denoise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fft.h"
#include "rnn_weights.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int F = DENOISE_FRAME, W = 2 * F, FREQ = F + 1, NB = DENOISE_BANDS;
constexpr int PMIN = 60, PMAX = 768, PFRAME = 960, PBUF = DENOISE_PITCH_BUF;
constexpr float PI = 3.14159265358979f;
const int16_t EB[NB] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 34, 40, 48, 60, 78, 100};  // (x4: bins)

struct Cpx {
    float r, i;
};
Cpx g_tw[W];
float g_win[F];
float g_dct[NB * NB];
float g_frac[400];  // (each bin's place in its band, 0..1: worked out once, not per bin per frame)
bool g_ready = false;

void tablesInit() {
    if (g_ready) return;
    for (int k = 0; k < W; k++) g_tw[k] = {(float)cos(2 * M_PI * k / W), (float)-sin(2 * M_PI * k / W)};
    for (int i = 0; i < F; i++) {
        double s = sin(.5 * M_PI * (i + .5) / F);
        g_win[i] = (float)sin(.5 * M_PI * s * s);
    }
    for (int i = 0; i < NB; i++)
        for (int j = 0; j < NB; j++) g_dct[i * NB + j] = (float)(cos((i + .5) * j * M_PI / NB) * (j == 0 ? sqrt(.5) : 1));
    for (int i = 0; i < NB - 1; i++) {
        int size = (EB[i + 1] - EB[i]) << 2;
        for (int j = 0; j < size; j++) g_frac[(EB[i] << 2) + j] = (float)j / size;
    }
    g_ready = true;
}

// --- the spectrum: 960-point real transforms, through the 480-point complex FFT (fft.h) ---

// X = the DFT of 960 real samples, divided by 960 (as RNNoise's, which its model learned on).
// Real samples: the even ones as real parts and the odd as imaginary through a 480-point FFT,
// then pulled apart (half the work of a 960-point one)
void forward(Cpx* X, const float* x) {
    constexpr int N2 = W / 2;
    static Cpx z[N2], Z[N2];
    for (int n = 0; n < N2; n++) z[n] = {x[2 * n], x[2 * n + 1]};
    static FftCpx work[N2];
    memcpy(Z, z, sizeof(z));
    fft480((FftCpx*)Z, work);
    for (int k = 0; k <= N2; k++) {
        Cpx a = Z[k % N2], b = Z[(N2 - k) % N2];  // (b: conjugated below)
        Cpx e = {(a.r + b.r) * .5f, (a.i - b.i) * .5f};   // the even samples' DFT
        Cpx o = {(a.i + b.i) * .5f, -(a.r - b.r) * .5f};  // the odd's: (a - conj b) / 2i
        Cpx w = g_tw[k];
        X[k] = {(e.r + w.r * o.r - w.i * o.i) / W, (e.i + w.r * o.i + w.i * o.r) / W};
    }
}

// and back: the inverse DFT, not divided (so forward then this gives the samples back), the same way round
void inverse(float* x, const Cpx* X) {
    constexpr int N2 = W / 2;
    static Cpx Z[N2], z[N2];
    for (int k = 0; k < N2; k++) {
        Cpx a = X[k], b = {X[N2 - k].r, -X[N2 - k].i};  // (X[k + 480] = conj X[480 - k])
        Cpx e = {a.r + b.r, a.i + b.i};
        Cpx d = {a.r - b.r, a.i - b.i}, w = g_tw[k];
        Cpx o = {d.r * w.r + d.i * w.i, d.i * w.r - d.r * w.i};  // (d times conj w)
        Z[k] = {e.r - o.i, e.i + o.r};  // e + i o; conjugated for the inverse below
        Z[k].i = -Z[k].i;
    }
    static FftCpx work[N2];
    memcpy(z, Z, sizeof(Z));
    fft480((FftCpx*)z, work);
    for (int n = 0; n < N2; n++) x[2 * n] = z[n].r, x[2 * n + 1] = -z[n].i;
}

void window(float* x) {
    for (int i = 0; i < F; i++) {
        x[i] *= g_win[i];
        x[W - 1 - i] *= g_win[i];
    }
}

// Each band's energy, each bin shared between the two bands it lies between (triangles)
void bandEnergy(float* E, const Cpx* X, const Cpx* P) {
    float sum[NB] = {0};
    for (int i = 0; i < NB - 1; i++) {
        int size = (EB[i + 1] - EB[i]) << 2;
        for (int j = 0; j < size; j++) {
            float frac = g_frac[(EB[i] << 2) + j];
            const Cpx& a = X[(EB[i] << 2) + j];
            const Cpx& b = P[(EB[i] << 2) + j];
            float t = a.r * b.r + a.i * b.i;
            sum[i] += (1 - frac) * t;
            sum[i + 1] += frac * t;
        }
    }
    sum[0] *= 2;
    sum[NB - 1] *= 2;
    for (int i = 0; i < NB; i++) E[i] = sum[i];
}

// A gain per band -> a gain per bin, interpolated (bins past the last band are left as they are)
void interp(float* g, const float* band) {
    for (int i = 0; i < NB - 1; i++) {
        int size = (EB[i + 1] - EB[i]) << 2;
        for (int j = 0; j < size; j++) {
            float frac = g_frac[(EB[i] << 2) + j];
            g[(EB[i] << 2) + j] = (1 - frac) * band[i] + frac * band[i + 1];
        }
    }
}

void dct(float* out, const float* in) {
    for (int i = 0; i < NB; i++) {
        float sum = 0;
        for (int j = 0; j < NB; j++) sum += in[j] * g_dct[j * NB + i];
        out[i] = sum * sqrtf(2.f / 22);
    }
}

// --- the pitch (as Opus's: on a 2x downsampled, whitened signal) ---

typedef float v4f __attribute__((vector_size(16)));

// (four lanes, four running sums each: the compiler won't vectorize a sum by itself, as that changes
// the order it's added in)
float inner(const float* x, const float* y, int n) {
    v4f a = {0, 0, 0, 0}, b = {0, 0, 0, 0};
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        v4f x0, x1, y0, y1;
        memcpy(&x0, x + i, 16), memcpy(&x1, x + i + 4, 16), memcpy(&y0, y + i, 16), memcpy(&y1, y + i + 4, 16);
        a += x0 * y0;
        b += x1 * y1;
    }
    a += b;
    float s = a[0] + a[1] + a[2] + a[3];
    for (; i < n; i++) s += x[i] * y[i];
    return s;
}

void downsample(const float* x, float* lp, int len) {
    int n = len >> 1;
    for (int i = 1; i < n; i++) lp[i] = .5f * (.5f * (x[2 * i - 1] + x[2 * i + 1]) + x[2 * i]);
    lp[0] = .5f * (.5f * x[1] + x[0]);
    float ac[5];
    for (int k = 0; k <= 4; k++) {
        float d = 0;
        for (int i = k; i < n; i++) d += lp[i] * lp[i - k];
        ac[k] = d;
    }
    ac[0] *= 1.0001f;
    for (int i = 1; i <= 4; i++) ac[i] -= ac[i] * (.008f * i) * (.008f * i);
    // LPC (Levinson-Durbin), 4th order
    float lpc[4] = {0, 0, 0, 0}, error = ac[0];
    if (ac[0] != 0) {
        for (int i = 0; i < 4; i++) {
            float rr = 0;
            for (int j = 0; j < i; j++) rr += lpc[j] * ac[i - j];
            rr += ac[i + 1];
            float r = -rr / error;
            lpc[i] = r;
            for (int j = 0; j < (i + 1) >> 1; j++) {
                float t1 = lpc[j], t2 = lpc[i - 1 - j];
                lpc[j] = t1 + r * t2;
                lpc[i - 1 - j] = t2 + r * t1;
            }
            error -= r * r * error;
            if (error < .001f * ac[0]) break;
        }
    }
    float tmp = 1;
    for (int i = 0; i < 4; i++) {
        tmp *= .9f;
        lpc[i] *= tmp;
    }
    const float c1 = .8f;
    float num[5] = {lpc[0] + .8f, lpc[1] + c1 * lpc[0], lpc[2] + c1 * lpc[1], lpc[3] + c1 * lpc[2], c1 * lpc[3]};
    float mem[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < n; i++) {  // (a 5-tap FIR, in place)
        float s = lp[i] + num[0] * mem[0] + num[1] * mem[1] + num[2] * mem[2] + num[3] * mem[3] + num[4] * mem[4];
        mem[4] = mem[3], mem[3] = mem[2], mem[2] = mem[1], mem[1] = mem[0], mem[0] = lp[i];
        lp[i] = s;
    }
}

void bestPitch(const float* xcorr, const float* y, int len, int max_pitch, int* best) {
    float syy = 1, best_num[2] = {-1, -1}, best_den[2] = {0, 0};
    best[0] = 0, best[1] = 1;
    for (int j = 0; j < len; j++) syy += y[j] * y[j];
    for (int i = 0; i < max_pitch; i++) {
        if (xcorr[i] > 0) {
            float x16 = xcorr[i] * 1e-12f;
            float num = x16 * x16;
            if (num * best_den[1] > best_num[1] * syy) {
                if (num * best_den[0] > best_num[0] * syy) {
                    best_num[1] = best_num[0], best_den[1] = best_den[0], best[1] = best[0];
                    best_num[0] = num, best_den[0] = syy, best[0] = i;
                } else
                    best_num[1] = num, best_den[1] = syy, best[1] = i;
            }
        }
        syy += y[i + len] * y[i + len] - y[i] * y[i];
        if (syy < 1) syy = 1;
    }
}

// The lag (in downsampled samples x2) where y best matches x_lp
int pitchSearch(const float* x_lp, const float* y, int len, int max_pitch) {
    static float x4[PFRAME >> 2], y4[(PFRAME + PMAX) >> 2], xcorr[PMAX >> 1];
    int lag = len + max_pitch;
    for (int j = 0; j < len >> 2; j++) x4[j] = x_lp[2 * j];
    for (int j = 0; j < lag >> 2; j++) y4[j] = y[2 * j];
    for (int i = 0; i < max_pitch >> 2; i++) xcorr[i] = inner(x4, y4 + i, len >> 2);
    int best[2];
    bestPitch(xcorr, y4, len >> 2, max_pitch >> 2, best);
    for (int i = 0; i < max_pitch >> 1; i++) {  // (finer, near the two best)
        xcorr[i] = 0;
        if (abs(i - 2 * best[0]) > 2 && abs(i - 2 * best[1]) > 2) continue;
        float s = inner(x_lp, y + i, len >> 1);
        xcorr[i] = s > -1 ? s : -1;
    }
    bestPitch(xcorr, y, len >> 1, max_pitch >> 1, best);
    int offset = 0;
    if (best[0] > 0 && best[0] < (max_pitch >> 1) - 1) {
        float a = xcorr[best[0] - 1], b = xcorr[best[0]], c = xcorr[best[0] + 1];
        if (c - a > .7f * (b - a)) offset = 1;
        else if (a - c > .7f * (b - c)) offset = -1;
    }
    return 2 * best[0] - offset;
}

float pitchGain(float xy, float xx, float yy) { return xy / sqrtf(1 + xx * yy); }

// A period half (or a third...) as long that fits as well is the real one (octave errors)
float removeDoubling(const float* x, int maxperiod, int minperiod, int n, int* t0_, int prev_period, float prev_gain) {
    static const int SECOND[16] = {0, 0, 3, 2, 3, 2, 5, 2, 3, 2, 3, 2, 5, 2, 3, 2};
    static float yyl[(PMAX >> 1) + 1];
    int minperiod0 = minperiod;
    maxperiod /= 2, minperiod /= 2, *t0_ /= 2, prev_period /= 2, n /= 2;
    x += maxperiod;
    if (*t0_ >= maxperiod) *t0_ = maxperiod - 1;
    int T = *t0_, T0 = *t0_;
    float xx = inner(x, x, n), xy = inner(x, x - T0, n);
    yyl[0] = xx;
    float yy = xx;
    for (int i = 1; i <= maxperiod; i++) {
        yy = yy + x[-i] * x[-i] - x[n - i] * x[n - i];
        yyl[i] = yy > 0 ? yy : 0;
    }
    yy = yyl[T0];
    float best_xy = xy, best_yy = yy;
    float g0 = pitchGain(xy, xx, yy), g = g0;
    for (int k = 2; k <= 15; k++) {
        int T1 = (2 * T0 + k) / (2 * k);
        if (T1 < minperiod) break;
        int T1b;
        if (k == 2) T1b = T1 + T0 > maxperiod ? T0 : T0 + T1;
        else T1b = (2 * SECOND[k] * T0 + k) / (2 * k);
        float xy1 = inner(x, x - T1, n), xy2 = inner(x, x - T1b, n);
        xy = .5f * (xy1 + xy2);
        yy = .5f * (yyl[T1] + yyl[T1b]);
        float g1 = pitchGain(xy, xx, yy);
        float cont = 0;
        if (abs(T1 - prev_period) <= 1) cont = prev_gain;
        else if (abs(T1 - prev_period) <= 2 && 5 * k * k < T0) cont = .5f * prev_gain;
        float thresh = .7f * g0 - cont > .3f ? .7f * g0 - cont : .3f;
        if (T1 < 3 * minperiod) thresh = .85f * g0 - cont > .4f ? .85f * g0 - cont : .4f;
        else if (T1 < 2 * minperiod) thresh = .9f * g0 - cont > .5f ? .9f * g0 - cont : .5f;
        if (g1 > thresh) best_xy = xy, best_yy = yy, T = T1, g = g1;
    }
    best_xy = best_xy > 0 ? best_xy : 0;
    float pg = best_yy <= best_xy ? 1 : best_xy / (best_yy + 1);
    float xc[3];
    for (int k = 0; k < 3; k++) xc[k] = inner(x, x - (T + k - 1), n);
    int offset = 0;
    if (xc[2] - xc[0] > .7f * (xc[1] - xc[0])) offset = 1;
    else if (xc[0] - xc[2] > .7f * (xc[1] - xc[2])) offset = -1;
    if (pg > g) pg = g;
    *t0_ = 2 * T + offset;
    if (*t0_ < minperiod0) *t0_ = minperiod0;
    return pg;
}

// --- the network ---

float activate(float x, int a) {
    if (a == 0) return tanhf(x);
    if (a == 1) return .5f + .5f * tanhf(.5f * x);
    return x < 0 ? 0 : x;
}

// (the weights are stored input by input, each a row of every output's weight for it: summed a row
// at a time, they're read in order, which is what makes this fast)
void dense(const int8_t* bias, const int8_t* w, int m, int n, int a, float* out, const float* in) {
    float sum[96];
    for (int i = 0; i < n; i++) sum[i] = bias[i];
    for (int j = 0; j < m; j++) {
        const int8_t* row = w + j * n;
        float x = in[j];
        for (int i = 0; i < n; i++) sum[i] += row[i] * x;
    }
    for (int i = 0; i < n; i++) out[i] = activate(sum[i] * (1.f / 256), a);
}

void gru(const int8_t* bias, const int8_t* w, const int8_t* rw, int m, int n, int a, float* state, const float* in) {
    // all three parts (update gate, reset gate, the candidate) at once: rows of 3n
    float acc[3 * 96], r[96];
    const int stride = 3 * n;
    for (int k = 0; k < stride; k++) acc[k] = bias[k];
    for (int j = 0; j < m; j++) {
        const int8_t* row = w + j * stride;
        float x = in[j];
        for (int k = 0; k < stride; k++) acc[k] += row[k] * x;
    }
    for (int j = 0; j < n; j++) {  // the state into the gates
        const int8_t* row = rw + j * stride;
        float s = state[j];
        for (int k = 0; k < 2 * n; k++) acc[k] += row[k] * s;
    }
    for (int i = 0; i < n; i++) r[i] = activate(acc[n + i] * (1.f / 256), 1);
    for (int j = 0; j < n; j++) {  // and, through the reset gate, into the candidate
        const int8_t* row = rw + j * stride + 2 * n;
        float s = state[j] * r[j];
        for (int i = 0; i < n; i++) acc[2 * n + i] += row[i] * s;
    }
    for (int i = 0; i < n; i++) {
        float z = activate(acc[i] * (1.f / 256), 1);
        state[i] = z * state[i] + (1 - z) * activate(acc[2 * n + i] * (1.f / 256), a);
    }
}

void network(Denoiser* d, float* gains, float* vad, const float* features) {
    float dense_out[24], in2[24 + 24 + 42], in3[24 + 48 + 42];
    dense(RNN_INPUT_DENSE_BIAS, RNN_INPUT_DENSE_WEIGHTS, RNN_INPUT_DENSE_IN, RNN_INPUT_DENSE_N, RNN_INPUT_DENSE_ACT, dense_out, features);
    gru(RNN_VAD_GRU_BIAS, RNN_VAD_GRU_WEIGHTS, RNN_VAD_GRU_RECURRENT_WEIGHTS, RNN_VAD_GRU_IN, RNN_VAD_GRU_N, RNN_VAD_GRU_ACT,
        d->vad_state, dense_out);
    dense(RNN_VAD_OUTPUT_BIAS, RNN_VAD_OUTPUT_WEIGHTS, RNN_VAD_OUTPUT_IN, RNN_VAD_OUTPUT_N, RNN_VAD_OUTPUT_ACT, vad, d->vad_state);
    memcpy(in2, dense_out, sizeof(dense_out));
    memcpy(in2 + 24, d->vad_state, 24 * sizeof(float));
    memcpy(in2 + 48, features, 42 * sizeof(float));
    gru(RNN_NOISE_GRU_BIAS, RNN_NOISE_GRU_WEIGHTS, RNN_NOISE_GRU_RECURRENT_WEIGHTS, RNN_NOISE_GRU_IN, RNN_NOISE_GRU_N,
        RNN_NOISE_GRU_ACT, d->noise_state, in2);
    memcpy(in3, d->vad_state, 24 * sizeof(float));
    memcpy(in3 + 24, d->noise_state, 48 * sizeof(float));
    memcpy(in3 + 72, features, 42 * sizeof(float));
    gru(RNN_DENOISE_GRU_BIAS, RNN_DENOISE_GRU_WEIGHTS, RNN_DENOISE_GRU_RECURRENT_WEIGHTS, RNN_DENOISE_GRU_IN, RNN_DENOISE_GRU_N,
        RNN_DENOISE_GRU_ACT, d->denoise_state, in3);
    dense(RNN_DENOISE_OUTPUT_BIAS, RNN_DENOISE_OUTPUT_WEIGHTS, RNN_DENOISE_OUTPUT_IN, RNN_DENOISE_OUTPUT_N, RNN_DENOISE_OUTPUT_ACT,
          gains, d->denoise_state);
}

}  // namespace

void denoiseInit(Denoiser* d) {
    memset(d, 0, sizeof(*d));
    tablesInit();
}

float denoiseFrame(Denoiser* d, float* out, const float* in) {
    tablesInit();
    static float x[F], buf[W], p[W], lp[PBUF >> 1];
    static Cpx X[FREQ], P[FREQ];
    float Ex[NB], Ep[NB], Exp[NB], features[42];
    // a high-pass (DC and rumble out)
    for (int i = 0; i < F; i++) {
        float xi = in[i], yi = xi + d->hp[0];
        d->hp[0] = d->hp[1] + (float)(-2 * (double)xi + 1.99599 * (double)yi);
        d->hp[1] = (float)((double)xi - 0.99600 * (double)yi);
        x[i] = yi;
    }
    // the spectrum: the last frame and this one, windowed
    memcpy(buf, d->analysis, sizeof(d->analysis));
    memcpy(buf + F, x, sizeof(x));
    memcpy(d->analysis, x, sizeof(x));
    window(buf);
    forward(X, buf);
    bandEnergy(Ex, X, X);
    // the pitch, and the spectrum of what came one period before
    memmove(d->pitch_buf, d->pitch_buf + F, (PBUF - F) * sizeof(float));
    memcpy(d->pitch_buf + PBUF - F, x, sizeof(x));
    downsample(d->pitch_buf, lp, PBUF);
    int period = PMAX - pitchSearch(lp + (PMAX >> 1), lp, PFRAME, PMAX - 3 * PMIN);
    float gain = removeDoubling(lp, PMAX, PMIN, PFRAME, &period, d->last_period, d->last_gain);
    d->last_period = period;
    d->last_gain = gain;
    for (int i = 0; i < W; i++) p[i] = d->pitch_buf[PBUF - W - period + i];
    window(p);
    forward(P, p);
    bandEnergy(Ep, P, P);
    bandEnergy(Exp, X, P);
    for (int i = 0; i < NB; i++) Exp[i] = Exp[i] / sqrtf(.001f + Ex[i] * Ep[i]);
    // the features
    float tmp[NB], Ly[NB], E = 0, logMax = -2, follow = -2;
    dct(tmp, Exp);
    for (int i = 0; i < 6; i++) features[NB + 12 + i] = tmp[i];
    features[NB + 12] -= 1.3f;
    features[NB + 13] -= 0.9f;
    features[NB + 18] = .01f * (period - 300);
    for (int i = 0; i < NB; i++) {
        Ly[i] = log10f(1e-2f + Ex[i]);
        float lo = logMax - 7 > follow - 1.5f ? logMax - 7 : follow - 1.5f;
        if (Ly[i] < lo) Ly[i] = lo;
        logMax = logMax > Ly[i] ? logMax : Ly[i];
        follow = follow - 1.5f > Ly[i] ? follow - 1.5f : Ly[i];
        E += Ex[i];
    }
    float vad = 0;
    if (E >= 0.04f) {  // (near silence passes untouched)
        dct(features, Ly);
        features[0] -= 12;
        features[1] -= 4;
        float* c0 = d->ceps[d->ceps_at];
        float* c1 = d->ceps[(d->ceps_at + 7) % 8];
        float* c2 = d->ceps[(d->ceps_at + 6) % 8];
        memcpy(c0, features, NB * sizeof(float));
        d->ceps_at = (d->ceps_at + 1) % 8;
        for (int i = 0; i < 6; i++) {
            features[i] = c0[i] + c1[i] + c2[i];
            features[NB + i] = c0[i] - c2[i];
            features[NB + 6 + i] = c0[i] - 2 * c1[i] + c2[i];
        }
        float variability = 0;
        for (int i = 0; i < 8; i++) {
            float mind = 1e15f;
            for (int j = 0; j < 8; j++) {
                if (j == i) continue;
                float dist = 0;
                for (int k = 0; k < NB; k++) {
                    float t = d->ceps[i][k] - d->ceps[j][k];
                    dist += t * t;
                }
                mind = dist < mind ? dist : mind;
            }
            variability += mind;
        }
        features[NB + 19] = variability / 8 - 2.1f;
        // the gains, and the pitch harmonics brought back where the noise took them
        float g[NB];
        network(d, g, &vad, features);
        float r[NB], rf[FREQ] = {0}, norm[NB], normf[FREQ] = {0}, gf[FREQ] = {1};
        for (int i = 0; i < NB; i++) {
            if (Exp[i] > g[i]) r[i] = 1;
            else r[i] = Exp[i] * Exp[i] * (1 - g[i] * g[i]) / (.001f + g[i] * g[i] * (1 - Exp[i] * Exp[i]));
            r[i] = sqrtf(r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i]);
            r[i] *= sqrtf(Ex[i] / (1e-8f + Ep[i]));
        }
        interp(rf, r);
        for (int i = 0; i < FREQ; i++) X[i].r += rf[i] * P[i].r, X[i].i += rf[i] * P[i].i;
        float newE[NB];
        bandEnergy(newE, X, X);
        for (int i = 0; i < NB; i++) norm[i] = sqrtf(Ex[i] / (1e-8f + newE[i]));
        interp(normf, norm);
        for (int i = 0; i < FREQ; i++) X[i].r *= normf[i], X[i].i *= normf[i];
        interp(gf, g);
        for (int i = 0; i < FREQ; i++) X[i].r *= gf[i], X[i].i *= gf[i];
    }
    // back to sound, overlapped with the last frame's second half
    inverse(buf, X);
    window(buf);
    for (int i = 0; i < F; i++) out[i] = buf[i] + d->synthesis[i];
    memcpy(d->synthesis, buf + F, sizeof(d->synthesis));
    return vad;
}
