/* PNG decoder: all colour types and bit depths, Adam7, tRNS. */
#include "img_int.h"

static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

#define CT(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

int png_probe(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 24 || memcmp(d, png_sig, 8) || rd_be32(d + 12) != CT('I', 'H', 'D', 'R')) return IMG_EFORMAT;
    uint32_t W = rd_be32(d + 16), H = rd_be32(d + 20);
    if (W > 0x7fffffff || H > 0x7fffffff) return IMG_ETOOBIG;
    int r = img_check_dims(W, H);
    if (r) return r;
    if (w) *w = (int)W;
    if (h) *h = (int)H;
    return 0;
}

/* Iterate chunks. Returns 1 with type, body and clen set, 0 at end of data, <0 on error. */
static int next_chunk(const uint8_t *d, size_t n, size_t *pos, uint32_t *type, const uint8_t **body, uint32_t *clen) {
    if (*pos >= n) return 0;
    if (n - *pos < 12) return IMG_ETRUNC;
    uint32_t l = rd_be32(d + *pos);
    if (l > 0x7fffffff) return IMG_ECORRUPT;
    if (n - *pos - 12 < l) return IMG_ETRUNC;
    *type = rd_be32(d + *pos + 4);
    *body = d + *pos + 8;
    *clen = l;
    *pos += 12 + (size_t)l;
    return 1;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static int unfilter(uint8_t *row, const uint8_t *prev, size_t rb, int bpp) {
    int f = row[0];
    uint8_t *x = row + 1;
    const uint8_t *p = prev ? prev + 1 : 0;
    size_t i;
    switch (f) {
    case 0: break;
    case 1: for (i = (size_t)bpp; i < rb; i++) x[i] = (uint8_t)(x[i] + x[i - bpp]); break;
    case 2: if (p) for (i = 0; i < rb; i++) x[i] = (uint8_t)(x[i] + p[i]); break;
    case 3:
        for (i = 0; i < rb; i++) {
            int a = i >= (size_t)bpp ? x[i - bpp] : 0, b = p ? p[i] : 0;
            x[i] = (uint8_t)(x[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (i = 0; i < rb; i++) {
            int a = i >= (size_t)bpp ? x[i - bpp] : 0, b = p ? p[i] : 0, c = (p && i >= (size_t)bpp) ? p[i - bpp] : 0;
            x[i] = (uint8_t)(x[i] + paeth(a, b, c));
        }
        break;
    default: return IMG_ECORRUPT;
    }
    return 0;
}

struct pngi {
    int w, h, depth, ct, interlace, ch;
    int npal, ntrns, has_trns;
    uint32_t pal[256];
    uint8_t trns[256];
    unsigned tkey[3];
};

static unsigned sample(const uint8_t *x, int depth, size_t idx) {
    switch (depth) {
    case 8: return x[idx];
    case 16: return (unsigned)x[idx * 2] << 8 | x[idx * 2 + 1];
    default: {
        size_t bit = idx * (size_t)depth;
        return (x[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    }
    }
}
static unsigned to8(unsigned v, int depth) {
    switch (depth) {
    case 1: return v * 255;
    case 2: return v * 85;
    case 4: return v * 17;
    case 16: return v >> 8;
    default: return v;
    }
}

static void emit_row(const struct pngi *p, const uint8_t *x, uint32_t *out, int pw, int dx) {
    int d = p->depth;
    for (int i = 0; i < pw; i++, out += dx) {
        unsigned r, g, b, a = 255;
        switch (p->ct) {
        case 0:
            g = sample(x, d, (size_t)i);
            if (p->has_trns && g == p->tkey[0]) a = 0;
            r = b = g = to8(g, d);
            break;
        case 2:
            r = sample(x, d, (size_t)i * 3);
            g = sample(x, d, (size_t)i * 3 + 1);
            b = sample(x, d, (size_t)i * 3 + 2);
            if (p->has_trns && r == p->tkey[0] && g == p->tkey[1] && b == p->tkey[2]) a = 0;
            r = to8(r, d); g = to8(g, d); b = to8(b, d);
            break;
        case 3: {
            unsigned idx = sample(x, d, (size_t)i);
            if ((int)idx < p->npal) {
                *out = p->pal[idx] | (uint32_t)((int)idx < p->ntrns ? p->trns[idx] : 255) << 24;
            } else {
                *out = 0xFF000000u;
            }
            continue;
        }
        case 4:
            r = g = b = to8(sample(x, d, (size_t)i * 2), d);
            a = to8(sample(x, d, (size_t)i * 2 + 1), d);
            break;
        default:
            r = to8(sample(x, d, (size_t)i * 4), d);
            g = to8(sample(x, d, (size_t)i * 4 + 1), d);
            b = to8(sample(x, d, (size_t)i * 4 + 2), d);
            a = to8(sample(x, d, (size_t)i * 4 + 3), d);
            break;
        }
        *out = a << 24 | r << 16 | g << 8 | b;
    }
}

static const uint8_t a7_xs[7] = {0, 4, 0, 2, 0, 1, 0}, a7_ys[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t a7_dx[7] = {8, 8, 4, 4, 2, 2, 1}, a7_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static int png_decode_inner(const uint8_t *d, size_t n, struct pngi *p, struct image *out,
                            const struct img_alloc *a, uint8_t **idatp, uint8_t **rawp) {
    int r = png_probe(d, n, 0, 0);
    if (r) return r;
    size_t pos = 8, first_idat = 0;
    uint64_t idat_total = 0;
    int seen_ihdr = 0, seen_plte = 0, seen_idat = 0, idat_done = 0;
    uint32_t type, clen;
    const uint8_t *b;
    for (;;) {
        size_t cpos = pos;
        r = next_chunk(d, n, &pos, &type, &b, &clen);
        if (r < 0) {
            if (seen_idat) break;             /* tolerate junk after image data */
            return r;
        }
        if (r == 0) break;
        if (!seen_ihdr) {
            if (type != CT('I', 'H', 'D', 'R') || clen != 13) return IMG_ECORRUPT;
            p->w = (int)rd_be32(b);
            p->h = (int)rd_be32(b + 4);
            p->depth = b[8];
            p->ct = b[9];
            p->interlace = b[12];
            if (b[10] != 0 || b[11] != 0 || p->interlace > 1) return IMG_ECORRUPT;
            int dp = p->depth, ok = 0;
            switch (p->ct) {
            case 0: ok = dp == 1 || dp == 2 || dp == 4 || dp == 8 || dp == 16; p->ch = 1; break;
            case 3: ok = dp == 1 || dp == 2 || dp == 4 || dp == 8; p->ch = 1; break;
            case 2: ok = dp == 8 || dp == 16; p->ch = 3; break;
            case 4: ok = dp == 8 || dp == 16; p->ch = 2; break;
            case 6: ok = dp == 8 || dp == 16; p->ch = 4; break;
            }
            if (!ok) return IMG_ECORRUPT;
            seen_ihdr = 1;
            continue;
        }
        if (type == CT('I', 'D', 'A', 'T')) {
            if (idat_done) continue;          /* stray IDAT after other chunks: ignore */
            if (!seen_idat) first_idat = cpos;
            seen_idat = 1;
            idat_total += clen;
            continue;
        }
        if (seen_idat) idat_done = 1;
        if (type == CT('I', 'E', 'N', 'D')) break;
        if (type == CT('P', 'L', 'T', 'E')) {
            if (seen_plte || clen % 3 || clen / 3 > 256 || clen == 0) {
                if (p->ct == 3) return IMG_ECORRUPT;
                continue;
            }
            p->npal = (int)(clen / 3);
            for (int i = 0; i < p->npal; i++)
                p->pal[i] = (uint32_t)b[i * 3] << 16 | (uint32_t)b[i * 3 + 1] << 8 | b[i * 3 + 2];
            seen_plte = 1;
        } else if (type == CT('t', 'R', 'N', 'S')) {
            if (p->ct == 3) {
                p->ntrns = clen > 256 ? 256 : (int)clen;
                memcpy(p->trns, b, (size_t)p->ntrns);
            } else if (p->ct == 0 && clen >= 2) {
                p->has_trns = 1;
                p->tkey[0] = rd_be16(b);
            } else if (p->ct == 2 && clen >= 6) {
                p->has_trns = 1;
                p->tkey[0] = rd_be16(b);
                p->tkey[1] = rd_be16(b + 2);
                p->tkey[2] = rd_be16(b + 4);
            }
        } else if (!(type & 0x20000000u)) {
            return IMG_EUNSUP;                /* unknown critical chunk */
        }
    }
    if (!seen_ihdr) return IMG_ECORRUPT;
    if (!seen_idat || idat_total == 0) return IMG_ETRUNC;
    if (p->ct == 3 && !seen_plte) return IMG_ECORRUPT;
    if (idat_total > (uint64_t)n) return IMG_ECORRUPT;

    /* gather IDAT data */
    uint8_t *idat = ia_alloc(a, (size_t)idat_total);
    if (!idat) return IMG_ENOMEM;
    *idatp = idat;
    size_t ip = 0;
    pos = first_idat;
    while (next_chunk(d, n, &pos, &type, &b, &clen) == 1 && type == CT('I', 'D', 'A', 'T')) {
        memcpy(idat + ip, b, clen);
        ip += clen;
    }

    /* raw (filtered) size */
    int bits = p->depth * p->ch, bpp = bits < 8 ? 1 : bits / 8;
    uint64_t total = 0;
    for (int ps = 0; ps < 7; ps++) {
        int xs = p->interlace ? a7_xs[ps] : 0, ys = p->interlace ? a7_ys[ps] : 0;
        int dx = p->interlace ? a7_dx[ps] : 1, dy = p->interlace ? a7_dy[ps] : 1;
        uint64_t pw = p->w > xs ? (uint64_t)(p->w - xs + dx - 1) / dx : 0;
        uint64_t ph = p->h > ys ? (uint64_t)(p->h - ys + dy - 1) / dy : 0;
        if (pw && ph) total += ((pw * bits + 7) / 8 + 1) * ph;
        if (!p->interlace) break;
    }
    uint8_t *raw = ia_alloc(a, (size_t)total);
    if (!raw) return IMG_ENOMEM;
    *rawp = raw;
    long got = img_zlib_inflate(idat, ip, raw, (size_t)total, 0, a);
    if (got < 0) return (int)got;
    if ((uint64_t)got != total) return IMG_ETRUNC;

    uint32_t *px = ia_alloc(a, (size_t)p->w * (size_t)p->h * 4);
    if (!px) return IMG_ENOMEM;
    out->px = px;
    uint8_t *row = raw;
    for (int ps = 0; ps < 7; ps++) {
        int xs = p->interlace ? a7_xs[ps] : 0, ys = p->interlace ? a7_ys[ps] : 0;
        int dx = p->interlace ? a7_dx[ps] : 1, dy = p->interlace ? a7_dy[ps] : 1;
        int pw = p->w > xs ? (p->w - xs + dx - 1) / dx : 0;
        int ph = p->h > ys ? (p->h - ys + dy - 1) / dy : 0;
        if (pw && ph) {
            size_t rb = ((size_t)pw * bits + 7) / 8;
            uint8_t *prev = 0;
            for (int y = 0; y < ph; y++) {
                r = unfilter(row, prev, rb, bpp);
                if (r) return r;
                emit_row(p, row + 1, px + (size_t)(ys + y * dy) * p->w + xs, pw, dx);
                prev = row;
                row += rb + 1;
            }
        }
        if (!p->interlace) break;
    }
    out->w = p->w;
    out->h = p->h;
    return 0;
}

int png_decode(const uint8_t *d, size_t n, struct image *out, const struct img_alloc *a) {
    if (!d || !out) return IMG_EARG;
    out->px = 0;
    out->w = out->h = 0;
    struct pngi *p = ia_zalloc(a, sizeof *p);
    if (!p) return IMG_ENOMEM;
    uint8_t *idat = 0, *raw = 0;
    int r = png_decode_inner(d, n, p, out, a, &idat, &raw);
    ia_free(a, idat);
    ia_free(a, raw);
    ia_free(a, p);
    if (r) {
        ia_free(a, out->px);
        out->px = 0;
        out->w = out->h = 0;
    }
    return r;
}
