/* BMP decoder: 1/4/8-bit palette, 16/24/32-bit (BI_RGB and BITFIELDS), bottom-up and top-down. */
#include "img_int.h"

struct bmph {
    long w, h;
    int topdown, bpp, comp, palesz, ncol;
    size_t off, paloff;
    uint32_t mask[4];
};

static int bmp_header(const uint8_t *d, size_t n, struct bmph *b) {
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return IMG_EFORMAT;
    uint32_t hsz = rd_le32(d + 14);
    b->off = rd_le32(d + 10);
    long h;
    uint32_t clr = 0;
    memset(b->mask, 0, sizeof b->mask);
    if (hsz == 12) {
        b->w = (long)rd_le16(d + 18);
        h = (int16_t)rd_le16(d + 20);
        b->bpp = (int)rd_le16(d + 24);
        b->comp = 0;
        b->palesz = 3;
    } else if (hsz >= 40 && hsz <= 1024) {
        if (n < 14 + (size_t)hsz) return IMG_ETRUNC;
        b->w = (int32_t)rd_le32(d + 18);
        h = (int32_t)rd_le32(d + 22);
        b->bpp = (int)rd_le16(d + 28);
        b->comp = (int)rd_le32(d + 30);
        clr = rd_le32(d + 46);
        b->palesz = 4;
        if (hsz >= 52) for (int i = 0; i < 3; i++) b->mask[i] = rd_le32(d + 54 + i * 4);
        if (hsz >= 56) b->mask[3] = rd_le32(d + 66);
    } else {
        return IMG_EUNSUP;
    }
    b->paloff = 14 + hsz;
    if (b->comp == 3 || b->comp == 6) {
        int nm = b->comp == 6 ? 4 : 3;
        if (hsz < 52 || (b->comp == 6 && hsz < 56)) {
            if (n < b->paloff + (size_t)nm * 4) return IMG_ETRUNC;
            for (int i = 0; i < nm; i++) b->mask[i] = rd_le32(d + b->paloff + i * 4);
            b->paloff += (size_t)nm * 4;
        }
        if (b->comp == 3 && hsz < 56) b->mask[3] = 0;
        if (b->bpp != 16 && b->bpp != 32) return IMG_EUNSUP;
    } else if (b->comp != 0) {
        return IMG_EUNSUP;   /* RLE, JPEG, PNG */
    }
    if (h < 0) { b->topdown = 1; h = -h; }
    else b->topdown = 0;
    b->h = h;
    int r = img_check_dims(b->w, b->h);
    if (r) return r;
    switch (b->bpp) {
    case 1: case 4: case 8:
        b->ncol = clr && clr < (1u << b->bpp) ? (int)clr : 1 << b->bpp;
        break;
    case 16:
        if (b->comp == 0) { b->mask[0] = 0x7C00; b->mask[1] = 0x3E0; b->mask[2] = 0x1F; b->mask[3] = 0; }
        break;
    case 24: if (b->comp) return IMG_EUNSUP; break;
    case 32:
        if (b->comp == 0) { b->mask[0] = 0xFF0000; b->mask[1] = 0xFF00; b->mask[2] = 0xFF; b->mask[3] = 0; }
        break;
    default: return IMG_EUNSUP;
    }
    return 0;
}

int bmp_probe(const uint8_t *d, size_t n, int *w, int *h) {
    struct bmph b;
    int r = bmp_header(d, n, &b);
    if (r) return r;
    if (w) *w = (int)b.w;
    if (h) *h = (int)b.h;
    return 0;
}

static unsigned mask_val(uint32_t p, uint32_t m, unsigned dflt) {
    if (!m) return dflt;
    int sh = 0, bits = 0;
    while (!((m >> sh) & 1)) sh++;
    while (sh + bits < 32 && ((m >> (sh + bits)) & 1)) bits++;
    uint32_t v = (p & m) >> sh;
    if (bits >= 8) return (v >> (bits - 8)) & 255;
    uint32_t max = (1u << bits) - 1;
    v &= max;
    return v * 255 / max;
}

int bmp_decode(const uint8_t *d, size_t n, struct image *out, const struct img_alloc *a) {
    if (!d || !out) return IMG_EARG;
    out->px = 0;
    out->w = out->h = 0;
    struct bmph b;
    int r = bmp_header(d, n, &b);
    if (r) return r;
    int W = (int)b.w, H = (int)b.h;
    uint32_t pal[256];
    if (b.bpp <= 8) {
        for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u;
        for (int i = 0; i < b.ncol; i++) {
            size_t o = b.paloff + (size_t)i * b.palesz;
            if (o + 3 > n || (b.off >= b.paloff && o + 3 > b.off)) break;
            pal[i] = 0xFF000000u | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 1] << 8 | d[o];
        }
    }
    uint64_t stride = ((uint64_t)W * b.bpp + 31) / 32 * 4;
    if (b.off > n || (uint64_t)(n - b.off) < stride * (uint64_t)(H - 1) + ((uint64_t)W * b.bpp + 7) / 8)
        return IMG_ETRUNC;
    uint32_t *px = ia_alloc(a, (size_t)W * H * 4);
    if (!px) return IMG_ENOMEM;
    for (int y = 0; y < H; y++) {
        const uint8_t *row = d + b.off + (size_t)stride * (size_t)(b.topdown ? y : H - 1 - y);
        uint32_t *o = px + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t p;
            switch (b.bpp) {
            case 1: o[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; continue;
            case 4: o[x] = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; continue;
            case 8: o[x] = pal[row[x]]; continue;
            case 24:
                o[x] = 0xFF000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
                continue;
            case 16: p = rd_le16(row + x * 2); break;
            default: p = rd_le32(row + x * 4); break;
            }
            o[x] = mask_val(p, b.mask[3], 255) << 24 | mask_val(p, b.mask[0], 0) << 16 |
                   mask_val(p, b.mask[1], 0) << 8 | mask_val(p, b.mask[2], 0);
        }
    }
    out->px = px;
    out->w = W;
    out->h = H;
    return 0;
}
