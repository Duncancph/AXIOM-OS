/* AXIOM MP3 decoder: math helpers, shared tables and the polyphase synthesis filterbank.
 * No libm: cos/sqrt/cbrt are computed here (only at table-build time). */
#include "mp3_int.h"

#define PI 3.14159265358979323846

double mp3_cos(double x) {
    /* reduce to [-pi, pi], then a Taylor series (converges well there in double) */
    double k = x / (2 * PI);
    long n = (long)(k < 0 ? k - 0.5 : k + 0.5);
    x -= (double)n * 2 * PI;
    double x2 = x * x, term = 1, sum = 1;
    for (int i = 1; i < 30; i++) {
        term *= -x2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

double mp3_sin(double x) { return mp3_cos(x - PI / 2); }

double mp3_sqrt(double x) {
    if (x <= 0) return 0;
    double y = x > 1 ? x : 1;
    for (int i = 0; i < 200; i++) {
        double ny = 0.5 * (y + x / y);
        if (ny == y) break;
        y = ny;
    }
    return y;
}

static double cbrt_pos(double x) {
    if (x <= 0) return 0;
    double y = 1;
    while (y * y * y < x) y *= 2;
    for (int i = 0; i < 100; i++) {
        double ny = (2 * y + x / (y * y)) / 3;
        if (ny == y) break;
        y = ny;
    }
    return y;
}

static double pow2i(int q) {   /* 2^q for -1022 <= q <= 1023 */
    union { uint64_t u; double d; } v;
    v.u = (uint64_t)(q + 1023) << 52;
    return v.d;
}

float mp3_pow43[8207];
#define P2Q_MIN (-1000)
#define P2Q_MAX 400
static float pow2q_tab[P2Q_MAX - P2Q_MIN];
static float synth_d[512];     /* synthesis window, pre-scaled to 16-bit output */
static float dct_c[32];        /* Lee DCT factors: n=32 at [16], 16 at [8], 8 at [4], 4 at [2], 2 at [1] */
static int dsp_ready;

void mp3_l3_init(void);
void mp3_l12_init(void);

void mp3_dsp_init(void) {
    if (dsp_ready) return;
    for (int i = 0; i < 8207; i++) mp3_pow43[i] = (float)(i * cbrt_pos(i));
    static const double frac[4] = { 1.0, 1.18920711500272106672, 1.41421356237309504880, 1.68179283050742908606 };
    for (int e = P2Q_MIN; e < P2Q_MAX; e++) {
        int q = e >> 2;  /* floor */
        double v = q < -1000 ? 0 : pow2i(q < -1022 ? -1022 : q) * frac[e & 3];
        if (q < -1022) v = 0;
        pow2q_tab[e - P2Q_MIN] = (float)v;
    }
    for (int i = 0; i <= 256; i++) {
        double v = mp3_window_d[i] * (32768.0 / 65536.0);
        synth_d[i] = (float)v;
        if (i > 0 && i < 256) synth_d[512 - i] = (float)((i & 63) ? -v : v);
    }
    for (int n = 2; n <= 32; n *= 2)
        for (int k = 0; k < n / 2; k++)
            dct_c[n / 2 + k] = (float)(1.0 / (2.0 * mp3_cos(PI * (2 * k + 1) / (2.0 * n))));
    mp3_l3_init();
    mp3_l12_init();
    dsp_ready = 1;
}

float mp3_pow2q(int e) {
    if (e < P2Q_MIN) return 0;
    if (e >= P2Q_MAX) e = P2Q_MAX - 1;
    return pow2q_tab[e - P2Q_MIN];
}

/* Unnormalized DCT-II in place: X[j] = sum_k x[k] cos(pi j (2k+1) / (2n)), Lee's recursive split.
   tmp: n floats of scratch. */
static void dct2(float *x, int n, float *tmp) {
    if (n == 1) return;
    int h = n >> 1;
    const float *c = dct_c + h;
    for (int k = 0; k < h; k++) {
        float a = x[k], b = x[n - 1 - k];
        tmp[k] = a + b;
        tmp[h + k] = (a - b) * c[k];
    }
    dct2(tmp, h, x);
    dct2(tmp + h, h, x);
    for (int j = 0; j < h; j++) x[2 * j] = tmp[j];
    for (int j = 0; j < h - 1; j++) x[2 * j + 1] = tmp[h + j] + tmp[h + j + 1];
    x[n - 1] = tmp[n - 1];
}

void mp3_synth_reset(struct mp3_synth *s) { memset(s, 0, sizeof *s); }

static inline int16_t clip16(float v) {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)(v >= 0 ? (int)(v + 0.5f) : -(int)(0.5f - v));
}

void mp3_synth32(struct mp3_synth *s, int ch, const float *sb, int16_t *out, int stride) {
    float x[32], t[32];
    for (int i = 0; i < 32; i++) x[i] = sb[i];
    dct2(x, 32, t);
    /* the stored slot index is per channel: slot holds two 4-bit counters */
    int n = ((s->slot >> (ch * 4)) - 1) & 15;
    s->slot = (s->slot & ~(15 << (ch * 4))) | (n << (ch * 4));
    float *v = s->v[ch][n];
    /* V[i] = sum_k S[k] cos((16+i)(2k+1)pi/64), expressed through X */
    for (int i = 0; i < 16; i++) v[i] = x[16 + i];
    v[16] = 0;
    for (int i = 17; i < 48; i++) v[i] = -x[48 - i];
    for (int i = 48; i < 64; i++) v[i] = -x[i - 48];
    float acc[32];
    for (int j = 0; j < 32; j++) acc[j] = 0;
    for (int a = 0; a < 8; a++) {
        const float *ve = s->v[ch][(n + 2 * a) & 15];
        const float *vo = s->v[ch][(n + 2 * a + 1) & 15] + 32;
        const float *de = synth_d + 64 * a, *dd = de + 32;
        for (int j = 0; j < 32; j++) acc[j] += ve[j] * de[j] + vo[j] * dd[j];
    }
    for (int j = 0; j < 32; j++) out[j * stride] = clip16(acc[j]);
}
