/* AXIOM H.264 decoder: macroblock reconstruction (prediction + residual) and per-MB bookkeeping. */
#include "h264_int.h"

extern const uint8_t (*h264_b_sub)[4];
extern const uint8_t (*h264_p_sub)[4];

/* ---------------------------------------------------------------- inter prediction */
static void mc_part(h264dec *d, int x4, int y4, int w4, int h4) {
    const struct h264_slice *sl = &d->sl;
    int ci = CI(x4, y4);
    int r0 = d->ref_c[0][ci], r1 = sl->type == 1 ? d->ref_c[1][ci] : -1;
    int sy = d->stride[0], sc = d->stride[1];
    int px = d->mb_x * 16 + x4 * 4, py = d->mb_y * 16 + y4 * 4, w = w4 * 4, h = h4 * 4;
    int pw = d->mb_w * 16, ph = d->mb_h * 16;
    uint8_t *dy = d->cur->plane[0] + py * sy + px;
    uint8_t *du = d->cur->plane[1] + (py >> 1) * sc + (px >> 1);
    uint8_t *dv = d->cur->plane[2] + (py >> 1) * sc + (px >> 1);
    if (r0 < 0 && r1 < 0) r0 = 0;   /* corrupt data: never happens for valid streams */
    int list = r0 >= 0 ? 0 : 1, ref = r0 >= 0 ? r0 : r1;
    const struct h264_pic *p = sl->ref[list][ref];
    const int16_t *mv = d->mv_c[list][ci];
    h264_mc_luma(dy, sy, p, sy, pw, ph, px, py, mv[0], mv[1], w, h, d->edge_buf, d->mc_tmp16);
    h264_mc_chroma(du, sc, p->plane[1], sc, pw >> 1, ph >> 1, px >> 1, py >> 1, mv[0], mv[1], w >> 1, h >> 1, d->edge_buf);
    h264_mc_chroma(dv, sc, p->plane[2], sc, pw >> 1, ph >> 1, px >> 1, py >> 1, mv[0], mv[1], w >> 1, h >> 1, d->edge_buf);
    if (r0 >= 0 && r1 >= 0) {
        const struct h264_pic *q = sl->ref[1][r1];
        const int16_t *mv1 = d->mv_c[1][ci];
        uint8_t *ty = d->mc_tmp[0], *tu = d->mc_tmp[1], *tv = d->mc_tmp[1] + 8;
        h264_mc_luma(ty, 16, q, sy, pw, ph, px, py, mv1[0], mv1[1], w, h, d->edge_buf, d->mc_tmp16);
        h264_mc_chroma(tu, 16, q->plane[1], sc, pw >> 1, ph >> 1, px >> 1, py >> 1, mv1[0], mv1[1], w >> 1, h >> 1, d->edge_buf);
        h264_mc_chroma(tv, 16, q->plane[2], sc, pw >> 1, ph >> 1, px >> 1, py >> 1, mv1[0], mv1[1], w >> 1, h >> 1, d->edge_buf);
        if (sl->wp_mode == 0 || (sl->wp_mode == 1 && !sl->wflag[0][r0] && !sl->wflag[1][r1])) {
            h264_avg(dy, sy, ty, 16, w, h);
            h264_avg(du, sc, tu, 16, w >> 1, h >> 1);
            h264_avg(dv, sc, tv, 16, w >> 1, h >> 1);
        } else if (sl->wp_mode == 2) {
            int w1 = sl->implicit_w[r0][r1];
            h264_biweight(dy, sy, ty, 16, w, h, 5, 64 - w1, w1, 0);
            h264_biweight(du, sc, tu, 16, w >> 1, h >> 1, 5, 64 - w1, w1, 0);
            h264_biweight(dv, sc, tv, 16, w >> 1, h >> 1, 5, 64 - w1, w1, 0);
        } else {
            h264_biweight(dy, sy, ty, 16, w, h, sl->luma_log2_wd, sl->lw[0][r0], sl->lw[1][r1],
                          (sl->lo[0][r0] + sl->lo[1][r1] + 1) >> 1);
            h264_biweight(du, sc, tu, 16, w >> 1, h >> 1, sl->chroma_log2_wd, sl->cw[0][r0][0], sl->cw[1][r1][0],
                          (sl->co[0][r0][0] + sl->co[1][r1][0] + 1) >> 1);
            h264_biweight(dv, sc, tv, 16, w >> 1, h >> 1, sl->chroma_log2_wd, sl->cw[0][r0][1], sl->cw[1][r1][1],
                          (sl->co[0][r0][1] + sl->co[1][r1][1] + 1) >> 1);
        }
    } else if (sl->wp_mode == 1 && sl->wflag[list][ref]) {
        h264_weight(dy, sy, w, h, sl->luma_log2_wd, sl->lw[list][ref], sl->lo[list][ref]);
        h264_weight(du, sc, w >> 1, h >> 1, sl->chroma_log2_wd, sl->cw[list][ref][0], sl->co[list][ref][0]);
        h264_weight(dv, sc, w >> 1, h >> 1, sl->chroma_log2_wd, sl->cw[list][ref][1], sl->co[list][ref][1]);
    }
}

H264_INLINE int same4(const h264dec *d, int a, int b) {
    return d->ref_c[0][a] == d->ref_c[0][b] && d->ref_c[1][a] == d->ref_c[1][b] &&
           RD32(d->mv_c[0][a]) == RD32(d->mv_c[0][b]) && RD32(d->mv_c[1][a]) == RD32(d->mv_c[1][b]);
}

static int direct8_uniform(const h264dec *d, int b8) {
    int x = (b8 & 1) * 2, y = (b8 >> 1) * 2, a = CI(x, y);
    return same4(d, a, a + 1) && same4(d, a, a + 8) && same4(d, a, a + 9);
}

static void mc_direct8(h264dec *d, int b8) {
    int x = (b8 & 1) * 2, y = (b8 >> 1) * 2;
    if (direct8_uniform(d, b8)) mc_part(d, x, y, 2, 2);
    else for (int j = 0; j < 4; j++) mc_part(d, x + (j & 1), y + (j >> 1), 1, 1);
}

static void inter_pred(h264dec *d) {
    uint32_t t = d->mb_type;
    if (t & MB_16x16) mc_part(d, 0, 0, 4, 4);
    else if (t & MB_16x8) { mc_part(d, 0, 0, 4, 2); mc_part(d, 0, 2, 4, 2); }
    else if (t & MB_8x16) { mc_part(d, 0, 0, 2, 4); mc_part(d, 2, 0, 2, 4); }
    else if (t & MB_DIRECT) {
        int a = CI(0, 0);
        if (direct8_uniform(d, 0) && direct8_uniform(d, 1) && direct8_uniform(d, 2) && direct8_uniform(d, 3) &&
            same4(d, a, CI(2, 0)) && same4(d, a, CI(0, 2)) && same4(d, a, CI(2, 2)))
            mc_part(d, 0, 0, 4, 4);
        else for (int b8 = 0; b8 < 4; b8++) mc_direct8(d, b8);
    } else {
        int is_b = d->sl.type == 1;
        for (int b8 = 0; b8 < 4; b8++) {
            int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            if (is_b && d->sub_type[b8] == 0) { mc_direct8(d, b8); continue; }
            const uint8_t *s = is_b ? h264_b_sub[d->sub_type[b8]] : h264_p_sub[d->sub_type[b8]];
            int w = s[1], h = s[2];
            for (int j = 0; j < s[0]; j++) {
                int x = bx, y = by;
                if (s[0] == 4) { x += j & 1; y += j >> 1; }
                else if (s[0] == 2) { if (w == 2) y += j; else x += j; }
                mc_part(d, x, y, w, h);
            }
        }
    }
}

/* ---------------------------------------------------------------- lossless (transform bypass) */
static const int16_t *bypass_res;   /* unused helper marker */

/* adds an n x n residual (raster, int) to dst; dir: 0 vertical DPCM, 1 horizontal DPCM, else plain */
static void bypass_add(uint8_t *dst, int stride, int *r, int n, int dir) {
    if (dir == 0) {
        for (int y = 1; y < n; y++) for (int x = 0; x < n; x++) r[y * n + x] += r[(y - 1) * n + x];
    } else if (dir == 1) {
        for (int y = 0; y < n; y++) for (int x = 1; x < n; x++) r[y * n + x] += r[y * n + x - 1];
    }
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dst[y * stride + x] = h264_clip8(dst[y * stride + x] + r[y * n + x]);
}

static void bypass_block4(h264dec *d, uint8_t *dst, int stride, int16_t *blk, int dir) {
    int r[16];
    for (int i = 0; i < 16; i++) { r[i] = blk[i]; blk[i] = 0; }
    bypass_add(dst, stride, r, 4, dir);
}

static void bypass_block8(h264dec *d, uint8_t *dst, int stride, int16_t *blk, int dir) {
    int r[64];
    for (int i = 0; i < 64; i++) { r[i] = blk[i]; blk[i] = 0; }
    bypass_add(dst, stride, r, 8, dir);
}

/* ---------------------------------------------------------------- residual helpers */
static void luma_i16_residual(h264dec *d, uint8_t *y, int sy) {
    int16_t *coef = d->coef;
    if (d->bypass) {
        int r[256];
        for (int blk = 0; blk < 16; blk++) {
            int rr = h264_blk_xy[blk], bx = (rr & 3) * 4, by = (rr >> 2) * 4;
            int16_t *b = coef + blk * 16;
            if (d->cbf_cur & (1u << 24)) b[0] = d->dc_y[rr];
            for (int i = 0; i < 16; i++) { r[(by + (i >> 2)) * 16 + bx + (i & 3)] = b[i]; b[i] = 0; }
        }
        for (int i = 0; i < 16; i++) d->dc_y[i] = 0;
        int m = d->intra16_mode;
        bypass_add(y, sy, r, 16, m <= 1 ? m : 2);
        return;
    }
    if (d->cbf_cur & (1u << 24)) {
        h264_luma_dc_dequant(coef, d->dc_y, (int)d->dq4[0][d->qp][0]);
        for (int i = 0; i < 16; i++) d->dc_y[i] = 0;
    }
    for (int blk = 0; blk < 16; blk++) {
        int16_t *b = coef + blk * 16;
        int rr = h264_blk_xy[blk];
        uint8_t *dst = y + (rr >> 2) * 4 * sy + (rr & 3) * 4;
        if (d->nz_luma & (1 << blk)) h264_idct4_add(dst, sy, b);
        else if (b[0]) h264_idct4_dc_add(dst, sy, b);
    }
}

static void chroma_residual(h264dec *d, uint8_t *u, uint8_t *v, int sc) {
    int intra = (d->mb_type & MB_INTRA) != 0;
    for (int c = 0; c < 2; c++) {
        uint8_t *dst = c ? v : u;
        int16_t *coef = d->coefc[c];
        int hasdc = (d->cbf_cur >> (25 + c)) & 1;
        if (!hasdc && !((d->nz_chroma >> (4 * c)) & 15)) continue;
        if (d->bypass) {
            int r[64];
            for (int i = 0; i < 4; i++) {
                int16_t *b = coef + i * 16;
                if (hasdc) b[0] = d->dc_c[c][i];
                for (int k = 0; k < 16; k++) { r[((i >> 1) * 4 + (k >> 2)) * 8 + (i & 1) * 4 + (k & 3)] = b[k]; b[k] = 0; }
                d->dc_c[c][i] = 0;
            }
            int m = d->chroma_pred;
            bypass_add(dst, sc, r, 8, intra && m == 2 ? 0 : intra && m == 1 ? 1 : 2);
            continue;
        }
        if (hasdc) {
            h264_chroma_dc_dequant(coef, d->dc_c[c], (int)d->dq4[(intra ? 1 : 4) + c][d->qpc[c]][0]);
            d->dc_c[c][0] = d->dc_c[c][1] = d->dc_c[c][2] = d->dc_c[c][3] = 0;
        }
        for (int i = 0; i < 4; i++) {
            int16_t *b = coef + i * 16;
            uint8_t *p = dst + (i >> 1) * 4 * sc + (i & 1) * 4;
            if ((d->nz_chroma >> (4 * c + i)) & 1) h264_idct4_add(p, sc, b);
            else if (b[0]) h264_idct4_dc_add(p, sc, b);
        }
    }
}

static const uint8_t tr_avail4[16] = { 2, 2, 1, 0, 2, 3, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0 };  /* 0 no, 1 yes, 2 top MB, 3 top-right MB */

void h264_recon_mb(h264dec *d) {
    struct h264_pic *pic = d->cur;
    int sy = d->stride[0], sc = d->stride[1];
    uint8_t *y = pic->plane[0] + d->mb_y * 16 * sy + d->mb_x * 16;
    uint8_t *u = pic->plane[1] + d->mb_y * 8 * sc + d->mb_x * 8;
    uint8_t *v = pic->plane[2] + d->mb_y * 8 * sc + d->mb_x * 8;
    uint32_t t = d->mb_type;
    if (t & MB_PCM) {
        const uint8_t *s = (const uint8_t *)d->coef;
        for (int i = 0; i < 16; i++) memcpy(y + i * sy, s + i * 16, 16);
        for (int i = 0; i < 8; i++) memcpy(u + i * sc, s + 256 + i * 8, 8);
        for (int i = 0; i < 8; i++) memcpy(v + i * sc, s + 320 + i * 8, 8);
        memset(d->coef, 0, 384);
        return;
    }
    if (t & MB_INTRA) {
        h264_intra_chroma(u, sc, d->chroma_pred, d->ia_l, d->ia_t, d->ia_tl);
        h264_intra_chroma(v, sc, d->chroma_pred, d->ia_l, d->ia_t, d->ia_tl);
        if (t & MB_I16) {
            h264_intra16x16(y, sy, d->intra16_mode, d->ia_l, d->ia_t, d->ia_tl);
            luma_i16_residual(d, y, sy);
        } else if (t & MB_I4x4) {
            for (int blk = 0; blk < 16; blk++) {
                int r = h264_blk_xy[blk], bx = r & 3, by = r >> 2;
                uint8_t *dst = y + by * 4 * sy + bx * 4;
                int al = bx ? 1 : d->ia_l, at = by ? 1 : d->ia_t;
                int atl = bx ? (by ? 1 : d->ia_t) : (by ? d->ia_l : d->ia_tl);
                int tra = tr_avail4[blk];
                int atr = tra == 2 ? d->ia_t : tra == 3 ? d->ia_tr : tra;
                int mode = d->ipred4[blk];
                h264_intra4x4(dst, sy, mode, al, at, atr, atl);
                int16_t *b = d->coef + blk * 16;
                if (d->nz_luma & (1 << blk)) {
                    if (d->bypass) bypass_block4(d, dst, sy, b, mode <= 1 ? mode : 2);
                    else if (d->nnz_c[0][CI(bx, by)] == 1 && b[0]) h264_idct4_dc_add(dst, sy, b);
                    else h264_idct4_add(dst, sy, b);
                }
            }
        } else {
            for (int b8 = 0; b8 < 4; b8++) {
                int bx = b8 & 1, by = b8 >> 1;
                uint8_t *dst = y + by * 8 * sy + bx * 8;
                int al = bx ? 1 : d->ia_l, at = by ? 1 : d->ia_t;
                int atl = bx ? (by ? 1 : d->ia_t) : (by ? d->ia_l : d->ia_tl);
                int atr = b8 == 0 ? d->ia_t : b8 == 1 ? d->ia_tr : b8 == 2 ? 1 : 0;
                int mode = d->ipred4[b8 * 4];
                h264_intra8x8(dst, sy, mode, al, at, atr, atl);
                if (d->nz_luma & (15 << (b8 * 4))) {
                    if (d->bypass) bypass_block8(d, dst, sy, d->coef + b8 * 64, mode <= 1 ? mode : 2);
                    else h264_idct8_add(dst, sy, d->coef + b8 * 64);
                }
            }
        }
    } else {
        inter_pred(d);
        if (d->nz_luma) {
            if (d->t8x8) {
                for (int b8 = 0; b8 < 4; b8++) {
                    if (!(d->nz_luma & (15 << (b8 * 4)))) continue;
                    uint8_t *dst = y + (b8 >> 1) * 8 * sy + (b8 & 1) * 8;
                    if (d->bypass) bypass_block8(d, dst, sy, d->coef + b8 * 64, 2);
                    else h264_idct8_add(dst, sy, d->coef + b8 * 64);
                }
            } else {
                for (int blk = 0; blk < 16; blk++) {
                    if (!(d->nz_luma & (1 << blk))) continue;
                    int r = h264_blk_xy[blk];
                    uint8_t *dst = y + (r >> 2) * 4 * sy + (r & 3) * 4;
                    int16_t *b = d->coef + blk * 16;
                    if (d->bypass) bypass_block4(d, dst, sy, b, 2);
                    else if (d->nnz_c[0][CI(r & 3, r >> 2)] == 1 && b[0]) h264_idct4_dc_add(dst, sy, b);
                    else h264_idct4_add(dst, sy, b);
                }
            }
        }
    }
    if (d->cbp & 0x30) chroma_residual(d, u, v, sc);
    (void)bypass_res;
}

/* ---------------------------------------------------------------- bookkeeping */
void h264_save_mb(h264dec *d) {
    struct h264_mbinfo *m = &d->mbi[d->mb_xy];
    uint32_t t = d->mb_type;
    int pcm = (t & MB_PCM) != 0;
    m->type = t;
    m->slice = d->sl.slice_num;
    m->qp = (int8_t)(pcm ? 0 : d->qp);
    m->cbp = (uint8_t)(pcm ? 0x2f : d->cbp);
    m->cbf = pcm ? 0x7ffffffu : d->cbf_cur;
    m->nzdb = pcm ? 0xffff : d->nzdb_cur;
    m->t8x8 = (uint8_t)d->t8x8;
    m->chroma_pred = (uint8_t)(((t & MB_INTRA) && !pcm) ? d->chroma_pred : 0);
    m->direct8 = d->direct8_cur;
    {
        uint8_t me = 0;
        int infer = d->act_sps->direct_8x8_inference;
        if (t & MB_INTRA) me = 0;
        else if (t & MB_16x16) me = 0;
        else if (t & MB_16x8) me = 0x40;
        else if (t & MB_8x16) me = 0x04;
        else if (t & MB_DIRECT) me = infer ? 0x44 : 0xee;
        else {
            int small = 0;
            for (int i = 0; i < 4; i++) {
                int st = d->sub_type[i];
                if (d->sl.type == 1) { if (st == 0 ? !infer : h264_b_sub[st][0] > 1) small = 1; }
                else if (st) small = 1;
            }
            me = small ? 0xee : 0x44;
        }
        m->mvedges = me;
    }
    if (pcm) memset(m->nnz, 16, 24);
    else {
        const int8_t *nz = d->nnz_c[0];
        for (int y = 0; y < 4; y++) WR32(&m->nnz[y * 4], RD32(&nz[CI(0, y)]));
        for (int c = 0; c < 2; c++) {
            const int8_t *n = d->nnz_c[1 + c];
            m->nnz[16 + c * 4 + 0] = (uint8_t)n[CI(0, 0)]; m->nnz[16 + c * 4 + 1] = (uint8_t)n[CI(1, 0)];
            m->nnz[16 + c * 4 + 2] = (uint8_t)n[CI(0, 1)]; m->nnz[16 + c * 4 + 3] = (uint8_t)n[CI(1, 1)];
        }
    }
    if (t & (MB_I4x4 | MB_I8x8)) {
        for (int y = 0; y < 4; y++) WR32(&m->ipred[y * 4], RD32(&d->ipm_c[CI(0, y)]));
    } else memset(m->ipred, 2, 16);

    struct h264_pic *pic = d->cur;
    const int b4s = d->mb_w * 4, b8s = d->mb_w * 2;
    const int b4 = d->mb_y * 4 * b4s + d->mb_x * 4, b8 = d->mb_y * 2 * b8s + d->mb_x * 2;
    if (d->sl.type == 2 || (t & MB_INTRA)) {
        for (int li = 0; li < 2; li++) {
            for (int y = 0; y < 4; y++) memset(pic->mv[li] + b4 + y * b4s, 0, 16);
            pic->refidx[li][b8] = pic->refidx[li][b8 + 1] = pic->refidx[li][b8 + b8s] = pic->refidx[li][b8 + b8s + 1] = -1;
            pic->refuid[li][b8] = pic->refuid[li][b8 + 1] = pic->refuid[li][b8 + b8s] = pic->refuid[li][b8 + b8s + 1] = -1;
            if (d->p->cabac) memset(m->mvd[li], 0, 32);
        }
        return;
    }
    for (int li = 0; li < 2; li++) {
        const int16_t (*mc)[2] = (const int16_t (*)[2])d->mv_c[li];
        int16_t (*mv)[2] = pic->mv[li] + b4;
        for (int y = 0; y < 4; y++) memcpy(mv + y * b4s, mc[CI(0, y)], 16);
        for (int j = 0; j < 4; j++) {
            int r = d->ref_c[li][CI((j & 1) * 2, (j >> 1) * 2)];
            if (r < 0) r = -1;
            int o = b8 + (j >> 1) * b8s + (j & 1);
            pic->refidx[li][o] = (int8_t)r;
            pic->refuid[li][o] = r >= 0 ? d->sl.ref_uid[li][r] : -1;
        }
        if (d->p->cabac) {
            for (int y = 0; y < 4; y++) memcpy(m->mvd[li][y * 4], d->mvd_c[li][CI(0, y)], 8);
        }
    }
}

/* Error concealment: copy the co-located macroblock of src (or mid-grey) and mark it as not decoded. */
void h264_conceal_mb(h264dec *d, int xy, const struct h264_pic *src) {
    struct h264_pic *pic = d->cur;
    int mx = xy % d->mb_w, my = xy / d->mb_w;
    for (int c = 0; c < 3; c++) {
        int s = d->stride[c ? 1 : 0], n = c ? 8 : 16;
        uint8_t *dst = pic->plane[c] + my * n * s + mx * n;
        if (src && src != pic) {
            const uint8_t *sp = src->plane[c] + my * n * s + mx * n;
            for (int i = 0; i < n; i++) memcpy(dst + i * s, sp + i * s, (size_t)n);
        } else for (int i = 0; i < n; i++) memset(dst + i * s, 128, (size_t)n);
    }
    struct h264_mbinfo *m = &d->mbi[xy];
    memset(m, 0, sizeof(*m));
    m->type = MB_INTER | MB_SKIP | MB_16x16;
    m->slice = -1;
    memset(m->ipred, 2, 16);
    const int b4s = d->mb_w * 4, b8s = d->mb_w * 2;
    const int b4 = my * 4 * b4s + mx * 4, b8 = my * 2 * b8s + mx * 2;
    for (int li = 0; li < 2; li++) {
        for (int y = 0; y < 4; y++) memset(pic->mv[li] + b4 + y * b4s, 0, 16);
        pic->refidx[li][b8] = pic->refidx[li][b8 + 1] = pic->refidx[li][b8 + b8s] = pic->refidx[li][b8 + b8s + 1] = -1;
        pic->refuid[li][b8] = pic->refuid[li][b8 + 1] = pic->refuid[li][b8 + b8s] = pic->refuid[li][b8 + b8s + 1] = -1;
    }
    d->concealed++;
}
