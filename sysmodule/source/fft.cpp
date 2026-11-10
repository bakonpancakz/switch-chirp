#include "fft.h"

#include <math.h>
#include <string.h>

namespace {

constexpr int RADIX[] = {4, 4, 2, 3, 5};
constexpr int STAGES = 5;
// each stage's twiddles: for p < m (its n / r), w^(p k) for k = 1..r-1, w = e^(-2 pi i / n)
constexpr int TW_TOTAL = 120 * 3 + 30 * 3 + 15 * 1 + 5 * 2 + 1 * 4;
FftCpx g_tw[TW_TOTAL];
int g_tw_at[STAGES];
bool g_ready = false;

void init() {
    if (g_ready) return;
    int n = FFT_N, at = 0;
    for (int st = 0; st < STAGES; st++) {
        int r = RADIX[st], m = n / r;
        g_tw_at[st] = at;
        for (int p = 0; p < m; p++)
            for (int k = 1; k < r; k++) {
                double a = -2 * 3.14159265358979323846 * p * k / n;
                g_tw[at++] = {(float)cos(a), (float)sin(a)};
            }
        n = m;
    }
    g_ready = true;
}

inline FftCpx add(FftCpx a, FftCpx b) { return {a.r + b.r, a.i + b.i}; }
inline FftCpx sub(FftCpx a, FftCpx b) { return {a.r - b.r, a.i - b.i}; }
inline FftCpx mul(FftCpx a, FftCpx w) { return {a.r * w.r - a.i * w.i, a.r * w.i + a.i * w.r}; }
inline FftCpx scale(FftCpx a, float s) { return {a.r * s, a.i * s}; }
inline FftCpx mulNegI(FftCpx a) { return {a.i, -a.r}; }  // -i a

}  // namespace

void fft480(FftCpx* x, FftCpx* work) {
    init();
    FftCpx *in = x, *out = work;
    int n = FFT_N, s = 1;
    for (int st = 0; st < STAGES; st++) {
        const int r = RADIX[st], m = n / r;
        const FftCpx* tw = g_tw + g_tw_at[st];
        // (Stockham, decimation in frequency: inputs a_j = in[q + s (p + j m)], outputs
        // out[q + s (r p + k)] = (the r-point DFT of the a's)_k times w^(p k))
        for (int p = 0; p < m; p++, tw += r - 1) {
            for (int q = 0; q < s; q++) {
                const FftCpx* a = in + q + s * p;
                FftCpx* y = out + q + s * r * p;
                const int sm = s * m;
                if (r == 4) {
                    FftCpx a0 = a[0], a1 = a[sm], a2 = a[2 * sm], a3 = a[3 * sm];
                    FftCpx t0 = add(a0, a2), t1 = sub(a0, a2), t2 = add(a1, a3), t3 = mulNegI(sub(a1, a3));
                    y[0] = add(t0, t2);
                    y[s] = mul(add(t1, t3), tw[0]);
                    y[2 * s] = mul(sub(t0, t2), tw[1]);
                    y[3 * s] = mul(sub(t1, t3), tw[2]);
                } else if (r == 2) {
                    FftCpx a0 = a[0], a1 = a[sm];
                    y[0] = add(a0, a1);
                    y[s] = mul(sub(a0, a1), tw[0]);
                } else if (r == 3) {
                    const float C = -0.5f, S = 0.86602540378f;  // (e^(-2 pi i / 3) = C - i S)
                    FftCpx a0 = a[0], a1 = a[sm], a2 = a[2 * sm];
                    FftCpx t = add(a1, a2), d = mulNegI(scale(sub(a1, a2), S));
                    FftCpx m0 = add(a0, scale(t, C));
                    y[0] = add(a0, t);
                    y[s] = mul(add(m0, d), tw[0]);
                    y[2 * s] = mul(sub(m0, d), tw[1]);
                } else {  // 5
                    const float C1 = 0.30901699437f, S1 = 0.95105651630f, C2 = -0.80901699437f, S2 = 0.58778525229f;
                    FftCpx a0 = a[0], a1 = a[sm], a2 = a[2 * sm], a3 = a[3 * sm], a4 = a[4 * sm];
                    FftCpx A = add(a1, a4), B = add(a2, a3), D1 = sub(a1, a4), D2 = sub(a2, a3);
                    FftCpx m1 = add(a0, add(scale(A, C1), scale(B, C2)));
                    FftCpx m2 = add(a0, add(scale(A, C2), scale(B, C1)));
                    FftCpx n1 = mulNegI(add(scale(D1, S1), scale(D2, S2)));
                    FftCpx n2 = mulNegI(sub(scale(D1, S2), scale(D2, S1)));
                    y[0] = add(a0, add(A, B));
                    y[s] = mul(add(m1, n1), tw[0]);
                    y[2 * s] = mul(add(m2, n2), tw[1]);
                    y[3 * s] = mul(sub(m2, n2), tw[2]);
                    y[4 * s] = mul(sub(m1, n1), tw[3]);
                }
            }
        }
        FftCpx* t = in;
        in = out, out = t;
        n = m, s *= r;
    }
    if (in != x) memcpy(x, in, FFT_N * sizeof(FftCpx));  // (an odd number of stages: it ended in work)
}
