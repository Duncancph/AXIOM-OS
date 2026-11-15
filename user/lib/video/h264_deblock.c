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
#ifdef H264_NO_SIMD
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
#endif

#ifndef H264_NO_SIMD
#include <emmintrin.h>

/* ---------------------------------------------------------------- SSE2 edge filters.
   rows[0..7] = p3 p2 p1 p0 q0 q1 q2 q3, 16 lanes each. tc0: per lane, -1 = do not filter. */
H264_INLINE __m128i absdiff8(__m128i a, __m128i b) { return _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a)); }
H264_INLINE __m128i lt8(__m128i v, __m128i lim) {   /* v < lim (unsigned bytes, lim >= 1) -> 0xff */
    return _mm_cmpeq_epi8(_mm_subs_epu8(v, _mm_sub_epi8(lim, _mm_set1_epi8(1))), _mm_setzero_si128());
}
H264_INLINE __m128i blend(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }

static void luma_filter_simd(__m128i *r, __m128i alpha, __m128i beta, __m128i tc0, int strong) {
    const __m128i z = _mm_setzero_si128();
    __m128i p3 = r[0], p2 = r[1], p1 = r[2], p0 = r[3], q0 = r[4], q1 = r[5], q2 = r[6], q3 = r[7];
    __m128i apq = absdiff8(p0, q0);
    __m128i m = _mm_and_si128(_mm_and_si128(lt8(apq, alpha), lt8(absdiff8(p1, p0), beta)), lt8(absdiff8(q1, q0), beta));
    m = _mm_and_si128(m, _mm_cmpgt_epi8(tc0, _mm_set1_epi8(-1)));
    if (_mm_movemask_epi8(m) == 0) return;
    __m128i ap = lt8(absdiff8(p2, p0), beta), aq = lt8(absdiff8(q2, q0), beta);
    if (!strong) {
        __m128i out[4];
        for (int h = 0; h < 2; h++) {
            __m128i P2, P1, P0, Q0, Q1, Q2, M, AP, AQ, TC0;
#define UNP(x) (h ? _mm_unpackhi_epi8(x, z) : _mm_unpacklo_epi8(x, z))
#define UNPS(x) (h ? _mm_unpackhi_epi8(x, x) : _mm_unpacklo_epi8(x, x))
            P2 = UNP(p2); P1 = UNP(p1); P0 = UNP(p0); Q0 = UNP(q0); Q1 = UNP(q1); Q2 = UNP(q2);
            M = UNPS(m); AP = _mm_and_si128(UNPS(ap), M); AQ = _mm_and_si128(UNPS(aq), M);
            TC0 = _mm_srai_epi16(UNPS(tc0), 8);
            __m128i tc = _mm_sub_epi16(_mm_sub_epi16(TC0, AP), AQ);
            __m128i dl = _mm_add_epi16(_mm_slli_epi16(_mm_sub_epi16(Q0, P0), 2), _mm_sub_epi16(P1, Q1));
            dl = _mm_srai_epi16(_mm_add_epi16(dl, _mm_set1_epi16(4)), 3);
            dl = _mm_min_epi16(_mm_max_epi16(dl, _mm_sub_epi16(z, tc)), tc);
            dl = _mm_and_si128(dl, M);
            __m128i avg = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(P0, Q0), _mm_set1_epi16(1)), 1);
            __m128i ntc0 = _mm_sub_epi16(z, TC0);
            __m128i dp = _mm_srai_epi16(_mm_sub_epi16(_mm_add_epi16(P2, avg), _mm_slli_epi16(P1, 1)), 1);
            dp = _mm_and_si128(_mm_min_epi16(_mm_max_epi16(dp, ntc0), TC0), AP);
            __m128i dq = _mm_srai_epi16(_mm_sub_epi16(_mm_add_epi16(Q2, avg), _mm_slli_epi16(Q1, 1)), 1);
            dq = _mm_and_si128(_mm_min_epi16(_mm_max_epi16(dq, ntc0), TC0), AQ);
            if (!h) {
                out[0] = _mm_add_epi16(P1, dp); out[1] = _mm_add_epi16(P0, dl);
                out[2] = _mm_sub_epi16(Q0, dl); out[3] = _mm_add_epi16(Q1, dq);
            } else {
                r[2] = _mm_packus_epi16(out[0], _mm_add_epi16(P1, dp));
                r[3] = _mm_packus_epi16(out[1], _mm_add_epi16(P0, dl));
                r[4] = _mm_packus_epi16(out[2], _mm_sub_epi16(Q0, dl));
                r[5] = _mm_packus_epi16(out[3], _mm_add_epi16(Q1, dq));
            }
        }
        return;
    }
    /* bS == 4 */
    __m128i small = lt8(apq, _mm_add_epi8(_mm_srli_epi16(_mm_and_si128(alpha, _mm_set1_epi8((char)0xfc)), 2), _mm_set1_epi8(2)));
    __m128i sp = _mm_and_si128(_mm_and_si128(ap, small), m), sq = _mm_and_si128(_mm_and_si128(aq, small), m);
    __m128i res[6][2];
    for (int h = 0; h < 2; h++) {
        __m128i P3 = UNP(p3), P2 = UNP(p2), P1 = UNP(p1), P0 = UNP(p0), Q0 = UNP(q0), Q1 = UNP(q1), Q2 = UNP(q2), Q3 = UNP(q3);
        const __m128i two = _mm_set1_epi16(2), four = _mm_set1_epi16(4);
        __m128i pq = _mm_add_epi16(P0, Q0);
        __m128i s1 = _mm_add_epi16(_mm_add_epi16(P1, pq), P2);                 /* p2 + p1 + p0 + q0 */
        __m128i P0s = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(s1, P1), _mm_add_epi16(pq, Q1)), four), 3);
        __m128i P1s = _mm_srli_epi16(_mm_add_epi16(s1, two), 2);
        __m128i P2s = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(_mm_add_epi16(P3, P2), 1), s1), four), 3);
        __m128i P0w = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(P1, 1), _mm_add_epi16(P0, Q1)), two), 2);
        __m128i t1 = _mm_add_epi16(_mm_add_epi16(Q1, pq), Q2);                 /* q2 + q1 + q0 + p0 */
        __m128i Q0s = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(t1, Q1), _mm_add_epi16(pq, P1)), four), 3);
        __m128i Q1s = _mm_srli_epi16(_mm_add_epi16(t1, two), 2);
        __m128i Q2s = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(_mm_add_epi16(Q3, Q2), 1), t1), four), 3);
        __m128i Q0w = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(Q1, 1), _mm_add_epi16(Q0, P1)), two), 2);
        res[0][h] = P2s; res[1][h] = P1s; res[2][h] = P0s; res[3][h] = P0w;
        res[4][h] = Q0s; res[5][h] = Q1s;
        if (!h) { r[0] = Q2s; r[7] = Q0w; }
        else {
            __m128i q2s = _mm_packus_epi16(r[0], Q2s), q0w = _mm_packus_epi16(r[7], Q0w);
            r[6] = blend(sq, q2s, q2);
            r[4] = blend(sq, _mm_packus_epi16(res[4][0], res[4][1]), blend(m, q0w, q0));
            r[5] = blend(sq, _mm_packus_epi16(res[5][0], res[5][1]), q1);
        }
    }
    r[1] = blend(sp, _mm_packus_epi16(res[0][0], res[0][1]), p2);
    r[2] = blend(sp, _mm_packus_epi16(res[1][0], res[1][1]), p1);
    r[3] = blend(sp, _mm_packus_epi16(res[2][0], res[2][1]), blend(m, _mm_packus_epi16(res[3][0], res[3][1]), p0));
    r[0] = p3; r[7] = q3;
}

/* chroma: rows[2..5] = p1 p0 q0 q1 (16 lanes: 8 Cb + 8 Cr) */
static void chroma_filter_simd(__m128i *r, __m128i alpha, __m128i beta, __m128i tc0, int strong) {
    const __m128i z = _mm_setzero_si128();
    __m128i p1 = r[2], p0 = r[3], q0 = r[4], q1 = r[5];
    __m128i m = _mm_and_si128(_mm_and_si128(lt8(absdiff8(p0, q0), alpha), lt8(absdiff8(p1, p0), beta)), lt8(absdiff8(q1, q0), beta));
    m = _mm_and_si128(m, _mm_cmpgt_epi8(tc0, _mm_set1_epi8(-1)));
    if (_mm_movemask_epi8(m) == 0) return;
    __m128i o[2][2];
    for (int h = 0; h < 2; h++) {
        __m128i P1 = UNP(p1), P0 = UNP(p0), Q0 = UNP(q0), Q1 = UNP(q1);
        if (!strong) {
            __m128i tc = _mm_add_epi16(_mm_srai_epi16(UNPS(tc0), 8), _mm_set1_epi16(1));
            __m128i dl = _mm_add_epi16(_mm_slli_epi16(_mm_sub_epi16(Q0, P0), 2), _mm_sub_epi16(P1, Q1));
            dl = _mm_srai_epi16(_mm_add_epi16(dl, _mm_set1_epi16(4)), 3);
            dl = _mm_min_epi16(_mm_max_epi16(dl, _mm_sub_epi16(z, tc)), tc);
            o[0][h] = _mm_add_epi16(P0, dl);
            o[1][h] = _mm_sub_epi16(Q0, dl);
        } else {
            const __m128i two = _mm_set1_epi16(2);
            o[0][h] = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(P1, 1), _mm_add_epi16(P0, Q1)), two), 2);
            o[1][h] = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(Q1, 1), _mm_add_epi16(Q0, P1)), two), 2);
        }
    }
    r[3] = blend(m, _mm_packus_epi16(o[0][0], o[0][1]), p0);
    r[4] = blend(m, _mm_packus_epi16(o[1][0], o[1][1]), q0);
#undef UNP
#undef UNPS
}

/* 16 rows of 8 bytes (at p[i * stride]) <-> 8 registers of 16 lanes */
static void load_t16x8(const uint8_t *p, int stride, __m128i *c) {
    __m128i a[8], b[8], e[8];
    for (int i = 0; i < 8; i++)
        a[i] = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(p + (2 * i) * stride)),
                                 _mm_loadl_epi64((const __m128i *)(const void *)(p + (2 * i + 1) * stride)));
    for (int i = 0; i < 4; i++) { b[2 * i] = _mm_unpacklo_epi16(a[2 * i], a[2 * i + 1]); b[2 * i + 1] = _mm_unpackhi_epi16(a[2 * i], a[2 * i + 1]); }
    /* b[0]: rows 0-3 cols 0-3, b[1]: rows 0-3 cols 4-7, b[2]: rows 4-7 cols 0-3 ... */
    for (int i = 0; i < 2; i++) {
        e[4 * i + 0] = _mm_unpacklo_epi32(b[4 * i], b[4 * i + 2]);       /* rows 8i..8i+7, cols 0-1 */
        e[4 * i + 1] = _mm_unpackhi_epi32(b[4 * i], b[4 * i + 2]);       /* cols 2-3 */
        e[4 * i + 2] = _mm_unpacklo_epi32(b[4 * i + 1], b[4 * i + 3]);   /* cols 4-5 */
        e[4 * i + 3] = _mm_unpackhi_epi32(b[4 * i + 1], b[4 * i + 3]);   /* cols 6-7 */
    }
    for (int k = 0; k < 4; k++) {
        c[2 * k] = _mm_unpacklo_epi64(e[k], e[4 + k]);
        c[2 * k + 1] = _mm_unpackhi_epi64(e[k], e[4 + k]);
    }
}
static void store_t16x8(uint8_t *p, int stride, const __m128i *c) {
    __m128i d[8], e[8];
    for (int i = 0; i < 4; i++) {
        d[2 * i] = _mm_unpacklo_epi8(c[2 * i], c[2 * i + 1]);       /* rows 0-7, cols 2i,2i+1 */
        d[2 * i + 1] = _mm_unpackhi_epi8(c[2 * i], c[2 * i + 1]);   /* rows 8-15 */
    }
    for (int hh = 0; hh < 2; hh++) {
        __m128i lo01 = _mm_unpacklo_epi16(d[hh], d[2 + hh]), hi01 = _mm_unpackhi_epi16(d[hh], d[2 + hh]);       /* cols 0-3 */
        __m128i lo23 = _mm_unpacklo_epi16(d[4 + hh], d[6 + hh]), hi23 = _mm_unpackhi_epi16(d[4 + hh], d[6 + hh]); /* cols 4-7 */
        e[0] = _mm_unpacklo_epi32(lo01, lo23); e[1] = _mm_unpackhi_epi32(lo01, lo23);
        e[2] = _mm_unpacklo_epi32(hi01, hi23); e[3] = _mm_unpackhi_epi32(hi01, hi23);
        for (int k = 0; k < 4; k++) {
            uint8_t *row = p + (hh * 8 + 2 * k) * stride;
            _mm_storel_epi64((__m128i *)(void *)row, e[k]);
            _mm_storel_epi64((__m128i *)(void *)(row + stride), _mm_unpackhi_epi64(e[k], e[k]));
        }
    }
}

/* per-lane tc0 for bS 0..4 (0xff = do not filter) */
H264_INLINE void tc_table(int ia, uint8_t *t) {
    t[0] = 0xff; t[1] = h264_tc0[ia][0]; t[2] = h264_tc0[ia][1]; t[3] = h264_tc0[ia][2]; t[4] = 0;
}

static void edge_luma(uint8_t *pix, int stride, int vert, const int *bs, int alpha, int beta, int ia) {
    __m128i r[8];
    uint8_t t[5];
    tc_table(ia, t);
    __m128i tc0 = _mm_set_epi32((int)(t[bs[3]] * 0x01010101u), (int)(t[bs[2]] * 0x01010101u),
                                (int)(t[bs[1]] * 0x01010101u), (int)(t[bs[0]] * 0x01010101u));
    __m128i av = _mm_set1_epi8((char)alpha), bv = _mm_set1_epi8((char)beta);
    if (vert) {
        load_t16x8(pix - 4, stride, r);
        luma_filter_simd(r, av, bv, tc0, bs[0] >= 4);
        store_t16x8(pix - 4, stride, r);
    } else {
        for (int i = 0; i < 8; i++) r[i] = _mm_loadu_si128((const __m128i *)(const void *)(pix + (i - 4) * stride));
        luma_filter_simd(r, av, bv, tc0, bs[0] >= 4);
        for (int i = 1; i < 7; i++) _mm_storeu_si128((__m128i *)(void *)(pix + (i - 4) * stride), r[i]);
    }
}

/* both chroma planes at once: lanes 0-7 Cb, 8-15 Cr */
static void edge_chroma(uint8_t *pu, uint8_t *pv, int stride, int vert, const int *bs, const int *alpha, const int *beta, const int *ia) {
    __m128i r[8];
    uint64_t tv[2], a[2], b[2];
    for (int c = 0; c < 2; c++) {
        a[c] = (uint64_t)(uint8_t)alpha[c] * 0x0101010101010101ull;
        b[c] = (uint64_t)(uint8_t)beta[c] * 0x0101010101010101ull;
        if (!alpha[c] || !beta[c]) { tv[c] = ~0ull; continue; }
        uint8_t t[5];
        tc_table(ia[c], t);
        tv[c] = 0;
        for (int k = 0; k < 4; k++) tv[c] |= (uint64_t)(t[bs[k]] * 0x0101u) << (16 * k);
    }
    __m128i tc0 = _mm_set_epi64x((long long)tv[1], (long long)tv[0]);
    __m128i av = _mm_set_epi64x((long long)a[1], (long long)a[0]), bv = _mm_set_epi64x((long long)b[1], (long long)b[0]);
    av = _mm_max_epu8(av, _mm_set1_epi8(1));
    bv = _mm_max_epu8(bv, _mm_set1_epi8(1));
    if (vert) {
        /* 8 rows of Cb then 8 rows of Cr, 8 bytes each starting 4 left of the edge */
        __m128i a2[8], b2[8], e2[8];
        for (int i = 0; i < 8; i++) {
            const uint8_t *p0 = i < 4 ? pu + (2 * i) * stride : pv + (2 * i - 8) * stride;
            a2[i] = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(p0 - 4)), _mm_loadl_epi64((const __m128i *)(const void *)(p0 + stride - 4)));
        }
        for (int i = 0; i < 4; i++) { b2[2 * i] = _mm_unpacklo_epi16(a2[2 * i], a2[2 * i + 1]); b2[2 * i + 1] = _mm_unpackhi_epi16(a2[2 * i], a2[2 * i + 1]); }
        for (int i = 0; i < 2; i++) {
            e2[4 * i + 0] = _mm_unpacklo_epi32(b2[4 * i], b2[4 * i + 2]);
            e2[4 * i + 1] = _mm_unpackhi_epi32(b2[4 * i], b2[4 * i + 2]);
            e2[4 * i + 2] = _mm_unpacklo_epi32(b2[4 * i + 1], b2[4 * i + 3]);
            e2[4 * i + 3] = _mm_unpackhi_epi32(b2[4 * i + 1], b2[4 * i + 3]);
        }
        for (int k = 0; k < 4; k++) { r[2 * k] = _mm_unpacklo_epi64(e2[k], e2[4 + k]); r[2 * k + 1] = _mm_unpackhi_epi64(e2[k], e2[4 + k]); }
        chroma_filter_simd(r, av, bv, tc0, bs[0] >= 4);
        /* write back: transpose as in store_t16x8 but to two planes */
        __m128i d[8];
        for (int i = 0; i < 4; i++) { d[2 * i] = _mm_unpacklo_epi8(r[2 * i], r[2 * i + 1]); d[2 * i + 1] = _mm_unpackhi_epi8(r[2 * i], r[2 * i + 1]); }
        for (int hh = 0; hh < 2; hh++) {
            uint8_t *base = hh ? pv : pu;
            __m128i lo01 = _mm_unpacklo_epi16(d[hh], d[2 + hh]), hi01 = _mm_unpackhi_epi16(d[hh], d[2 + hh]);
            __m128i lo23 = _mm_unpacklo_epi16(d[4 + hh], d[6 + hh]), hi23 = _mm_unpackhi_epi16(d[4 + hh], d[6 + hh]);
            __m128i e[4];
            e[0] = _mm_unpacklo_epi32(lo01, lo23); e[1] = _mm_unpackhi_epi32(lo01, lo23);
            e[2] = _mm_unpacklo_epi32(hi01, hi23); e[3] = _mm_unpackhi_epi32(hi01, hi23);
            for (int k = 0; k < 4; k++) {
                uint8_t *row = base + (2 * k) * stride - 4;
                _mm_storel_epi64((__m128i *)(void *)row, e[k]);
                _mm_storel_epi64((__m128i *)(void *)(row + stride), _mm_unpackhi_epi64(e[k], e[k]));
            }
        }
    } else {
        for (int i = 2; i < 6; i++)
            r[i] = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)(const void *)(pu + (i - 4) * stride)),
                                      _mm_loadl_epi64((const __m128i *)(const void *)(pv + (i - 4) * stride)));
        chroma_filter_simd(r, av, bv, tc0, bs[0] >= 4);
        for (int i = 3; i < 5; i++) {
            _mm_storel_epi64((__m128i *)(void *)(pu + (i - 4) * stride), r[i]);
            _mm_storel_epi64((__m128i *)(void *)(pv + (i - 4) * stride), _mm_unpackhi_epi64(r[i], r[i]));
        }
    }
}
#endif

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
        int any = 0;
        for (int e = 0; e < 4; e++) {
            const struct h264_mbinfo *pm = e ? m : n;
            int *b = bs[e];
            b[0] = b[1] = b[2] = b[3] = 0;
            if (e == 0 && !filter_edge0) continue;
            if (e && m->t8x8 && (e & 1)) continue;
            if (qintra || (pm->type & MB_INTRA)) { b[0] = b[1] = b[2] = b[3] = e ? 3 : 4; any = 1; continue; }
            /* non-zero coefficients */
            int qn, pn;
            if (dir == 0) {
                qn = (m->nzdb >> e) & 0x1111;
                pn = (pm->nzdb >> ((e + 3) & 3)) & 0x1111;
                qn |= pn;
                b[0] = (qn & 1) << 1; b[1] = (qn >> 3) & 2; b[2] = (qn >> 7) & 2; b[3] = (qn >> 11) & 2;
            } else {
                qn = (m->nzdb >> (4 * e)) & 15;
                pn = (pm->nzdb >> (4 * ((e + 3) & 3))) & 15;
                qn |= pn;
                b[0] = (qn & 1) << 1; b[1] = qn & 2; b[2] = (qn >> 1) & 2; b[3] = (qn >> 2) & 2;
            }
            if (qn) any = 1;
            if (qn == 15 || qn == 0x1111) continue;
            if (e && !((m->mvedges >> (dir * 4 + e)) & 1)) continue;
            for (int k = 0; k < 4; k++) {
                if (b[k]) continue;
                int qx = dir ? k : e, qy = dir ? e : k;
                int px = dir ? k : e - 1, pyy = dir ? e - 1 : k;
                int q4 = b4 + qy * b4s + qx, q8i = b8 + (qy >> 1) * b8s + (qx >> 1);
                int p4 = b4 + pyy * b4s + px, p8i;
                if (pyy < 0) p8i = b8 - b8s + (px >> 1);
                else if (px < 0) p8i = b8 + (pyy >> 1) * b8s - 1;
                else p8i = b8 + (pyy >> 1) * b8s + (px >> 1);
                b[k] = mv_bs(pic, q4, q8i, p4, p8i);
                any |= b[k];
            }
        }
        if (!any) continue;
        /* luma */
        for (int e = 0; e < 4; e++) {
            if (!(bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3])) continue;
            const struct h264_mbinfo *pm = e ? m : n;
            int qpav = (m->qp + pm->qp + 1) >> 1;
            int ia = h264_clip(qpav + si->alpha_off, 0, 51), ib = h264_clip(qpav + si->beta_off, 0, 51);
            int alpha = h264_alpha[ia], beta = h264_beta[ib];
            if (!alpha || !beta) continue;
#ifndef H264_NO_SIMD
            edge_luma(dir ? py + e * 4 * sy : py + e * 4, sy, !dir, bs[e], alpha, beta, ia);
#else
            for (int k = 0; k < 4; k++) {
                if (!bs[e][k]) continue;
                uint8_t *pix = dir ? py + e * 4 * sy + k * 4 : py + k * 4 * sy + e * 4;
                int tc0 = bs[e][k] < 4 ? h264_tc0[ia][bs[e][k] - 1] : 0;
                filter_luma(pix, dir ? sy : 1, dir ? 1 : sy, bs[e][k], alpha, beta, tc0);
            }
#endif
        }
        /* chroma: edges 0 and 2 (luma units) */
        for (int e = 0; e < 4; e += 2) {
            if (!(bs[e][0] | bs[e][1] | bs[e][2] | bs[e][3])) continue;
            const struct h264_mbinfo *pm = e ? m : n;
            int alpha[2], beta[2], iav[2];
            for (int c = 0; c < 2; c++) {
                int off = si->cqp_offset[c];
                int qq = h264_chroma_qp[h264_clip(m->qp + off, 0, 51)];
                int qp = h264_chroma_qp[h264_clip(pm->qp + off, 0, 51)];
                int qpav = (qq + qp + 1) >> 1;
                iav[c] = h264_clip(qpav + si->alpha_off, 0, 51);
                int ib = h264_clip(qpav + si->beta_off, 0, 51);
                alpha[c] = h264_alpha[iav[c]]; beta[c] = h264_beta[ib];
            }
#ifndef H264_NO_SIMD
            if (!(alpha[0] && beta[0]) && !(alpha[1] && beta[1])) continue;
            edge_chroma(dir ? pu + e * 2 * sc : pu + e * 2, dir ? pv + e * 2 * sc : pv + e * 2, sc, !dir, bs[e], alpha, beta, iav);
#else
            for (int c = 0; c < 2; c++) {
                if (!alpha[c] || !beta[c]) continue;
                uint8_t *base = c ? pv : pu;
                for (int k = 0; k < 4; k++) {
                    if (!bs[e][k]) continue;
                    uint8_t *pix = dir ? base + (e * 2) * sc + k * 2 : base + k * 2 * sc + e * 2;
                    int tc0 = bs[e][k] < 4 ? h264_tc0[iav[c]][bs[e][k] - 1] : 0;
                    filter_chroma(pix, dir ? sc : 1, dir ? 1 : sc, bs[e][k], alpha[c], beta[c], tc0);
                }
            }
#endif
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
