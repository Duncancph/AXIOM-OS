/* AXIOM AAC decoder: math helpers (no libm), windows, FFT, IMDCT/MDCT and Huffman table construction.
 * Transcendental functions are only used to build tables (and by SBR/PS per frame for a few values). */
#include "aac_int.h"

#define PI AAC_PI

double aac_cos(double x) {
    double k = x / (2 * PI);
    long n = (long)(k < 0 ? k - 0.5 : k + 0.5);
    x -= (double)n * 2 * PI;                       /* [-pi, pi] */
    double s = 1;
    if (x < 0) x = -x;
    if (x > PI / 2) { x = PI - x; s = -1; }        /* [0, pi/2] */
    double x2 = x * x, term = 1, sum = 1;
    for (int i = 1; i < 14; i++) {
        term *= -x2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return s * sum;
}

double aac_sin(double x) { return aac_cos(x - PI / 2); }

double aac_sqrt(double x) {
    if (x <= 0) return 0;
    double y = x > 1 ? x : 1;
    for (int i = 0; i < 300; i++) {
        double ny = 0.5 * (y + x / y);
        if (ny >= y) break;
        y = ny;
    }
    return y;
}

static double ldexp_i(double x, int e) {
    while (e > 1000) { x *= 1.0715086071862673e301; e -= 1000; }
    while (e < -1000) { x *= 9.332636185032189e-302; e += 1000; }
    union { uint64_t u; double d; } v;
    v.u = (uint64_t)(e + 1023) << 52;
    return x * v.d;
}

double aac_exp(double x) {
    if (x > 700) x = 700;
    if (x < -700) return 0;
    /* x = k ln2 + r, |r| <= ln2/2 */
    double kf = x / 0.69314718055994530942;
    int k = (int)(kf < 0 ? kf - 0.5 : kf + 0.5);
    double r = x - k * 0.69314718055994530942;
    double term = 1, sum = 1;
    for (int i = 1; i < 20; i++) {
        term *= r / i;
        sum += term;
    }
    return ldexp_i(sum, k);
}

double aac_log(double x) {
    if (x <= 0) return -1e300;
    union { uint64_t u; double d; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;   /* m in [1, 2) */
    double m = v.d;
    if (m > 1.41421356237309504880) { m *= 0.5; e++; }
    double t = (m - 1) / (m + 1), t2 = t * t, term = t, sum = 0;
    for (int i = 1; i < 60; i += 2) {
        sum += term / i;
        term *= t2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

double aac_pow(double x, double y) { return aac_exp(y * aac_log(x)); }

/* ---------------------------------------------------------------- tables */
float aac_pow43[8192];
static float pow2q_tab[1025];
float aac_sine_long[1024], aac_sine_short[128], aac_kbd_long[1024], aac_kbd_short[128];
static struct aac_cpx tw512[256];       /* exp(-2 pi i k / 512) */
static uint16_t bitrev[10][512];
static struct aac_cpx imdct_tw_long[512], imdct_tw_short[64];
static int dsp_ready;

float aac_pow2q(int q) {
    if (q < -512) q = -512;
    if (q > 512) q = 512;
    return pow2q_tab[q + 512];
}

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 60; k++) {
        term *= (x / 2) / k;
        double t2 = term * term;
        sum += t2;
        if (t2 < sum * 1e-18) break;
    }
    return sum;
}

static void kbd_window(float *w, int n2, double alpha) {   /* rising half, n2 values (window length 2*n2) */
    double tmp[1025], sum = 0;
    for (int i = 0; i <= n2; i++) {
        double r = (double)(i - n2 / 2) / (n2 / 2);
        double a = 1 - r * r;
        tmp[i] = bessel_i0(PI * alpha * aac_sqrt(a < 0 ? 0 : a));
        sum += tmp[i];
    }
    double acc = 0;
    for (int i = 0; i < n2; i++) {
        acc += tmp[i];
        w[i] = (float)aac_sqrt(acc / sum);
    }
}

static double cbrt_pos(double x) {
    if (x <= 0) return 0;
    double y = 1;
    while (y * y * y < x) y *= 2;
    for (int i = 0; i < 200; i++) {
        double ny = (2 * y + x / (y * y)) / 3;
        if (ny >= y) break;
        y = ny;
    }
    return y;
}

void aac_dsp_init(void) {
    if (dsp_ready) return;
    for (int i = 0; i < 8192; i++) aac_pow43[i] = (float)(i * cbrt_pos(i));
    static const double frac[4] = { 1.0, 1.18920711500272106672, 1.41421356237309504880, 1.68179283050742908606 };
    for (int q = -512; q <= 512; q++) {
        int e = q >> 2;   /* floor */
        pow2q_tab[q + 512] = (float)ldexp_i(frac[q & 3], e);
    }
    for (int i = 0; i < 1024; i++) aac_sine_long[i] = (float)aac_sin(PI / 2048 * (i + 0.5));
    for (int i = 0; i < 128; i++) aac_sine_short[i] = (float)aac_sin(PI / 256 * (i + 0.5));
    kbd_window(aac_kbd_long, 1024, 4);
    kbd_window(aac_kbd_short, 128, 6);
    for (int k = 0; k < 256; k++) {
        tw512[k].re = (float)aac_cos(2 * PI * k / 512);
        tw512[k].im = (float)-aac_sin(2 * PI * k / 512);
    }
    for (int l = 1; l <= 9; l++) {
        int n = 1 << l;
        for (int i = 0; i < n; i++) {
            int r = 0;
            for (int b = 0; b < l; b++)
                if (i & (1 << b)) r |= 1 << (l - 1 - b);
            bitrev[l][i] = (uint16_t)r;
        }
    }
    /* DCT-IV twiddles exp(-i pi (k + 1/8) / M), M = n/4 complex points * 2 */
    for (int k = 0; k < 512; k++) {
        double a = -PI * (k + 0.125) / 1024;
        imdct_tw_long[k].re = (float)aac_cos(a);
        imdct_tw_long[k].im = (float)aac_sin(a);
    }
    for (int k = 0; k < 64; k++) {
        double a = -PI * (k + 0.125) / 128;
        imdct_tw_short[k].re = (float)aac_cos(a);
        imdct_tw_short[k].im = (float)aac_sin(a);
    }
    dsp_ready = 1;
}

/* ---------------------------------------------------------------- FFT */
void aac_fft(struct aac_cpx *z, int log2n) {
    int n = 1 << log2n;
    const uint16_t *rev = bitrev[log2n];
    for (int i = 0; i < n; i++) {
        int j = rev[i];
        if (j > i) { struct aac_cpx t = z[i]; z[i] = z[j]; z[j] = t; }
    }
    /* radix-2 first stage */
    for (int i = 0; i < n; i += 2) {
        struct aac_cpx a = z[i], b = z[i + 1];
        z[i].re = a.re + b.re; z[i].im = a.im + b.im;
        z[i + 1].re = a.re - b.re; z[i + 1].im = a.im - b.im;
    }
    for (int size = 4; size <= n; size <<= 1) {
        int half = size >> 1, step = 512 / size;
        for (int i = 0; i < n; i += size) {
            struct aac_cpx *p = z + i, *q = z + i + half;
            for (int k = 0; k < half; k++) {
                struct aac_cpx w = tw512[k * step];
                float tr = q[k].re * w.re - q[k].im * w.im;
                float ti = q[k].re * w.im + q[k].im * w.re;
                q[k].re = p[k].re - tr; q[k].im = p[k].im - ti;
                p[k].re += tr; p[k].im += ti;
            }
        }
    }
}

/* DCT-IV of size M (M = 1024 or 128), unscaled: u[n] = sum X[k] cos(pi/M (n+1/2)(k+1/2)) */
static void dct4(const float *x, float *u, int M) {
    struct aac_cpx z[512];
    const struct aac_cpx *tw = M == 1024 ? imdct_tw_long : imdct_tw_short;
    int h = M / 2;
    for (int k = 0; k < h; k++) {
        float re = x[2 * k], im = x[M - 1 - 2 * k];
        z[k].re = re * tw[k].re - im * tw[k].im;
        z[k].im = re * tw[k].im + im * tw[k].re;
    }
    aac_fft(z, M == 1024 ? 9 : 6);
    for (int k = 0; k < h; k++) {
        float re = z[k].re * tw[k].re - z[k].im * tw[k].im;
        float im = z[k].re * tw[k].im + z[k].im * tw[k].re;
        u[2 * k] = re;
        u[M - 1 - 2 * k] = -im;
    }
}

void aac_imdct(const float *in, float *out, int n) {
    int M = n / 2;
    float u[1024];
    dct4(in, u, M);
    float s = 2.0f / (float)n;
    int h = M / 2;
    for (int i = 0; i < h; i++) out[i] = u[i + h] * s;                      /* n in [0, M/2) */
    for (int i = h; i < 3 * h; i++) out[i] = -u[3 * h - 1 - i] * s;        /* [M/2, 3M/2) */
    for (int i = 3 * h; i < 4 * h; i++) out[i] = -u[i - 3 * h] * s;        /* [3M/2, 2M) */
}

/* forward MDCT: X[k] = sum_{n<N} x[n] cos(2pi/N (n + n0)(k + 1/2)), N = n, n0 = (N/2 + 1)/2.
   Folding to a DCT-IV of size M = N/2. */
void aac_mdct(const float *in, float *out, int n) {
    int M = n / 2, h = M / 2;
    float t[1024];
    /* X[k] = sum_{m<M} t[m] cos(pi/M (m+1/2)(k+1/2)) with t from the time-domain aliasing */
    for (int m = 0; m < h; m++) {
        t[m] = -in[3 * h - 1 - m] - in[3 * h + m];
        t[h + m] = in[m] - in[M - 1 - m];
    }
    /* reorder: the DCT-IV index is m' = m + ... (see derivation in README) */
    float v[1024];
    for (int m = 0; m < M; m++) v[m] = t[m];
    dct4(v, out, M);
}

/* ---------------------------------------------------------------- Huffman tables */
static uint32_t hcode(const struct aac_hsrc *s, int i) { return s->code16 ? s->code16[i] : s->code32[i]; }

int aac_vlc_build(struct aac_vlc *v, const struct aac_hsrc *src, int bits, int sym_offset) {
    int maxlen = 0;
    for (int i = 0; i < src->n; i++)
        if (src->bits[i] > maxlen) maxlen = src->bits[i];
    int prim = 1 << bits;
    int sub[1024];
    memset(sub, 0, sizeof(int) * prim);
    for (int i = 0; i < src->n; i++) {
        int len = src->bits[i];
        if (len > bits) {
            int p = (int)(hcode(src, i) >> (len - bits));
            if (len - bits > sub[p]) sub[p] = len - bits;
        }
    }
    int total = prim;
    for (int p = 0; p < prim; p++)
        if (sub[p]) total += 1 << sub[p];
    if (total <= 0 || total > (1 << 22)) return 0;
    uint32_t *t = calloc((size_t)total, sizeof *t);
    if (!t) return 0;
    int off = prim;
    for (int p = 0; p < prim; p++)
        if (sub[p]) {
            t[p] = 0x80000000u | (uint32_t)off << 5 | (uint32_t)sub[p];
            off += 1 << sub[p];
        }
    for (int i = 0; i < src->n; i++) {
        int len = src->bits[i];
        uint32_t code = hcode(src, i);
        uint32_t sym = (uint32_t)(i + sym_offset + 0x40000);
        if (len <= bits) {
            int shift = bits - len;
            for (int j = 0; j < (1 << shift); j++) t[(code << shift) + (uint32_t)j] = sym << 5 | (uint32_t)len;
        } else {
            int p = (int)(code >> (len - bits));
            int sb = sub[p];
            int base = (int)((t[p] & 0x7fffffffu) >> 5);
            int rest = len - bits;
            uint32_t low = code & ((1u << rest) - 1);
            int shift = sb - rest;
            for (int j = 0; j < (1 << shift); j++) t[base + (int)(low << shift) + j] = sym << 5 | (uint32_t)rest;
        }
    }
    v->tab = t;
    v->bits = bits;
    return 1;
}
