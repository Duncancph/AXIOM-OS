/* AXIOM AAC decoder: internal declarations shared by the decoder sources. */
#pragma once
#include "media_port.h"
#include "aac.h"

#define AAC_MAX_CH 10          /* decoded channels before the downmix */
#define AAC_MAX_ELEM 16

enum { EL_SCE, EL_CPE, EL_CCE, EL_LFE, EL_DSE, EL_PCE, EL_FIL, EL_END };
enum { ONLY_LONG, LONG_START, EIGHT_SHORT, LONG_STOP };
enum { ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };
enum { AOT_MAIN = 1, AOT_LC = 2, AOT_SSR = 3, AOT_LTP = 4, AOT_SBR = 5, AOT_PS = 29 };

/* ---- math (aac_dsp.c) */
double aac_cos(double x);
double aac_sin(double x);
double aac_sqrt(double x);
double aac_exp(double x);
double aac_log(double x);
double aac_pow(double x, double y);   /* x > 0 */
static inline float aac_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
#define AAC_PI 3.14159265358979323846

/* ---- bit reader: the buffer must have >= 8 readable zero bytes after `end` */
struct aac_bits { const uint8_t *buf; int pos, end, err; };
static inline uint32_t aac_peek(const struct aac_bits *b, int n) {   /* n = 1..25 */
    const uint8_t *p = b->buf + (b->pos >> 3);
    uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (v << (b->pos & 7)) >> (32 - n);
}
static inline void aac_skip(struct aac_bits *b, int n) {
    b->pos += n;
    if (b->pos > b->end) { b->pos = b->end; b->err = 1; }
}
static inline uint32_t aac_get(struct aac_bits *b, int n) {         /* n = 0..25 */
    if (n == 0) return 0;
    uint32_t v = aac_peek(b, n);
    aac_skip(b, n);
    return v;
}
static inline uint32_t aac_get1(struct aac_bits *b) { return aac_get(b, 1); }
static inline uint32_t aac_getl(struct aac_bits *b, int n) {        /* n = 0..32 */
    if (n <= 16) return aac_get(b, n);
    uint32_t h = aac_get(b, n - 16);
    return h << 16 | aac_get(b, 16);
}
static inline int aac_left(const struct aac_bits *b) { return b->end - b->pos; }
static inline void aac_align(struct aac_bits *b, int start) {
    int r = (b->pos - start) & 7;
    if (r) aac_skip(b, 8 - r);
}

/* ---- Huffman decoding: two-level lookup tables built at init */
struct aac_hsrc { const uint16_t *code16; const uint32_t *code32; const uint8_t *bits; int n; };
struct aac_vlc { uint32_t *tab; int bits; };
/* entry: subtable: 0x80000000 | offset << 5 | subbits; leaf: sym << 5 | len (len 0: invalid code) */
int aac_vlc_build(struct aac_vlc *v, const struct aac_hsrc *src, int bits, int sym_offset);
static inline int aac_vlc_get(struct aac_bits *b, const struct aac_vlc *v) {
    uint32_t e = v->tab[aac_peek(b, v->bits)];
    if (e & 0x80000000u) {
        aac_skip(b, v->bits);
        int sb = (int)(e & 31);
        e = v->tab[((e & 0x7fffffffu) >> 5) + aac_peek(b, sb)];
    }
    int len = (int)(e & 31);
    if (!len) { b->err = 1; aac_skip(b, 1); return 0; }
    aac_skip(b, len);
    return (int)(e >> 5) - 0x40000;   /* symbols are stored with an offset so negatives fit */
}

/* ---- tables (aac_tables.c) */
extern const uint32_t aac_sf_code[121];
extern const uint8_t aac_sf_bits[121];
extern const struct aac_hsrc aac_spec_src[11];
extern const uint16_t *const aac_swb_long[13];
extern const uint16_t *const aac_swb_short[13];
extern const uint8_t aac_num_swb_long[13], aac_num_swb_short[13];
extern const uint8_t aac_tns_max_long[13], aac_tns_max_short[13];
extern const uint8_t aac_pred_sfb_max[13];
extern const int aac_rates[16];

/* ---- dsp (aac_dsp.c) */
void aac_dsp_init(void);
extern float aac_pow43[8192];
float aac_pow2q(int q);                 /* 2^(q/4), q in [-512, 512] (clamped) */
extern float aac_sine_long[1024], aac_sine_short[128], aac_kbd_long[1024], aac_kbd_short[128];   /* rising halves */
struct aac_cpx { float re, im; };
void aac_fft(struct aac_cpx *z, int log2n);   /* forward, in place, n <= 512 */
void aac_imdct(const float *in, float *out, int n);   /* n = 2048 or 256 output samples (n/2 inputs), scaled by 2/n */
void aac_mdct(const float *in, float *out, int n);    /* n input samples -> n/2 coefficients (forward MDCT, for LTP) */

/* ---- per-channel state */
struct aac_ics {
    int ws, shape, max_sfb, num_windows, num_groups;
    int group_len[8];
    const uint16_t *swb;
    int num_swb;
    int pred_present, pred_reset_group;
    uint8_t pred_used[48];
    int ltp_present, ltp_lag;
    float ltp_coef;
    uint8_t ltp_used[48];
};

struct aac_tns {
    int present;
    int n_filt[8];
    int length[8][4], order[8][4], dir[8][4];
    float lpc[8][4][20];
};

struct aac_pred { float r0, r1, cor0, cor1, var0, var1; };

struct aac_chan {
    struct aac_ics ics;
    uint8_t band_type[128];
    float sf[128];
    float coef[1024];
    float overlap[1024];
    int prev_shape;
    struct aac_tns tns;
    float *ltp_buf;          /* LTP: 3072 past output samples (AOT 4 only) */
    struct aac_pred *pred;   /* Main prediction: 672 predictors (AOT 1 only) */
    float out[2048];         /* time output of the last frame (1024, or 2048 after SBR) */
    int role;
};

struct aac_sbr;
struct aac_elem {
    int type, ch[2], nch;
    struct aac_sbr *sbr;
    int sbr_seen;            /* SBR data parsed in this frame */
};

/* output roles for the downmix */
enum { R_NONE, R_FC, R_FL, R_FR, R_FLC, R_FRC, R_SL, R_SR, R_BL, R_BR, R_BC, R_LFE };

struct aac_pce {
    int nfront, nside, nback, nlfe;
    uint8_t front_cpe[16], side_cpe[16], back_cpe[16];
};

struct aacdec {
    /* configuration */
    int aot, sfi, core_rate, chan_cfg;
    int sbr;                 /* 1: SBR present (explicit or seen), 0: no, -1: unknown (implicit possible) */
    int ps;                  /* 1: PS present, 0: no, -1: unknown */
    int ext_rate;
    int adts;                /* configured from ADTS headers */
    int configured;
    int have_pce;
    struct aac_pce pce;
    int out_ch, out_rate;
    int all_channels;        /* test hook: output every decoded channel in decode order, no downmix */
    /* state */
    struct aac_elem elem[AAC_MAX_ELEM];
    int elem_type_idx[AAC_MAX_ELEM];
    int nelem;
    struct aac_chan *ch[AAC_MAX_CH];
    int nch;
    struct aac_chan *scratch;        /* for coupling channel elements (parsed, not applied) */
    uint32_t rand_state;
    uint8_t *buf;
    int bufcap;
    int frames;
};

/* aac.c */
int aac_decode_ics(aacdec *d, struct aac_chan *c, struct aac_bits *b, int common_window, int scale_flag);

/* SBR / PS (aac_sbr.c, aac_ps.c) */
struct aac_sbr *aac_sbr_new(int core_rate);
void aac_sbr_free(struct aac_sbr *s);
void aac_sbr_reset(struct aac_sbr *s);
/* parses one SBR extension payload of cnt bytes (b at the extension_type nibble's end); returns 0 if bad */
int aac_sbr_parse(struct aac_sbr *s, struct aac_bits *b, int bits, int is_cpe, int crc, int *ps_seen);
/* in place: in[0..1023] core output -> out[0..2047]; ch1 may be 0. With PS (mono element), ch1 receives
   the right channel. */
void aac_sbr_apply(struct aac_sbr *s, float *ch0, float *ch1, int is_cpe, int ps_out);
