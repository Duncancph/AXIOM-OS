/* AXIOM H.264 decoder: CAVLC macroblock layer (7.3.5, 9.2). */
#include "h264_int.h"

/* ---------------------------------------------------------------- VLC tables
   Two-level lookup: the first level is indexed by the next 8 bits; codes longer than 8 bits point to a
   second-level table indexed by the following 8 bits. Entry: sym (or sub-table offset), len (code length,
   0 = invalid, <0 = sub-table). */
struct vlc_e { int16_t sym; int8_t len; };

static const uint8_t ct_len[4][68] = {
    { 1,0,0,0, 6,2,0,0, 8,6,3,0, 9,8,7,5, 10,9,8,6, 11,10,9,7, 13,11,10,8, 13,13,11,9, 13,13,13,10, 14,14,13,11,
      14,14,14,13, 15,15,14,14, 15,15,15,14, 16,15,15,15, 16,16,16,15, 16,16,16,16, 16,16,16,16 },
    { 2,0,0,0, 6,2,0,0, 6,5,3,0, 7,6,6,4, 8,6,6,4, 8,7,7,5, 9,8,8,6, 11,9,9,6, 11,11,11,7, 12,11,11,9,
      12,12,12,11, 12,12,12,11, 13,13,13,12, 13,13,13,13, 13,14,13,13, 14,14,14,13, 14,14,14,14 },
    { 4,0,0,0, 6,4,0,0, 6,5,4,0, 6,5,5,4, 7,5,5,4, 7,5,5,4, 7,6,6,4, 7,6,6,4, 8,7,7,5, 8,8,7,6,
      9,8,8,7, 9,9,8,8, 9,9,9,8, 10,9,9,9, 10,10,10,10, 10,10,10,10, 10,10,10,10 },
    { 6,0,0,0, 6,6,0,0, 6,6,6,0, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6,
      6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6 },
};
static const uint8_t ct_bits[4][68] = {
    { 1,0,0,0, 5,1,0,0, 7,4,1,0, 7,6,5,3, 7,6,5,3, 7,6,5,4, 15,6,5,4, 11,14,5,4, 8,10,13,4, 15,14,9,4,
      11,10,13,12, 15,14,9,12, 11,10,13,8, 15,1,9,12, 11,14,13,8, 7,10,9,12, 4,6,5,8 },
    { 3,0,0,0, 11,2,0,0, 7,7,3,0, 7,10,9,5, 7,6,5,4, 4,6,5,6, 7,6,5,8, 15,6,5,4, 11,14,13,4, 15,10,9,4,
      11,14,13,12, 8,10,9,8, 15,14,13,12, 11,10,9,12, 7,11,6,8, 9,8,10,1, 7,6,5,4 },
    { 15,0,0,0, 15,14,0,0, 11,15,13,0, 8,12,14,12, 15,10,11,11, 11,8,9,10, 9,14,13,9, 8,10,9,8, 15,14,13,13,
      11,14,10,12, 15,10,13,12, 11,14,9,12, 8,10,13,8, 13,7,9,12, 9,12,11,10, 5,8,7,6, 1,4,3,2 },
    { 3,0,0,0, 0,1,0,0, 4,5,6,0, 8,9,10,11, 12,13,14,15, 16,17,18,19, 20,21,22,23, 24,25,26,27, 28,29,30,31,
      32,33,34,35, 36,37,38,39, 40,41,42,43, 44,45,46,47, 48,49,50,51, 52,53,54,55, 56,57,58,59, 60,61,62,63 },
};
static const uint8_t cdc_len[20] = { 2,0,0,0, 6,1,0,0, 6,6,3,0, 6,7,7,6, 6,8,8,7 };
static const uint8_t cdc_bits[20] = { 1,0,0,0, 7,1,0,0, 4,6,1,0, 3,3,2,5, 2,3,2,0 };
static const uint8_t tz_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9}, {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6}, {4,3,3,3,4,4,3,3,4,5,5,6,5,6},
    {5,3,4,4,3,3,3,4,3,4,5,5,5}, {4,4,4,3,3,3,3,3,4,5,4,5}, {6,5,3,3,3,3,3,3,4,3,6}, {6,5,3,3,3,2,3,4,3,6},
    {6,4,5,3,2,2,3,3,6}, {6,6,4,2,2,3,2,5}, {5,5,3,2,2,2,4}, {4,4,3,3,1,3}, {4,4,2,1,3}, {3,3,1,2}, {2,2,1}, {1,1},
};
static const uint8_t tz_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1}, {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0}, {5,7,6,5,4,3,4,3,2,3,2,1,1,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0}, {5,4,3,7,6,5,4,3,2,1,1,0}, {1,1,7,6,5,4,3,2,1,1,0}, {1,1,5,4,3,3,2,1,1,0},
    {1,1,1,3,3,2,2,1,0}, {1,0,1,3,2,1,1,1}, {1,0,1,3,2,1,1}, {0,1,1,2,1,3}, {0,1,1,1,1}, {0,1,1,1}, {0,1,1}, {0,1},
};
static const uint8_t ctz_len[3][4] = { {1,2,3,3}, {1,2,2}, {1,1} };
static const uint8_t ctz_bits[3][4] = { {1,1,1,0}, {1,1,0}, {1,0} };
static const uint8_t run_len[7][16] = {
    {1,1}, {1,2,2}, {2,2,2,2}, {2,2,2,3,3}, {2,2,3,3,3,3}, {2,3,3,3,3,3,3}, {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11},
};
static const uint8_t run_bits[7][16] = {
    {1,0}, {1,1,0}, {3,2,1,0}, {3,2,1,1,0}, {3,2,3,2,1,0}, {3,0,1,3,2,5,4}, {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1},
};

#define VLC_POOL 12000
static struct vlc_e vlc_pool[VLC_POOL];
static int vlc_used;
static const struct vlc_e *vlc_coeff[5], *vlc_tz[15], *vlc_ctz[3], *vlc_run[7];
static int vlc_ready;

static const struct vlc_e *vlc_build(const uint8_t *lens, const uint8_t *bits, int n, int coeff_token) {
    struct vlc_e *t = &vlc_pool[vlc_used];
    vlc_used += 256;
    for (int s = 0; s < n; s++) {
        int len = lens[s];
        if (!len) continue;
        if (coeff_token && (s & 3) > (s >> 2)) continue;
        uint32_t code = bits[s];
        int16_t sym = (int16_t)s;
        if (len <= 8) {
            int sh = 8 - len;
            for (uint32_t i = 0; i < (1u << sh); i++) { t[(code << sh) | i].sym = sym; t[(code << sh) | i].len = (int8_t)len; }
        } else {
            uint32_t hi = code >> (len - 8);
            if (t[hi].len == 0) {
                t[hi].len = -1;
                t[hi].sym = (int16_t)vlc_used;
                vlc_used += 256;
            }
            struct vlc_e *s2 = &vlc_pool[t[hi].sym];
            int rem = len - 8, sh = 8 - rem;
            uint32_t lo = code & ((1u << rem) - 1);
            for (uint32_t i = 0; i < (1u << sh); i++) { s2[(lo << sh) | i].sym = sym; s2[(lo << sh) | i].len = (int8_t)len; }
        }
    }
    return t;
}

static void vlc_init(void) {
    if (vlc_ready) return;
    memset(vlc_pool, 0, sizeof(vlc_pool));
    vlc_used = 0;
    for (int i = 0; i < 4; i++) vlc_coeff[i] = vlc_build(ct_len[i], ct_bits[i], 68, 1);
    vlc_coeff[4] = vlc_build(cdc_len, cdc_bits, 20, 1);
    for (int i = 0; i < 15; i++) vlc_tz[i] = vlc_build(tz_len[i], tz_bits[i], 16 - i, 0);
    for (int i = 0; i < 3; i++) vlc_ctz[i] = vlc_build(ctz_len[i], ctz_bits[i], 4 - i, 0);
    for (int i = 0; i < 7; i++) vlc_run[i] = vlc_build(run_len[i], run_bits[i], i == 6 ? 15 : i + 2, 0);
    vlc_ready = 1;
}

H264_INLINE int vlc_read(struct h264_br *b, const struct vlc_e *t) {
    uint32_t v = br_peek32(b);
    struct vlc_e e = t[v >> 24];
    if (e.len > 0) { b->pos += (uint32_t)e.len; return e.sym; }
    if (e.len == 0) return -1;
    e = vlc_pool[e.sym + ((v >> 16) & 255)];
    if (e.len <= 0) return -1;
    b->pos += (uint32_t)e.len;
    return e.sym;
}

/* residual_block_cavlc: returns TotalCoeff or -1. Coefficients go to blk[scan[i]], scaled by dq when dq != 0. */
static int cavlc_block(struct h264_br *b, int nc, int maxc, int16_t *blk, const uint8_t *scan, const uint32_t *dq) {
    const struct vlc_e *t = nc < 0 ? vlc_coeff[4] : nc < 2 ? vlc_coeff[0] : nc < 4 ? vlc_coeff[1] : nc < 8 ? vlc_coeff[2] : vlc_coeff[3];
    int sym = vlc_read(b, t);
    if (sym < 0) return -1;
    int tc = sym >> 2, t1 = sym & 3;
    if (tc == 0) return 0;
    if (tc > maxc) return -1;
    int level[16];
    int i = 0;
    if (t1) {
        uint32_t s = br_bits(b, t1);
        for (; i < t1; i++) level[i] = 1 - 2 * (int)((s >> (t1 - 1 - i)) & 1);
    }
    int sl = (tc > 10 && t1 < 3) ? 1 : 0;
    for (; i < tc; i++) {
        uint32_t v = br_peek32(b);
        if (v == 0) return -1;
        int prefix = __builtin_clz(v);
        if (prefix > 25) return -1;
        b->pos += (uint32_t)prefix + 1;
        int code;
        int ssize = (prefix == 14 && sl == 0) ? 4 : prefix >= 15 ? prefix - 3 : sl;
        code = (h264_min(15, prefix) << sl);
        if (ssize) code += (int)br_bits(b, ssize);
        if (prefix >= 15 && sl == 0) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) code += 2;
        int lv = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
        level[i] = lv;
        if (sl == 0) sl = 1;
        if (h264_abs(lv) > (3 << (sl - 1)) && sl < 6) sl++;
    }
    int zeros = 0;
    if (tc < maxc) {
        zeros = vlc_read(b, maxc == 4 ? vlc_ctz[tc - 1] : vlc_tz[tc - 1]);
        if (zeros < 0) return -1;
        if (tc + zeros > maxc) return -1;
    }
    int pos = tc + zeros - 1;
    for (i = 0; i < tc; i++) {
        int k = scan[pos];
        if (dq) blk[k] = (int16_t)(((int64_t)level[i] * dq[k] + 32) >> 6);
        else blk[k] = (int16_t)level[i];
        if (i == tc - 1) break;
        int run = 0;
        if (zeros > 0) {
            run = vlc_read(b, vlc_run[h264_min(zeros, 7) - 1]);
            if (run < 0 || run > zeros) return -1;
            zeros -= run;
        }
        pos -= run + 1;
    }
    return tc;
}

H264_INLINE int pred_nc(const int8_t *nzc, int idx) {
    int a = nzc[idx - 1], b = nzc[idx - 8];
    if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
    if (a >= 0) return a;
    if (b >= 0) return b;
    return 0;
}

static uint8_t scan8x8_cavlc[4][16];
static int scan8_ready;

/* luma residual + chroma residual (7.3.5.3) */
static int cavlc_residual(h264dec *d) {
    struct h264_br *b = &d->br;
    int cbp = d->cbp;
    int intra = (d->mb_type & MB_INTRA) != 0;
    int qp = d->qp;
    int8_t *nzc = d->nnz_c[0];
    if (!scan8_ready) {
        for (int i = 0; i < 4; i++) for (int k = 0; k < 16; k++) scan8x8_cavlc[i][k] = h264_zigzag8[4 * k + i];
        scan8_ready = 1;
    }
    const uint32_t *dq4 = d->dq4[intra ? 0 : 3][qp];
    if (d->mb_type & MB_I16) {
        int16_t dc[16] = { 0 };
        int n = cavlc_block(b, pred_nc(nzc, CI(0, 0)), 16, dc, h264_zigzag4, 0);
        if (n < 0) return -1;
        if (n) {
            d->cbf_cur |= 1u << 24;
            for (int i = 0; i < 16; i++) d->dc_y[i] = dc[i];
        }
        if (cbp & 15) {
            for (int blk = 0; blk < 16; blk++) {
                int r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
                int n2 = cavlc_block(b, pred_nc(nzc, CI(x, y)), 15, d->coef + blk * 16, h264_zigzag4 + 1, dq4);
                if (n2 < 0) return -1;
                nzc[CI(x, y)] = (int8_t)n2;
                if (n2) { d->nz_luma |= 1 << blk; d->cbf_cur |= 1u << r; }
            }
        }
    } else {
        for (int b8 = 0; b8 < 4; b8++) {
            if (!(cbp & (1 << b8))) continue;
            if (d->t8x8) {
                const uint32_t *dq8 = d->dq8[intra ? 0 : 1][qp];
                int tot = 0;
                for (int i = 0; i < 4; i++) {
                    int blk = b8 * 4 + i, r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
                    int n = cavlc_block(b, pred_nc(nzc, CI(x, y)), 16, d->coef + b8 * 64, scan8x8_cavlc[i], dq8);
                    if (n < 0) return -1;
                    nzc[CI(x, y)] = (int8_t)n;
                    tot += n;
                }
                if (tot) {
                    d->nz_luma |= 15 << (b8 * 4);
                    int r = h264_blk_xy[b8 * 4];
                    d->nzdb_cur |= (uint16_t)(0x33 << r);
                }
            } else {
                for (int i = 0; i < 4; i++) {
                    int blk = b8 * 4 + i, r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
                    int n = cavlc_block(b, pred_nc(nzc, CI(x, y)), 16, d->coef + blk * 16, h264_zigzag4, dq4);
                    if (n < 0) return -1;
                    nzc[CI(x, y)] = (int8_t)n;
                    if (n) { d->nz_luma |= 1 << blk; d->nzdb_cur |= (uint16_t)(1 << r); }
                }
            }
        }
    }
    if (cbp & 0x30) {
        for (int c = 0; c < 2; c++) {
            int16_t dc[4] = { 0, 0, 0, 0 };
            static const uint8_t ident[4] = { 0, 1, 2, 3 };
            int n = cavlc_block(b, -1, 4, dc, ident, 0);
            if (n < 0) return -1;
            for (int i = 0; i < 4; i++) d->dc_c[c][i] = dc[i];
            if (n) d->cbf_cur |= 1u << (25 + c);
        }
        if (cbp & 0x20) {
            for (int c = 0; c < 2; c++) {
                const uint32_t *dqc = d->dq4[(intra ? 1 : 4) + c][d->qpc[c]];
                int8_t *nc = d->nnz_c[1 + c];
                for (int i = 0; i < 4; i++) {
                    int x = i & 1, y = i >> 1;
                    int n = cavlc_block(b, pred_nc(nc, CI(x, y)), 15, d->coefc[c] + i * 16, h264_zigzag4 + 1, dqc);
                    if (n < 0) return -1;
                    nc[CI(x, y)] = (int8_t)n;
                    if (n) { d->nz_chroma |= (uint8_t)(1 << (c * 4 + i)); d->cbf_cur |= 1u << (16 + c * 4 + i); }
                }
            }
        }
    }
    return 0;
}

static const uint8_t p_part[4][3] = { { MB_16x16 >> 6, 1, 1 }, { MB_16x8 >> 6, 1, 1 }, { MB_8x16 >> 6, 1, 1 }, { 0, 0, 0 } };
/* B mb_type 1..21: shape (0 16x16, 1 16x8, 2 8x16), pred of part 0 and 1 */
static const uint8_t b_part[22][3] = {
    { 0, 0, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 0, 3, 0 }, { 1, 1, 1 }, { 2, 1, 1 }, { 1, 2, 2 }, { 2, 2, 2 },
    { 1, 1, 2 }, { 2, 1, 2 }, { 1, 2, 1 }, { 2, 2, 1 }, { 1, 1, 3 }, { 2, 1, 3 }, { 1, 2, 3 }, { 2, 2, 3 },
    { 1, 3, 1 }, { 2, 3, 1 }, { 1, 3, 2 }, { 2, 3, 2 }, { 1, 3, 3 }, { 2, 3, 3 },
};
/* sub_mb_type: number of parts, part w, h (4x4 units), pred flags */
static const uint8_t p_sub[4][4] = { { 1, 2, 2, 1 }, { 2, 2, 1, 1 }, { 2, 1, 2, 1 }, { 4, 1, 1, 1 } };
static const uint8_t b_sub[13][4] = {
    { 4, 1, 1, 0 }, { 1, 2, 2, 1 }, { 1, 2, 2, 2 }, { 1, 2, 2, 3 }, { 2, 2, 1, 1 }, { 2, 1, 2, 1 }, { 2, 2, 1, 2 },
    { 2, 1, 2, 2 }, { 2, 2, 1, 3 }, { 2, 1, 2, 3 }, { 4, 1, 1, 1 }, { 4, 1, 1, 2 }, { 4, 1, 1, 3 },
};
const uint8_t (*h264_b_part)[3] = b_part;
const uint8_t (*h264_b_sub)[4] = b_sub;
const uint8_t (*h264_p_sub)[4] = p_sub;

static int intra_modes_cavlc(h264dec *d, int t8) {
    struct h264_br *b = &d->br;
    int8_t *ipm = d->ipm_c;
    if (t8) {
        for (int b8 = 0; b8 < 4; b8++) {
            int x = (b8 & 1) * 2, y = (b8 >> 1) * 2;
            int a = ipm[CI(x - 1, y)], bb = ipm[CI(x, y - 1)];
            int pred = (a < 0 || bb < 0) ? 2 : h264_min(a, bb);
            int mode;
            if (br_bit(b)) mode = pred;
            else { int rem = (int)br_bits(b, 3); mode = rem < pred ? rem : rem + 1; }
            ipm[CI(x, y)] = ipm[CI(x + 1, y)] = ipm[CI(x, y + 1)] = ipm[CI(x + 1, y + 1)] = (int8_t)mode;
            d->ipred4[b8 * 4] = mode;
        }
    } else {
        for (int blk = 0; blk < 16; blk++) {
            int r = h264_blk_xy[blk], x = r & 3, y = r >> 2;
            int a = ipm[CI(x - 1, y)], bb = ipm[CI(x, y - 1)];
            int pred = (a < 0 || bb < 0) ? 2 : h264_min(a, bb);
            int mode;
            if (br_bit(b)) mode = pred;
            else { int rem = (int)br_bits(b, 3); mode = rem < pred ? rem : rem + 1; }
            ipm[CI(x, y)] = (int8_t)mode;
            d->ipred4[blk] = mode;
        }
    }
    uint32_t cm = br_ue(b);
    if (cm > 3) return -1;
    d->chroma_pred = (int)cm;
    return 0;
}

H264_INLINE int read_ref(struct h264_br *b, int n) {
    if (n <= 1) return 0;
    uint32_t r = br_te(b, n - 1);
    if (r >= (uint32_t)n) return -1;
    return (int)r;
}

H264_INLINE void read_mvd(struct h264_br *b, int *mx, int *my) {
    *mx = br_se(b);
    *my = br_se(b);
}

/* inter prediction syntax for 16x16 / 16x8 / 8x16 partitions */
static int inter_parts_cavlc(h264dec *d, int shape, int pred0, int pred1) {
    struct h264_br *b = &d->br;
    struct h264_slice *sl = &d->sl;
    int n = shape == 0 ? 1 : 2;
    int px[2], py[2], pw, ph, pred[2] = { pred0, pred1 }, ref[2][2];
    if (shape == 0) { px[0] = py[0] = 0; pw = ph = 4; }
    else if (shape == 1) { px[0] = px[1] = 0; py[0] = 0; py[1] = 2; pw = 4; ph = 2; }
    else { px[0] = 0; px[1] = 2; py[0] = py[1] = 0; pw = 2; ph = 4; }
    for (int li = 0; li < 2; li++)
        for (int p = 0; p < n; p++) {
            ref[li][p] = -1;
            if (pred[p] & (1 << li)) {
                ref[li][p] = read_ref(b, sl->num_ref[li]);
                if (ref[li][p] < 0) return -1;
            }
        }
    for (int li = 0; li < 2; li++)
        for (int p = 0; p < n; p++) {
            if (pred[p] & (1 << li)) {
                int mx, my;
                int16_t mvp[2];
                read_mvd(b, &mx, &my);
                h264_mv_pred(d, li, px[p], py[p], pw, ph, ref[li][p], mvp);
                h264_fill_part(d, li, px[p], py[p], pw, ph, ref[li][p], mvp[0] + mx, mvp[1] + my);
            } else h264_fill_part(d, li, px[p], py[p], pw, ph, -1, 0, 0);
        }
    return 0;
}

static int inter_8x8_cavlc(h264dec *d, int is_b, int ref0) {
    struct h264_br *b = &d->br;
    struct h264_slice *sl = &d->sl;
    int ref[2][4];
    int dmask = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t st = br_ue(b);
        if (st > (is_b ? 12u : 3u)) return -1;
        d->sub_type[i] = (int)st;
        if (is_b && st == 0) dmask |= 1 << i;
    }
    if (dmask) h264_direct(d, dmask);
    for (int li = 0; li < (is_b ? 2 : 1); li++)
        for (int i = 0; i < 4; i++) {
            int pr = is_b ? b_sub[d->sub_type[i]][3] : 1;
            ref[li][i] = -1;
            if ((dmask >> i) & 1) continue;
            if (pr & (1 << li)) {
                if (ref0) ref[li][i] = 0;
                else { ref[li][i] = read_ref(b, sl->num_ref[li]); if (ref[li][i] < 0) return -1; }
            }
        }
    for (int li = 0; li < (is_b ? 2 : 1); li++)
        for (int i = 0; i < 4; i++) {
            if ((dmask >> i) & 1) continue;
            const uint8_t *s = is_b ? b_sub[d->sub_type[i]] : p_sub[d->sub_type[i]];
            int bx = (i & 1) * 2, by = (i >> 1) * 2;
            if (!(s[3] & (1 << li))) { h264_fill_part(d, li, bx, by, 2, 2, -1, 0, 0); continue; }
            int w = s[1], h = s[2];
            for (int j = 0; j < s[0]; j++) {
                int x = bx, y = by;
                if (s[0] == 4) { x += j & 1; y += j >> 1; }
                else if (s[0] == 2) { if (w == 2) y += j; else x += j; }
                int mx, my;
                int16_t mvp[2];
                read_mvd(b, &mx, &my);
                h264_mv_pred(d, li, x, y, w, h, ref[li][i], mvp);
                h264_fill_part(d, li, x, y, w, h, ref[li][i], mvp[0] + mx, mvp[1] + my);
            }
        }
    if (!is_b) for (int i = 0; i < 4; i++) h264_fill_part(d, 1, (i & 1) * 2, (i >> 1) * 2, 2, 2, -1, 0, 0);
    d->mb_type |= MB_8x8;
    return 0;
}

int h264_mb_cavlc(h264dec *d) {
    struct h264_br *b = &d->br;
    struct h264_slice *sl = &d->sl;
    vlc_init();
    d->mb_type = 0; d->cbp = 0; d->t8x8 = 0; d->chroma_pred = 0;
    d->nz_luma = 0; d->nz_chroma = 0; d->nzdb_cur = 0; d->cbf_cur = 0;
    h264_fill_cache(d);

    if (sl->type != 2) {
        if (d->skip_run < 0) {
            uint32_t r = br_ue(b);
            if (r > (uint32_t)d->mb_count) return -1;
            d->skip_run = (int)r;
        }
        if (d->skip_run > 0) {
            d->skip_run--;
            if (sl->type == 0) {
                d->mb_type = MB_SKIP | MB_16x16 | MB_INTER;
                h264_pskip_mv(d);
                h264_fill_part(d, 1, 0, 0, 4, 4, -1, 0, 0);
            } else {
                d->mb_type = MB_SKIP | MB_DIRECT | MB_INTER;
                h264_direct(d, 15);
            }
            return 0;
        }
        d->skip_run = -1;
    }

    uint32_t mbt = br_ue(b);
    int itype = -1;
    if (sl->type == 2) itype = (int)mbt;
    else if (sl->type == 0) { if (mbt < 5) itype = -1; else itype = (int)mbt - 5; }
    else { if (mbt < 23) itype = -1; else itype = (int)mbt - 23; }
    if (mbt > 48 || itype > 25) return -1;

    int no_sub_lt8 = 1;
    if (itype >= 0) {
        if (itype == 25) {
            d->mb_type = MB_PCM;
            b->pos = (b->pos + 7) & ~7u;
            uint8_t *pcm = (uint8_t *)d->coef;
            for (int i = 0; i < 384; i++) pcm[i] = (uint8_t)br_bits(b, 8);
            if (br_overrun(b)) return -1;
            return 0;
        }
        if (itype == 0) {
            int t8 = d->p->transform_8x8 ? (int)br_bit(b) : 0;
            d->t8x8 = t8;
            d->mb_type = t8 ? MB_I8x8 : MB_I4x4;
            if (intra_modes_cavlc(d, t8) < 0) return -1;
        } else {
            d->mb_type = MB_I16;
            d->intra16_mode = (itype - 1) & 3;
            d->cbp = (((itype - 1) >> 2) % 3) << 4 | (itype >= 13 ? 15 : 0);
            uint32_t cm = br_ue(b);
            if (cm > 3) return -1;
            d->chroma_pred = (int)cm;
        }
        if (sl->type != 2) {
            h264_fill_part(d, 0, 0, 0, 4, 4, -1, 0, 0);
            h264_fill_part(d, 1, 0, 0, 4, 4, -1, 0, 0);
        }
    } else if (sl->type == 0) {
        d->mb_type = MB_INTER;
        if (mbt < 3) {
            d->mb_type |= p_part[mbt][0] << 6;
            if (inter_parts_cavlc(d, (int)mbt, 1, 1) < 0) return -1;
        } else {
            if (inter_8x8_cavlc(d, 0, mbt == 4) < 0) return -1;
            for (int i = 0; i < 4; i++) if (d->sub_type[i]) no_sub_lt8 = 0;
        }
    } else {
        d->mb_type = MB_INTER;
        if (mbt == 0) {
            d->mb_type |= MB_DIRECT;
            h264_direct(d, 15);
            if (!d->act_sps->direct_8x8_inference) no_sub_lt8 = 0;
        } else if (mbt < 22) {
            const uint8_t *bp = b_part[mbt];
            d->mb_type |= bp[0] == 0 ? MB_16x16 : bp[0] == 1 ? MB_16x8 : MB_8x16;
            if (inter_parts_cavlc(d, bp[0], bp[1], bp[2]) < 0) return -1;
        } else {
            if (inter_8x8_cavlc(d, 1, 0) < 0) return -1;
            for (int i = 0; i < 4; i++) {
                if (d->sub_type[i] == 0) { if (!d->act_sps->direct_8x8_inference) no_sub_lt8 = 0; }
                else if (b_sub[d->sub_type[i]][0] > 1) no_sub_lt8 = 0;
            }
        }
    }

    if (!(d->mb_type & MB_I16)) {
        uint32_t c = br_ue(b);
        if (c > 47) return -1;
        d->cbp = (d->mb_type & MB_INTRA) ? h264_golomb_to_intra_cbp[c] : h264_golomb_to_inter_cbp[c];
        if ((d->cbp & 15) && d->p->transform_8x8 && (d->mb_type & MB_INTER) && no_sub_lt8)
            d->t8x8 = (int)br_bit(b);
    }
    if (d->cbp || (d->mb_type & MB_I16)) {
        int dq = br_se(b);
        if (dq < -26 || dq > 25) return -1;
        d->qp = (d->qp + dq + 52) % 52;
        d->qpc[0] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[0], 0, 51)];
        d->qpc[1] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[1], 0, 51)];
        if (cavlc_residual(d) < 0) return -1;
    }
    if (br_overrun(b)) return -1;
    return 0;
}
