/* Format sniffing and dispatch. */
#include "img_int.h"

enum { F_NONE, F_PNG, F_JPEG, F_GIF, F_BMP };

static int sniff(const uint8_t *d, size_t n) {
    if (!d) return F_NONE;
    if (n >= 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return F_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return F_JPEG;
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return F_GIF;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return F_BMP;
    return F_NONE;
}

int img_decode(const uint8_t *data, size_t len, struct image *out, const struct img_alloc *a) {
    if (!out) return IMG_EARG;
    out->w = out->h = 0;
    out->px = 0;
    if (!a || !a->alloc) return IMG_EARG;
    switch (sniff(data, len)) {
    case F_PNG: return png_decode(data, len, out, a);
    case F_JPEG: return jpeg_decode(data, len, out, a);
    case F_GIF: return gif_decode(data, len, out, a);
    case F_BMP: return bmp_decode(data, len, out, a);
    default: return IMG_EFORMAT;
    }
}

int img_probe(const uint8_t *data, size_t len, int *w, int *h) {
    switch (sniff(data, len)) {
    case F_PNG: return png_probe(data, len, w, h);
    case F_JPEG: return jpeg_probe(data, len, w, h);
    case F_GIF: return gif_probe(data, len, w, h);
    case F_BMP: return bmp_probe(data, len, w, h);
    default: return IMG_EFORMAT;
    }
}

const char *img_strerror(int e) {
    switch (e) {
    case IMG_OK: return "ok";
    case IMG_EFORMAT: return "unrecognised image format";
    case IMG_ECORRUPT: return "corrupt data";
    case IMG_ETRUNC: return "truncated data";
    case IMG_ENOMEM: return "out of memory";
    case IMG_ETOOBIG: return "image too large";
    case IMG_EUNSUP: return "unsupported feature";
    case IMG_ESPACE: return "output buffer too small";
    case IMG_EARG: return "bad argument";
    case IMG_ECHECK: return "checksum mismatch";
    default: return "unknown error";
    }
}
