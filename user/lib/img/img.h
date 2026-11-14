/* DuncOS image codecs: PNG/JPEG/GIF/BMP decoding, PNG encoding, zlib/gzip.
 * Freestanding, integer-only, allocator-driven. */
#pragma once
#include <stdint.h>
#include <stddef.h>

struct img_alloc {
    void *ctx;
    void *(*alloc)(void *ctx, size_t n);
    void (*free)(void *ctx, void *p);
};

/* px: w*h pixels, 0xAARRGGBB, row-major, alpha 255 = opaque.
 * Allocated with the caller's allocator; caller frees it. */
struct image { int w, h; uint32_t *px; };

enum {
    IMG_OK = 0,
    IMG_EFORMAT = -1,   /* unrecognised format */
    IMG_ECORRUPT = -2,  /* malformed data */
    IMG_ETRUNC = -3,    /* data ends early */
    IMG_ENOMEM = -4,    /* allocator failed */
    IMG_ETOOBIG = -5,   /* dimensions over the limits */
    IMG_EUNSUP = -6,    /* valid but unsupported feature */
    IMG_ESPACE = -7,    /* output buffer too small */
    IMG_EARG = -8,      /* bad argument */
    IMG_ECHECK = -9,    /* checksum mismatch */
};

#define IMG_MAX_DIM    16384
#define IMG_MAX_PIXELS (64u * 1024 * 1024)

int img_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a);
int img_probe(const uint8_t *data, size_t len, int *w, int *h);
const char *img_strerror(int e);

/* Individual decoders (img_decode sniffs and dispatches to these). */
int png_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a);
int jpeg_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a);
int gif_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a);
int bmp_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a);

/* zlib/deflate. Return bytes written or a negative IMG_E* code. */
long inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *consumed,
                 const struct img_alloc *a);
long zlib_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, const struct img_alloc *a);
long gunzip(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, const struct img_alloc *a);
long gzip_size(const uint8_t *in, size_t inlen);   /* ISIZE from the trailer (size mod 2^32), <0 if invalid */

uint32_t img_crc32(uint32_t crc, const uint8_t *p, size_t n);    /* start with 0 */
uint32_t img_adler32(uint32_t adler, const uint8_t *p, size_t n); /* start with 1 */

/* PNG encoder: px is 0xAARRGGBB, or 0x00RRGGBB with opaque=1 (writes RGB).
 * Returns PNG size or negative error. outmax >= png_encode_bound(w,h) always suffices. */
long png_encode(const uint32_t *px, int w, int h, int opaque, uint8_t *out, size_t outmax);
size_t png_encode_bound(int w, int h);
