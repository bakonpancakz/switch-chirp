#include "opusenc.h"

#include <math.h>
#include <string.h>

#include "celt_tables.h"
#include "fft.h"

namespace {

constexpr int N = OPUSENC_FRAME, OVERLAP = OPUSENC_OVERLAP, NB = OPUSENC_BANDS;
constexpr int LM = 3;      // 20 ms: 8 short blocks' worth (2^3)
constexpr int BITRES = 3;  // allocation counts in eighths of a bit
constexpr int MAX_FINE_BITS = 8, FINE_OFFSET = 21, QTHETA_OFFSET = 4;
constexpr int SPREAD_NORMAL = 2;
constexpr float PREEMPH = 0.8500061035f;
constexpr float PI = 3.14159265358979f;

int ilog(uint32_t x) { return x ? 32 - __builtin_clz(x) : 0; }

// --- the range coder (RFC 6716 4.1 / 5.1), with raw bits written from the end ---

struct Range {
    uint8_t* buf;
    uint32_t storage, offs, end_offs;
    uint32_t end_window;
    int nend_bits, nbits_total;
    uint32_t rng, val, ext;
    int rem;

    static constexpr uint32_t TOP = 1u << 31, BOT = TOP >> 8;
    static constexpr int SHIFT = 23;

    void init(uint8_t* b, uint32_t size) {
        buf = b, storage = size, offs = end_offs = 0, end_window = 0, nend_bits = 0;
        nbits_total = 33, rng = TOP, val = 0, ext = 0, rem = -1;
    }
    void byte(unsigned v) {
        if (offs + end_offs < storage) buf[offs++] = (uint8_t)v;
    }
    void byteAtEnd(unsigned v) {
        if (offs + end_offs < storage) buf[storage - ++end_offs] = (uint8_t)v;
    }
    void carryOut(int c) {
        if (c != 255) {
            int carry = c >> 8;
            if (rem >= 0) byte(rem + carry);
            for (; ext > 0; ext--) byte((255 + carry) & 255);
            rem = c & 255;
        } else
            ext++;
    }
    void normalize() {
        while (rng <= BOT) {
            carryOut((int)(val >> SHIFT));
            val = (val << 8) & (TOP - 1);
            rng <<= 8;
            nbits_total += 8;
        }
    }
    void encode(unsigned fl, unsigned fh, unsigned ft) {
        uint32_t r = rng / ft;
        if (fl > 0) {
            val += rng - r * (ft - fl);
            rng = r * (fh - fl);
        } else
            rng -= r * (ft - fh);
        normalize();
    }
    void encodeBin(unsigned fl, unsigned fh, unsigned bits) {
        uint32_t r = rng >> bits;
        if (fl > 0) {
            val += rng - r * ((1u << bits) - fl);
            rng = r * (fh - fl);
        } else
            rng -= r * ((1u << bits) - fh);
        normalize();
    }
    void bitLogp(int v, unsigned logp) {
        uint32_t s = rng >> logp, r = rng - s;
        if (v) val += r;
        rng = v ? s : r;
        normalize();
    }
    void icdf(int s, const uint8_t* t, unsigned ftb) {
        uint32_t r = rng >> ftb;
        if (s > 0) {
            val += rng - r * t[s - 1];
            rng = r * (t[s - 1] - t[s]);
        } else
            rng -= r * t[s];
        normalize();
    }
    void bits(uint32_t v, int n) {
        if (nend_bits + n > 32) {
            do {
                byteAtEnd(end_window & 255);
                end_window >>= 8;
                nend_bits -= 8;
            } while (nend_bits >= 8);
        }
        end_window |= v << nend_bits;
        nend_bits += n;
        nbits_total += n;
    }
    void uint(uint32_t v, uint32_t ft) {
        ft--;
        int ftb = ilog(ft);
        if (ftb > 8) {
            ftb -= 8;
            unsigned top = (ft >> ftb) + 1;
            unsigned f = (unsigned)(v >> ftb);
            encode(f, f + 1, top);
            bits(v & ((1u << ftb) - 1), ftb);
        } else
            encode(v, v + 1, ft + 1);
    }
    int tell() const { return nbits_total - ilog(rng); }
    int tellFrac() const {
        static const unsigned CORRECTION[8] = {35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
        int nbits = nbits_total << BITRES;
        int l = ilog(rng);
        uint32_t r = rng >> (l - 16);
        unsigned b = (r >> 12) - 8;
        b += r > CORRECTION[b];
        l = (l << 3) + b;
        return nbits - l;
    }
    void done() {
        int l = 32 - ilog(rng);
        uint32_t msk = (TOP - 1) >> l;
        uint32_t end = (val + msk) & ~msk;
        if ((end | msk) >= val + rng) {
            l++;
            msk >>= 1;
            end = (val + msk) & ~msk;
        }
        while (l > 0) {
            carryOut((int)(end >> SHIFT));
            end = (end << 8) & (TOP - 1);
            l -= 8;
        }
        if (rem >= 0 || ext > 0) carryOut(0);
        uint32_t window = end_window;
        int used = nend_bits;
        while (used >= 8) {
            byteAtEnd(window & 255);
            window >>= 8;
            used -= 8;
        }
        memset(buf + offs, 0, storage - offs - end_offs);
        if (used > 0 && end_offs < storage) buf[storage - end_offs - 1] |= (uint8_t)window;
    }
};

// Coarse energy's Laplace-like distribution (RFC 6716 4.3.2.1)
void laplace(Range& rc, int* value, unsigned fs, int decay) {
    constexpr unsigned MINP = 1, NMIN = 16;
    unsigned fl = 0;
    int v = *value;
    if (v) {
        int s = -(v < 0);
        v = (v + s) ^ s;
        fl = fs;
        fs = (32768 - MINP * (2 * NMIN) - fs) * (uint32_t)(16384 - decay) >> 15;
        int i;
        for (i = 1; fs > 0 && i < v; i++) {
            fs *= 2;
            fl += fs + 2 * MINP;
            fs = (fs * (uint32_t)decay) >> 15;
        }
        if (!fs) {
            int ndi_max = (int)(32768 - fl + MINP - 1);
            ndi_max = (ndi_max - s) >> 1;
            int di = v - i < ndi_max - 1 ? v - i : ndi_max - 1;
            fl += (2 * di + 1 + s) * MINP;
            fs = MINP < 32768 - fl ? MINP : 32768 - fl;
            *value = (i + di + s) ^ s;
        } else {
            fs += MINP;
            fl += fs & ~s;
        }
    }
    rc.encodeBin(fl, fl + fs, 15);
}

// --- the MDCT: 1920 windowed samples -> 960 coefficients, through a 480-point complex FFT ---

struct Cpx {
    float r, i;
};
float g_cos[480], g_sin[480];  // (the MDCT's rotations: angle 2 pi (i + 1/8) / 1920)
float g_window[OVERLAP];
bool g_ready = false;

void tablesInit() {
    if (g_ready) return;
    for (int i = 0; i < 480; i++) {
        double a = 2 * 3.14159265358979323846 * (i + .125) / 1920;
        g_cos[i] = (float)cos(a), g_sin[i] = (float)sin(a);
    }
    for (int i = 0; i < OVERLAP; i++) {
        double s = sin(.5 * 3.14159265358979323846 * (i + .5) / OVERLAP);
        g_window[i] = (float)sin(.5 * 3.14159265358979323846 * s * s);
    }
    g_ready = true;
}

}  // namespace

void opusencMdct(const float* in, float* out) {
    tablesInit();
    constexpr int N2 = 960, N4 = 480;
    static float f[N2];
    static Cpx z[N4], Z[N4];
    // window, shuffle and fold the 1920 (of which 1080 are nonzero) into 960 (as libopus's clt_mdct_forward)
    {
        const float* xp1 = in + (OVERLAP >> 1);
        const float* xp2 = in + N2 - 1 + (OVERLAP >> 1);
        float* yp = f;
        const float* wp1 = g_window + (OVERLAP >> 1);
        const float* wp2 = g_window + (OVERLAP >> 1) - 1;
        int i = 0;
        for (; i < ((OVERLAP + 3) >> 2); i++) {
            *yp++ = *wp2 * xp1[N2] + *wp1 * *xp2;
            *yp++ = *wp1 * *xp1 - *wp2 * xp2[-N2];
            xp1 += 2, xp2 -= 2, wp1 += 2, wp2 -= 2;
        }
        wp1 = g_window;
        wp2 = g_window + OVERLAP - 1;
        for (; i < N4 - ((OVERLAP + 3) >> 2); i++) {
            *yp++ = *xp2;
            *yp++ = *xp1;
            xp1 += 2, xp2 -= 2;
        }
        for (; i < N4; i++) {
            *yp++ = -*wp1 * xp1[-N2] + *wp2 * *xp2;
            *yp++ = *wp2 * *xp1 + *wp1 * xp2[N2];
            xp1 += 2, xp2 -= 2, wp1 += 2, wp2 -= 2;
        }
    }
    // pre-rotation (and the FFT's 1/n). libopus's second twiddle, cos(2 pi (i + 480 + 1/8) / 1920), is -sin
    for (int i = 0; i < N4; i++) {
        float re = f[2 * i], im = f[2 * i + 1], c = g_cos[i], s = -g_sin[i];
        z[i] = {(re * c - im * s) / N4, (im * c + re * s) / N4};
    }
    static FftCpx work[N4];
    memcpy(Z, z, sizeof(z));
    fft480((FftCpx*)Z, work);
    for (int i = 0; i < N4; i++) {  // post-rotation
        float c = g_cos[i], s = -g_sin[i];
        out[2 * i] = Z[i].i * s - Z[i].r * c;
        out[N2 - 1 - 2 * i] = Z[i].r * s + Z[i].i * c;
    }
}

namespace {

// --- bit allocation (RFC 6716 4.3.3): has to come out exactly as the decoder's ---

const uint8_t* cacheFor(int lm, int band) { return CELT_CACHE_BITS + CELT_CACHE_INDEX[(lm + 1) * NB + band]; }

int getPulses(int i) { return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1); }

int bits2pulses(int band, int lm, int bits) {
    const uint8_t* cache = cacheFor(lm, band);
    int lo = 0, hi = cache[0];
    bits--;
    for (int i = 0; i < 6; i++) {
        int mid = (lo + hi + 1) >> 1;
        if ((int)cache[mid] >= bits) hi = mid;
        else lo = mid;
    }
    return bits - (lo == 0 ? -1 : (int)cache[lo]) <= (int)cache[hi] - bits ? lo : hi;
}

int pulses2bits(int band, int lm, int pulses) { return pulses == 0 ? 0 : cacheFor(lm, band)[pulses] + 1; }

int width(int i) { return CELT_EBANDS[i + 1] - CELT_EBANDS[i]; }

const uint8_t LOG2_FRAC[24] = {0, 8, 13, 16, 19, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 37};

// (clt_compute_allocation and interp_bits2pulses, for one channel, no boosts)
int allocate(Range& rc, const int* cap, int trim, int total, int* balance_out, int* pulses, int* ebits, int* fine_priority,
             int prev) {
    const int start = 0, end = NB, C = 1;
    int bits1[NB], bits2[NB], thresh[NB], trim_offset[NB];
    total = total > 0 ? total : 0;
    int skip_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
    total -= skip_rsv;
    for (int j = start; j < end; j++) {
        thresh[j] = C << BITRES > (3 * width(j) << LM << BITRES) >> 4 ? C << BITRES : (3 * width(j) << LM << BITRES) >> 4;
        trim_offset[j] = C * width(j) * (trim - 5 - LM) * (end - j - 1) * (1 << (LM + BITRES)) >> 6;
        if (width(j) << LM == 1) trim_offset[j] -= C << BITRES;
    }
    int lo = 1, hi = 11 - 1;
    do {
        int done = 0, psum = 0, mid = (lo + hi) >> 1;
        for (int j = end; j-- > start;) {
            int b = C * width(j) * CELT_ALLOC[mid][j] << LM >> 2;
            if (b > 0) b = b + trim_offset[j] > 0 ? b + trim_offset[j] : 0;
            if (b >= thresh[j] || done) {
                done = 1;
                psum += b < cap[j] ? b : cap[j];
            } else if (b >= C << BITRES)
                psum += C << BITRES;
        }
        if (psum > total) hi = mid - 1;
        else lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (int j = start; j < end; j++) {
        int b1 = C * width(j) * CELT_ALLOC[lo][j] << LM >> 2;
        int b2 = hi >= 11 ? cap[j] : C * width(j) * CELT_ALLOC[hi][j] << LM >> 2;
        if (b1 > 0) b1 = b1 + trim_offset[j] > 0 ? b1 + trim_offset[j] : 0;
        if (b2 > 0) b2 = b2 + trim_offset[j] > 0 ? b2 + trim_offset[j] : 0;
        b2 = b2 - b1 > 0 ? b2 - b1 : 0;
        bits1[j] = b1, bits2[j] = b2;
    }
    // interp_bits2pulses
    const int skip_start = start, alloc_floor = C << BITRES, logM = LM << BITRES;
    int* bits = pulses;
    lo = 0, hi = 1 << 6;
    for (int i = 0; i < 6; i++) {
        int mid = (lo + hi) >> 1, psum = 0, done = 0;
        for (int j = end; j-- > start;) {
            int tmp = bits1[j] + (mid * bits2[j] >> 6);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += tmp < cap[j] ? tmp : cap[j];
            } else if (tmp >= alloc_floor)
                psum += alloc_floor;
        }
        if (psum > total) hi = mid;
        else lo = mid;
    }
    int psum = 0, done = 0;
    for (int j = end; j-- > start;) {
        int tmp = bits1[j] + (lo * bits2[j] >> 6);
        if (tmp < thresh[j] && !done) tmp = tmp >= alloc_floor ? alloc_floor : 0;
        else done = 1;
        tmp = tmp < cap[j] ? tmp : cap[j];
        bits[j] = tmp;
        psum += tmp;
    }
    int coded;
    for (coded = end;; coded--) {
        int j = coded - 1;
        if (j <= skip_start) {
            total += skip_rsv;
            break;
        }
        int left = total - psum;
        int span = CELT_EBANDS[coded] - CELT_EBANDS[start];
        int percoeff = (int)((unsigned)left / (unsigned)span);
        left -= span * percoeff;
        int rem = left - (CELT_EBANDS[j] - CELT_EBANDS[start]) > 0 ? left - (CELT_EBANDS[j] - CELT_EBANDS[start]) : 0;
        int band_width = CELT_EBANDS[coded] - CELT_EBANDS[j];
        int band_bits = bits[j] + percoeff * band_width + rem;
        int floor1 = alloc_floor + (1 << BITRES);
        if (band_bits >= (thresh[j] > floor1 ? thresh[j] : floor1)) {
            int depth_threshold = coded > 17 ? (j < prev ? 7 : 9) : 0;
            if (coded <= start + 2 || (band_bits > (depth_threshold * band_width << LM << BITRES) >> 4 && j <= end - 1)) {
                rc.bitLogp(1, 1);
                break;
            }
            rc.bitLogp(0, 1);
            psum += 1 << BITRES;
            band_bits -= 1 << BITRES;
        }
        psum -= bits[j];
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else
            bits[j] = 0;
    }
    int left = total - psum;
    int span = CELT_EBANDS[coded] - CELT_EBANDS[start];
    int percoeff = (int)((unsigned)left / (unsigned)span);
    left -= span * percoeff;
    for (int j = start; j < coded; j++) bits[j] += percoeff * width(j);
    for (int j = start; j < coded; j++) {
        int tmp = left < width(j) ? left : width(j);
        bits[j] += tmp;
        left -= tmp;
    }
    int balance = 0, j;
    for (j = start; j < coded; j++) {
        int n = width(j) << LM;
        int bit = bits[j] + balance, excess;
        if (n > 1) {
            excess = bit - cap[j] > 0 ? bit - cap[j] : 0;
            bits[j] = bit - excess;
            int den = C * n;
            int nclogn = den * (CELT_LOGN[j] + logM);
            int offset = (nclogn >> 1) - den * FINE_OFFSET;
            if (n == 2) offset += den << BITRES >> 2;
            if (bits[j] + offset < den * 2 << BITRES) offset += nclogn >> 2;
            else if (bits[j] + offset < den * 3 << BITRES) offset += nclogn >> 3;
            int eb = bits[j] + offset + (den << (BITRES - 1));
            eb = eb > 0 ? eb : 0;
            eb = (int)((unsigned)eb / (unsigned)den) >> BITRES;
            if (C * eb > (bits[j] >> BITRES)) eb = bits[j] >> BITRES;
            eb = eb < MAX_FINE_BITS ? eb : MAX_FINE_BITS;
            ebits[j] = eb;
            fine_priority[j] = eb * (den << BITRES) >= bits[j] + offset;
            bits[j] -= C * eb << BITRES;
        } else {
            excess = bit - (C << BITRES) > 0 ? bit - (C << BITRES) : 0;
            bits[j] = bit - excess;
            ebits[j] = 0;
            fine_priority[j] = 1;
        }
        if (excess > 0) {
            int extra_fine = excess >> BITRES;
            if (extra_fine > MAX_FINE_BITS - ebits[j]) extra_fine = MAX_FINE_BITS - ebits[j];
            ebits[j] += extra_fine;
            int extra_bits = extra_fine * C << BITRES;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *balance_out = balance;
    for (; j < end; j++) {
        ebits[j] = bits[j] >> BITRES;
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }
    return coded;
}

// --- the bands' shapes: PVQ (RFC 6716 4.3.4) ---

// U(n, k) for n = 0..: the codeword counting cwrs.c's tables hold, one row at a time
struct URow {
    uint32_t u[132];
    int n, kmax;
    void start(int k) {  // n = 0
        kmax = k;
        for (int i = 0; i <= kmax; i++) u[i] = i == 0;
        n = 0;
    }
    void next() {  // n -> n + 1: U(n+1, k) = U(n, k) + U(n+1, k-1) + U(n, k-1)
        uint32_t prev = u[0];
        u[0] = 0;
        for (int k = 1; k <= kmax; k++) {
            uint32_t cur = u[k];
            u[k] = cur + u[k - 1] + prev;
            prev = cur;
        }
        n++;
    }
};

// The vector's index among all with n dimensions and k pulses, and how many there are (cwrs.c's icwrs)
void encodePulses(Range& rc, const int* y, int n, int k) {
    URow row;
    row.start(k + 1);
    row.next();  // n = 1
    int j = n - 1;
    uint32_t i = y[j] < 0;
    int kk = abs(y[j]);
    do {
        j--;
        row.next();  // n - j
        i += row.u[kk];
        kk += abs(y[j]);
        if (y[j] < 0) i += row.u[kk + 1];
    } while (j > 0);
    rc.uint(i, row.u[k] + row.u[k + 1]);
}

void rotation1(float* x, int len, int stride, float c, float s) {
    float ms = -s;
    float* p = x;
    for (int i = 0; i < len - stride; i++) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p++ = c * x1 + ms * x2;
    }
    p = &x[len - 2 * stride - 1];
    for (int i = len - 2 * stride - 1; i >= 0; i--) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p-- = c * x1 + ms * x2;
    }
}

// The spreading rotation the decoder undoes (SPREAD_NORMAL, one block)
void spread(float* x, int len, int k) {
    if (2 * k >= len) return;
    float gain = (float)len / (float)(len + 10 * k);
    float theta = .5f * gain * gain;
    float c = cosf(.5f * PI * theta), s = cosf(.5f * PI * (1 - theta));
    int stride2 = 0;
    if (len >= 8) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) < len) stride2++;
    }
    rotation1(x, len, 1, c, -s);
    if (stride2) rotation1(x, len, stride2, s, -c);
}

// The k-pulse vector nearest x's direction (greedy, as libopus's op_pvq_search)
void pvqSearch(const float* x0, int* iy, int k, int n) {
    static float x[176], y[176];
    static int sign[176];
    for (int j = 0; j < n; j++) {
        sign[j] = x0[j] < 0;
        x[j] = fabsf(x0[j]);
        iy[j] = 0;
        y[j] = 0;
    }
    float xy = 0, yy = 0;
    int left = k;
    if (k > (n >> 1)) {  // (most of the way at once: each x's share, rounded down)
        float sum = 0;
        for (int j = 0; j < n; j++) sum += x[j];
        if (!(sum > 1e-15f && sum < 64)) {
            x[0] = 1;
            for (int j = 1; j < n; j++) x[j] = 0;
            sum = 1;
        }
        float rcp = (k + .8f) / sum;
        for (int j = 0; j < n; j++) {
            iy[j] = (int)floorf(rcp * x[j]);
            y[j] = (float)iy[j];
            yy += y[j] * y[j];
            xy += x[j] * y[j];
            y[j] *= 2;
            left -= iy[j];
        }
    }
    if (left > n + 3) {
        float t = (float)left;
        yy += t * t + t * y[0];
        iy[0] += left;
        left = 0;
    }
    for (int i = 0; i < left; i++) {
        int best = 0;
        yy += 1;
        float best_num = (xy + x[0]) * (xy + x[0]), best_den = yy + y[0];
        for (int j = 1; j < n; j++) {
            float num = (xy + x[j]) * (xy + x[j]), den = yy + y[j];
            if (best_den * num > den * best_num) best_den = den, best_num = num, best = j;
        }
        xy += x[best];
        yy += y[best];
        y[best] += 2;
        iy[best]++;
    }
    for (int j = 0; j < n; j++) iy[j] = (iy[j] ^ -sign[j]) + sign[j];
}

void algQuant(Range& rc, float* x, int n, int k) {
    static int iy[176];
    spread(x, n, k);
    pvqSearch(x, iy, k, n);
    encodePulses(rc, iy, n, k);
}

int16_t fracMul16(int a, int b) { return (int16_t)((16384 + (int32_t)(int16_t)a * (int16_t)b) >> 15); }

int16_t bitexactCos(int16_t x) {
    int32_t tmp = (4096 + (int32_t)x * x) >> 13;
    int16_t x2 = (int16_t)tmp;
    x2 = (int16_t)((32767 - x2) + fracMul16(x2, (-7651 + fracMul16(x2, (8277 + fracMul16(-626, x2))))));
    return (int16_t)(1 + x2);
}

int bitexactLog2tan(int isin, int icos) {
    int lc = ilog((uint32_t)icos), ls = ilog((uint32_t)isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + fracMul16(isin, fracMul16(isin, -2597) + 7932) - fracMul16(icos, fracMul16(icos, -2597) + 7932);
}

int computeQn(int n, int b, int offset, int pulse_cap) {
    static const int16_t EXP2[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int n2 = 2 * n - 1;
    int qb = (b + n2 * offset) / n2;  // (C's division: toward zero, as celt_sudiv)
    if (b - pulse_cap - (4 << BITRES) < qb) qb = b - pulse_cap - (4 << BITRES);
    if (8 << BITRES < qb) qb = 8 << BITRES;
    if (qb < (1 << BITRES >> 1)) return 1;
    int qn = EXP2[qb & 7] >> (14 - (qb >> BITRES));
    return (qn + 1) >> 1 << 1;
}

struct Bands {
    Range* rc;
    int band;
    int remaining;  // (eighths of a bit)
};

// A band (or part of one): split in two halves while it has more bits than one PVQ codeword can use
void partition(Bands& ctx, float* x, int n, int b, int lm) {
    Range& rc = *ctx.rc;
    const uint8_t* cache = cacheFor(lm, ctx.band);
    if (lm != -1 && b > cache[cache[0]] + 12 && n > 2) {
        n >>= 1;
        float* y = x + n;
        lm -= 1;
        // theta: how the energy divides between the halves (compute_theta, mono, one block)
        int pulse_cap = CELT_LOGN[ctx.band] + lm * (1 << BITRES);
        int offset = (pulse_cap >> 1) - QTHETA_OFFSET;
        int qn = computeQn(n, b, offset, pulse_cap);
        int itheta = 0;
        int tell = rc.tellFrac();
        if (qn != 1) {
            float em = 1e-15f, es = 1e-15f;
            for (int j = 0; j < n; j++) em += x[j] * x[j], es += y[j] * y[j];
            int raw = (int)floorf(.5f + 16384 * 0.63662f * atan2f(sqrtf(es), sqrtf(em)));
            itheta = (raw * qn + 8192) >> 14;
            // (a triangular pdf: 0 and qn least likely)
            int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
            int fs = itheta <= (qn >> 1) ? itheta + 1 : qn + 1 - itheta;
            int fl = itheta <= (qn >> 1) ? itheta * (itheta + 1) >> 1 : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
            rc.encode(fl, fl + fs, ft);
            itheta = itheta * 16384 / qn;
        }
        int qalloc = rc.tellFrac() - tell;
        b -= qalloc;
        int delta;
        if (itheta == 0) delta = -16384;
        else if (itheta == 16384) delta = 16384;
        else {
            int imid = bitexactCos((int16_t)itheta), iside = bitexactCos((int16_t)(16384 - itheta));
            delta = fracMul16((n - 1) << 7, bitexactLog2tan(iside, imid));
        }
        int mbits = (b - delta) / 2;
        mbits = mbits < b ? mbits : b;
        mbits = mbits > 0 ? mbits : 0;
        int sbits = b - mbits;
        ctx.remaining -= qalloc;
        int rebalance = ctx.remaining;
        if (mbits >= sbits) {
            partition(ctx, x, n, mbits, lm);
            rebalance = mbits - (rebalance - ctx.remaining);
            if (rebalance > 3 << BITRES && itheta != 0) sbits += rebalance - (3 << BITRES);
            partition(ctx, y, n, sbits, lm);
        } else {
            partition(ctx, y, n, sbits, lm);
            rebalance = sbits - (rebalance - ctx.remaining);
            if (rebalance > 3 << BITRES && itheta != 16384) mbits += rebalance - (3 << BITRES);
            partition(ctx, x, n, mbits, lm);
        }
        return;
    }
    int q = bits2pulses(ctx.band, lm, b);
    int curr = pulses2bits(ctx.band, lm, q);
    ctx.remaining -= curr;
    while (ctx.remaining < 0 && q > 0) {
        ctx.remaining += curr;
        q--;
        curr = pulses2bits(ctx.band, lm, q);
        ctx.remaining -= curr;
    }
    if (q != 0) algQuant(rc, x, n, getPulses(q));
    // (no pulses: the decoder folds or makes noise; nothing is sent)
}

}  // namespace

void opusencInit(OpusEnc* e) {
    memset(e, 0, sizeof(*e));
    tablesInit();
}

void opusencFrame(OpusEnc* e, const int16_t* pcm, uint8_t* out, size_t bytes) {
    tablesInit();
    if (bytes < 3) bytes = 3;
    if (bytes > OPUSENC_MAX_BYTES) bytes = OPUSENC_MAX_BYTES;
    out[0] = 0xF8;  // TOC: CELT-only, fullband, 20 ms, mono, one frame
    const int nbytes = (int)bytes - 1;
    const int total_bits = nbytes * 8;
    Range rc;
    rc.init(out + 1, (uint32_t)nbytes);

    // the signal: pre-emphasized, after the last frame's overlap
    static float in[OVERLAP + N], freq[N], X[N];
    memcpy(in, e->overlap, sizeof(e->overlap));
    float m = e->preemph;
    for (int i = 0; i < N; i++) {
        float x = pcm[i];
        in[OVERLAP + i] = x - m;
        m = PREEMPH * x;
    }
    e->preemph = m;
    memcpy(e->overlap, in + N, sizeof(e->overlap));

    rc.bitLogp(0, 15);                                // not silence
    if (rc.tell() + 16 <= total_bits) rc.bitLogp(0, 1);  // no pitch pre-filter
    if (rc.tell() + 3 <= total_bits) rc.bitLogp(0, 3);   // long blocks

    // each band's energy (log2, less its mean) and its shape at unit energy
    opusencMdct(in, freq);
    float logE[NB];
    for (int i = 0; i < NB; i++) {
        int a = CELT_EBANDS[i] << LM, z = CELT_EBANDS[i + 1] << LM;
        float sum = 1e-27f;
        for (int j = a; j < z; j++) sum += freq[j] * freq[j];
        float amp = sqrtf(sum);
        float g = 1.f / (1e-27f + amp);
        for (int j = a; j < z; j++) X[j] = freq[j] * g;
        logE[i] = log2f(amp) - CELT_E_MEANS[i];
    }

    // coarse energy: whole steps of 6 dB, predicted from the band below and the last frame
    float error[NB];
    {
        bool intra = !e->started;
        e->started = true;
        if (rc.tell() + 3 <= total_bits) rc.bitLogp(intra, 3);
        else intra = false;
        static const float PRED[4] = {29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f, 16384 / 32768.f};
        static const float BETA[4] = {30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f};
        float coef = intra ? 0 : PRED[LM], beta = intra ? 4915 / 32768.f : BETA[LM];
        const uint8_t* model = CELT_E_PROB[intra ? 1 : 0];
        float max_decay = .125f * nbytes < 16 ? .125f * nbytes : 16;
        float prev = 0;
        for (int i = 0; i < NB; i++) {
            float x = logE[i];
            float oldE = e->old_e[i] > -9 ? e->old_e[i] : -9;
            float f = x - coef * oldE - prev;
            int qi = (int)floorf(.5f + f);
            float decay_bound = (e->old_e[i] > -28 ? e->old_e[i] : -28) - max_decay;
            if (qi < 0 && x < decay_bound) {
                qi += (int)(decay_bound - x);
                if (qi > 0) qi = 0;
            }
            int tell = rc.tell();
            int bits_left = total_bits - tell - 3 * (NB - i);
            if (i != 0 && bits_left < 30) {
                if (bits_left < 24 && qi > 1) qi = 1;
                if (bits_left < 16 && qi < -1) qi = -1;
            }
            if (total_bits - tell >= 15) {
                int pi = 2 * (i < 20 ? i : 20);
                laplace(rc, &qi, model[pi] << 7, model[pi + 1] << 6);
            } else if (total_bits - tell >= 2) {
                static const uint8_t SMALL[3] = {2, 1, 0};
                qi = qi < -1 ? -1 : qi > 1 ? 1 : qi;
                rc.icdf(2 * qi ^ -(qi < 0), SMALL, 2);
            } else if (total_bits - tell >= 1) {
                qi = qi < 0 ? qi : 0;
                rc.bitLogp(-qi, 1);
            } else
                qi = -1;
            error[i] = f - qi;
            float q = (float)qi;
            float tmp = coef * oldE + prev + q;
            e->old_e[i] = tmp > -28 ? tmp : -28;
            prev = prev + q - beta * q;
        }
    }

    // time-frequency resolution: unchanged in every band
    {
        uint32_t budget = (uint32_t)nbytes * 8;
        int tell = rc.tell();
        int logp = 4;
        if (tell + logp + 1 <= (int)budget) budget--;  // (tf_select's reserve: never needed with no changes)
        for (int i = 0; i < NB; i++) {
            if (tell + logp <= (int)budget) {
                rc.bitLogp(0, logp);
                tell = rc.tell();
            }
            logp = 5;
        }
    }
    static const uint8_t SPREAD_ICDF[4] = {25, 23, 2, 0};
    if (rc.tell() + 4 <= total_bits) rc.icdf(SPREAD_NORMAL, SPREAD_ICDF, 5);

    int cap[NB];
    for (int i = 0; i < NB; i++) cap[i] = (CELT_CACHE_CAPS[NB * (2 * LM) + i] + 64) * (width(i) << LM) >> 2;

    // no band boosted (dynalloc): one "no" each while there's room
    int total_frac = total_bits << BITRES;
    int tell = rc.tellFrac();
    for (int i = 0; i < NB; i++)
        if (tell + (6 << BITRES) < total_frac && 0 < cap[i]) {
            rc.bitLogp(0, 6);
            tell = rc.tellFrac();
        }
    static const uint8_t TRIM_ICDF[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
    if (tell + (6 << BITRES) <= total_frac) rc.icdf(5, TRIM_ICDF, 7);

    // how the rest divides between the bands
    int pulses[NB], ebits[NB], fine_priority[NB], balance = 0;
    int bits = (nbytes * 8 << BITRES) - rc.tellFrac() - 1;
    int coded = allocate(rc, cap, 5, bits, &balance, pulses, ebits, fine_priority, e->last_coded);
    if (e->last_coded) {
        int lo = e->last_coded - 1, hi = e->last_coded + 1;
        e->last_coded = coded < lo ? lo : coded > hi ? hi : coded;
    } else
        e->last_coded = coded;

    // fine energy
    for (int i = 0; i < NB; i++) {
        if (ebits[i] <= 0) continue;
        int frac = 1 << ebits[i];
        int q2 = (int)floorf((error[i] + .5f) * frac);
        q2 = q2 > frac - 1 ? frac - 1 : q2 < 0 ? 0 : q2;
        rc.bits((uint32_t)q2, ebits[i]);
        float off = (q2 + .5f) * (1 << (14 - ebits[i])) * (1.f / 16384) - .5f;
        e->old_e[i] += off;
        error[i] -= off;
    }

    // the shapes (quant_all_bands, mono, one block)
    {
        Bands ctx = {&rc, 0, 0};
        int total = nbytes * (8 << BITRES);
        for (int i = 0; i < NB; i++) {
            int t = rc.tellFrac();
            if (i != 0) balance -= t;
            int remaining = total - t - 1;
            ctx.remaining = remaining;
            ctx.band = i;
            int b = 0;
            if (i <= coded - 1) {
                int div = coded - i < 3 ? coded - i : 3;
                int curr_balance = balance / div;
                b = pulses[i] + curr_balance;
                if (remaining + 1 < b) b = remaining + 1;
                if (16383 < b) b = 16383;
                if (b < 0) b = 0;
            }
            partition(ctx, X + (CELT_EBANDS[i] << LM), width(i) << LM, b, LM);
            balance += pulses[i] + t;
        }
    }

    // what's left goes to one more bit of energy for the bands that need it most
    int bits_left = total_bits - rc.tell();
    for (int prio = 0; prio < 2; prio++)
        for (int i = 0; i < NB && bits_left >= 1; i++) {
            if (ebits[i] >= MAX_FINE_BITS || fine_priority[i] != prio) continue;
            int q2 = error[i] < 0 ? 0 : 1;
            rc.bits((uint32_t)q2, 1);
            float off = (q2 - .5f) * (1 << (14 - ebits[i] - 1)) * (1.f / 16384);
            e->old_e[i] += off;
            error[i] -= off;
            bits_left--;
        }
    e->final_range = rc.rng;
    rc.done();
}
