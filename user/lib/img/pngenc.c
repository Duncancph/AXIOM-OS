/* PNG encoder for screenshots: adaptive row filters + greedy LZ77 with fixed
 * Huffman codes, falling back to stored blocks where those are smaller.
 * Works without heap memory: filtered bytes are recomputed from the pixels. */
#include "img_int.h"

#define RING   2048          /* rows whose filter type is remembered */
#define HBITS  11
#define LA     512           /* lookahead cache (power of two, > 258) */

struct bitw {
    uint8_t *out;
    size_t max, pos;
    uint64_t bb;
    int bc, over;
};

static void bw_byte(struct bitw *w, unsigned b) {
    if (w->pos < w->max) w->out[w->pos] = (uint8_t)b;
    else w->over = 1;
    w->pos++;
}
static void bw_put(struct bitw *w, unsigned v, int n) {
    w->bb |= (uint64_t)v << w->bc;
    w->bc += n;
    while (w->bc >= 8) {
        bw_byte(w, (unsigned)(w->bb & 255));
        w->bb >>= 8;
        w->bc -= 8;
    }
}
static void bw_align(struct bitw *w) {
    if (w->bc) bw_put(w, 0, 8 - w->bc);
}

struct enc {
    const uint32_t *px;
    int w, h, bpp;
    uint64_t rowlen, total;
    uint64_t ready;               /* rows with a chosen filter */
    uint8_t ft[RING];
    uint64_t pos;                 /* current input offset */
    uint8_t la[LA];
    unsigned la_off, la_n;
    uint32_t ad_a, ad_b, ad_cnt;
};

static inline unsigned rawb(const struct enc *e, uint64_t row, uint64_t i) {
    uint32_t p = e->px[row * (uint64_t)e->w + i / (unsigned)e->bpp];
    switch (i % (unsigned)e->bpp) {
    case 0: return (p >> 16) & 255;
    case 1: return (p >> 8) & 255;
    case 2: return p & 255;
    default: return p >> 24;
    }
}

static inline int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static inline unsigned filt(const struct enc *e, int f, uint64_t row, uint64_t i) {
    int x = (int)rawb(e, row, i);
    if (f == 0) return (unsigned)x;
    int bpp = e->bpp;
    int a = i >= (uint64_t)bpp ? (int)rawb(e, row, i - bpp) : 0;
    int b = row ? (int)rawb(e, row - 1, i) : 0;
    switch (f) {
    case 1: return (unsigned)(x - a) & 255;
    case 2: return (unsigned)(x - b) & 255;
    case 3: return (unsigned)(x - ((a + b) >> 1)) & 255;
    default: {
        int c = (row && i >= (uint64_t)bpp) ? (int)rawb(e, row - 1, i - bpp) : 0;
        return (unsigned)(x - paeth(a, b, c)) & 255;
    }
    }
}

static void choose_filter(struct enc *e, uint64_t row) {
    uint64_t best = ~0ull;
    int bf = 0;
    uint64_t n = e->rowlen - 1;
    for (int f = 0; f < 5; f++) {
        uint64_t s = 0;
        for (uint64_t i = 0; i < n && s < best; i++) {
            unsigned v = filt(e, f, row, i);
            s += v < 128 ? v : 256 - v;
        }
        if (s < best) { best = s; bf = f; }
    }
    e->ft[row & (RING - 1)] = (uint8_t)bf;
}

static inline int ftype(struct enc *e, uint64_t row) {
    while (e->ready <= row) choose_filter(e, e->ready++);
    return e->ft[row & (RING - 1)];
}

static unsigned fbyte(struct enc *e, uint64_t off) {
    uint64_t row = off / e->rowlen, col = off % e->rowlen;
    int f = ftype(e, row);
    if (col == 0) return (unsigned)f;
    return filt(e, f, row, col - 1);
}

static inline unsigned la_get(struct enc *e, unsigned k) {
    while (e->la_n <= k) {
        e->la[(e->la_off + e->la_n) & (LA - 1)] = (uint8_t)fbyte(e, e->pos + e->la_n);
        e->la_n++;
    }
    return e->la[(e->la_off + k) & (LA - 1)];
}

static void advance(struct enc *e, unsigned n) {
    for (unsigned k = 0; k < n; k++) {
        unsigned b = la_get(e, k);
        e->ad_a += b;
        e->ad_b += e->ad_a;
        if (++e->ad_cnt == 5552) {
            e->ad_a %= 65521;
            e->ad_b %= 65521;
            e->ad_cnt = 0;
        }
    }
    e->pos += n;
    e->la_off = (e->la_off + n) & (LA - 1);
    e->la_n -= n;
}

static uint16_t lcode[288];
static uint8_t llen[288];
static int tables_ready;
static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                   4097,6145,8193,12289,16385,24577};
static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static unsigned rev(unsigned c, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; i++) r |= ((c >> i) & 1) << (n - 1 - i);
    return r;
}
static void init_tables(void) {
    if (tables_ready) return;
    for (unsigned v = 0; v < 288; v++) {
        unsigned c;
        int l;
        if (v < 144) { c = 0x30 + v; l = 8; }
        else if (v < 256) { c = 0x190 + v - 144; l = 9; }
        else if (v < 280) { c = v - 256; l = 7; }
        else { c = 0xC0 + v - 280; l = 8; }
        lcode[v] = (uint16_t)rev(c, l);
        llen[v] = (uint8_t)l;
    }
    tables_ready = 1;
}

static unsigned match_len(struct enc *e, uint64_t dist, unsigned maxl) {
    uint64_t s = e->pos - dist;
    unsigned l = 0;
    while (l < maxl && fbyte(e, s + l) == la_get(e, l)) l++;
    return l;
}

static void put_match(struct bitw *w, unsigned len, unsigned dist) {
    int i = 28;
    while (lbase[i] > len) i--;
    bw_put(w, lcode[257 + i], llen[257 + i]);
    if (lext[i]) bw_put(w, len - lbase[i], lext[i]);
    int d = 29;
    while (dbase[d] > dist) d--;
    bw_put(w, rev((unsigned)d, 5), 5);
    if (dext[d]) bw_put(w, dist - dbase[d], dext[d]);
}

static void deflate_png(struct enc *e, struct bitw *w) {
    uint32_t head[1 << HBITS];
    for (int i = 0; i < (1 << HBITS); i++) head[i] = 0xFFFFFFFFu;
    uint64_t blimit = (uint64_t)(RING - 200) * e->rowlen;
    if (blimit > 65535) blimit = 65535;
    while (e->pos < e->total) {
        struct bitw save = *w;
        uint64_t bstart = e->pos;
        bw_put(w, 0, 1);
        bw_put(w, 1, 2);
        while (e->pos < e->total && e->pos - bstart < blimit) {
            uint64_t left = e->total - e->pos, room = blimit - (e->pos - bstart);
            unsigned maxl = (unsigned)(left < 258 ? left : 258);
            if (room < maxl) maxl = (unsigned)room;
            unsigned best = 0, bd = 0;
            uint64_t currow = e->pos / e->rowlen;
            if (maxl >= 3) {
                uint64_t cands[3];
                int nc = 0;
                cands[nc++] = (uint64_t)e->bpp;
                cands[nc++] = e->rowlen;
                uint32_t hv = 0;
                if (maxl >= 4) {
                    hv = (la_get(e, 0) | la_get(e, 1) << 8 | la_get(e, 2) << 16 | la_get(e, 3) << 24) * 2654435761u >>
                         (32 - HBITS);
                    uint32_t c = head[hv];
                    if (c != 0xFFFFFFFFu && c < e->pos) cands[nc++] = e->pos - c;
                    head[hv] = (uint32_t)e->pos;
                }
                for (int i = 0; i < nc; i++) {
                    uint64_t dist = cands[i];
                    if (dist == 0 || dist > 32768 || dist > e->pos) continue;
                    if (currow - (e->pos - dist) / e->rowlen > RING - 128) continue;
                    unsigned l = match_len(e, dist, maxl);
                    if (l > best) { best = l; bd = (unsigned)dist; }
                    if (best == maxl) break;
                }
            }
            if (best >= 3) {
                put_match(w, best, bd);
                advance(e, best);
            } else {
                unsigned b = la_get(e, 0);
                bw_put(w, lcode[b], llen[b]);
                advance(e, 1);
            }
        }
        bw_put(w, 0, 7);   /* end of block */
        uint64_t blen = e->pos - bstart;
        uint64_t fixed_bits = (w->pos - save.pos) * 8 + (uint64_t)w->bc - (uint64_t)save.bc;
        uint64_t stored_bits = 3 + 7 + 32 + blen * 8;
        if (w->over || fixed_bits > stored_bits) {
            *w = save;
            w->over = 0;
            bw_put(w, 0, 1);
            bw_put(w, 0, 2);
            bw_align(w);
            bw_put(w, (unsigned)blen, 16);
            bw_put(w, (unsigned)blen ^ 0xFFFF, 16);
            for (uint64_t i = 0; i < blen; i++) bw_byte(w, fbyte(e, bstart + i));
        }
    }
    bw_put(w, 1, 1);    /* final, empty fixed block */
    bw_put(w, 1, 2);
    bw_put(w, 0, 7);
    bw_align(w);
}

size_t png_encode_bound(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    uint64_t raw = ((uint64_t)w * 4 + 1) * (uint64_t)h;
    return (size_t)(raw + 5 * (raw / 1024 + 4) + 128);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

long png_encode(const uint32_t *px, int w, int h, int opaque, uint8_t *out, size_t outmax) {
    if (!px || !out) return IMG_EARG;
    int r = img_check_dims(w, h);
    if (r) return r;
    init_tables();
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (outmax < 8 + 25 + 8 + 2 + 4 + 4 + 12 + 8) return IMG_ESPACE;
    memcpy(out, sig, 8);
    uint8_t *ih = out + 8;
    put32(ih, 13);
    memcpy(ih + 4, "IHDR", 4);
    put32(ih + 8, (uint32_t)w);
    put32(ih + 12, (uint32_t)h);
    ih[16] = 8;
    ih[17] = opaque ? 2 : 6;
    ih[18] = ih[19] = ih[20] = 0;
    put32(ih + 21, img_crc32(0, ih + 4, 17));
    uint8_t *id = out + 33;
    memcpy(id + 4, "IDAT", 4);
    id[8] = 0x78;
    id[9] = 0x01;

    /* ~5 KiB of encoder state plus a 16 KiB hash table live on the stack (no allocator in this API) */
    struct enc es, *e = &es;
    memset(e, 0, sizeof *e);
    e->px = px;
    e->w = w;
    e->h = h;
    e->bpp = opaque ? 3 : 4;
    e->rowlen = (uint64_t)w * e->bpp + 1;
    e->total = e->rowlen * (uint64_t)h;
    e->ad_a = 1;

    size_t tail = 4 + 4 + 12;   /* adler + crc + IEND */
    struct bitw bw = {out + 43, outmax - 43 - tail, 0, 0, 0, 0};
    deflate_png(e, &bw);
    if (bw.over) return IMG_ESPACE;
    size_t zlen = 2 + bw.pos + 4;
    uint8_t *p = out + 43 + bw.pos;
    put32(p, (e->ad_b % 65521) << 16 | (e->ad_a % 65521));
    put32(id, (uint32_t)zlen);
    put32(p + 4, img_crc32(0, id + 4, 4 + zlen));
    p += 8;
    put32(p, 0);
    memcpy(p + 4, "IEND", 4);
    put32(p + 8, img_crc32(0, p + 4, 4));
    return (long)(p + 12 - out);
}
