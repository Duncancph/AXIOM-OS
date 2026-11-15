/* AXIOM H.264 decoder: neighbour context cache, motion vector prediction, P_Skip and direct modes. */
#include "h264_int.h"

/* Loads the neighbour data of the current macroblock (d->mb_x, d->mb_y) into the context caches. */
void h264_fill_cache(h264dec *d) {
    const int mbw = d->mb_w, xy = d->mb_xy, sn = d->sl.slice_num;
    const struct h264_mbinfo *mbi = d->mbi;
    const int cip = d->p->constrained_intra;
    int l = d->mb_x > 0 && mbi[xy - 1].slice == sn;
    int t = d->mb_y > 0 && mbi[xy - mbw].slice == sn;
    int tr = d->mb_y > 0 && d->mb_x < mbw - 1 && mbi[xy - mbw + 1].slice == sn;
    int tl = d->mb_y > 0 && d->mb_x > 0 && mbi[xy - mbw - 1].slice == sn;
    d->avail_l = l; d->avail_t = t; d->avail_tr = tr; d->avail_tl = tl;
    const struct h264_mbinfo *L = l ? &mbi[xy - 1] : 0, *T = t ? &mbi[xy - mbw] : 0;
    d->left_type = l ? (int)L->type : 0;
    d->top_type = t ? (int)T->type : 0;
    d->left_cbp = l ? L->cbp : 0;
    d->top_cbp = t ? T->cbp : 0;
    d->direct8_l = l ? L->direct8 : 0;
    d->direct8_t = t ? T->direct8 : 0;
    d->ia_l = l && (!cip || (L->type & MB_INTRA));
    d->ia_t = t && (!cip || (T->type & MB_INTRA));
    d->ia_tr = tr && (!cip || (mbi[xy - mbw + 1].type & MB_INTRA));
    d->ia_tl = tl && (!cip || (mbi[xy - mbw - 1].type & MB_INTRA));

    /* total_coeff (CAVLC nC) and intra modes */
    int8_t *nz = d->nnz_c[0], *ncb = d->nnz_c[1], *ncr = d->nnz_c[2];
    for (int i = 0; i < 4; i++) {
        nz[CI(i, -1)] = t ? (int8_t)T->nnz[12 + i] : -1;
        nz[CI(-1, i)] = l ? (int8_t)L->nnz[i * 4 + 3] : -1;
    }
    for (int i = 0; i < 2; i++) {
        ncb[CI(i, -1)] = t ? (int8_t)T->nnz[16 + 2 + i] : -1;
        ncr[CI(i, -1)] = t ? (int8_t)T->nnz[20 + 2 + i] : -1;
        ncb[CI(-1, i)] = l ? (int8_t)L->nnz[16 + i * 2 + 1] : -1;
        ncr[CI(-1, i)] = l ? (int8_t)L->nnz[20 + i * 2 + 1] : -1;
    }
    for (int y = 0; y < 4; y++) WR32(&nz[CI(0, y)], 0);
    WR16(&ncb[CI(0, 0)], 0); WR16(&ncb[CI(0, 1)], 0);
    WR16(&ncr[CI(0, 0)], 0); WR16(&ncr[CI(0, 1)], 0);

    int8_t *ipm = d->ipm_c;
    for (int i = 0; i < 4; i++) {
        ipm[CI(i, -1)] = (!t || (cip && !(T->type & MB_INTRA))) ? -1 : T->ipred[12 + i];
        ipm[CI(-1, i)] = (!l || (cip && !(L->type & MB_INTRA))) ? -1 : L->ipred[i * 4 + 3];
    }

    if (d->sl.type == 2) return;

    /* motion: refs, mvs, mvds */
    const struct h264_pic *cur = d->cur;
    const int b4s = mbw * 4, b8s = mbw * 2;
    const int b4 = d->mb_y * 4 * b4s + d->mb_x * 4, b8 = d->mb_y * 2 * b8s + d->mb_x * 2;
    int lists = d->sl.type == 1 ? 2 : 1;
    for (int li = 0; li < lists; li++) {
        int8_t *rc = d->ref_c[li];
        int16_t (*mc)[2] = d->mv_c[li];
        uint8_t (*md)[2] = d->mvd_c[li];
        const int8_t *rf = cur->refidx[li];
        const int16_t (*mv)[2] = (const int16_t (*)[2])cur->mv[li];
        if (t) {
            const int16_t (*s)[2] = mv + b4 - b4s;
            for (int i = 0; i < 4; i++) WR32(mc[CI(i, -1)], RD32(s[i]));
            rc[CI(0, -1)] = rc[CI(1, -1)] = rf[b8 - b8s];
            rc[CI(2, -1)] = rc[CI(3, -1)] = rf[b8 - b8s + 1];
            for (int i = 0; i < 4; i++) WR16(md[CI(i, -1)], RD16(T->mvd[li][12 + i]));
        } else {
            for (int i = 0; i < 4; i++) { WR32(mc[CI(i, -1)], 0); WR16(md[CI(i, -1)], 0); rc[CI(i, -1)] = -2; }
        }
        if (tr) {
            WR32(mc[CI(4, -1)], RD32(mv[b4 - b4s + 4]));
            rc[CI(4, -1)] = rf[b8 - b8s + 2];
        } else {
            WR32(mc[CI(4, -1)], 0);
            rc[CI(4, -1)] = -2;
        }
        if (tl) {
            WR32(mc[CI(-1, -1)], RD32(mv[b4 - b4s - 1]));
            rc[CI(-1, -1)] = rf[b8 - b8s - 1];
        } else {
            WR32(mc[CI(-1, -1)], 0);
            rc[CI(-1, -1)] = -2;
        }
        if (l) {
            for (int i = 0; i < 4; i++) {
                WR32(mc[CI(-1, i)], RD32(mv[b4 + i * b4s - 1]));
                WR16(md[CI(-1, i)], RD16(L->mvd[li][i * 4 + 3]));
            }
            rc[CI(-1, 0)] = rc[CI(-1, 1)] = rf[b8 - 1];
            rc[CI(-1, 2)] = rc[CI(-1, 3)] = rf[b8 + b8s - 1];
        } else {
            for (int i = 0; i < 4; i++) { WR32(mc[CI(-1, i)], 0); WR16(md[CI(-1, i)], 0); rc[CI(-1, i)] = -2; }
        }
    }
}

/* Writes ref/mv for a w x h (4x4 units) block at (x, y) into the cache. */
void h264_fill_part(h264dec *d, int list, int x, int y, int w, int h, int ref, int mx, int my) {
    int8_t *rc = d->ref_c[list];
    int16_t (*mc)[2] = d->mv_c[list];
    uint32_t mv = (uint16_t)mx | (uint32_t)(uint16_t)my << 16;
    for (int j = 0; j < h; j++) {
        int base = CI(x, y + j);
        for (int i = 0; i < w; i++) {
            rc[base + i] = (int8_t)ref;
            WR32(mc[base + i], mv);
        }
    }
}

/* 8.4.1.3: motion vector predictor for the partition at (x, y), size w x h (4x4 units). */
void h264_mv_pred(h264dec *d, int list, int x, int y, int w, int h, int ref, int16_t mvp[2]) {
    const int8_t *rc = d->ref_c[list];
    const int16_t (*mc)[2] = (const int16_t (*)[2])d->mv_c[list];
    int ia = CI(x - 1, y), ib = CI(x, y - 1), ic;
    int cx = x + w, cy = y - 1;
    int refc;
    if (cy < 0) {
        ic = CI(cx, -1);
        refc = rc[ic];
    } else if (cx >= 4) {
        refc = -2;
        ic = 0;
    } else {
        int b8c = (cy >> 1) * 2 + (cx >> 1), b8 = (y >> 1) * 2 + (x >> 1);
        if (b8c <= b8) { ic = CI(cx, cy); refc = rc[ic]; }
        else { refc = -2; ic = 0; }
    }
    if (refc == -2) {
        ic = CI(x - 1, y - 1);
        refc = rc[ic];
    }
    int refa = rc[ia], refb = rc[ib];
    if (w == 4 && h == 2) {
        if (y == 0) { if (refb == ref) { mvp[0] = mc[ib][0]; mvp[1] = mc[ib][1]; return; } }
        else if (refa == ref) { mvp[0] = mc[ia][0]; mvp[1] = mc[ia][1]; return; }
    } else if (w == 2 && h == 4) {
        if (x == 0) { if (refa == ref) { mvp[0] = mc[ia][0]; mvp[1] = mc[ia][1]; return; } }
        else if (refc == ref) { mvp[0] = mc[ic][0]; mvp[1] = mc[ic][1]; return; }
    }
    int match = (refa == ref) + (refb == ref) + (refc == ref);
    if (match == 1) {
        int i = refa == ref ? ia : refb == ref ? ib : ic;
        mvp[0] = mc[i][0]; mvp[1] = mc[i][1];
        return;
    }
    if (match == 0 && refb == -2 && refc == -2 && refa != -2) {
        mvp[0] = mc[ia][0]; mvp[1] = mc[ia][1];
        return;
    }
    mvp[0] = (int16_t)h264_median(mc[ia][0], mc[ib][0], mc[ic][0]);
    mvp[1] = (int16_t)h264_median(mc[ia][1], mc[ib][1], mc[ic][1]);
}

/* 8.4.1.1 */
void h264_pskip_mv(h264dec *d) {
    const int8_t *rc = d->ref_c[0];
    const int16_t (*mc)[2] = (const int16_t (*)[2])d->mv_c[0];
    int ia = CI(-1, 0), ib = CI(0, -1);
    int16_t mvp[2] = { 0, 0 };
    if (rc[ia] != -2 && rc[ib] != -2 &&
        !(rc[ia] == 0 && RD32(mc[ia]) == 0) && !(rc[ib] == 0 && RD32(mc[ib]) == 0))
        h264_mv_pred(d, 0, 0, 0, 4, 4, 0, mvp);
    h264_fill_part(d, 0, 0, 0, 4, 4, 0, mvp[0], mvp[1]);
}

/* Per-slice preparation for temporal direct: DistScaleFactor per refIdxL0. */
void h264_direct_prepare(h264dec *d) {
    struct h264_slice *sl = &d->sl;
    int poc1 = sl->ref_poc[1][0];
    for (int i = 0; i < sl->num_ref[0]; i++) {
        int poc0 = sl->ref_poc[0][i];
        int tb = h264_clip(d->cur->poc - poc0, -128, 127);
        int td = h264_clip(poc1 - poc0, -128, 127);
        if (sl->ref_long[0][i] || td == 0) { sl->dist_scale[i] = 9999; continue; }
        int tx = (16384 + h264_abs(td / 2)) / td;
        sl->dist_scale[i] = h264_clip((tb * tx + 32) >> 6, -1024, 1023);
    }
}

H264_INLINE int min_positive(int a, int b) { return (a >= 0 && b >= 0) ? h264_min(a, b) : h264_max(a, b); }

/* Direct prediction (8.4.1.2) for the 8x8 blocks set in mask: fills the ref/mv caches. */
void h264_direct(h264dec *d, int mask) {
    struct h264_slice *sl = &d->sl;
    const struct h264_pic *col = sl->ref[1][0];
    const int b4s = d->mb_w * 4, b8s = d->mb_w * 2;
    const int b4 = d->mb_y * 4 * b4s + d->mb_x * 4, b8 = d->mb_y * 2 * b8s + d->mb_x * 2;
    const int infer = d->act_sps->direct_8x8_inference;
    const int8_t *cr0 = col->refidx[0], *cr1 = col->refidx[1];
    const int16_t (*cm0)[2] = (const int16_t (*)[2])col->mv[0], (*cm1)[2] = (const int16_t (*)[2])col->mv[1];

    if (sl->direct_spatial) {
        int ref[2];
        int16_t mvp[2][2] = { { 0, 0 }, { 0, 0 } };
        for (int li = 0; li < 2; li++) {
            const int8_t *rc = d->ref_c[li];
            int ra = rc[CI(-1, 0)], rb = rc[CI(0, -1)], rcc = rc[CI(4, -1)];
            if (rcc == -2) rcc = rc[CI(-1, -1)];
            ref[li] = min_positive(ra, min_positive(rb, rcc));
            if (ref[li] < 0) ref[li] = -1;
        }
        int zero = 0;
        if (ref[0] < 0 && ref[1] < 0) { ref[0] = ref[1] = 0; zero = 1; }
        if (!zero) {
            for (int li = 0; li < 2; li++)
                if (ref[li] >= 0) h264_mv_pred(d, li, 0, 0, 4, 4, ref[li], mvp[li]);
        }
        int col_short = !sl->col_long;
        for (int b = 0; b < 4; b++) {
            if (!(mask & (1 << b))) continue;
            int bx = (b & 1) * 2, by = (b >> 1) * 2;
            int c8 = b8 + (by >> 1) * b8s + (bx >> 1);
            int rcol = cr0[c8] >= 0 ? cr0[c8] : cr1[c8];
            const int16_t (*cmv)[2] = cr0[c8] >= 0 ? cm0 : cm1;
            for (int li = 0; li < 2; li++) {
                int8_t *rc = d->ref_c[li];
                for (int j = 0; j < 2; j++) { rc[CI(bx, by + j)] = (int8_t)ref[li]; rc[CI(bx + 1, by + j)] = (int8_t)ref[li]; }
            }
            for (int j = 0; j < 4; j++) {
                int x = bx + (j & 1), y = by + (j >> 1);
                int cx = infer ? (b & 1) * 3 : x, cy = infer ? (b >> 1) * 3 : y;
                const int16_t *m = cmv[b4 + cy * b4s + cx];
                int colzero = col_short && rcol == 0 && m[0] >= -1 && m[0] <= 1 && m[1] >= -1 && m[1] <= 1;
                for (int li = 0; li < 2; li++) {
                    uint32_t v = 0;
                    if (!zero && ref[li] >= 0 && !(ref[li] == 0 && colzero))
                        v = (uint16_t)mvp[li][0] | (uint32_t)(uint16_t)mvp[li][1] << 16;
                    WR32(d->mv_c[li][CI(x, y)], v);
                }
            }
        }
    } else {
        for (int b = 0; b < 4; b++) {
            if (!(mask & (1 << b))) continue;
            int bx = (b & 1) * 2, by = (b >> 1) * 2;
            int c8 = b8 + (by >> 1) * b8s + (bx >> 1);
            int use1 = cr0[c8] < 0;
            int rcol = use1 ? cr1[c8] : cr0[c8];
            const int16_t (*cmv)[2] = use1 ? cm1 : cm0;
            int ref0 = 0;
            if (rcol >= 0) {
                int32_t uid = (use1 ? col->refuid[1] : col->refuid[0])[c8];
                for (int i = 0; i < sl->num_ref[0]; i++)
                    if (sl->ref_uid[0][i] == uid) { ref0 = i; break; }
            }
            int dsf = sl->dist_scale[ref0];
            for (int j = 0; j < 2; j++) {
                d->ref_c[0][CI(bx, by + j)] = d->ref_c[0][CI(bx + 1, by + j)] = (int8_t)ref0;
                d->ref_c[1][CI(bx, by + j)] = d->ref_c[1][CI(bx + 1, by + j)] = 0;
            }
            for (int j = 0; j < 4; j++) {
                int x = bx + (j & 1), y = by + (j >> 1);
                int cx = infer ? (b & 1) * 3 : x, cy = infer ? (b >> 1) * 3 : y;
                int mx = 0, my = 0;
                if (rcol >= 0) { mx = cmv[b4 + cy * b4s + cx][0]; my = cmv[b4 + cy * b4s + cx][1]; }
                int m0x, m0y, m1x, m1y;
                if (dsf == 9999) { m0x = mx; m0y = my; m1x = 0; m1y = 0; }
                else {
                    m0x = (dsf * mx + 128) >> 8; m0y = (dsf * my + 128) >> 8;
                    m1x = m0x - mx; m1y = m0y - my;
                }
                WR32(d->mv_c[0][CI(x, y)], (uint16_t)m0x | (uint32_t)(uint16_t)m0y << 16);
                WR32(d->mv_c[1][CI(x, y)], (uint16_t)m1x | (uint32_t)(uint16_t)m1y << 16);
            }
        }
    }
}
