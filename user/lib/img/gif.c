/* GIF decoder: GIF87a/89a, first frame composited onto a transparent canvas. */
#include "img_int.h"

struct gifctx {
    uint16_t prefix[4096];
    uint8_t suffix[4096], first[4096], stack[4100];
    uint32_t gpal[256], lpal[256];
};

struct subr {
    const uint8_t *d;
    size_t n, pos, left;
    int done;
};

static int sub_byte(struct subr *s) {
    if (s->done) return -1;
    while (s->left == 0) {
        if (s->pos >= s->n) { s->done = 1; return -1; }
        s->left = s->d[s->pos++];
        if (s->left == 0) { s->done = 1; return -1; }
    }
    if (s->pos >= s->n) { s->done = 1; return -1; }
    s->left--;
    return s->d[s->pos++];
}

/* Skip a chain of sub-blocks; returns new position or n if truncated. */
static size_t skip_subblocks(const uint8_t *d, size_t n, size_t pos) {
    while (pos < n) {
        size_t l = d[pos++];
        if (l == 0) return pos;
        if (n - pos < l) return n;
        pos += l;
    }
    return n;
}

int gif_probe(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 13 || memcmp(d, "GIF", 3) || (memcmp(d + 3, "87a", 3) && memcmp(d + 3, "89a", 3))) return IMG_EFORMAT;
    int W = (int)rd_le16(d + 6), H = (int)rd_le16(d + 8);
    int r = img_check_dims(W, H);
    if (r) return r;
    if (w) *w = W;
    if (h) *h = H;
    return 0;
}

static void read_pal(uint32_t *pal, const uint8_t *p, int cnt) {
    for (int i = 0; i < 256; i++)
        pal[i] = i < cnt ? 0xFF000000u | (uint32_t)p[i * 3] << 16 | (uint32_t)p[i * 3 + 1] << 8 | p[i * 3 + 2] : 0xFF000000u;
}

/* Decode LZW into idx[0..npix); returns number of pixels produced. */
static size_t lzw(struct gifctx *g, struct subr *s, int mcs, uint8_t *idx, size_t npix) {
    int clear = 1 << mcs, eoi = clear + 1;
    int csize = mcs + 1, next = clear + 2, prev = -1;
    uint32_t bits = 0;
    int nbits = 0;
    size_t cnt = 0;
    for (int i = 0; i < clear; i++) {
        g->prefix[i] = 0xFFFF;
        g->suffix[i] = (uint8_t)i;
        g->first[i] = (uint8_t)i;
    }
    while (cnt < npix) {
        while (nbits < csize) {
            int b = sub_byte(s);
            if (b < 0) return cnt;
            bits |= (uint32_t)b << nbits;
            nbits += 8;
        }
        int code = (int)(bits & ((1u << csize) - 1));
        bits >>= csize;
        nbits -= csize;
        if (code == clear) {
            csize = mcs + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) break;
            idx[cnt++] = (uint8_t)code;
            prev = code;
            continue;
        }
        if (code > next || (code == next && next >= 4096)) break;
        if (next < 4096) {
            g->prefix[next] = (uint16_t)prev;
            g->suffix[next] = code < next ? g->first[code] : g->first[prev];
            g->first[next] = g->first[prev];
            next++;
            if (next == (1 << csize) && csize < 12) csize++;
        }
        int sp = 0, c = code;
        while (c >= clear && sp < 4096) {
            g->stack[sp++] = g->suffix[c];
            c = g->prefix[c];
        }
        if (c >= clear) break;
        g->stack[sp++] = (uint8_t)c;
        while (sp && cnt < npix) idx[cnt++] = g->stack[--sp];
        prev = code;
    }
    return cnt;
}

static int gif_inner(const uint8_t *d, size_t n, struct gifctx *g, struct image *out, const struct img_alloc *a,
                     uint8_t **idxp) {
    int W, H;
    if (n < 13 || memcmp(d, "GIF", 3) || (memcmp(d + 3, "87a", 3) && memcmp(d + 3, "89a", 3))) return IMG_EFORMAT;
    W = (int)rd_le16(d + 6);
    H = (int)rd_le16(d + 8);
    size_t pos = 13;
    int flags = d[10];
    int has_gpal = flags & 0x80, gcnt = 2 << (flags & 7);
    if (has_gpal) {
        if (n - pos < (size_t)gcnt * 3) return IMG_ETRUNC;
        read_pal(g->gpal, d + pos, gcnt);
        pos += (size_t)gcnt * 3;
    }
    int tidx = -1;
    for (;;) {
        if (pos >= n) return IMG_ETRUNC;
        int b = d[pos++];
        if (b == 0x3B) return IMG_ECORRUPT;      /* trailer before any image */
        if (b == 0x21) {
            if (pos >= n) return IMG_ETRUNC;
            int label = d[pos++];
            if (label == 0xF9 && pos + 5 <= n && d[pos] >= 4) {
                tidx = (d[pos + 1] & 1) ? d[pos + 4] : -1;
            }
            pos = skip_subblocks(d, n, pos);
            continue;
        }
        if (b != 0x2C) return IMG_ECORRUPT;
        if (n - pos < 10) return IMG_ETRUNC;
        int fx = (int)rd_le16(d + pos), fy = (int)rd_le16(d + pos + 2);
        int fw = (int)rd_le16(d + pos + 4), fh = (int)rd_le16(d + pos + 6);
        int lf = d[pos + 8];
        pos += 9;
        const uint32_t *pal = has_gpal ? g->gpal : 0;
        if (lf & 0x80) {
            int lcnt = 2 << (lf & 7);
            if (n - pos < (size_t)lcnt * 3) return IMG_ETRUNC;
            read_pal(g->lpal, d + pos, lcnt);
            pos += (size_t)lcnt * 3;
            pal = g->lpal;
        }
        if (!pal) {
            /* no colour table: use a grey ramp */
            for (int i = 0; i < 256; i++) g->lpal[i] = 0xFF000000u | (uint32_t)i * 0x010101u;
            pal = g->lpal;
        }
        if (W == 0 || H == 0) { W = fx + fw; H = fy + fh; }
        int r = img_check_dims(W, H);
        if (r) return r;
        if (pos >= n) return IMG_ETRUNC;
        int mcs = d[pos++];
        if (mcs < 1 || mcs > 11) return IMG_ECORRUPT;
        uint32_t *px = ia_zalloc(a, (size_t)W * H * 4);
        if (!px) return IMG_ENOMEM;
        out->px = px;
        out->w = W;
        out->h = H;
        size_t npix = (size_t)fw * fh;
        if (!npix) return 0;
        if (npix > IMG_MAX_PIXELS) return IMG_ETOOBIG;
        uint8_t *idx = ia_alloc(a, npix);
        if (!idx) return IMG_ENOMEM;
        *idxp = idx;
        struct subr s = {d, n, pos, 0, 0};
        size_t cnt = lzw(g, &s, mcs, idx, npix);
        int interlace = lf & 0x40;
        int pass = 0, y = 0;
        static const int istart[4] = {0, 4, 2, 1}, istep[4] = {8, 8, 4, 2};
        size_t k = 0;
        for (int row = 0; row < fh && k < cnt; row++) {
            int yy = interlace ? y : row;
            for (int x = 0; x < fw && k < cnt; x++, k++) {
                int cx = fx + x, cy = fy + yy;
                int ci = idx[k];
                if (ci == tidx || cx >= W || cy >= H) continue;
                px[(size_t)cy * W + cx] = pal[ci];
            }
            if (interlace) {
                y += istep[pass];
                while (y >= fh && pass < 3) {
                    pass++;
                    y = istart[pass];
                }
            }
        }
        return 0;
    }
}

int gif_decode(const uint8_t *d, size_t n, struct image *out, const struct img_alloc *a) {
    if (!d || !out) return IMG_EARG;
    out->px = 0;
    out->w = out->h = 0;
    struct gifctx *g = ia_alloc(a, sizeof *g);
    if (!g) return IMG_ENOMEM;
    uint8_t *idx = 0;
    int r = gif_inner(d, n, g, out, a, &idx);
    ia_free(a, idx);
    ia_free(a, g);
    if (r) {
        ia_free(a, out->px);
        out->px = 0;
        out->w = out->h = 0;
    }
    return r;
}
