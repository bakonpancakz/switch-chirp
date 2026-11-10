// The 480-point complex FFT both the noise suppression (its 960-point real
// transforms are done as 480-point complex ones) and the Opus encoder (its
// MDCT) are built on. Stockham, iterative: radix 4, 4, 2, 3, 5, each stage's
// butterflies written out and its twiddles worked out once; the output comes
// in order without a bit-reversal pass. Forward (e^-i), not scaled.
#pragma once

#define FFT_N 480

struct FftCpx {
    float r, i;
};

// x (FFT_N) in place; work: FFT_N more for the stages to go back and forth between
void fft480(FftCpx* x, FftCpx* work);
