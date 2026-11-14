/* JPEG decoder: baseline + progressive Huffman, 8-bit, any sampling factors,
 * restart intervals, grayscale / YCbCr / RGB / Adobe CMYK & YCCK.
 * Integer IDCT (libjpeg "islow") and libjpeg-compatible fancy upsampling. */
#include "img_int.h"

static const uint8_t zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct jhuff {
    uint16_t fast[512];   /* len << 8 | sym for codes <= 9 bits, 0 = slow path */
    int32_t maxcode[18], valptr[17], mincode[17];
    uint8_t vals[256];
    int defined;
};

struct jcomp {
    int id, h, v, tq, td, ta;
    int bw, bh;     /* plane size in blocks */
    int dw, dh;     /* downsampled size in samples */
    int pred;
    uint8_t *plane; /* bw*8 x bh*8 */
    int16_t *coef;  /* progressive: bw*bh*64, natural order */
};

struct jdec {
    const uint8_t *d;
    size_t n, pos;
    uint32_t bb;
    int bc, marker_hit;
    uint16_t qt[4][64];   /* natural order */
    int qt_def[4];
    struct jhuff dc[4], ac[4];
    struct jcomp c[4];
    int nc, W, H, hmax, vmax, mcux, mcuy, progressive, frame;
    int restart, adobe, transform, jfif, scans;
    int ns, sc[4], ss, se, ah, al, eobrun;
};

/* ---------------- bit reader ---------------- */
static void jfill(struct jdec *j) {
    while (j->bc <= 24) {
        unsigned b = 0;
        if (!j->marker_hit) {
            if (j->pos >= j->n) {
                j->marker_hit = 1;
            } else {
                b = j->d[j->pos];
                if (b == 0xFF) {
                    if (j->pos + 1 < j->n && j->d[j->pos + 1] == 0) {
                        j->pos += 2;
                    } else {
                        j->marker_hit = 1;
                        b = 0;
                    }
                } else {
                    j->pos++;
                }
            }
        }
        j->bb |= (uint32_t)b << (24 - j->bc);
        j->bc += 8;
    }
}

static int jbits(struct jdec *j, int n) {
    if (!n) return 0;
    if (j->bc < n) jfill(j);
    unsigned v = j->bb >> (32 - n);
    j->bb <<= n;
    j->bc -= n;
    return (int)v;
}
static int jbit(struct jdec *j) { return jbits(j, 1); }

static int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

static int jdecode(struct jdec *j, const struct jhuff *h) {
    if (j->bc < 16) jfill(j);
    unsigned e = h->fast[j->bb >> 23];
    if (e) {
        int l = (int)(e >> 8);
        j->bb <<= l;
        j->bc -= l;
        return (int)(e & 255);
    }
    for (int l = 10; l <= 16; l++) {
        int32_t code = (int32_t)(j->bb >> (32 - l));
        if (code <= h->maxcode[l]) {
            int idx = h->valptr[l] + code - h->mincode[l];
            if (idx < 0 || idx > 255) return -1;
            j->bb <<= l;
            j->bc -= l;
            return h->vals[idx];
        }
    }
    return -1;
}

static int jbuild(struct jhuff *h, const uint8_t *counts, const uint8_t *vals, int nv) {
    memset(h->fast, 0, sizeof h->fast);
    int32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        for (int i = 0; i < counts[l - 1]; i++, k++, code++) {
            if (l <= 9) {
                int base = code << (9 - l);
                for (int f = 0; f < (1 << (9 - l)); f++) h->fast[base + f] = (uint16_t)(l << 8 | vals[k]);
            }
        }
        h->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        if (code > (1 << l)) return -1;
        code <<= 1;
    }
    memset(h->vals, 0, sizeof h->vals);
    memcpy(h->vals, vals, (size_t)nv);
    h->defined = 1;
    return 0;
}

/* ---------------- IDCT (libjpeg jidctint "islow") ---------------- */
#define CB 13
#define P1 2
static void idct8(const int32_t *in, uint8_t *out, int stride) {
    int64_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int32_t *p = in + c;
        int64_t *w = ws + c;
        if (!(p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56])) {
            int64_t dc = (int64_t)p[0] * (1 << P1);
            for (int i = 0; i < 8; i++) w[i * 8] = dc;
            continue;
        }
        int64_t z2 = p[16], z3 = p[48];
        int64_t z1 = (z2 + z3) * 4433;
        int64_t tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        z2 = p[0]; z3 = p[32];
        int64_t tmp0 = (z2 + z3) * (1 << CB), tmp1 = (z2 - z3) * (1 << CB);
        int64_t t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
        tmp0 = p[56]; tmp1 = p[40]; tmp2 = p[24]; tmp3 = p[8];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3, z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int sh = CB - P1;
        const int64_t r = (int64_t)1 << (sh - 1);
        w[0] = (t10 + tmp3 + r) >> sh;  w[56] = (t10 - tmp3 + r) >> sh;
        w[8] = (t11 + tmp2 + r) >> sh;  w[48] = (t11 - tmp2 + r) >> sh;
        w[16] = (t12 + tmp1 + r) >> sh; w[40] = (t12 - tmp1 + r) >> sh;
        w[24] = (t13 + tmp0 + r) >> sh; w[32] = (t13 - tmp0 + r) >> sh;
    }
    for (int rI = 0; rI < 8; rI++, out += stride) {
        const int64_t *w = ws + rI * 8;
        int64_t o[8];
        int64_t z2 = w[2], z3 = w[6];
        int64_t z1 = (z2 + z3) * 4433;
        int64_t tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        int64_t tmp0 = (w[0] + w[4]) * (1 << CB), tmp1 = (w[0] - w[4]) * (1 << CB);
        int64_t t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
        tmp0 = w[7]; tmp1 = w[5]; tmp2 = w[3]; tmp3 = w[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        int64_t z4 = tmp1 + tmp3, z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int sh = CB + P1 + 3;
        const int64_t r = ((int64_t)1 << (sh - 1)) + ((int64_t)128 << sh);
        o[0] = (t10 + tmp3 + r) >> sh; o[7] = (t10 - tmp3 + r) >> sh;
        o[1] = (t11 + tmp2 + r) >> sh; o[6] = (t11 - tmp2 + r) >> sh;
        o[2] = (t12 + tmp1 + r) >> sh; o[5] = (t12 - tmp1 + r) >> sh;
        o[3] = (t13 + tmp0 + r) >> sh; o[4] = (t13 - tmp0 + r) >> sh;
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)(o[i] < 0 ? 0 : o[i] > 255 ? 255 : o[i]);
    }
}

/* ---------------- block decoding ---------------- */
static int block_baseline(struct jdec *j, struct jcomp *c, int bx, int by) {
    int32_t blk[64];
    memset(blk, 0, sizeof blk);
    const uint16_t *q = j->qt[c->tq];
    int t = jdecode(j, &j->dc[c->td]);
    if (t < 0 || t > 15) return IMG_ECORRUPT;
    int diff = t ? extend(jbits(j, t), t) : 0;
    c->pred = (int16_t)(c->pred + diff);
    blk[0] = c->pred * (int32_t)q[0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0) return IMG_ECORRUPT;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return IMG_ECORRUPT;
        blk[zz[k]] = extend(jbits(j, s), s) * (int32_t)q[zz[k]];
        k++;
    }
    idct8(blk, c->plane + (size_t)by * 8 * (c->bw * 8) + (size_t)bx * 8, c->bw * 8);
    return 0;
}

static int block_prog(struct jdec *j, struct jcomp *c, int bx, int by) {
    int16_t *b = c->coef + ((size_t)by * c->bw + bx) * 64;
    if (j->ss == 0) {                        /* DC scan */
        if (j->ah == 0) {
            int t = jdecode(j, &j->dc[c->td]);
            if (t < 0 || t > 15) return IMG_ECORRUPT;
            int diff = t ? extend(jbits(j, t), t) : 0;
            c->pred = (int16_t)(c->pred + diff);
            b[0] = (int16_t)(c->pred * (1 << j->al));
        } else if (jbit(j)) {
            b[0] = (int16_t)(b[0] | (1 << j->al));
        }
        return 0;
    }
    if (j->ah == 0) {                        /* AC first */
        if (j->eobrun > 0) { j->eobrun--; return 0; }
        for (int k = j->ss; k <= j->se; k++) {
            int rs = jdecode(j, &j->ac[c->ta]);
            if (rs < 0) return IMG_ECORRUPT;
            int r = rs >> 4, s = rs & 15;
            if (s) {
                k += r;
                if (k > 63) return IMG_ECORRUPT;
                b[zz[k]] = (int16_t)(extend(jbits(j, s), s) * (1 << j->al));
            } else {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += jbits(j, r);
                    break;
                }
                k += 15;
            }
        }
        return 0;
    }
    /* AC refinement */
    int p1 = 1 << j->al, m1 = -(1 << j->al);
    int k = j->ss;
    if (j->eobrun <= 0) {
        for (; k <= j->se; k++) {
            int rs = jdecode(j, &j->ac[c->ta]);
            if (rs < 0) return IMG_ECORRUPT;
            int r = rs >> 4, s = rs & 15;
            if (s) {
                s = jbit(j) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += jbits(j, r);
                break;
            }
            do {
                int16_t *cf = b + zz[k];
                if (*cf) {
                    if (jbit(j) && (*cf & p1) == 0) *cf = (int16_t)(*cf >= 0 ? *cf + p1 : *cf + m1);
                } else {
                    if (--r < 0) break;
                }
                k++;
            } while (k <= j->se);
            if (s && k <= 63) b[zz[k]] = (int16_t)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= j->se; k++) {
            int16_t *cf = b + zz[k];
            if (*cf && jbit(j) && (*cf & p1) == 0) *cf = (int16_t)(*cf >= 0 ? *cf + p1 : *cf + m1);
        }
        j->eobrun--;
    }
    return 0;
}

static void reset_entropy(struct jdec *j) {
    j->bb = 0;
    j->bc = 0;
    j->marker_hit = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].pred = 0;
}

static void do_restart(struct jdec *j) {
    while (j->pos + 1 < j->n) {
        if (j->d[j->pos] == 0xFF) {
            uint8_t m = j->d[j->pos + 1];
            if (m >= 0xD0 && m <= 0xD7) { j->pos += 2; break; }
            if (m != 0 && m != 0xFF) break;   /* some other marker: stop here */
        }
        j->pos++;
    }
    reset_entropy(j);
}

static int decode_scan(struct jdec *j) {
    reset_entropy(j);
    int restarts_left = j->restart;
    if (j->ns == 1) {
        struct jcomp *c = &j->c[j->sc[0]];
        int bw = (c->dw + 7) / 8, bh = (c->dh + 7) / 8;
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                if (j->restart) {
                    if (!restarts_left) { do_restart(j); restarts_left = j->restart; }
                    restarts_left--;
                }
                int r = j->progressive ? block_prog(j, c, bx, by) : block_baseline(j, c, bx, by);
                if (r) return r;
            }
        return 0;
    }
    for (int my = 0; my < j->mcuy; my++)
        for (int mx = 0; mx < j->mcux; mx++) {
            if (j->restart) {
                if (!restarts_left) { do_restart(j); restarts_left = j->restart; }
                restarts_left--;
            }
            for (int i = 0; i < j->ns; i++) {
                struct jcomp *c = &j->c[j->sc[i]];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        int r = j->progressive ? block_prog(j, c, bx, by) : block_baseline(j, c, bx, by);
                        if (r) return r;
                    }
            }
        }
    return 0;
}

/* ---------------- marker segments ---------------- */
static int parse_sof(struct jdec *j, const uint8_t *s, size_t n, const struct img_alloc *a) {
    if (j->frame) return IMG_ECORRUPT;
    if (n < 6) return IMG_ETRUNC;
    if (s[0] != 8) return IMG_EUNSUP;
    j->H = (int)rd_be16(s + 1);
    j->W = (int)rd_be16(s + 3);
    j->nc = s[5];
    if (j->H == 0) return IMG_EUNSUP;      /* DNL */
    int r = img_check_dims(j->W, j->H);
    if (r) return r;
    if (j->nc != 1 && j->nc != 3 && j->nc != 4) return IMG_EUNSUP;
    if (n < 6 + (size_t)j->nc * 3) return IMG_ETRUNC;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        c->id = s[6 + i * 3];
        c->h = s[7 + i * 3] >> 4;
        c->v = s[7 + i * 3] & 15;
        c->tq = s[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) return IMG_ECORRUPT;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    j->mcux = (j->W + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->H + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->dw = (j->W * c->h + j->hmax - 1) / j->hmax;
        c->dh = (j->H * c->v + j->vmax - 1) / j->vmax;
        size_t sz = (size_t)c->bw * c->bh * 64;
        c->plane = ia_zalloc(a, sz);
        if (!c->plane) return IMG_ENOMEM;
        if (j->progressive) {
            c->coef = ia_zalloc(a, sz * 2);
            if (!c->coef) return IMG_ENOMEM;
        }
    }
    j->frame = 1;
    return 0;
}

static int parse_dht(struct jdec *j, const uint8_t *s, size_t n) {
    while (n) {
        if (n < 17) return IMG_ETRUNC;
        int tc = s[0] >> 4, th = s[0] & 15;
        if (tc > 1 || th > 3) return IMG_ECORRUPT;
        int tot = 0;
        for (int i = 0; i < 16; i++) tot += s[1 + i];
        if (tot > 256) return IMG_ECORRUPT;
        if (n < 17 + (size_t)tot) return IMG_ETRUNC;
        if (jbuild(tc ? &j->ac[th] : &j->dc[th], s + 1, s + 17, tot)) return IMG_ECORRUPT;
        s += 17 + tot;
        n -= 17 + (size_t)tot;
    }
    return 0;
}

static int parse_dqt(struct jdec *j, const uint8_t *s, size_t n) {
    while (n) {
        int pq = s[0] >> 4, tq = s[0] & 15;
        if (pq > 1 || tq > 3) return IMG_ECORRUPT;
        size_t need = 1 + (pq ? 128 : 64);
        if (n < need) return IMG_ETRUNC;
        for (int k = 0; k < 64; k++)
            j->qt[tq][zz[k]] = (uint16_t)(pq ? rd_be16(s + 1 + k * 2) : s[1 + k]);
        j->qt_def[tq] = 1;
        s += need;
        n -= need;
    }
    return 0;
}

static int parse_sos(struct jdec *j, const uint8_t *s, size_t n) {
    if (!j->frame) return IMG_ECORRUPT;
    if (n < 1) return IMG_ETRUNC;
    j->ns = s[0];
    if (j->ns < 1 || j->ns > 4 || j->ns > j->nc) return IMG_ECORRUPT;
    if (n < 4 + (size_t)j->ns * 2) return IMG_ETRUNC;
    for (int i = 0; i < j->ns; i++) {
        int id = s[1 + i * 2], k;
        for (k = 0; k < j->nc; k++)
            if (j->c[k].id == id) break;
        if (k == j->nc) return IMG_ECORRUPT;
        for (int q = 0; q < i; q++)
            if (j->sc[q] == k) return IMG_ECORRUPT;
        j->sc[i] = k;
        j->c[k].td = s[2 + i * 2] >> 4;
        j->c[k].ta = s[2 + i * 2] & 15;
        if (j->c[k].td > 3 || j->c[k].ta > 3) return IMG_ECORRUPT;
    }
    const uint8_t *t = s + 1 + j->ns * 2;
    j->ss = t[0];
    j->se = t[1];
    j->ah = t[2] >> 4;
    j->al = t[2] & 15;
    int blocks = 0;
    for (int i = 0; i < j->ns; i++) blocks += j->c[j->sc[i]].h * j->c[j->sc[i]].v;
    if (j->ns > 1 && blocks > 10) return IMG_ECORRUPT;
    for (int i = 0; i < j->ns; i++)
        if (!j->qt_def[j->c[j->sc[i]].tq]) return IMG_ECORRUPT;
    if (!j->progressive) {
        for (int i = 0; i < j->ns; i++) {
            struct jcomp *c = &j->c[j->sc[i]];
            if (!j->dc[c->td].defined || !j->ac[c->ta].defined) return IMG_ECORRUPT;
        }
        return 0;
    }
    if (j->al > 13 || j->ah > 13 || j->se > 63 || j->ss > j->se) return IMG_ECORRUPT;
    if (j->ss == 0) {
        if (j->se != 0) return IMG_ECORRUPT;
        if (j->ah == 0)
            for (int i = 0; i < j->ns; i++)
                if (!j->dc[j->c[j->sc[i]].td].defined) return IMG_ECORRUPT;
    } else {
        if (j->ns != 1) return IMG_ECORRUPT;
        if (!j->ac[j->c[j->sc[0]].ta].defined) return IMG_ECORRUPT;
    }
    return 0;
}

static void skip_entropy(struct jdec *j) {
    while (j->pos + 1 < j->n) {
        if (j->d[j->pos] == 0xFF) {
            uint8_t m = j->d[j->pos + 1];
            if (m != 0 && m != 0xFF && !(m >= 0xD0 && m <= 0xD7)) return;
        }
        j->pos++;
    }
    j->pos = j->n;
}

/* ---------------- output ---------------- */
static void upsample_row(const struct jdec *j, const struct jcomp *c, int y, uint8_t *dst, uint8_t *tmp) {
    int W = j->W, stride = c->bw * 8;
    int hs = j->hmax / c->h, vs = j->vmax / c->v;
    int exact = j->hmax % c->h == 0 && j->vmax % c->v == 0;
    if (c->h == j->hmax && c->v == j->vmax) {
        memcpy(dst, c->plane + (size_t)y * stride, (size_t)W);
        return;
    }
    if (exact && ((hs == 2 && vs == 1 && c->dw > 2) || (hs == 1 && vs == 2) || (hs == 2 && vs == 2 && c->dw > 2))) {
        int r = y / vs;
        const uint8_t *in0 = c->plane + (size_t)r * stride, *in1 = in0;
        int odd = y & 1;
        if (vs == 2) {
            int r1 = odd ? r + 1 : r - 1;
            if (r1 < 0) r1 = 0;
            if (r1 > c->dh - 1) r1 = c->dh - 1;
            in1 = c->plane + (size_t)r1 * stride;
        }
        if (hs == 1) {
            int bias = odd ? 2 : 1;
            for (int x = 0; x < W; x++) dst[x] = (uint8_t)((in0[x] * 3 + in1[x] + bias) >> 2);
            return;
        }
        uint8_t *o = tmp;
        int dw = c->dw;
        if (vs == 1) {
            int iv = in0[0];
            *o++ = (uint8_t)iv;
            *o++ = (uint8_t)((iv * 3 + in0[1] + 2) >> 2);
            int i = 1;
            for (int cc = dw - 2; cc > 0; cc--, i++) {
                iv = in0[i] * 3;
                *o++ = (uint8_t)((iv + in0[i - 1] + 1) >> 2);
                *o++ = (uint8_t)((iv + in0[i + 1] + 2) >> 2);
            }
            iv = in0[i];
            *o++ = (uint8_t)((iv * 3 + in0[i - 1] + 1) >> 2);
            *o++ = (uint8_t)iv;
        } else {
            int this_ = in0[0] * 3 + in1[0], next = in0[1] * 3 + in1[1], last;
            *o++ = (uint8_t)((this_ * 4 + 8) >> 4);
            *o++ = (uint8_t)((this_ * 3 + next + 7) >> 4);
            last = this_;
            this_ = next;
            int i = 2;
            for (int cc = dw - 2; cc > 0; cc--, i++) {
                next = in0[i] * 3 + in1[i];
                *o++ = (uint8_t)((this_ * 3 + last + 8) >> 4);
                *o++ = (uint8_t)((this_ * 3 + next + 7) >> 4);
                last = this_;
                this_ = next;
            }
            *o++ = (uint8_t)((this_ * 3 + last + 8) >> 4);
            *o++ = (uint8_t)((this_ * 4 + 7) >> 4);
        }
        memcpy(dst, tmp, (size_t)W);
        return;
    }
    const uint8_t *in = c->plane + (size_t)(y * c->v / j->vmax) * stride;
    for (int x = 0; x < W; x++) dst[x] = in[x * c->h / j->hmax];
}

static inline uint32_t clamp8(int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline int muldiv255(int a, int b) { int t = a * b + 128; return ((t >> 8) + t) >> 8; }

static int jpeg_output(struct jdec *j, struct image *out, const struct img_alloc *a) {
    int W = j->W, H = j->H;
    if (j->progressive) {
        for (int i = 0; i < j->nc; i++) {
            struct jcomp *c = &j->c[i];
            const uint16_t *q = j->qt[c->tq];
            int32_t blk[64];
            for (int by = 0; by < c->bh; by++)
                for (int bx = 0; bx < c->bw; bx++) {
                    const int16_t *cf = c->coef + ((size_t)by * c->bw + bx) * 64;
                    for (int k = 0; k < 64; k++) blk[k] = cf[k] * (int32_t)q[k];
                    idct8(blk, c->plane + (size_t)by * 8 * (c->bw * 8) + (size_t)bx * 8, c->bw * 8);
                }
        }
    }
    uint32_t *px = ia_alloc(a, (size_t)W * H * 4);
    uint8_t *rows = ia_alloc(a, (size_t)(W + 16) * 5 * 2);
    if (!px || !rows) {
        ia_free(a, px);
        ia_free(a, rows);
        return IMG_ENOMEM;
    }
    uint8_t *rw[4], *tmp = rows + (size_t)(W + 16) * 4;
    for (int i = 0; i < 4; i++) rw[i] = rows + (size_t)(W + 16) * i;
    int mode = 0;   /* 0 gray, 1 ycc, 2 rgb, 3 cmyk, 4 ycck */
    if (j->nc == 3) {
        mode = 1;
        if (j->adobe) mode = j->transform ? 1 : 2;
        else if (!j->jfif && j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B') mode = 2;
    } else if (j->nc == 4) {
        mode = (j->adobe && j->transform == 2) ? 4 : 3;
    }
    for (int y = 0; y < H; y++) {
        for (int i = 0; i < j->nc; i++) upsample_row(j, &j->c[i], y, rw[i], tmp);
        uint32_t *o = px + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int r, g, b;
            if (mode == 0) {
                r = g = b = rw[0][x];
            } else if (mode == 2) {
                r = rw[0][x]; g = rw[1][x]; b = rw[2][x];
            } else {
                int Y = rw[0][x], cb = rw[1][x] - 128, cr = rw[2][x] - 128;
                r = Y + ((91881 * cr + 32768) >> 16);
                g = Y + ((-22554 * cb - 46802 * cr + 32768) >> 16);
                b = Y + ((116130 * cb + 32768) >> 16);
                if (mode >= 3) {
                    int C, M, Ye, K = rw[3][x];
                    if (mode == 3) { C = rw[0][x]; M = rw[1][x]; Ye = rw[2][x]; }
                    else { C = 255 - (int)clamp8(r); M = 255 - (int)clamp8(g); Ye = 255 - (int)clamp8(b); }
                    /* Adobe CMYK is stored inverted; Pillow-compatible conversion */
                    int nk = K;
                    r = nk - muldiv255(255 - C, nk);
                    g = nk - muldiv255(255 - M, nk);
                    b = nk - muldiv255(255 - Ye, nk);
                }
            }
            o[x] = 0xFF000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
        }
    }
    ia_free(a, rows);
    out->px = px;
    out->w = W;
    out->h = H;
    return 0;
}

/* ---------------- top level ---------------- */
static int jpeg_run(struct jdec *j, struct image *out, const struct img_alloc *a) {
    if (j->n < 4 || j->d[0] != 0xFF || j->d[1] != 0xD8) return IMG_EFORMAT;
    j->pos = 2;
    for (;;) {
        while (j->pos < j->n && j->d[j->pos] != 0xFF) j->pos++;      /* skip garbage */
        while (j->pos < j->n && j->d[j->pos] == 0xFF) j->pos++;
        if (j->pos >= j->n) break;
        int m = j->d[j->pos++];
        if (m == 0xD9) break;
        if (m == 0 || m == 1 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (j->n - j->pos < 2) break;
        size_t L = rd_be16(j->d + j->pos);
        if (L < 2) return IMG_ECORRUPT;
        if (j->n - j->pos < L) {
            if (m == 0xDA || !j->scans) return IMG_ETRUNC;
            break;
        }
        const uint8_t *s = j->d + j->pos + 2;
        size_t sl = L - 2;
        j->pos += L;
        int r = 0;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2:
            j->progressive = m == 0xC2;
            r = parse_sof(j, s, sl, a);
            break;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            return IMG_EUNSUP;
        case 0xC4: r = parse_dht(j, s, sl); break;
        case 0xDB: r = parse_dqt(j, s, sl); break;
        case 0xDD:
            if (sl < 2) return IMG_ETRUNC;
            j->restart = (int)rd_be16(s);
            break;
        case 0xE0:
            if (sl >= 5 && !memcmp(s, "JFIF", 5)) j->jfif = 1;
            break;
        case 0xEE:
            if (sl >= 12 && !memcmp(s, "Adobe", 5)) { j->adobe = 1; j->transform = s[11]; }
            break;
        case 0xDA:
            r = parse_sos(j, s, sl);
            if (r) return r;
            if (++j->scans > 1000) return IMG_ECORRUPT;
            (void)decode_scan(j);   /* damaged entropy data: keep what was decoded */
            skip_entropy(j);
            break;
        default: break;
        }
        if (r) return r;
    }
    if (!j->frame || !j->scans) return j->frame ? IMG_ETRUNC : IMG_ECORRUPT;
    return jpeg_output(j, out, a);
}

static void jpeg_cleanup(struct jdec *j, const struct img_alloc *a) {
    for (int i = 0; i < 4; i++) {
        ia_free(a, j->c[i].plane);
        ia_free(a, j->c[i].coef);
    }
}

int jpeg_decode(const uint8_t *d, size_t n, struct image *out, const struct img_alloc *a) {
    if (!d || !out) return IMG_EARG;
    out->px = 0;
    out->w = out->h = 0;
    struct jdec *j = ia_zalloc(a, sizeof *j);
    if (!j) return IMG_ENOMEM;
    j->d = d;
    j->n = n;
    int r = jpeg_run(j, out, a);
    jpeg_cleanup(j, a);
    ia_free(a, j);
    if (r) {
        out->px = 0;
        out->w = out->h = 0;
    }
    return r;
}

int jpeg_probe(const uint8_t *d, size_t n, int *w, int *h) {
    if (!d || n < 4 || d[0] != 0xFF || d[1] != 0xD8) return IMG_EFORMAT;
    size_t pos = 2;
    for (;;) {
        while (pos < n && d[pos] != 0xFF) pos++;
        while (pos < n && d[pos] == 0xFF) pos++;
        if (pos >= n) return IMG_ETRUNC;
        int m = d[pos++];
        if (m == 0xD9 || m == 0xDA) return IMG_ECORRUPT;
        if (m == 0 || m == 1 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (n - pos < 2) return IMG_ETRUNC;
        size_t L = rd_be16(d + pos);
        if (L < 2) return IMG_ECORRUPT;
        if (n - pos < L) return IMG_ETRUNC;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (L < 7) return IMG_ETRUNC;
            int H = (int)rd_be16(d + pos + 3), W = (int)rd_be16(d + pos + 5);
            if (H == 0) return IMG_EUNSUP;
            int r = img_check_dims(W, H);
            if (r) return r;
            if (w) *w = W;
            if (h) *h = H;
            return 0;
        }
        pos += L;
    }
}
