/* AXIOM MP3 decoder: Layer III (MPEG-1, MPEG-2 LSF, MPEG-2.5).
 * Pipeline per granule and channel: side info -> scalefactors -> Huffman -> requantize ->
 * stereo (MS / intensity) -> short-block reorder -> alias reduction -> IMDCT + overlap ->
 * frequency inversion -> polyphase synthesis. Spectra are kept in "storage order"
 * (short blocks as [sfb][window][line]) until the reorder step. */
#include "mp3_int.h"

#define PI 3.14159265358979323846

/* Scalefactor band widths, sr_table order 44.1 48 32 22.05 24 16 11.025 12 8 kHz. */
static const uint8_t sfb_long_w[9][22] = {
    { 4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 20, 24, 28, 34, 42, 50, 54, 76, 158 },
    { 4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 10, 12, 16, 18, 22, 28, 34, 40, 46, 54, 54, 192 },
    { 4, 4, 4, 4, 4, 4, 6, 6, 8, 10, 12, 16, 20, 24, 30, 38, 46, 56, 68, 84, 102, 26 },
    { 6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54 },
    { 6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 18, 22, 26, 32, 38, 46, 54, 62, 70, 76, 36 },
    { 6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54 },
    { 6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54 },
    { 6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54 },
    { 12, 12, 12, 12, 12, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 76, 90, 2, 2, 2, 2, 2 },
};
static const uint8_t sfb_short_w[9][13] = {
    { 4, 4, 4, 4, 6, 8, 10, 12, 14, 18, 22, 30, 56 },
    { 4, 4, 4, 4, 6, 6, 10, 12, 14, 16, 20, 26, 66 },
    { 4, 4, 4, 4, 6, 8, 12, 16, 20, 26, 34, 42, 12 },
    { 4, 4, 4, 6, 6, 8, 10, 14, 18, 26, 32, 42, 18 },
    { 4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 32, 44, 12 },
    { 4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18 },
    { 4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18 },
    { 4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18 },
    { 8, 8, 8, 12, 16, 20, 24, 28, 36, 2, 2, 2, 26 },
};
static const uint8_t pretab[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0 };
static const uint8_t slen_tab[2][16] = {
    { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 },
    { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 },
};
/* MPEG-2 LSF: scalefactors per partition [table][long, short, mixed][partition] */
static const uint8_t lsf_nsfb[6][3][4] = {
    { { 6, 5, 5, 5 }, { 9, 9, 9, 9 }, { 6, 9, 9, 9 } },
    { { 6, 5, 7, 3 }, { 9, 9, 12, 6 }, { 6, 9, 12, 6 } },
    { { 11, 10, 0, 0 }, { 18, 18, 0, 0 }, { 15, 18, 0, 0 } },
    { { 7, 7, 7, 0 }, { 12, 12, 12, 0 }, { 6, 15, 12, 0 } },
    { { 6, 6, 6, 3 }, { 12, 9, 9, 6 }, { 6, 12, 9, 6 } },
    { { 8, 8, 5, 0 }, { 15, 12, 9, 0 }, { 6, 18, 9, 0 } },
};

/* ------------------------------------------------------------ tables built at init */
#define HLUT_MAX 24576
static uint32_t hlut[HLUT_MAX];
static int hlut_n;
static int hroot[34], hrootbits[34];
static float imdct_l[4][36][18];   /* window * cos, per block type (2 unused) */
static float imdct_s[12][6];       /* short window * cos */
static float alias_cs[8], alias_ca[8];
static float is1_l[7], is1_r[7];   /* MPEG-1 intensity ratios */
static float is2_tab[2][32];       /* MPEG-2 intensity: i0^n for intensity_scale 0/1 */

/* Builds one level of a Huffman lookup table for the codes sharing `prefix` (depth bits long). */
static int hbuild(const struct mp3_hufsrc *t, int n, int depth, uint32_t prefix, int bits) {
    int base = hlut_n;
    hlut_n += 1 << bits;
    if (hlut_n > HLUT_MAX) { hlut_n = base; return -1; }
    uint8_t maxrem[256];
    for (int i = 0; i < (1 << bits); i++) { hlut[base + i] = 0; maxrem[i] = 0; }
    for (int i = 0; i < n; i++) {
        int L = t->len[i];
        if (L <= depth || (uint32_t)(t->code[i] >> (L - depth)) != prefix) continue;
        int rem = L - depth;
        uint32_t low = t->code[i] & ((1u << rem) - 1);
        uint32_t sym = (uint32_t)((i / t->side) << 4 | (i % t->side));
        if (rem <= bits) {
            int idx = (int)(low << (bits - rem));
            for (int k = 0; k < (1 << (bits - rem)); k++) hlut[base + idx + k] = sym | (uint32_t)rem << 16;
        } else {
            int idx = (int)(low >> (rem - bits));
            if (rem - bits > maxrem[idx]) maxrem[idx] = (uint8_t)(rem - bits);
        }
    }
    for (int idx = 0; idx < (1 << bits); idx++) {
        if (!maxrem[idx]) continue;
        int sub = maxrem[idx] > 8 ? 8 : maxrem[idx];
        int child = hbuild(t, n, depth + bits, (prefix << bits) | (uint32_t)idx, sub);
        if (child < 0) return -1;
        hlut[base + idx] = 0x80000000u | (uint32_t)sub << 24 | (uint32_t)child;
    }
    return base;
}

void mp3_l3_init(void) {
    hlut_n = 0;
    for (int t = 0; t < 34; t++) {
        const struct mp3_hufsrc *s = &mp3_hufsrc[t];
        hroot[t] = -1;
        if (!s->code) continue;
        if (t > 16 && t < 24) { hroot[t] = hroot[16]; hrootbits[t] = hrootbits[16]; continue; }
        if (t > 24 && t < 32) { hroot[t] = hroot[24]; hrootbits[t] = hrootbits[24]; continue; }
        int n = s->side * s->side, maxl = 0;
        for (int i = 0; i < n; i++) if (s->len[i] > maxl) maxl = s->len[i];
        hrootbits[t] = maxl > 8 ? 8 : maxl;
        hroot[t] = hbuild(s, n, 0, 0, hrootbits[t]);
    }
    /* IMDCT with windows folded in */
    for (int bt = 0; bt < 4; bt++) {
        for (int i = 0; i < 36; i++) {
            double w;
            if (bt == 0 || bt == 2) w = mp3_sin(PI / 36 * (i + 0.5));
            else if (bt == 1) w = i < 18 ? mp3_sin(PI / 36 * (i + 0.5)) : i < 24 ? 1 : i < 30 ? mp3_sin(PI / 12 * (i - 18 + 0.5)) : 0;
            else w = i < 6 ? 0 : i < 12 ? mp3_sin(PI / 12 * (i - 6 + 0.5)) : i < 18 ? 1 : mp3_sin(PI / 36 * (i + 0.5));
            for (int k = 0; k < 18; k++)
                imdct_l[bt][i][k] = (float)(w * mp3_cos(PI / 72 * (2 * i + 1 + 18) * (2 * k + 1)));
        }
    }
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++)
            imdct_s[i][k] = (float)(mp3_sin(PI / 12 * (i + 0.5)) * mp3_cos(PI / 24 * (2 * i + 1 + 6) * (2 * k + 1)));
    static const double ci[8] = { -0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037 };
    for (int i = 0; i < 8; i++) {
        double sq = mp3_sqrt(1 + ci[i] * ci[i]);
        alias_cs[i] = (float)(1 / sq);
        alias_ca[i] = (float)(ci[i] / sq);
    }
    for (int i = 0; i < 7; i++) {
        double s = mp3_sin(PI / 12 * i), c = mp3_cos(PI / 12 * i);
        if (i == 6) c = 0;
        is1_l[i] = (float)(s / (s + c));
        is1_r[i] = (float)(c / (s + c));
    }
    for (int sc = 0; sc < 2; sc++) {
        double i0 = sc ? 0.70710678118654752440 : 0.84089641525371454303, v = 1;
        for (int n = 0; n < 32; n++) { is2_tab[sc][n] = (float)v; v *= i0; }
    }
}

/* ------------------------------------------------------------ side info */
struct gran {
    int part23, big_values, global_gain, sfc, block_type, mixed;
    int table[3], sbg[3], r1start, r2start, preflag, scale, c1table;
};
struct side {
    int mdb, scfsi[2];
    struct gran g[2][2];
};

static int parse_side(struct mp3_bits *b, const struct mp3_hdr *h, struct side *s) {
    int nch = h->channels, ngr = h->lsf ? 1 : 2;
    const uint8_t *lw = sfb_long_w[h->sr_table];
    if (!h->lsf) {
        s->mdb = (int)mp3_getbits(b, 9);
        mp3_getbits(b, nch == 1 ? 5 : 3);
        for (int ch = 0; ch < nch; ch++) s->scfsi[ch] = (int)mp3_getbits(b, 4);
    } else {
        s->mdb = (int)mp3_getbits(b, 8);
        mp3_getbits(b, nch == 1 ? 1 : 2);
        s->scfsi[0] = s->scfsi[1] = 0;
    }
    for (int gr = 0; gr < ngr; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            struct gran *g = &s->g[gr][ch];
            g->part23 = (int)mp3_getbits(b, 12);
            g->big_values = (int)mp3_getbits(b, 9);
            if (g->big_values > 288) g->big_values = 288;
            g->global_gain = (int)mp3_getbits(b, 8);
            g->sfc = (int)mp3_getbits(b, h->lsf ? 9 : 4);
            int r0, r1;
            if (mp3_getbits(b, 1)) {
                g->block_type = (int)mp3_getbits(b, 2);
                g->mixed = (int)mp3_getbits(b, 1);
                for (int i = 0; i < 2; i++) g->table[i] = (int)mp3_getbits(b, 5);
                g->table[2] = 0;
                for (int i = 0; i < 3; i++) g->sbg[i] = (int)mp3_getbits(b, 3);
                if (g->block_type == 0) return -1;   /* forbidden */
                if (g->block_type != 2) g->mixed = 0;
                if (g->block_type == 2) {
                    const uint8_t *sw = sfb_short_w[h->sr_table];
                    g->r1start = 3 * (sw[0] + sw[1] + sw[2]);
                } else {
                    int st = 0;
                    for (int i = 0; i < 8; i++) st += lw[i];
                    g->r1start = st;
                }
                g->r2start = 576;
            } else {
                g->block_type = 0;
                g->mixed = 0;
                for (int i = 0; i < 3; i++) g->table[i] = (int)mp3_getbits(b, 5);
                g->sbg[0] = g->sbg[1] = g->sbg[2] = 0;
                r0 = (int)mp3_getbits(b, 4);
                r1 = (int)mp3_getbits(b, 3);
                int st = 0, i;
                for (i = 0; i < r0 + 1 && i < 22; i++) st += lw[i];
                g->r1start = st;
                for (; i < r0 + r1 + 2 && i < 22; i++) st += lw[i];
                g->r2start = st;
            }
            g->preflag = h->lsf ? 0 : (int)mp3_getbits(b, 1);
            g->scale = (int)mp3_getbits(b, 1);
            g->c1table = (int)mp3_getbits(b, 1);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ band layout in storage order */
struct bands {
    int n;
    uint16_t start[40];
    uint8_t width[40];
    int8_t win[40];     /* -1: long band, 0..2: short window */
    int8_t sfb[40];
    int long_end;       /* first line of the short part (576 for long blocks) */
};

static void make_bands(const struct mp3_hdr *h, const struct gran *g, struct bands *bd) {
    const uint8_t *lw = sfb_long_w[h->sr_table], *sw = sfb_short_w[h->sr_table];
    int n = 0, pos = 0;
    if (g->block_type != 2) {
        for (int s = 0; s < 22; s++) {
            bd->start[n] = (uint16_t)pos; bd->width[n] = lw[s]; bd->win[n] = -1; bd->sfb[n] = (int8_t)s;
            pos += lw[s]; n++;
        }
        bd->long_end = 576;
    } else {
        int s0 = 0;
        if (g->mixed) {
            int nl = h->lsf ? 6 : 8;
            for (int s = 0; s < nl; s++) {
                bd->start[n] = (uint16_t)pos; bd->width[n] = lw[s]; bd->win[n] = -1; bd->sfb[n] = (int8_t)s;
                pos += lw[s]; n++;
            }
            s0 = 3;
        }
        bd->long_end = pos;
        for (int s = s0; s < 13; s++)
            for (int w = 0; w < 3; w++) {
                bd->start[n] = (uint16_t)pos; bd->width[n] = sw[s]; bd->win[n] = (int8_t)w; bd->sfb[n] = (int8_t)s;
                pos += sw[s]; n++;
            }
    }
    bd->n = n;
}

/* ------------------------------------------------------------ scalefactors */
static void read_scf_mpeg1(struct mp3_bits *b, const struct gran *g, int scfsi, int gr, uint8_t *scf, uint8_t *prev) {
    int s1 = slen_tab[0][g->sfc], s2 = slen_tab[1][g->sfc];
    memset(scf, 0, 40);
    if (g->block_type == 2) {
        int n1 = g->mixed ? 17 : 18, i = 0;
        for (int k = 0; k < n1; k++) scf[i++] = (uint8_t)mp3_getbits(b, s1);
        for (int k = 0; k < 18; k++) scf[i++] = (uint8_t)mp3_getbits(b, s2);
    } else {
        static const uint8_t grp[5] = { 0, 6, 11, 16, 21 };
        for (int k = 0; k < 4; k++) {
            int sl = k < 2 ? s1 : s2;
            if (gr == 1 && (scfsi >> (3 - k) & 1)) {
                for (int i = grp[k]; i < grp[k + 1]; i++) scf[i] = prev[i];
            } else {
                for (int i = grp[k]; i < grp[k + 1]; i++) scf[i] = (uint8_t)mp3_getbits(b, sl);
            }
        }
    }
    if (gr == 0) memcpy(prev, scf, 40);
}

static void read_scf_lsf(struct mp3_bits *b, const struct gran *g, int is_right, uint8_t *scf, uint8_t *ismax, int *preflag) {
    int slen[4], tab, sfc = g->sfc;
    *preflag = 0;
    if (!is_right) {
        if (sfc < 400) {
            slen[0] = (sfc >> 4) / 5; slen[1] = (sfc >> 4) % 5; slen[2] = (sfc & 15) >> 2; slen[3] = sfc & 3; tab = 0;
        } else if (sfc < 500) {
            sfc -= 400;
            slen[0] = (sfc >> 2) / 5; slen[1] = (sfc >> 2) % 5; slen[2] = sfc & 3; slen[3] = 0; tab = 1;
        } else {
            sfc -= 500;
            slen[0] = sfc / 3; slen[1] = sfc % 3; slen[2] = 0; slen[3] = 0; tab = 2;
            *preflag = 1;
        }
    } else {
        int isc = sfc >> 1;
        if (isc < 180) {
            slen[0] = isc / 36; slen[1] = (isc % 36) / 6; slen[2] = (isc % 36) % 6; slen[3] = 0; tab = 3;
        } else if (isc < 244) {
            isc -= 180;
            slen[0] = (isc & 63) >> 4; slen[1] = (isc & 15) >> 2; slen[2] = isc & 3; slen[3] = 0; tab = 4;
        } else {
            isc -= 244;
            slen[0] = isc / 3; slen[1] = isc % 3; slen[2] = 0; slen[3] = 0; tab = 5;
        }
    }
    int bt = g->block_type == 2 ? (g->mixed ? 2 : 1) : 0;
    int n = 0;
    memset(scf, 0, 40);
    for (int p = 0; p < 4; p++)
        for (int k = 0; k < lsf_nsfb[tab][bt][p] && n < 40; k++) {
            scf[n] = (uint8_t)mp3_getbits(b, slen[p]);
            ismax[n] = (uint8_t)((1 << slen[p]) - 1);
            n++;
        }
    for (; n < 40; n++) ismax[n] = 0;
}

/* ------------------------------------------------------------ Huffman */
static inline uint32_t hdecode(struct mp3_bits *b, int t) {
    uint32_t w = mp3_peek32(b);
    int base = hroot[t], bits = hrootbits[t], used = 0;
    for (;;) {
        uint32_t e = hlut[base + (int)(w >> (32 - bits))];
        if (!(e & 0x80000000u)) { b->pos += used + (int)((e >> 16) & 0xff); return e & 0xffff; }
        used += bits;
        w <<= bits;
        bits = (int)(e >> 24) & 15;
        base = (int)(e & 0xffffff);
    }
}

/* Decodes the spectrum values of one granule/channel into iv[576]; returns the count of decoded lines. */
static int huffman(struct mp3_bits *b, const struct gran *g, int *iv) {
    int i = 0, bigend = g->big_values * 2;
    int rend[3] = { g->r1start, g->r2start, 576 };
    for (int r = 0; r < 3; r++) {
        int e = rend[r] < bigend ? rend[r] : bigend;
        int t = g->table[r];
        if (i >= e) continue;
        if (hroot[t] < 0) {   /* table 0 (or invalid 4/14): zeros */
            for (; i < e; i++) iv[i] = 0;
            continue;
        }
        int lin = mp3_hufsrc[t].linbits;
        for (; i < e; i += 2) {
            if (b->pos >= b->end) {
                for (; i < 576; i++) iv[i] = 0;
                return 576;
            }
            uint32_t s = hdecode(b, t);
            int x = (int)(s >> 4), y = (int)(s & 15);
            if (x == 15 && lin) x += (int)mp3_getbits(b, lin);
            if (x && mp3_getbits(b, 1)) x = -x;
            if (y == 15 && lin) y += (int)mp3_getbits(b, lin);
            if (y && mp3_getbits(b, 1)) y = -y;
            iv[i] = x;
            iv[i + 1] = y;
        }
    }
    int ct = 32 + g->c1table;
    while (i + 4 <= 576 && b->pos < b->end) {
        uint32_t s = hdecode(b, ct);   /* index v*8+w*4+x*2+y, stored as (v*2+w)<<4 | (x*2+y) */
        int v[4] = { (int)(s >> 5) & 1, (int)(s >> 4) & 1, (int)(s >> 1) & 1, (int)s & 1 };
        for (int k = 0; k < 4; k++)
            if (v[k] && mp3_getbits(b, 1)) v[k] = -1;
        if (b->pos > b->end) break;   /* the last quadruple overran part2_3_length: discard it */
        iv[i] = v[0]; iv[i + 1] = v[1]; iv[i + 2] = v[2]; iv[i + 3] = v[3];
        i += 4;
    }
    int nz = i;
    for (; i < 576; i++) iv[i] = 0;
    return nz;
}

/* ------------------------------------------------------------ requantization */
static void requantize(const struct gran *g, const struct bands *bd, const uint8_t *scf, int preflag,
                       const int *iv, int nz, float *xr) {
    int shift = g->scale + 1;
    int gg = g->global_gain - 210;
    for (int k = 0; k < bd->n; k++) {
        int s = bd->start[k], e = s + bd->width[k];
        if (s >= nz) {
            for (int i = s; i < 576; i++) xr[i] = 0;
            return;
        }
        int ex;
        if (bd->win[k] < 0) ex = gg - ((scf[k] + (preflag ? pretab[bd->sfb[k]] : 0)) << shift);
        else ex = gg - 8 * g->sbg[bd->win[k]] - (scf[k] << shift);
        float gain = mp3_pow2q(ex);
        if (e > 576) e = 576;
        for (int i = s; i < e; i++) {
            int v = iv[i];
            xr[i] = v == 0 ? 0.0f : v > 0 ? mp3_pow43[v] * gain : -mp3_pow43[-v] * gain;
        }
    }
}

/* ------------------------------------------------------------ stereo */
static void ms_lines(float *l, float *r, int s, int e) {
    const float k = 0.70710678118654752440f;
    for (int i = s; i < e; i++) {
        float m = l[i], d = r[i];
        l[i] = (m + d) * k;
        r[i] = (m - d) * k;
    }
}

static void stereo(const struct mp3_hdr *h, const struct gran *g1, const struct bands *bd,
                   const uint8_t *scf1, const uint8_t *ismax, float *l, float *r, int nz) {
    int ms = h->mode_ext & 2, is = h->mode_ext & 1;
    if (!is) {
        if (ms) ms_lines(l, r, 0, nz);
        return;
    }
    /* last band (by sfb) with non-zero right-channel data, per window; long part separately */
    int lastw[3] = { -1, -1, -1 }, last_long = -1;
    for (int k = 0; k < bd->n; k++) {
        int s = bd->start[k], e = s + bd->width[k], any = 0;
        if (e > 576) e = 576;
        for (int i = s; i < e; i++) if (r[i] != 0) { any = 1; break; }
        if (!any) continue;
        if (bd->win[k] < 0) last_long = bd->sfb[k];
        else if (bd->sfb[k] > lastw[bd->win[k]]) lastw[bd->win[k]] = bd->sfb[k];
    }
    int short_nz = lastw[0] >= 0 || lastw[1] >= 0 || lastw[2] >= 0;
    int has_short = g1->block_type == 2;
    int iscale = g1->sfc & 1;
    for (int k = 0; k < bd->n; k++) {
        int w = bd->win[k], sfb = bd->sfb[k];
        int in_is;
        if (w < 0) in_is = sfb > last_long && !(has_short && short_nz);
        else in_is = sfb > lastw[w];
        int s = bd->start[k], e = s + bd->width[k];
        if (e > 576) e = 576;
        int pos = -1;
        if (in_is) {
            /* the last band has no scalefactor of its own: it uses the one of the band below (same window) */
            int kk = (w < 0 ? sfb == 21 : sfb == 12) ? k - (w < 0 ? 1 : 3) : k;
            pos = scf1[kk];
            if (h->lsf ? pos == ismax[kk] : pos >= 7) pos = -1;
        }
        if (pos < 0) {
            if (ms) ms_lines(l, r, s, e);
            continue;
        }
        float kl, kr;
        if (!h->lsf) { kl = is1_l[pos]; kr = is1_r[pos]; }
        else if (pos == 0) { kl = kr = 1; }
        else if (pos & 1) { kl = is2_tab[iscale][(pos + 1) >> 1]; kr = 1; }
        else { kl = 1; kr = is2_tab[iscale][pos >> 1]; }
        for (int i = s; i < e; i++) {
            float v = l[i];
            l[i] = v * kl;
            r[i] = v * kr;
        }
    }
}

/* ------------------------------------------------------------ hybrid filterbank */
static void reorder(const struct bands *bd, float *xr, float *tmp) {
    int k = 0;
    while (k < bd->n && bd->win[k] < 0) k++;
    int base = bd->long_end, p = base;
    for (; k + 2 < bd->n; k += 3) {
        int w = bd->width[k];
        for (int j = 0; j < w; j++)
            for (int win = 0; win < 3; win++) tmp[p - base + 3 * j + win] = xr[p + win * w + j];
        p += 3 * w;
    }
    memcpy(xr + base, tmp, (size_t)(p - base) * sizeof(float));
}

static void alias_reduce(float *xr, int nsb) {
    for (int sb = 1; sb < nsb; sb++) {
        float *a = xr + 18 * sb;
        for (int i = 0; i < 8; i++) {
            float lo = a[-1 - i], hi = a[i];
            a[-1 - i] = lo * alias_cs[i] - hi * alias_ca[i];
            a[i] = hi * alias_cs[i] + lo * alias_ca[i];
        }
    }
}

/* IMDCT + overlap-add + frequency inversion for one granule/channel; out[t][sb]. */
static void hybrid(const struct gran *g, int long_end, float *xr, float ov[32][18], float out[18][32]) {
    int last = 575;
    while (last >= 0 && xr[last] == 0) last--;
    int nsb = (last + 18) / 18;   /* subbands with data */
    for (int sb = 0; sb < 32; sb++) {
        float y[36];
        const float *x = xr + 18 * sb;
        if (sb >= nsb) {
            for (int t = 0; t < 18; t++) { out[t][sb] = ov[sb][t]; ov[sb][t] = 0; }
        } else if (g->block_type != 2 || sb * 18 < long_end) {
            int bt = g->block_type == 2 ? 0 : g->block_type;
            for (int i = 0; i < 36; i++) {
                const float *c = imdct_l[bt][i];
                float s = 0;
                for (int k = 0; k < 18; k++) s += x[k] * c[k];
                y[i] = s;
            }
            for (int t = 0; t < 18; t++) { out[t][sb] = y[t] + ov[sb][t]; ov[sb][t] = y[18 + t]; }
        } else {
            for (int i = 0; i < 36; i++) y[i] = 0;
            for (int w = 0; w < 3; w++)
                for (int i = 0; i < 12; i++) {
                    const float *c = imdct_s[i];
                    float s = 0;
                    for (int k = 0; k < 6; k++) s += x[3 * k + w] * c[k];
                    y[6 + 6 * w + i] += s;
                }
            for (int t = 0; t < 18; t++) { out[t][sb] = y[t] + ov[sb][t]; ov[sb][t] = y[18 + t]; }
        }
        if (sb & 1)
            for (int t = 1; t < 18; t += 2) out[t][sb] = -out[t][sb];
    }
}

/* ------------------------------------------------------------ frame */
static uint16_t crc16(uint16_t crc, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

void mp3_l3_reset(mp3dec *d) {
    memset(d->l3.overlap, 0, sizeof d->l3.overlap);
    d->l3.res_len = 0;
    d->res_ok = 0;
}

int mp3_l3_frame(mp3dec *d, const struct mp3_hdr *h, const uint8_t *p, int16_t *pcm, int out_ch, int decode) {
    struct mp3_l3 *st = &d->l3;
    int nch = h->channels, ngr = h->lsf ? 1 : 2;
    int side_len = h->lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
    int hdr_len = 4 + (h->crc ? 2 : 0);
    uint8_t sbuf[48];
    struct side si;
    int ok = 1;
    memset(sbuf, 0, sizeof sbuf);
    if (h->size < hdr_len + side_len) return -1;
    memcpy(sbuf, p + hdr_len, (size_t)side_len);
    if (h->crc) {
        uint16_t c = crc16(0xffff, p + 2, 2);
        c = crc16(c, sbuf, side_len);
        if (c != (uint16_t)(p[4] << 8 | p[5])) ok = 0;
    }
    struct mp3_bits sb = { sbuf, 0, side_len * 8 };
    if (parse_side(&sb, h, &si) < 0) ok = 0;

    /* bit reservoir */
    int md_len = h->size - hdr_len - side_len;
    if (md_len > MP3_RES_SIZE - 512) md_len = MP3_RES_SIZE - 512;
    if (!d->res_ok) st->res_len = 0;
    int start = st->res_len - si.mdb;
    if (start < 0) ok = 0;
    memcpy(st->res + st->res_len, p + hdr_len + side_len, (size_t)md_len);
    int total = st->res_len + md_len;
    memset(st->res + total, 0, 64);
    int keep = total < 511 ? total : 511;
    int nsamp = 576 * ngr;

    if (decode) {
        long gpos = (long)(start < 0 ? 0 : start) * 8;
        long limit = (long)total * 8;
        for (int gr = 0; gr < ngr; gr++) {
            struct bands bd[2];
            uint8_t scf[2][40], ismax[40] = { 0 };
            int nz[2] = { 0, 0 };
            for (int ch = 0; ch < nch; ch++) {
                struct gran *g = &si.g[gr][ch];
                make_bands(h, g, &bd[ch]);
                if (!ok || gpos >= limit) {
                    memset(st->xr[ch], 0, sizeof st->xr[ch]);
                    memset(scf[ch], 0, 40);
                    continue;
                }
                long end = gpos + g->part23;
                struct mp3_bits b = { st->res, gpos, end < limit ? end : limit };
                int preflag = g->preflag;
                if (!h->lsf) read_scf_mpeg1(&b, g, si.scfsi[ch], gr, scf[ch], st->scf_prev[ch]);
                else read_scf_lsf(&b, g, ch == 1 && (h->mode_ext & 1) && h->mode == 1, scf[ch], ismax, &preflag);
                nz[ch] = huffman(&b, g, st->ival);
                requantize(g, &bd[ch], scf[ch], preflag, st->ival, nz[ch], st->xr[ch]);
                gpos = end;
            }
            if (nch == 2 && h->mode == 1 && ok) {
                int n = nz[0] > nz[1] ? nz[0] : nz[1];
                stereo(h, &si.g[gr][1], &bd[1], scf[1], ismax, st->xr[0], st->xr[1], n);
            }
            for (int ch = 0; ch < nch; ch++) {
                struct gran *g = &si.g[gr][ch];
                float hy[18][32];
                float *xr = st->xr[ch];
                if (g->block_type == 2) {
                    reorder(&bd[ch], xr, st->tmp);
                    if (g->mixed) alias_reduce(xr, bd[ch].long_end / 18);
                } else {
                    alias_reduce(xr, 32);
                }
                hybrid(g, ok ? bd[ch].long_end : 576, xr, st->overlap[ch], hy);
                int16_t *o = pcm + (long)gr * 576 * out_ch + ch;
                for (int t = 0; t < 18; t++) mp3_synth32(&d->synth, ch, hy[t], o + t * 32 * out_ch, out_ch);
            }
        }
    }
    memmove(st->res, st->res + total - keep, (size_t)keep);
    st->res_len = keep;
    d->res_ok = 1;
    return nsamp;
}
