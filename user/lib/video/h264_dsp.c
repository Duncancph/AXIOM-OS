/* AXIOM H.264 decoder: inverse transforms (8.5) and motion compensation (8.4.2.2). */
#include "h264_int.h"
#include <emmintrin.h>

/* ---------------------------------------------------------------- transforms */
void h264_idct4_add(uint8_t *dst, int stride, int16_t *b) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int d0 = b[i * 4], d1 = b[i * 4 + 1], d2 = b[i * 4 + 2], d3 = b[i * 4 + 3];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int i = 0; i < 4; i++) {
        int d0 = t[i], d1 = t[4 + i], d2 = t[8 + i], d3 = t[12 + i];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        dst[i] = h264_clip8(dst[i] + ((e0 + e3 + 32) >> 6));
        dst[stride + i] = h264_clip8(dst[stride + i] + ((e1 + e2 + 32) >> 6));
        dst[2 * stride + i] = h264_clip8(dst[2 * stride + i] + ((e1 - e2 + 32) >> 6));
        dst[3 * stride + i] = h264_clip8(dst[3 * stride + i] + ((e0 - e3 + 32) >> 6));
    }
    memset(b, 0, 32);
}

void h264_idct4_dc_add(uint8_t *dst, int stride, int16_t *b) {
    int dc = (b[0] + 32) >> 6;
    b[0] = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) dst[y * stride + x] = h264_clip8(dst[y * stride + x] + dc);
}

void h264_idct8_add(uint8_t *dst, int stride, int16_t *b) {
    int t[64];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++) {
            int d0, d1, d2, d3, d4, d5, d6, d7;
            if (pass == 0) {
                const int16_t *s = b + i * 8;
                d0 = s[0]; d1 = s[1]; d2 = s[2]; d3 = s[3]; d4 = s[4]; d5 = s[5]; d6 = s[6]; d7 = s[7];
            } else {
                d0 = t[i]; d1 = t[8 + i]; d2 = t[16 + i]; d3 = t[24 + i]; d4 = t[32 + i]; d5 = t[40 + i]; d6 = t[48 + i]; d7 = t[56 + i];
            }
            int e0 = d0 + d4, e1 = -d3 + d5 - d7 - (d7 >> 1), e2 = d0 - d4, e3 = d1 + d7 - d3 - (d3 >> 1);
            int e4 = (d2 >> 1) - d6, e5 = -d1 + d7 + d5 + (d5 >> 1), e6 = d2 + (d6 >> 1), e7 = d3 + d5 + d1 + (d1 >> 1);
            int f0 = e0 + e6, f1 = e1 + (e7 >> 2), f2 = e2 + e4, f3 = e3 + (e5 >> 2);
            int f4 = e2 - e4, f5 = (e3 >> 2) - e5, f6 = e0 - e6, f7 = e7 - (e1 >> 2);
            int g[8] = { f0 + f7, f2 + f5, f4 + f3, f6 + f1, f6 - f1, f4 - f3, f2 - f5, f0 - f7 };
            if (pass == 0) for (int k = 0; k < 8; k++) t[i * 8 + k] = g[k];
            else for (int k = 0; k < 8; k++) dst[k * stride + i] = h264_clip8(dst[k * stride + i] + ((g[k] + 32) >> 6));
        }
    }
    memset(b, 0, 128);
}

void h264_idct8_dc_add(uint8_t *dst, int stride, int16_t *b) {
    int dc = (b[0] + 32) >> 6;
    b[0] = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) dst[y * stride + x] = h264_clip8(dst[y * stride + x] + dc);
}

/* Intra16x16 DC: Hadamard + scaling; dc is the 4x4 matrix in raster block order. Output to coef[blk*16]. */
static const uint8_t raster_to_blk[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };
void h264_luma_dc_dequant(int16_t *coef, const int16_t *dc, int qmul) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a = dc[i * 4], b = dc[i * 4 + 1], c = dc[i * 4 + 2], d = dc[i * 4 + 3];
        t[i * 4] = a + b + c + d; t[i * 4 + 1] = a + b - c - d; t[i * 4 + 2] = a - b - c + d; t[i * 4 + 3] = a - b + c - d;
    }
    for (int i = 0; i < 4; i++) {
        int a = t[i], b = t[4 + i], c = t[8 + i], d = t[12 + i];
        int f[4] = { a + b + c + d, a + b - c - d, a - b - c + d, a - b + c - d };
        for (int k = 0; k < 4; k++)
            coef[raster_to_blk[k * 4 + i] * 16] = (int16_t)(((int64_t)f[k] * qmul + 128) >> 8);
    }
}

void h264_chroma_dc_dequant(int16_t *coef, const int16_t *dc, int qmul) {
    int a = dc[0], b = dc[1], c = dc[2], d = dc[3];
    int f0 = a + b + c + d, f1 = a - b + c - d, f2 = a + b - c - d, f3 = a - b - c + d;
    coef[0] = (int16_t)(((int64_t)f0 * qmul) >> 7);
    coef[16] = (int16_t)(((int64_t)f1 * qmul) >> 7);
    coef[32] = (int16_t)(((int64_t)f2 * qmul) >> 7);
    coef[48] = (int16_t)(((int64_t)f3 * qmul) >> 7);
}

/* ---------------------------------------------------------------- luma interpolation */
#define TAP6(p, s) ((p)[-2 * (s)] - 5 * (p)[-(s)] + 20 * (p)[0] + 20 * (p)[(s)] - 5 * (p)[2 * (s)] + (p)[3 * (s)])

static void put_copy(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    if (w == 16) for (int y = 0; y < h; y++) { WR64(dst + y * ds, RD64(src + y * ss)); WR64(dst + y * ds + 8, RD64(src + y * ss + 8)); }
    else if (w == 8) for (int y = 0; y < h; y++) WR64(dst + y * ds, RD64(src + y * ss));
    else for (int y = 0; y < h; y++) WR32(dst + y * ds, RD32(src + y * ss));
}

#ifdef H264_NO_SIMD
static void put_h(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) dst[y * ds + x] = h264_clip8((TAP6(src + y * ss + x, 1) + 16) >> 5);
}
static void put_v(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) dst[y * ds + x] = h264_clip8((TAP6(src + y * ss + x, ss) + 16) >> 5);
}
static void put_hv(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    int16_t tmp[21 * 16];
    for (int y = -2; y < h + 3; y++)
        for (int x = 0; x < w; x++) tmp[(y + 2) * 16 + x] = (int16_t)TAP6(src + y * ss + x, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int16_t *t = tmp + (y + 2) * 16 + x;
            int v = t[-32] - 5 * t[-16] + 20 * t[0] + 20 * t[16] - 5 * t[32] + t[48];
            dst[y * ds + x] = h264_clip8((v + 512) >> 10);
        }
}
static void avg2(uint8_t *dst, int ds, const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) dst[y * ds + x] = (uint8_t)((a[y * as + x] + b[y * bs + x] + 1) >> 1);
}
#else
H264_INLINE __m128i ld8(const uint8_t *p) {
    return _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)p), _mm_setzero_si128());
}
H264_INLINE void st8(uint8_t *p, __m128i v, int w) {   /* stores w (4 or 8) bytes of the low half */
    if (w == 8) _mm_storel_epi64((__m128i *)(void *)p, v);
    else WR32(p, (uint32_t)_mm_cvtsi128_si32(v));
}
/* 6-tap filter of 8 lanes (16 bit, unscaled): taps at p-2s .. p+3s */
H264_INLINE __m128i tap6(const uint8_t *p, int s) {
    __m128i cd = _mm_add_epi16(ld8(p), ld8(p + s));
    __m128i be = _mm_add_epi16(ld8(p - s), ld8(p + 2 * s));
    __m128i af = _mm_add_epi16(ld8(p - 2 * s), ld8(p + 3 * s));
    __m128i t = _mm_sub_epi16(_mm_slli_epi16(cd, 2), be);
    t = _mm_add_epi16(t, _mm_slli_epi16(t, 2));
    return _mm_add_epi16(t, af);
}
static void put_h(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    const __m128i r16 = _mm_set1_epi16(16);
    int ww = w > 8 ? 8 : w;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 8) {
            __m128i v = _mm_srai_epi16(_mm_add_epi16(tap6(src + y * ss + x, 1), r16), 5);
            st8(dst + y * ds + x, _mm_packus_epi16(v, v), ww);
        }
}
static void put_v(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    const __m128i r16 = _mm_set1_epi16(16);
    int ww = w > 8 ? 8 : w;
    for (int x = 0; x < w; x += 8) {
        const uint8_t *s = src + x;
        __m128i a = ld8(s - 2 * ss), b = ld8(s - ss), c = ld8(s), d = ld8(s + ss), e = ld8(s + 2 * ss);
        for (int y = 0; y < h; y++) {
            __m128i f = ld8(s + (y + 3) * ss);
            __m128i cd = _mm_add_epi16(c, d), be = _mm_add_epi16(b, e), af = _mm_add_epi16(a, f);
            __m128i t = _mm_sub_epi16(_mm_slli_epi16(cd, 2), be);
            t = _mm_add_epi16(_mm_add_epi16(t, _mm_slli_epi16(t, 2)), af);
            t = _mm_srai_epi16(_mm_add_epi16(t, r16), 5);
            st8(dst + y * ds + x, _mm_packus_epi16(t, t), ww);
            a = b; b = c; c = d; d = e; e = f;
        }
    }
}
static void put_hv(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    __m128i tmp[21];
    const __m128i one = _mm_set1_epi16(1), m5 = _mm_set1_epi16(-5), p20 = _mm_set1_epi16(20);
    const __m128i r512 = _mm_set1_epi32(512);
    int ww = w > 8 ? 8 : w;
    for (int x = 0; x < w; x += 8) {
        for (int y = 0; y < h + 5; y++) tmp[y] = tap6(src + (y - 2) * ss + x, 1);
        for (int y = 0; y < h; y++) {
            __m128i a = tmp[y], b = tmp[y + 1], c = tmp[y + 2], d = tmp[y + 3], e = tmp[y + 4], f = tmp[y + 5];
            __m128i lo = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(a, f), one),
                                                     _mm_madd_epi16(_mm_unpacklo_epi16(b, e), m5)),
                                       _mm_madd_epi16(_mm_unpacklo_epi16(c, d), p20));
            __m128i hi = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(a, f), one),
                                                     _mm_madd_epi16(_mm_unpackhi_epi16(b, e), m5)),
                                       _mm_madd_epi16(_mm_unpackhi_epi16(c, d), p20));
            lo = _mm_srai_epi32(_mm_add_epi32(lo, r512), 10);
            hi = _mm_srai_epi32(_mm_add_epi32(hi, r512), 10);
            __m128i v = _mm_packs_epi32(lo, hi);
            st8(dst + y * ds + x, _mm_packus_epi16(v, v), ww);
        }
    }
}
static void avg2(uint8_t *dst, int ds, const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h) {
    if (w == 16) {
        for (int y = 0; y < h; y++)
            _mm_storeu_si128((__m128i *)(void *)(dst + y * ds), _mm_avg_epu8(_mm_loadu_si128((const __m128i *)(const void *)(a + y * as)),
                                                                      _mm_loadu_si128((const __m128i *)(const void *)(b + y * bs))));
    } else {
        for (int y = 0; y < h; y++) {
            __m128i v = _mm_avg_epu8(_mm_loadl_epi64((const __m128i *)(const void *)(a + y * as)), _mm_loadl_epi64((const __m128i *)(const void *)(b + y * bs)));
            st8(dst + y * ds, v, w);
        }
    }
}
#endif

static void luma_qpel(uint8_t *dst, int ds, const uint8_t *src, int ss, int fx, int fy, int w, int h) {
    uint8_t A[256] H264_ALIGN(16), B[256] H264_ALIGN(16);
    switch (fy * 4 + fx) {
    case 0: put_copy(dst, ds, src, ss, w, h); return;
    case 1: put_h(A, 16, src, ss, w, h); avg2(dst, ds, A, 16, src, ss, w, h); return;
    case 2: put_h(dst, ds, src, ss, w, h); return;
    case 3: put_h(A, 16, src, ss, w, h); avg2(dst, ds, A, 16, src + 1, ss, w, h); return;
    case 4: put_v(A, 16, src, ss, w, h); avg2(dst, ds, A, 16, src, ss, w, h); return;
    case 8: put_v(dst, ds, src, ss, w, h); return;
    case 12: put_v(A, 16, src, ss, w, h); avg2(dst, ds, A, 16, src + ss, ss, w, h); return;
    case 5: put_h(A, 16, src, ss, w, h); put_v(B, 16, src, ss, w, h); break;
    case 7: put_h(A, 16, src, ss, w, h); put_v(B, 16, src + 1, ss, w, h); break;
    case 13: put_h(A, 16, src + ss, ss, w, h); put_v(B, 16, src, ss, w, h); break;
    case 15: put_h(A, 16, src + ss, ss, w, h); put_v(B, 16, src + 1, ss, w, h); break;
    case 10: put_hv(dst, ds, src, ss, w, h); return;
    case 6: put_hv(A, 16, src, ss, w, h); put_h(B, 16, src, ss, w, h); break;
    case 14: put_hv(A, 16, src, ss, w, h); put_h(B, 16, src + ss, ss, w, h); break;
    case 9: put_hv(A, 16, src, ss, w, h); put_v(B, 16, src, ss, w, h); break;
    case 11: put_hv(A, 16, src, ss, w, h); put_v(B, 16, src + 1, ss, w, h); break;
    }
    avg2(dst, ds, A, 16, B, 16, w, h);
}

void h264_mc_luma(uint8_t *dst, int ds, const struct h264_pic *ref, int stride, int pw, int ph,
                  int x, int y, int mvx, int mvy, int w, int h, uint8_t *edge, int16_t *tmp) {
    int xi = x + (mvx >> 2), yi = y + (mvy >> 2);
    const uint8_t *plane = ref->plane[0];
    const uint8_t *src;
    int ss = stride;
    if (xi - 2 < -H264_PAD || yi - 2 < -H264_PAD || xi + w + 3 > pw + H264_PAD || yi + h + 3 > ph + H264_PAD) {
        for (int r = 0; r < h + 5; r++) {
            int sy = h264_clip(yi - 2 + r, 0, ph - 1);
            const uint8_t *row = plane + (ptrdiff_t)sy * stride;
            for (int c = 0; c < w + 5; c++) edge[r * 32 + c] = row[h264_clip(xi - 2 + c, 0, pw - 1)];
        }
        src = edge + 2 * 32 + 2;
        ss = 32;
    } else src = plane + (ptrdiff_t)yi * stride + xi;
    luma_qpel(dst, ds, src, ss, mvx & 3, mvy & 3, w, h);
}

void h264_mc_chroma(uint8_t *dst, int ds, const uint8_t *plane, int stride, int pw, int ph,
                    int x, int y, int mvx, int mvy, int w, int h, uint8_t *edge) {
    int xi = x + (mvx >> 3), yi = y + (mvy >> 3), fx = mvx & 7, fy = mvy & 7;
    const uint8_t *src;
    int ss = stride;
    if (xi < -H264_PADC || yi < -H264_PADC || xi + w + 1 > pw + H264_PADC || yi + h + 1 > ph + H264_PADC) {
        for (int r = 0; r < h + 1; r++) {
            int sy = h264_clip(yi + r, 0, ph - 1);
            const uint8_t *row = plane + (ptrdiff_t)sy * stride;
            for (int c = 0; c < w + 1; c++) edge[r * 32 + c] = row[h264_clip(xi + c, 0, pw - 1)];
        }
        src = edge;
        ss = 32;
    } else src = plane + (ptrdiff_t)yi * stride + xi;
    if (!(fx | fy)) {
        if (w == 8) for (int j = 0; j < h; j++) WR64(dst + j * ds, RD64(src + j * ss));
        else if (w == 4) for (int j = 0; j < h; j++) WR32(dst + j * ds, RD32(src + j * ss));
        else for (int j = 0; j < h; j++) WR16(dst + j * ds, RD16(src + j * ss));
        return;
    }
    int A = (8 - fx) * (8 - fy), B = fx * (8 - fy), C = (8 - fx) * fy, D = fx * fy;
#ifdef H264_NO_SIMD
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint8_t *s = src + j * ss + i;
            dst[j * ds + i] = (uint8_t)((A * s[0] + B * s[1] + C * s[ss] + D * s[ss + 1] + 32) >> 6);
        }
#else
    const __m128i a = _mm_set1_epi16((short)A), b = _mm_set1_epi16((short)B), c = _mm_set1_epi16((short)C), d = _mm_set1_epi16((short)D);
    const __m128i r32 = _mm_set1_epi16(32);
    __m128i s0 = ld8(src), s1 = ld8(src + 1);
    for (int j = 0; j < h; j++) {
        __m128i s2 = ld8(src + (j + 1) * ss), s3 = ld8(src + (j + 1) * ss + 1);
        __m128i v = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(s0, a), _mm_mullo_epi16(s1, b)),
                                  _mm_add_epi16(_mm_mullo_epi16(s2, c), _mm_mullo_epi16(s3, d)));
        v = _mm_srli_epi16(_mm_add_epi16(v, r32), 6);
        v = _mm_packus_epi16(v, v);
        if (w == 8) _mm_storel_epi64((__m128i *)(void *)(dst + j * ds), v);
        else if (w == 4) WR32(dst + j * ds, (uint32_t)_mm_cvtsi128_si32(v));
        else WR16(dst + j * ds, (uint16_t)_mm_cvtsi128_si32(v));
        s0 = s2; s1 = s3;
    }
#endif
}

/* ---------------------------------------------------------------- bi-prediction / weighting */
void h264_avg(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    if (w == 2) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < 2; x++) dst[y * ds + x] = (uint8_t)((dst[y * ds + x] + src[y * ss + x] + 1) >> 1);
        return;
    }
    avg2(dst, ds, dst, ds, src, ss, w, h);
}

void h264_weight(uint8_t *dst, int ds, int w, int h, int log2wd, int wt, int ofs) {
#ifndef H264_NO_SIMD
    if (w >= 4) {
        const __m128i wv = _mm_set1_epi16((short)wt), rv = _mm_set1_epi16((short)(log2wd ? 1 << (log2wd - 1) : 0));
        const __m128i ov = _mm_set1_epi16((short)ofs), sh = _mm_cvtsi32_si128(log2wd);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x += 8) {
                __m128i v = _mm_add_epi16(_mm_mullo_epi16(ld8(dst + y * ds + x), wv), rv);
                v = _mm_add_epi16(_mm_sra_epi16(v, sh), ov);
                st8(dst + y * ds + x, _mm_packus_epi16(v, v), w > 8 ? 8 : w);
            }
        return;
    }
#endif
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int p = dst[y * ds + x];
            int v = log2wd >= 1 ? ((p * wt + (1 << (log2wd - 1))) >> log2wd) + ofs : p * wt + ofs;
            dst[y * ds + x] = h264_clip8(v);
        }
}

void h264_biweight(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h, int log2wd, int w0, int w1, int ofs) {
#ifndef H264_NO_SIMD
    if (w >= 4) {
        const __m128i wv = _mm_set1_epi32((int)((uint32_t)(uint16_t)w0 | (uint32_t)(uint16_t)w1 << 16));
        const __m128i rv = _mm_set1_epi32(1 << log2wd), ov = _mm_set1_epi16((short)ofs), sh = _mm_cvtsi32_si128(log2wd + 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x += 8) {
                __m128i dd = ld8(dst + y * ds + x), sv = ld8(src + y * ss + x);
                __m128i lo = _mm_sra_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(dd, sv), wv), rv), sh);
                __m128i hi = _mm_sra_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(dd, sv), wv), rv), sh);
                __m128i v = _mm_adds_epi16(_mm_packs_epi32(lo, hi), ov);
                st8(dst + y * ds + x, _mm_packus_epi16(v, v), w > 8 ? 8 : w);
            }
        return;
    }
#endif
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = ((dst[y * ds + x] * w0 + src[y * ss + x] * w1 + (1 << log2wd)) >> (log2wd + 1)) + ofs;
            dst[y * ds + x] = h264_clip8(v);
        }
}
