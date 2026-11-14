/* Internal helpers shared by the image codecs. */
#pragma once
#include "img.h"
#include "../../../common/str.h"

static inline void *ia_alloc(const struct img_alloc *a, size_t n) {
    if (!a || !a->alloc || n == 0) return 0;
    return a->alloc(a->ctx, n);
}
static inline void *ia_zalloc(const struct img_alloc *a, size_t n) {
    void *p = ia_alloc(a, n);
    if (p) memset(p, 0, n);
    return p;
}
static inline void ia_free(const struct img_alloc *a, void *p) {
    if (p && a && a->free) a->free(a->ctx, p);
}

static inline int img_check_dims(long w, long h) {
    if (w <= 0 || h <= 0) return IMG_ECORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) return IMG_ETOOBIG;
    if ((uint64_t)w * (uint64_t)h > IMG_MAX_PIXELS) return IMG_ETOOBIG;
    return 0;
}

static inline uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static inline unsigned rd_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline unsigned rd_le16(const uint8_t *p) { return (unsigned)p[1] << 8 | p[0]; }

/* Decode a zlib stream into out[0..outmax); exact = require precisely outmax bytes.
 * check_adler = verify the trailer. */
long img_zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, int check_adler,
                      const struct img_alloc *a);

int png_probe(const uint8_t *d, size_t n, int *w, int *h);
int jpeg_probe(const uint8_t *d, size_t n, int *w, int *h);
int gif_probe(const uint8_t *d, size_t n, int *w, int *h);
int bmp_probe(const uint8_t *d, size_t n, int *w, int *h);
