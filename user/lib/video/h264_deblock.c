/* AXIOM H.264 decoder: deblocking filter (8.7) and reference picture padding. */
#include "h264_int.h"

H264_INLINE int mvdiff(const int16_t *a, const int16_t *b) {
    return h264_abs(a[0] - b[0]) >= 4 || h264_abs(a[1] - b[1]) >= 4;
}

/* bS 1/0 from motion for inter blocks p and q (picture 4x4 / 8x8 indices) */
static int mv_bs(const struct h264_pic *pic, int q4, int q8, int p4, int p8) {
    int32_t rq0 = pic->refuid[0][q8], rq1 = pic->refuid[1][q8];
    int32_t rp0 = pic->refuid[0][p8], rp1 = pic->refuid[1][p8];
    int nq = (rq0 >= 0) + (rq1 >= 0), np = (rp0 >= 0) + (rp1 >= 0);
    if (nq != np) return 1;
    const int16_t *q0 = pic->mv[0][q4], *q1 = pic->mv[1][q4], *p0 = pic->mv[0][p4], *p1 = pic->mv[1][p4];
    if (nq == 1) {
        int32_t rq = rq0 >= 0 ? rq0 : rq1, rp = rp0 >= 0 ? rp0 : rp1;
        if (rq != rp) return 1;
        return mvdiff(rq0 >= 0 ? q0 : q1, rp0 >= 0 ? p0 : p1);
    }
    if (nq == 0) return 0;
    if (!((rp0 == rq0 && rp1 == rq1) || (rp0 == rq1 && rp1 == rq0))) return 1;
    if (rp0 != rp1) {
        if (rp0 == rq0) return mvdiff(p0, q0) || mvdiff(p1, q1);
        return mvdiff(p0, q1) || mvdiff(p1, q0);
    }
    return (mvdiff(p0, q0) || mvdiff(p1, q1)) && (mvdiff(p0, q1) || mvdiff(p1, q0));
}

/* filters one 4-sample segment of a luma edge; pix points at q0 of the first line, xs = step across the
   edge, ys = step along the edge */
static void filter_luma(uint8_t *pix, int xs, int ys, int bs, int alpha, int beta, int tc0) {
    for (int i = 0; i < 4; i++, pix += ys) {
        int p0 = pix[-xs], p1 = pix[-2 * xs], p2 = pix[-3 * xs];
        int q0 = pix[0], q1 = pix[xs], q2 = pix[2 * xs];
        if (h264_abs(p0 - q0) >= alpha || h264_abs(p1 - p0) >= beta || h264_abs(q1 - q0) >= beta) continue;
        int ap = h264_abs(p2 - p0), aq = h264_abs(q2 - q0);
        if (bs < 4) {
            int tc = tc0 + (ap < beta) + (aq < beta);
            int delta = h264_clip((((q0 - p0) * 4) + (p1 - q1) + 4) >> 3, -tc, tc);
            pix[-xs] = h264_clip8(p0 + delta);
            pix[0] = h264_clip8(q0 - delta);
            if (ap < beta) pix[-2 * xs] = (uint8_t)(p1 + h264_clip((p2 + ((p0 + q0 + 1) >> 1) - (p1 * 2)) >> 1, -tc0, tc0));
            if (aq < beta) pix[xs] = (uint8_t)(q1 + h264_clip((q2 + ((p0 + q0 + 1) >> 1) - (q1 * 2)) >> 1, -tc0, tc0));
        } else {
            int small = h264_abs(p0 - q0) < ((alpha >> 2) + 2);
            if (ap < beta && small) {
                int p3 = pix[-4 * xs];
                pix[-xs] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                pix[-2 * xs] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                pix[-3 * xs] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            if (aq < beta && small) {
                int q3 = pix[3 * xs];
                pix[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                pix[xs] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                pix[2 * xs] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

/* one 2-sample segment of a chroma edge */
static void filter_chroma(uint8_t *pix, int xs, int ys, int bs, int alpha, int beta, int tc0) {
    for (int i = 0; i < 2; i++, pix += ys) {
        int p0 = pix[-xs], p1 = pix[-2 * xs], q0 = pix[0], q1 = pix[xs];
        if (h264_abs(p0 - q0) >= alpha || h264_abs(p1 - p0) >= beta || h264_abs(q1 - q0) >= beta) continue;
        if (bs < 4) {
            int tc = tc0 + 1;
            int delta = h264_clip((((q0 - p0) * 4) + (p1 - q1) + 4) >> 3, -tc, tc);
            pix[-xs] = h264_clip8(p0 + delta);
            pix[0] = h264_clip8(q0 - delta);
        } else {
            pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void deblock_mb(h264dec *d, int mx, int my) {
    const int mbw = d->mb_w, xy = my * mbw + mx;
    const struct h264_mbinfo *m = &d->mbi[xy];
    if (m->slice < 0) return;
    const struct h264_slice_info *si = &d->slices[m->slice];
    if (si->deblock_idc == 1) return;
    const struct h264_pic *pic = d->cur;
    const int b4s = mbw * 4, b8s = mbw * 2;
    const int b4 = my * 4 * b4s + mx * 4, b8 = my * 2 * b8s + mx * 2;
    int sy = d->stride[0], sc = d->stride[1];
    uint8_t *py = pic->plane[0] + my * 16 * sy + mx * 16;
    uint8_t *pu = pic->plane[1] + my * 8 * sc + mx * 8;
    uint8_t *pv = pic->plane[2] + my * 8 * sc + mx * 8;
    int qintra = (m->type & MB_INTRA) != 0;

    for (int dir = 0; dir < 2; dir++) {
        const struct h264_mbinfo *n = 0;
        int filter_edge0;
        if (dir == 0) { filter_edge0 = mx > 0; if (filter_edge0) n = &d->mbi[xy - 1]; }
        else { filter_edge0 = my > 0; if (filter_edge0) n = &d->mbi[xy - mbw]; }
        if (n && (n->slice < 0 || (si->deblock_idc == 2 && n->slice != m->slice))) filter_edge0 = 0;
        int bs[4][4];
        for (int e = 0; e < 4; e++) {
            const struct h264_mbinfo *pm = e ? m : n;
            if (e == 0 && !filter_edge0) { bs[e][0] = bs[e][1] = bs[e][2] = bs[e][3] = 0; continue; }
            if (e && m->t8x8 && (e & 1)) { bs[e][0] = bs[e][1] = bs[e][2] = bs[e][3] = 0; continue; }
            int pintra = (pm->type & MB_INTRA) != 0;
            if (qintra || pintra) { bs[e][0] = bs[e][1] = bs[e][2] = bs[e][3] = e ? 3 : 4; continue; }
            for (int k = 0; k < 4; k++) {
                int qx = dir ? k : e, qy = dir ? e : k;
                int px = dir ? k : e - 1, pyy = dir ? e - 1 : k;
                int pbit = (px & 3) + (pyy & 3) * 4;
                if (((m->nzdb >> (qy * 4 + qx)) & 1) || ((pm->nzdb >> pbit) & 1)) { bs[e][k] = 2; continue; }
                int q4 = b4 + qy * b4s + qx, q8i = b8 + (qy >> 1) * b8s + (qx >> 1);
                int p4 = b4 + pyy * b4s + px, p8i = b8 + (pyy >> 1) * b8s + (px >> 1);
                if (pyy < 0) p8i = b8 - b8s + (px >> 1);
                if (px < 0) p8i = b8 + (pyy >> 1) * b8s - 1;
                bs[e][k] = mv_bs(pic, q4, q8i, p4, p8i);
            }
        }
        /* luma */
        for (int e = 0; e < 4; e++) {
            if (!(bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3])) continue;
            const struct h264_mbinfo *pm = e ? m : n;
            int qpav = (m->qp + pm->qp + 1) >> 1;
            int ia = h264_clip(qpav + si->alpha_off, 0, 51), ib = h264_clip(qpav + si->beta_off, 0, 51);
            int alpha = h264_alpha[ia], beta = h264_beta[ib];
            if (!alpha || !beta) continue;
            for (int k = 0; k < 4; k++) {
                if (!bs[e][k]) continue;
                uint8_t *pix = dir ? py + e * 4 * sy + k * 4 : py + k * 4 * sy + e * 4;
                int tc0 = bs[e][k] < 4 ? h264_tc0[ia][bs[e][k] - 1] : 0;
                filter_luma(pix, dir ? sy : 1, dir ? 1 : sy, bs[e][k], alpha, beta, tc0);
            }
        }
        /* chroma: edges 0 and 2 (luma units) */
        for (int e = 0; e < 4; e += 2) {
            if (!(bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3])) continue;
            const struct h264_mbinfo *pm = e ? m : n;
            for (int c = 0; c < 2; c++) {
                int off = si->cqp_offset[c];
                int qq = h264_chroma_qp[h264_clip(m->qp + off, 0, 51)];
                int qp = h264_chroma_qp[h264_clip(pm->qp + off, 0, 51)];
                int qpav = (qq + qp + 1) >> 1;
                int ia = h264_clip(qpav + si->alpha_off, 0, 51), ib = h264_clip(qpav + si->beta_off, 0, 51);
                int alpha = h264_alpha[ia], beta = h264_beta[ib];
                if (!alpha || !beta) continue;
                uint8_t *base = c ? pv : pu;
                for (int k = 0; k < 4; k++) {
                    if (!bs[e][k]) continue;
                    uint8_t *pix = dir ? base + (e * 2) * sc + k * 2 : base + k * 2 * sc + e * 2;
                    int tc0 = bs[e][k] < 4 ? h264_tc0[ia][bs[e][k] - 1] : 0;
                    filter_chroma(pix, dir ? sc : 1, dir ? 1 : sc, bs[e][k], alpha, beta, tc0);
                }
            }
        }
    }
}

void h264_deblock_picture(h264dec *d) {
    for (int my = 0; my < d->mb_h; my++)
        for (int mx = 0; mx < d->mb_w; mx++) deblock_mb(d, mx, my);
}

void h264_pad_picture(h264dec *d, struct h264_pic *p) {
    for (int c = 0; c < 3; c++) {
        int s = d->stride[c ? 1 : 0], pad = c ? H264_PADC : H264_PAD;
        int w = d->mb_w * (c ? 8 : 16), h = d->mb_h * (c ? 8 : 16);
        uint8_t *pl = p->plane[c];
        for (int y = 0; y < h; y++) {
            uint8_t *row = pl + (ptrdiff_t)y * s;
            memset(row - pad, row[0], (size_t)pad);
            memset(row + w, row[w - 1], (size_t)pad);
        }
        for (int y = 1; y <= pad; y++) {
            memcpy(pl - (ptrdiff_t)y * s - pad, pl - pad, (size_t)(w + 2 * pad));
            memcpy(pl + (ptrdiff_t)(h - 1 + y) * s - pad, pl + (ptrdiff_t)(h - 1) * s - pad, (size_t)(w + 2 * pad));
        }
    }
}
