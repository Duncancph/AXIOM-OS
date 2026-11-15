/* AXIOM H.264 decoder: CABAC engine and macroblock layer (9.3). */
#include "h264_int.h"

extern const uint8_t (*h264_b_part)[3];
extern const uint8_t (*h264_b_sub)[4];
extern const uint8_t (*h264_p_sub)[4];

void h264_cabac_start(struct h264_cabac *c, const uint8_t *p, const uint8_t *end) {
    c->start = c->p = p;
    c->end = end;
    c->low = 0;
    c->bits = -9;            /* the first 9 bits form codIOffset */
    cabac_refill(c);         /* bits = 23 */
    c->range = 510;
}

const uint8_t *h264_cabac_pcm_start(struct h264_cabac *c) {
    long consumed = (long)(c->p - c->start) * 8 - c->bits;   /* bits read by the 9-bit-register model */
    return c->start + (consumed + 7) / 8;
}

void h264_cabac_init_slice(h264dec *d) {
    int idx = d->sl.type == 2 ? 0 : d->sl.cabac_init_idc + 1;
    int qp = h264_clip(d->sl.qp, 0, 51);
    const int8_t (*t)[2] = h264_cabac_init[idx];
    for (int i = 0; i < 460; i++) {
        int pre = h264_clip(((t[i][0] * qp) >> 4) + t[i][1], 1, 126);
        d->cabac_state[i] = (uint8_t)(pre <= 63 ? (63 - pre) << 1 : ((pre - 64) << 1) | 1);
    }
}

/* ---------------------------------------------------------------- residual */
static const uint8_t cbf_off[5] = { 0, 4, 8, 12, 16 };
static const uint8_t sig_off[5] = { 0, 15, 29, 44, 47 };
static const uint8_t abs_off[5] = { 0, 10, 20, 30, 39 };

/* Decodes significance map + levels; returns the number of coefficients (cbf already decoded as 1). */
static int cabac_block(h264dec *d, int cat, int maxc, int16_t *blk, const uint8_t *scan, const uint32_t *dq) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    uint8_t *sig, *last, *absc;
    uint8_t idx[64];
    int n = 0;
    if (cat == 5) {
        sig = st + 402; last = st + 417; absc = st + 426;
        int i;
        for (i = 0; i < 63; i++) {
            if (cabac_decide(c, sig + h264_sig8_ctx[i])) {
                idx[n++] = (uint8_t)i;
                if (cabac_decide(c, last + h264_last8_ctx[i])) goto done;
            }
        }
        idx[n++] = 63;
    } else {
        sig = st + 105 + sig_off[cat]; last = st + 166 + sig_off[cat]; absc = st + 227 + abs_off[cat];
        int i;
        if (cat == 3) {
            for (i = 0; i < maxc - 1; i++) {
                int ci = i < 2 ? i : 2;
                if (cabac_decide(c, sig + ci)) {
                    idx[n++] = (uint8_t)i;
                    if (cabac_decide(c, last + ci)) goto done;
                }
            }
        } else {
            for (i = 0; i < maxc - 1; i++) {
                if (cabac_decide(c, sig + i)) {
                    idx[n++] = (uint8_t)i;
                    if (cabac_decide(c, last + i)) goto done;
                }
            }
        }
        idx[n++] = (uint8_t)(maxc - 1);
    }
done:;
    int gt1 = 0, eq1 = 0;
    int gtmax = cat == 3 ? 3 : 4;
    for (int k = n - 1; k >= 0; k--) {
        int v;
        if (!cabac_decide(c, absc + (gt1 ? 0 : h264_min(4, 1 + eq1)))) {
            v = 1;
            eq1++;
        } else {
            uint8_t *c2 = absc + 5 + h264_min(gtmax, gt1);
            int m = 1;
            while (m < 14 && cabac_decide(c, c2)) m++;
            if (m == 14) {
                int kk = 0, suf = 0;
                while (cabac_bypass(c)) {
                    suf += 1 << kk;
                    if (++kk > 24) return -1;
                }
                while (kk--) suf += cabac_bypass(c) << kk;
                m += suf;
            }
            v = m + 1;
            gt1++;
        }
        if (cabac_bypass(c)) v = -v;
        int pos = scan[idx[k]];
        if (dq) blk[pos] = (int16_t)(((int64_t)v * dq[pos] + 32) >> 6);
        else blk[pos] = (int16_t)v;
    }
    return n;
}

H264_INLINE int cbf_bit(uint32_t w, int b) { return (int)((w >> b) & 1); }

static int cabac_residual(h264dec *d) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    int cbp = d->cbp;
    int intra = (d->mb_type & MB_INTRA) != 0;
    int qp = d->qp;
    uint32_t lw = d->avail_l ? d->mbi[d->mb_xy - 1].cbf : (intra ? 0x7ffffff : 0);
    uint32_t tw = d->avail_t ? d->mbi[d->mb_xy - d->mb_w].cbf : (intra ? 0x7ffffff : 0);
    const uint32_t *dq4 = d->bypass ? h264_dq_flat : d->dq4[intra ? 0 : 3][qp];

    if (d->mb_type & MB_I16) {
        int inc = cbf_bit(lw, 24) + 2 * cbf_bit(tw, 24);
        if (cabac_decide(c, st + 85 + cbf_off[0] + inc)) {
            int16_t dc[16] = { 0 };
            if (cabac_block(d, 0, 16, dc, h264_zigzag4, 0) < 0) return -1;
            for (int i = 0; i < 16; i++) d->dc_y[i] = dc[i];
            d->cbf_cur |= 1u << 24;
        }
        if (cbp & 15) {
            for (int blk = 0; blk < 16; blk++) {
                int r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
                int a = x ? cbf_bit(d->cbf_cur, r - 1) : cbf_bit(lw, r + 3);
                int b = y ? cbf_bit(d->cbf_cur, r - 4) : cbf_bit(tw, r + 12);
                if (cabac_decide(c, st + 85 + cbf_off[1] + a + 2 * b)) {
                    int n = cabac_block(d, 1, 15, d->coef + blk * 16, h264_zigzag4 + 1, dq4);
                    if (n < 0) return -1;
                    d->nnz_c[0][CI(x, y)] = (int8_t)n;
                    d->nz_luma |= (uint16_t)(1 << blk);
                    d->cbf_cur |= 1u << r;
                }
            }
        }
    } else {
        for (int b8 = 0; b8 < 4; b8++) {
            if (!(cbp & (1 << b8))) continue;
            if (d->t8x8) {
                const uint32_t *dq8 = d->bypass ? h264_dq_flat : d->dq8[intra ? 0 : 1][qp];
                int n = cabac_block(d, 5, 64, d->coef + b8 * 64, h264_zigzag8, dq8);
                if (n < 0) return -1;
                int r = h264_blk_xy[b8 * 4];
                d->cbf_cur |= 0x33u << r;
                d->nz_luma |= (uint16_t)(15 << (b8 * 4));
                d->nzdb_cur |= (uint16_t)(0x33 << r);
                int8_t *nz = d->nnz_c[0];
                nz[CI(r & 3, r >> 2)] = nz[CI((r & 3) + 1, r >> 2)] = nz[CI(r & 3, (r >> 2) + 1)] =
                    nz[CI((r & 3) + 1, (r >> 2) + 1)] = (int8_t)n;
            } else {
                for (int i = 0; i < 4; i++) {
                    int blk = b8 * 4 + i, r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
                    int a = x ? cbf_bit(d->cbf_cur, r - 1) : cbf_bit(lw, r + 3);
                    int b = y ? cbf_bit(d->cbf_cur, r - 4) : cbf_bit(tw, r + 12);
                    if (cabac_decide(c, st + 85 + cbf_off[2] + a + 2 * b)) {
                        int n = cabac_block(d, 2, 16, d->coef + blk * 16, h264_zigzag4, dq4);
                        if (n < 0) return -1;
                        d->nnz_c[0][CI(x, y)] = (int8_t)n;
                        d->nz_luma |= (uint16_t)(1 << blk);
                        d->nzdb_cur |= (uint16_t)(1 << r);
                        d->cbf_cur |= 1u << r;
                    }
                }
            }
        }
    }
    if (cbp & 0x30) {
        static const uint8_t ident[4] = { 0, 1, 2, 3 };
        for (int ch = 0; ch < 2; ch++) {
            int inc = cbf_bit(lw, 25 + ch) + 2 * cbf_bit(tw, 25 + ch);
            if (cabac_decide(c, st + 85 + cbf_off[3] + inc)) {
                int16_t dc[4] = { 0, 0, 0, 0 };
                if (cabac_block(d, 3, 4, dc, ident, 0) < 0) return -1;
                for (int i = 0; i < 4; i++) d->dc_c[ch][i] = dc[i];
                d->cbf_cur |= 1u << (25 + ch);
            }
        }
        if (cbp & 0x20) {
            for (int ch = 0; ch < 2; ch++) {
                const uint32_t *dqc = d->bypass ? h264_dq_flat : d->dq4[(intra ? 1 : 4) + ch][d->qpc[ch]];
                int base = 16 + ch * 4;
                for (int i = 0; i < 4; i++) {
                    int x = i & 1, y = i >> 1;
                    int a = x ? cbf_bit(d->cbf_cur, base + i - 1) : cbf_bit(lw, base + i + 1);
                    int b = y ? cbf_bit(d->cbf_cur, base + i - 2) : cbf_bit(tw, base + i + 2);
                    if (cabac_decide(c, st + 85 + cbf_off[4] + a + 2 * b)) {
                        int n = cabac_block(d, 4, 15, d->coefc[ch] + i * 16, h264_zigzag4 + 1, dqc);
                        if (n < 0) return -1;
                        d->nnz_c[1 + ch][CI(x, y)] = (int8_t)n;
                        d->nz_chroma |= (uint8_t)(1 << (ch * 4 + i));
                        d->cbf_cur |= 1u << (base + i);
                    }
                }
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- syntax elements */
int h264_cabac_skip(h264dec *d) {
    int inc = (d->avail_l && !(d->left_type & MB_SKIP)) + (d->avail_t && !(d->top_type & MB_SKIP));
    return cabac_decide(&d->cabac, d->cabac_state + (d->sl.type == 1 ? 24 : 11) + inc);
}

/* returns 0 I_NxN, 1..24 I_16x16, 25 I_PCM */
static int intra_mb_type(h264dec *d, int base, int islice) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state + base;
    if (islice) {
        int inc = (d->avail_l && (d->left_type & (MB_I16 | MB_PCM))) + (d->avail_t && (d->top_type & (MB_I16 | MB_PCM)));
        if (!cabac_decide(c, st + inc)) return 0;
        st += 2;
    } else {
        if (!cabac_decide(c, st)) return 0;
    }
    if (cabac_terminate(c)) return 25;
    int t = 1;
    t += 12 * cabac_decide(c, st + 1);
    if (cabac_decide(c, st + 2)) t += 4 + 4 * cabac_decide(c, st + 2 + islice);
    t += 2 * cabac_decide(c, st + 3 + islice);
    t += cabac_decide(c, st + 3 + 2 * islice);
    return t;
}

static int ref_idx_cabac(h264dec *d, int list, int x, int y, int n) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state + 54;
    const int8_t *rc = d->ref_c[list];
    int dirA, dirB;
    if (x > 0) dirA = (d->direct8_cur >> ((y >> 1) * 2 + ((x - 1) >> 1))) & 1;
    else dirA = (d->direct8_l >> ((y >> 1) * 2 + 1)) & 1;
    if (y > 0) dirB = (d->direct8_cur >> (((y - 1) >> 1) * 2 + (x >> 1))) & 1;
    else dirB = (d->direct8_t >> (2 + (x >> 1))) & 1;
    int inc = (rc[CI(x - 1, y)] > 0 && !dirA) + 2 * (rc[CI(x, y - 1)] > 0 && !dirB);
    if (!cabac_decide(c, st + inc)) return 0;
    if (!cabac_decide(c, st + 4)) return 1;
    int r = 2;
    while (cabac_decide(c, st + 5)) {
        if (++r >= n) return -1;
    }
    if (r >= n) return -1;
    return r;
}

static int mvd_cabac(h264dec *d, int base, int sum) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state + base;
    int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
    if (!cabac_decide(c, st + inc)) return 0;
    int m = 1;
    static const uint8_t ctx[8] = { 3, 4, 5, 6, 6, 6, 6, 6 };
    while (m < 9 && cabac_decide(c, st + ctx[m - 1])) m++;
    if (m >= 9) {
        int k = 3;
        while (cabac_bypass(c)) {
            m += 1 << k;
            if (++k > 24) return 1 << 30;
        }
        while (k--) m += cabac_bypass(c) << k;
    }
    return cabac_bypass(c) ? -m : m;
}

static int read_mvd_part(h264dec *d, int list, int x, int y, int w, int h, int *mx, int *my) {
    uint8_t (*md)[2] = d->mvd_c[list];
    int a = CI(x - 1, y), b = CI(x, y - 1);
    int vx = mvd_cabac(d, 40, md[a][0] + md[b][0]);
    if (vx == (1 << 30) || vx == -(1 << 30)) return -1;
    int vy = mvd_cabac(d, 47, md[a][1] + md[b][1]);
    if (vy == (1 << 30) || vy == -(1 << 30)) return -1;
    *mx = vx; *my = vy;
    uint8_t ax = (uint8_t)h264_min(h264_abs(vx), 127), ay = (uint8_t)h264_min(h264_abs(vy), 127);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) { md[CI(x + i, y + j)][0] = ax; md[CI(x + i, y + j)][1] = ay; }
    return 0;
}

static void zero_mvd(h264dec *d, int list, int x, int y, int w, int h) {
    uint8_t (*md)[2] = d->mvd_c[list];
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) WR16(md[CI(x + i, y + j)], 0);
}

static int inter_parts_cabac(h264dec *d, int shape, int pred0, int pred1) {
    struct h264_slice *sl = &d->sl;
    int n = shape == 0 ? 1 : 2;
    int px[2], py[2], pw, ph, pred[2] = { pred0, pred1 }, ref[2][2];
    if (shape == 0) { px[0] = py[0] = 0; pw = ph = 4; }
    else if (shape == 1) { px[0] = px[1] = 0; py[0] = 0; py[1] = 2; pw = 4; ph = 2; }
    else { px[0] = 0; px[1] = 2; py[0] = py[1] = 0; pw = 2; ph = 4; }
    for (int li = 0; li < 2; li++)
        for (int p = 0; p < n; p++) h264_fill_part(d, li, px[p], py[p], pw, ph, -1, 0, 0);
    for (int li = 0; li < 2; li++)
        for (int p = 0; p < n; p++) {
            ref[li][p] = -1;
            if (pred[p] & (1 << li)) {
                ref[li][p] = sl->num_ref[li] > 1 ? ref_idx_cabac(d, li, px[p], py[p], sl->num_ref[li]) : 0;
                if (ref[li][p] < 0) return -1;
                h264_fill_part(d, li, px[p], py[p], pw, ph, ref[li][p], 0, 0);
            }
        }
    for (int li = 0; li < 2; li++)
        for (int p = 0; p < n; p++) {
            if (pred[p] & (1 << li)) {
                int mx, my;
                int16_t mvp[2];
                if (read_mvd_part(d, li, px[p], py[p], pw, ph, &mx, &my) < 0) return -1;
                h264_mv_pred(d, li, px[p], py[p], pw, ph, ref[li][p], mvp);
                h264_fill_part(d, li, px[p], py[p], pw, ph, ref[li][p], mvp[0] + mx, mvp[1] + my);
            } else zero_mvd(d, li, px[p], py[p], pw, ph);
        }
    return 0;
}

static int sub_type_cabac(h264dec *d, int is_b) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    if (!is_b) {
        if (cabac_decide(c, st + 21)) return 0;
        if (!cabac_decide(c, st + 22)) return 1;
        if (cabac_decide(c, st + 23)) return 2;
        return 3;
    }
    if (!cabac_decide(c, st + 36)) return 0;
    if (!cabac_decide(c, st + 37)) return 1 + cabac_decide(c, st + 39);
    int t = 3;
    if (cabac_decide(c, st + 38)) {
        if (cabac_decide(c, st + 39)) return 11 + cabac_decide(c, st + 39);
        t += 4;
    }
    t += 2 * cabac_decide(c, st + 39);
    t += cabac_decide(c, st + 39);
    return t;
}

static int inter_8x8_cabac(h264dec *d, int is_b) {
    struct h264_slice *sl = &d->sl;
    int ref[2][4];
    int dmask = 0;
    for (int i = 0; i < 4; i++) {
        d->sub_type[i] = sub_type_cabac(d, is_b);
        if (is_b && d->sub_type[i] == 0) dmask |= 1 << i;
    }
    d->direct8_cur = (uint8_t)dmask;
    for (int li = 0; li < 2; li++)
        for (int i = 0; i < 4; i++)
            if (!((dmask >> i) & 1)) h264_fill_part(d, li, (i & 1) * 2, (i >> 1) * 2, 2, 2, -1, 0, 0);
    if (dmask) {
        h264_direct(d, dmask);
        for (int i = 0; i < 4; i++)
            if ((dmask >> i) & 1) { zero_mvd(d, 0, (i & 1) * 2, (i >> 1) * 2, 2, 2); zero_mvd(d, 1, (i & 1) * 2, (i >> 1) * 2, 2, 2); }
    }
    for (int li = 0; li < (is_b ? 2 : 1); li++)
        for (int i = 0; i < 4; i++) {
            ref[li][i] = -1;
            if ((dmask >> i) & 1) continue;
            int pr = is_b ? h264_b_sub[d->sub_type[i]][3] : 1;
            if (pr & (1 << li)) {
                ref[li][i] = sl->num_ref[li] > 1 ? ref_idx_cabac(d, li, (i & 1) * 2, (i >> 1) * 2, sl->num_ref[li]) : 0;
                if (ref[li][i] < 0) return -1;
                h264_fill_part(d, li, (i & 1) * 2, (i >> 1) * 2, 2, 2, ref[li][i], 0, 0);
            }
        }
    for (int li = 0; li < 2; li++)
        for (int i = 0; i < 4; i++) {
            if ((dmask >> i) & 1) continue;
            int bx = (i & 1) * 2, by = (i >> 1) * 2;
            const uint8_t *s = is_b ? h264_b_sub[d->sub_type[i]] : h264_p_sub[d->sub_type[i]];
            if (li == 1 && !is_b) { zero_mvd(d, 1, bx, by, 2, 2); continue; }
            if (!(s[3] & (1 << li))) { zero_mvd(d, li, bx, by, 2, 2); continue; }
            int w = s[1], h = s[2];
            for (int j = 0; j < s[0]; j++) {
                int x = bx, y = by;
                if (s[0] == 4) { x += j & 1; y += j >> 1; }
                else if (s[0] == 2) { if (w == 2) y += j; else x += j; }
                int mx, my;
                int16_t mvp[2];
                if (read_mvd_part(d, li, x, y, w, h, &mx, &my) < 0) return -1;
                h264_mv_pred(d, li, x, y, w, h, ref[li][i], mvp);
                h264_fill_part(d, li, x, y, w, h, ref[li][i], mvp[0] + mx, mvp[1] + my);
            }
        }
    d->mb_type |= MB_8x8;
    return 0;
}

static int intra_modes_cabac(h264dec *d, int t8) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    int8_t *ipm = d->ipm_c;
    int nb = t8 ? 4 : 16;
    for (int k = 0; k < nb; k++) {
        int blk = t8 ? k * 4 : k;
        int r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
        int a = ipm[CI(x - 1, y)], b = ipm[CI(x, y - 1)];
        int pred = (a < 0 || b < 0) ? 2 : h264_min(a, b);
        int mode;
        if (cabac_decide(c, st + 68)) mode = pred;
        else {
            int rem = cabac_decide(c, st + 69);
            rem |= cabac_decide(c, st + 69) << 1;
            rem |= cabac_decide(c, st + 69) << 2;
            mode = rem < pred ? rem : rem + 1;
        }
        if (t8) ipm[CI(x, y)] = ipm[CI(x + 1, y)] = ipm[CI(x, y + 1)] = ipm[CI(x + 1, y + 1)] = (int8_t)mode;
        else ipm[CI(x, y)] = (int8_t)mode;
        d->ipred4[blk] = mode;
    }
    return 0;
}

static int chroma_pred_cabac(h264dec *d) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state + 64;
    const struct h264_mbinfo *mbi = d->mbi;
    int a = d->avail_l && mbi[d->mb_xy - 1].chroma_pred != 0;
    int b = d->avail_t && mbi[d->mb_xy - d->mb_w].chroma_pred != 0;
    if (!cabac_decide(c, st + a + b)) return 0;
    if (!cabac_decide(c, st + 3)) return 1;
    return cabac_decide(c, st + 3) ? 3 : 2;
}

static int cbp_cabac(h264dec *d) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    int cbp = 0;
    int lpcm = d->left_type & MB_PCM, tpcm = d->top_type & MB_PCM;
    for (int b8 = 0; b8 < 4; b8++) {
        int x = b8 & 1, y = b8 >> 1;
        int a, b;
        if (x) a = !((cbp >> (b8 - 1)) & 1);
        else a = (d->avail_l && !lpcm) ? !((d->left_cbp >> (b8 + 1)) & 1) : 0;
        if (y) b = !((cbp >> (b8 - 2)) & 1);
        else b = (d->avail_t && !tpcm) ? !((d->top_cbp >> (b8 + 2)) & 1) : 0;
        cbp |= cabac_decide(c, st + 73 + a + 2 * b) << b8;
    }
    int a = d->avail_l && (d->left_cbp >> 4) != 0;
    int b = d->avail_t && (d->top_cbp >> 4) != 0;
    if (cabac_decide(c, st + 77 + a + 2 * b)) {
        a = d->avail_l && (d->left_cbp >> 4) == 2;
        b = d->avail_t && (d->top_cbp >> 4) == 2;
        cbp |= (1 + cabac_decide(c, st + 77 + 4 + a + 2 * b)) << 4;
    }
    return cbp;
}

int h264_mb_cabac(h264dec *d) {
    struct h264_cabac *c = &d->cabac;
    uint8_t *st = d->cabac_state;
    struct h264_slice *sl = &d->sl;
    d->mb_type = 0; d->cbp = 0; d->t8x8 = 0; d->chroma_pred = 0;
    d->nz_luma = 0; d->nz_chroma = 0; d->nzdb_cur = 0; d->cbf_cur = 0;
    d->direct8_cur = 0;
    h264_fill_cache(d);
    if (sl->type != 2) {
        for (int i = 0; i < 16; i++) { int r = CI(i & 3, i >> 2); WR16(d->mvd_c[0][r], 0); WR16(d->mvd_c[1][r], 0); }
    }

    if (sl->type != 2 && h264_cabac_skip(d)) {
        if (sl->type == 0) {
            d->mb_type = MB_SKIP | MB_16x16 | MB_INTER;
            h264_pskip_mv(d);
            h264_fill_part(d, 1, 0, 0, 4, 4, -1, 0, 0);
        } else {
            d->mb_type = MB_SKIP | MB_DIRECT | MB_INTER;
            d->direct8_cur = 15;
            h264_direct(d, 15);
        }
        d->last_qp_delta = 0;
        return 0;
    }

    int itype = -1, no_sub_lt8 = 1;
    if (sl->type == 2) itype = intra_mb_type(d, 3, 1);
    else if (sl->type == 0) {
        if (!cabac_decide(c, st + 14)) {
            int t;
            if (!cabac_decide(c, st + 15)) t = 3 * cabac_decide(c, st + 16);
            else t = 2 - cabac_decide(c, st + 17);
            d->mb_type = MB_INTER;
            if (t < 3) {
                d->mb_type |= t == 0 ? MB_16x16 : t == 1 ? MB_16x8 : MB_8x16;
                if (inter_parts_cabac(d, t, 1, 1) < 0) return -1;
            } else {
                if (inter_8x8_cabac(d, 0) < 0) return -1;
                for (int i = 0; i < 4; i++) if (d->sub_type[i]) no_sub_lt8 = 0;
            }
        } else itype = intra_mb_type(d, 17, 0);
    } else {
        int inc = (d->avail_l && !(d->left_type & MB_DIRECT)) + (d->avail_t && !(d->top_type & MB_DIRECT));
        int t;
        if (!cabac_decide(c, st + 27 + inc)) t = 0;
        else if (!cabac_decide(c, st + 27 + 3)) t = 1 + cabac_decide(c, st + 27 + 5);
        else {
            int bits = cabac_decide(c, st + 27 + 4) << 3;
            bits |= cabac_decide(c, st + 27 + 5) << 2;
            bits |= cabac_decide(c, st + 27 + 5) << 1;
            bits |= cabac_decide(c, st + 27 + 5);
            if (bits < 8) t = bits + 3;
            else if (bits == 13) t = -1;
            else if (bits == 14) t = 11;
            else if (bits == 15) t = 22;
            else t = ((bits << 1) | cabac_decide(c, st + 27 + 5)) - 4;
        }
        if (t < 0) itype = intra_mb_type(d, 32, 0);
        else {
            d->mb_type = MB_INTER;
            if (t == 0) {
                d->mb_type |= MB_DIRECT;
                d->direct8_cur = 15;
                h264_direct(d, 15);
                if (!d->act_sps->direct_8x8_inference) no_sub_lt8 = 0;
            } else if (t < 22) {
                const uint8_t *bp = h264_b_part[t];
                d->mb_type |= bp[0] == 0 ? MB_16x16 : bp[0] == 1 ? MB_16x8 : MB_8x16;
                if (inter_parts_cabac(d, bp[0], bp[1], bp[2]) < 0) return -1;
            } else {
                if (inter_8x8_cabac(d, 1) < 0) return -1;
                for (int i = 0; i < 4; i++) {
                    if (d->sub_type[i] == 0) { if (!d->act_sps->direct_8x8_inference) no_sub_lt8 = 0; }
                    else if (h264_b_sub[d->sub_type[i]][0] > 1) no_sub_lt8 = 0;
                }
            }
        }
    }

    if (itype >= 0) {
        if (sl->type != 2) {
            h264_fill_part(d, 0, 0, 0, 4, 4, -1, 0, 0);
            h264_fill_part(d, 1, 0, 0, 4, 4, -1, 0, 0);
        }
        if (itype == 25) {
            d->mb_type = MB_PCM;
            const uint8_t *p = h264_cabac_pcm_start(c);
            if (p + 384 > c->end) return -1;
            memcpy(d->coef, p, 384);
            h264_cabac_start(c, p + 384, c->end);
            d->last_qp_delta = 0;
            return 0;
        }
        if (itype == 0) {
            int t8 = 0;
            if (d->p->transform_8x8) {
                int inc = (d->avail_l && d->mbi[d->mb_xy - 1].t8x8) + (d->avail_t && d->mbi[d->mb_xy - d->mb_w].t8x8);
                t8 = cabac_decide(c, st + 399 + inc);
            }
            d->t8x8 = t8;
            d->mb_type = t8 ? MB_I8x8 : MB_I4x4;
            intra_modes_cabac(d, t8);
        } else {
            d->mb_type = MB_I16;
            d->intra16_mode = (itype - 1) & 3;
            d->cbp = (((itype - 1) >> 2) % 3) << 4 | (itype >= 13 ? 15 : 0);
        }
        d->chroma_pred = chroma_pred_cabac(d);
    }

    if (!(d->mb_type & MB_I16)) {
        d->cbp = cbp_cabac(d);
        if ((d->cbp & 15) && d->p->transform_8x8 && (d->mb_type & MB_INTER) && no_sub_lt8) {
            int inc = (d->avail_l && d->mbi[d->mb_xy - 1].t8x8) + (d->avail_t && d->mbi[d->mb_xy - d->mb_w].t8x8);
            d->t8x8 = cabac_decide(c, st + 399 + inc);
        }
    }
    if (d->cbp || (d->mb_type & MB_I16)) {
        int dq = 0;
        if (cabac_decide(c, st + 60 + (d->last_qp_delta != 0))) {
            int k = 1;
            if (cabac_decide(c, st + 62)) {
                k = 2;
                while (cabac_decide(c, st + 63)) if (++k > 52) return -1;
            }
            dq = (k & 1) ? (k + 1) / 2 : -(k / 2);
        }
        if (dq < -26 || dq > 25) return -1;
        d->last_qp_delta = dq;
        d->qp = (d->qp + dq + 52) % 52;
        d->qpc[0] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[0], 0, 51)];
        d->qpc[1] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[1], 0, 51)];
        d->bypass = d->act_sps->transform_bypass && d->qp == 0;
        if (cabac_residual(d) < 0) return -1;
    } else d->last_qp_delta = 0;
    if (c->p > c->end + 64) return -1;
    return 0;
}
