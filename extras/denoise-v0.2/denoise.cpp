#include "denoise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rnn_weights2.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int F = DENOISE_FRAME, W = 2 * F, FREQ = F + 1, NB = DENOISE_BANDS;
constexpr int PMIN = 60, PMAX = 768, PFRAME = 960, PBUF = DENOISE_PITCH_BUF;

struct Cpx {
    float r, i;
};
Cpx g_tw[W];
bool g_ready = false;

void tablesInit() {
    if (g_ready) return;
    for (int k = 0; k < W; k++) g_tw[k] = {(float)cos(2 * M_PI * k / W), (float)-sin(2 * M_PI * k / W)};
    g_ready = true;
}

// --- the spectrum: a 960-point FFT (mixed radix: 4, 4, 4, 3, 5) ---

void fft(const Cpx* in, int stride, Cpx* out, int n) {
    if (n == 1) {
        out[0] = in[0];
        return;
    }
    int p = n % 4 == 0 ? 4 : n % 2 == 0 ? 2 : n % 3 == 0 ? 3 : 5;
    int m = n / p;
    for (int q = 0; q < p; q++) fft(in + q * stride, stride * p, out + q * m, m);
    int step = W / n;
    Cpx t[5];
    for (int k = 0; k < m; k++) {
        for (int q = 0; q < p; q++) {  // (q * k * step stays under W)
            Cpx a = out[q * m + k], w = g_tw[q * k * step];
            t[q] = {a.r * w.r - a.i * w.i, a.r * w.i + a.i * w.r};
        }
        if (p == 4) {  // (the 4-point DFT: additions, and -i as a swap)
            Cpx a0 = {t[0].r + t[2].r, t[0].i + t[2].i}, a1 = {t[0].r - t[2].r, t[0].i - t[2].i};
            Cpx b0 = {t[1].r + t[3].r, t[1].i + t[3].i}, b1 = {t[1].r - t[3].r, t[1].i - t[3].i};
            out[k] = {a0.r + b0.r, a0.i + b0.i};
            out[k + m] = {a1.r + b1.i, a1.i - b1.r};
            out[k + 2 * m] = {a0.r - b0.r, a0.i - b0.i};
            out[k + 3 * m] = {a1.r - b1.i, a1.i + b1.r};
            continue;
        }
        if (p == 2) {
            out[k] = {t[0].r + t[1].r, t[0].i + t[1].i};
            out[k + m] = {t[0].r - t[1].r, t[0].i - t[1].i};
            continue;
        }
        for (int s = 0; s < p; s++) {  // (3 and 5: as written, twiddles from (q * s) mod p)
            Cpx sum = t[0];
            for (int q = 1; q < p; q++) {
                Cpx w = g_tw[((q * s) % p) * (W / p)];
                sum.r += t[q].r * w.r - t[q].i * w.i;
                sum.i += t[q].r * w.i + t[q].i * w.r;
            }
            out[k + s * m] = sum;
        }
    }
}

// X = the DFT of 960 real samples, divided by 960 (as RNNoise's, which its model learned on).
// Real samples: the even ones as real parts and the odd as imaginary through a 480-point FFT,
// then pulled apart (half the work of a 960-point one)
void forward(Cpx* X, const float* x) {
    constexpr int N2 = W / 2;
    static Cpx z[N2], Z[N2];
    for (int n = 0; n < N2; n++) z[n] = {x[2 * n], x[2 * n + 1]};
    fft(z, 1, Z, N2);
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
    fft(Z, 1, z, N2);
    for (int n = 0; n < N2; n++) x[2 * n] = z[n].r, x[2 * n + 1] = -z[n].i;
}

void window(float* x) {
    for (int i = 0; i < F; i++) {
        x[i] *= RNN2_WINDOW[i];
        x[W - 1 - i] *= RNN2_WINDOW[i];
    }
}

// (the bands: ERB-spaced, in FFT bins; two half-bands at the ends, folded in)
const int16_t EB[NB + 2] = {0,  2,  4,  6,  8,  10, 12, 15, 18,  21,  24,  28,  32,  36,  41,  47,  53,
                            60, 68, 77, 87, 98, 110, 124, 140, 157, 176, 198, 223, 251, 282, 317, 356, 400};

// Each band's energy (or X and P's correlation), each bin shared between the two bands it lies between
void bandEnergy(float* E, const Cpx* X, const Cpx* P) {
    float sum[NB + 2] = {0};
    for (int i = 0; i < NB + 1; i++) {
        int size = EB[i + 1] - EB[i];
        for (int j = 0; j < size; j++) {
            float frac = (float)j / size;
            const Cpx& a = X[EB[i] + j];
            const Cpx& b = P[EB[i] + j];
            float t = a.r * b.r + a.i * b.i;
            sum[i] += (1 - frac) * t;
            sum[i + 1] += frac * t;
        }
    }
    sum[1] = (sum[0] + sum[1]) * 2 / 3;
    sum[NB] = (sum[NB] + sum[NB + 1]) * 2 / 3;
    for (int i = 0; i < NB; i++) E[i] = sum[i + 1];
}

// A gain per band -> a gain per bin (bins past the last band are left as they are)
void interp(float* g, const float* band) {
    for (int i = 1; i < NB; i++) {
        int size = EB[i + 1] - EB[i];
        for (int j = 0; j < size; j++) {
            float frac = (float)j / size;
            g[EB[i] + j] = (1 - frac) * band[i - 1] + frac * band[i];
        }
    }
    for (int j = 0; j < EB[1]; j++) g[j] = band[0];
    for (int j = EB[NB]; j < EB[NB + 1]; j++) g[j] = band[NB - 1];
}

void dct(float* out, const float* in) {
    for (int i = 0; i < NB; i++) {
        float sum = 0;
        for (int j = 0; j < NB; j++) sum += in[j] * RNN2_DCT[j * NB + i];
        out[i] = sum * (float)sqrt(2. / 22);  // (22, not 32: as the model was trained)
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

// --- the network (as xiph's nnet.c, its plain C path) ---

typedef int8_t v8c __attribute__((vector_size(8)));
typedef int16_t v8h __attribute__((vector_size(16)));
typedef int32_t v8i __attribute__((vector_size(32)));

float tanhApprox(float x) {  // (xiph's rational approximation: what the model's numbers expect)
    const float N0 = 952.52801514f, N1 = 96.39235687f, N2 = 0.60863042f;
    const float D0 = 952.72399902f, D1 = 413.36801147f, D2 = 11.88600922f;
    float x2 = x * x;
    float num = ((N2 * x2 + N1) * x2 + N0) * x / ((D2 * x2 + D1) * x2 + D0);
    return num < -1.f ? -1.f : num > 1.f ? 1.f : num;
}
void vecTanh(float* y, const float* x, int n) {
    for (int i = 0; i < n; i++) y[i] = tanhApprox(x[i]);
}
void vecSigmoid(float* y, const float* x, int n) {
    for (int i = 0; i < n; i++) y[i] = .5f + .5f * tanhApprox(.5f * x[i]);
}

// The inputs as int8 (x is -1..1: tanh outputs and GRU states)
void quantize(int8_t* q, const float* x, int n) {
    for (int i = 0; i < n; i++) q[i] = (int8_t)floorf(.5f + 127 * x[i]);
}

// One 8x4 block: 8 outputs' sums over 4 inputs. Two products at a time in 16 bits (at most
// 2 x 128 x 127: no overflow), then widened: on NEON, a handful of instructions
static inline __attribute__((always_inline)) v8i block(const int8_t* w, int x0, int x1, int x2, int x3) {
    v8c c0, c1, c2, c3;
    memcpy(&c0, w, 8), memcpy(&c1, w + 8, 8), memcpy(&c2, w + 16, 8), memcpy(&c3, w + 24, 8);
    v8h p01 = __builtin_convertvector(c0, v8h) * (int16_t)x0 + __builtin_convertvector(c1, v8h) * (int16_t)x1;
    v8h p23 = __builtin_convertvector(c2, v8h) * (int16_t)x2 + __builtin_convertvector(c3, v8h) * (int16_t)x3;
    return __builtin_convertvector(p01, v8i) + __builtin_convertvector(p23, v8i);
}

// out = scale * (W x) for int8 W, dense (rows x cols, cols a multiple of 4)
void cgemv(float* out, const int8_t* w, const float* scale, int rows, int cols, const int8_t* x) {
    for (int i = 0; i < rows; i += 8) {
        v8i acc = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int j = 0; j < cols; j += 4, w += 32) acc += block(w, x[j], x[j + 1], x[j + 2], x[j + 3]);
        for (int r = 0; r < 8; r++) out[i + r] = acc[r] * scale[i + r];
    }
}

// the same, block-sparse: per 8 outputs, how many blocks, then each block's first input
void sparseCgemv(float* out, const int8_t* w, const uint16_t* idx, const float* scale, int rows, const int8_t* x) {
    for (int i = 0; i < rows; i += 8) {
        v8i acc = {0, 0, 0, 0, 0, 0, 0, 0};
        int blocks = *idx++;
        for (int b = 0; b < blocks; b++, w += 32) {
            int p = *idx++;
            acc += block(w, x[p], x[p + 1], x[p + 2], x[p + 3]);
        }
        for (int r = 0; r < 8; r++) out[i + r] = acc[r] * scale[i + r];
    }
}

// out = W x + b for float W, stored input by input
void sgemv(float* out, const float* w, const float* b, int rows, int cols, const float* x) {
    for (int i = 0; i < rows; i++) out[i] = b[i];
    for (int j = 0; j < cols; j++) {
        const float* row = w + j * rows;
        float xj = x[j];
        for (int i = 0; i < rows; i++) out[i] += row[i] * xj;
    }
}

struct Sparse {
    const int8_t* w;
    const uint16_t* idx;
    const float *scale, *bias, *diag;
};
#ifdef RNN2_DENSE
#define IDX(name) nullptr
#else
#define IDX(name) name
#endif
const Sparse GRU[3][2] = {
    {{RNN2_GRU1_IN_W, IDX(RNN2_GRU1_IN_IDX), RNN2_GRU1_IN_SCALE, RNN2_GRU1_IN_B, nullptr},
     {RNN2_GRU1_REC_W, IDX(RNN2_GRU1_REC_IDX), RNN2_GRU1_REC_SCALE, RNN2_GRU1_REC_B, RNN2_GRU1_REC_DIAG}},
    {{RNN2_GRU2_IN_W, IDX(RNN2_GRU2_IN_IDX), RNN2_GRU2_IN_SCALE, RNN2_GRU2_IN_B, nullptr},
     {RNN2_GRU2_REC_W, IDX(RNN2_GRU2_REC_IDX), RNN2_GRU2_REC_SCALE, RNN2_GRU2_REC_B, RNN2_GRU2_REC_DIAG}},
    {{RNN2_GRU3_IN_W, IDX(RNN2_GRU3_IN_IDX), RNN2_GRU3_IN_SCALE, RNN2_GRU3_IN_B, nullptr},
     {RNN2_GRU3_REC_W, IDX(RNN2_GRU3_REC_IDX), RNN2_GRU3_REC_SCALE, RNN2_GRU3_REC_B, RNN2_GRU3_REC_DIAG}},
};
constexpr int H = 384;  // (each GRU's width)

void sparseLinear(float* out, const Sparse& l, const float* in) {
    static int8_t q[H];
    quantize(q, in, H);
    if (l.idx) sparseCgemv(out, l.w, l.idx, l.scale, 3 * H, q);
    else cgemv(out, l.w, l.scale, 3 * H, H, q);  // (a dense model: every block, in order)
    for (int i = 0; i < 3 * H; i++) out[i] += l.bias[i];
    if (l.diag)
        for (int i = 0; i < H; i++) {
            out[i] += l.diag[i] * in[i];
            out[i + H] += l.diag[i + H] * in[i];
            out[i + 2 * H] += l.diag[i + 2 * H] * in[i];
        }
}

void gru(const Sparse* layer, float* state, const float* in) {
    static float zrh[3 * H], rec[3 * H];
    sparseLinear(zrh, layer[0], in);
    sparseLinear(rec, layer[1], state);
    for (int i = 0; i < 2 * H; i++) zrh[i] += rec[i];
    vecSigmoid(zrh, zrh, 2 * H);
    float *z = zrh, *r = zrh + H, *h = zrh + 2 * H;
    for (int i = 0; i < H; i++) h[i] += rec[2 * H + i] * r[i];
    vecTanh(h, h, H);
    for (int i = 0; i < H; i++) state[i] = z[i] * state[i] + (1 - z[i]) * h[i];
}

void network(Denoiser* d, float* gains, float* vad, const float* features) {
    static float t1[195], c1[128], t2[384], c2[384];
    static int8_t q[384];
    // conv1: this frame's features and the last two's (float weights)
    memcpy(t1, d->conv1, sizeof(d->conv1));
    memcpy(t1 + 130, features, 65 * sizeof(float));
    sgemv(c1, RNN2_CONV1_W, RNN2_CONV1_B, 128, 195, t1);
    vecTanh(c1, c1, 128);
    memcpy(d->conv1, t1 + 65, sizeof(d->conv1));
    // conv2 (int8, dense)
    memcpy(t2, d->conv2, sizeof(d->conv2));
    memcpy(t2 + 256, c1, sizeof(c1));
    quantize(q, t2, 384);
    cgemv(c2, RNN2_CONV2_W, RNN2_CONV2_SCALE, 384, 384, q);
    for (int i = 0; i < 384; i++) c2[i] += RNN2_CONV2_B[i];
    vecTanh(c2, c2, 384);
    memcpy(d->conv2, t2 + 128, sizeof(d->conv2));
    // the GRUs, each on the one before
    gru(GRU[0], d->gru1, c2);
    gru(GRU[1], d->gru2, d->gru1);
    gru(GRU[2], d->gru3, d->gru2);
#if RNN2_OUT_IN == 1536  // (4 x 384; the preprocessor can't see H)
    // (later models: the outputs read every layer's output, joined)
    static float all[4 * H];
    memcpy(all, c2, H * sizeof(float));
    memcpy(all + H, d->gru1, H * sizeof(float));
    memcpy(all + 2 * H, d->gru2, H * sizeof(float));
    memcpy(all + 3 * H, d->gru3, H * sizeof(float));
    const float* last = all;
#else
    const float* last = d->gru3;
#endif
    sgemv(gains, RNN2_OUT_W, RNN2_OUT_B, 32, RNN2_OUT_IN, last);
    vecSigmoid(gains, gains, 32);
    sgemv(vad, RNN2_VAD_W, RNN2_VAD_B, 1, RNN2_OUT_IN, last);
    vecSigmoid(vad, vad, 1);
}

// Reinforces the pitch's harmonics where the noise took them (P: the spectrum a period before)
void pitchFilter(Cpx* X, const Cpx* P, const float* Ex, const float* Ep, const float* Exp, const float* g) {
    float r[NB], rf[FREQ] = {0}, newE[NB], norm[NB], normf[FREQ] = {0};
    for (int i = 0; i < NB; i++) {
        if (Exp[i] > g[i]) r[i] = 1;
        else r[i] = Exp[i] * Exp[i] * (1 - g[i] * g[i]) / (.001f + g[i] * g[i] * (1 - Exp[i] * Exp[i]));
        r[i] = sqrtf(r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i]);
        r[i] *= sqrtf(Ex[i] / (1e-8f + Ep[i]));
    }
    interp(rf, r);
    for (int i = 0; i < FREQ; i++) X[i].r += rf[i] * P[i].r, X[i].i += rf[i] * P[i].i;
    bandEnergy(newE, X, X);
    for (int i = 0; i < NB; i++) norm[i] = sqrtf(Ex[i] / (1e-8f + newE[i]));
    interp(normf, norm);
    for (int i = 0; i < FREQ; i++) X[i].r *= normf[i], X[i].i *= normf[i];
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
    float Ex[NB], Ep[NB], Exp[NB], features[65];
    Cpx* dX = (Cpx*)d->dX;
    Cpx* dP = (Cpx*)d->dP;
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
    // the features: the cepstrum, the pitch correlations' DCT, the period
    dct(features + NB, Exp);
    features[2 * NB] = .01f * (period - 300);
    float Ly[NB], E = 0, logMax = -2, follow = -2;
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
        float g[NB], gf[FREQ] = {1};
        network(d, g, &vad, features);
        // (the gains are for the frame before: the model looks one ahead)
        pitchFilter(dX, dP, d->dEx, d->dEp, d->dExp, g);
        for (int i = 0; i < NB; i++) {
            g[i] = g[i] > .6f * d->lastg[i] ? g[i] : .6f * d->lastg[i];  // (gains fall back gently)
#if RNN2_OUT_IN == 1536  // (4 x 384; the preprocessor can't see H)
            // (later RNNoise: the next frame's floor follows the energy's change, so a sudden noise
            // (a thump, a rub) doesn't ride in on the last frame's open gain)
            float next = g[i] * (d->dEx[i] + 1e-3f) / (Ex[i] + 1e-3f);
            d->lastg[i] = next < 1.f ? next : 1.f;
#else
            d->lastg[i] = g[i];
#endif
        }
        interp(gf, g);
        for (int i = 0; i < FREQ; i++) dX[i].r *= gf[i], dX[i].i *= gf[i];
    }
    // back to sound (the frame before), overlapped with the one before that
    inverse(buf, dX);
    window(buf);
    for (int i = 0; i < F; i++) out[i] = buf[i] + d->synthesis[i];
    memcpy(d->synthesis, buf + F, sizeof(d->synthesis));
    memcpy(dX, X, sizeof(X));
    memcpy(dP, P, sizeof(P));
    memcpy(d->dEx, Ex, sizeof(Ex));
    memcpy(d->dEp, Ep, sizeof(Ep));
    memcpy(d->dExp, Exp, sizeof(Exp));
    return vad;
}

#ifdef DENOISE_TEST
// (the network alone, for test/denoise_net_test.cpp)
void denoiseNetTest(Denoiser* d, float* gains, float* vad, const float* features) { network(d, gains, vad, features); }
#endif
