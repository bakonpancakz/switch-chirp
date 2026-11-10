// Chirp's RNNoise network alone against xiph's: the same features in, layer by layer out
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "denoise.h"

void denoiseNetTest(Denoiser* d, float* gains, float* vad, const float* features);
extern "C" void ref_net(const float* features, float* gains, float* vad);
extern "C" const float* ref_gru1(void);

int main() {
    static Denoiser d;
    denoiseInit(&d);
    unsigned s = 3;
    for (int f = 0; f < 6; f++) {
        float feat[65], g1[32], g2[32], v1, v2;
        for (int i = 0; i < 65; i++) {
            s = s * 1664525 + 1013904223;
            feat[i] = ((int)(s >> 8) % 2000) / 1000.f - 1;
        }
        denoiseNetTest(&d, g1, &v1, feat);
        ref_net(feat, g2, &v2);
        double e = 0, m = 0;
        for (int i = 0; i < 32; i++) e += fabs(g1[i] - g2[i]), m += g2[i];
        double eg = 0;
        for (int i = 0; i < 384; i++) eg += fabs(d.gru1[i] - ref_gru1()[i]);
        printf("frame %d: gains off by %.4f (mean %.3f), vad %.3f vs %.3f, gru1 off by %.4f\n", f, e / 32, m / 32, v1, v2, eg / 384);
    }
}
