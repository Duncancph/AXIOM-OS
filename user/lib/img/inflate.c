/* DEFLATE / zlib / gzip decompression, CRC32 and Adler32. */
#include "img_int.h"

/* ---------------- checksums ---------------- */
static uint32_t crc_tab[256];
static int crc_ready;

uint32_t img_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t img_adler32(uint32_t adler, const uint8_t *p, size_t n) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return b << 16 | a;
}

/* ---------------- inflate ---------------- */
#define FASTBITS 9
struct huff {
    uint16_t fast[1 << FASTBITS];   /* sym << 4 | len; 0 = use the slow path */
    uint16_t count[16];
    uint16_t sym[288];
};

struct inf {
    const uint8_t *in;
    size_t inlen, pos;
    uint64_t bb;
    int bc;
    uint8_t *out;
    size_t outmax, outpos;
    struct huff lit, dist;
    uint8_t lens[320];
};

static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                   4097,6145,8193,12289,16385,24577};
static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void refill(struct inf *s) {
    while (s->bc <= 56) {
        uint64_t b = s->pos < s->inlen ? s->in[s->pos] : 0;
        s->pos++;
        s->bb |= b << s->bc;
        s->bc += 8;
    }
}
static int overrun(const struct inf *s) {
    return (uint64_t)s->pos * 8 - (uint64_t)s->bc > (uint64_t)s->inlen * 8;
}
static unsigned getbits(struct inf *s, int n) {
    unsigned v = (unsigned)(s->bb & ((1ull << n) - 1));
    s->bb >>= n;
    s->bc -= n;
    return v;
}

static int build(struct huff *h, const uint8_t *lens, int n) {
    uint16_t offs[16], next[16];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;          /* over-subscribed */
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    unsigned code = 0;
    for (int l = 1; l < 16; l++) {
        next[l] = (uint16_t)code;
        code = (code + h->count[l]) << 1;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        h->sym[offs[l]++] = (uint16_t)i;
        unsigned c = next[l]++;
        if (l <= FASTBITS) {
            unsigned r = 0;
            for (int k = 0; k < l; k++) r |= ((c >> k) & 1) << (l - 1 - k);
            for (unsigned j = r; j < (1u << FASTBITS); j += 1u << l) h->fast[j] = (uint16_t)(i << 4 | l);
        }
    }
    return 0;
}

static int decode(struct inf *s, const struct huff *h) {
    unsigned e = h->fast[s->bb & ((1u << FASTBITS) - 1)];
    if (e) { s->bb >>= e & 15; s->bc -= e & 15; return (int)(e >> 4); }
    int code = 0, first = 0, index = 0;
    uint64_t b = s->bb;
    for (int len = 1; len <= 15; len++) {
        code |= (int)(b & 1);
        b >>= 1;
        int count = h->count[len];
        if (code - count < first) {
            s->bb >>= len;
            s->bc -= len;
            return h->sym[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int fixed_tables(struct inf *s) {
    int i = 0;
    for (; i < 144; i++) s->lens[i] = 8;
    for (; i < 256; i++) s->lens[i] = 9;
    for (; i < 280; i++) s->lens[i] = 7;
    for (; i < 288; i++) s->lens[i] = 8;
    for (i = 0; i < 30; i++) s->lens[288 + i] = 5;
    if (build(&s->lit, s->lens, 288) || build(&s->dist, s->lens + 288, 30)) return IMG_ECORRUPT;
    return 0;
}

static int dynamic_tables(struct inf *s) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    refill(s);
    int hlit = (int)getbits(s, 5) + 257, hdist = (int)getbits(s, 5) + 1, hclen = (int)getbits(s, 4) + 4;
    if (hlit > 286 || hdist > 30) return IMG_ECORRUPT;
    memset(s->lens, 0, sizeof s->lens);
    refill(s);
    for (int i = 0; i < hclen; i++) s->lens[order[i]] = (uint8_t)getbits(s, 3);
    if (build(&s->lit, s->lens, 19)) return IMG_ECORRUPT;
    int n = hlit + hdist, i = 0;
    uint8_t ln[320];
    while (i < n) {
        refill(s);
        if (overrun(s)) return IMG_ETRUNC;
        int sym = decode(s, &s->lit);
        if (sym < 0) return IMG_ECORRUPT;
        if (sym < 16) { ln[i++] = (uint8_t)sym; continue; }
        int rep, val = 0;
        if (sym == 16) {
            if (i == 0) return IMG_ECORRUPT;
            val = ln[i - 1];
            rep = 3 + (int)getbits(s, 2);
        } else if (sym == 17) rep = 3 + (int)getbits(s, 3);
        else rep = 11 + (int)getbits(s, 7);
        if (i + rep > n) return IMG_ECORRUPT;
        while (rep--) ln[i++] = (uint8_t)val;
    }
    if (ln[256] == 0) return IMG_ECORRUPT;
    if (build(&s->lit, ln, hlit) || build(&s->dist, ln + hlit, hdist)) return IMG_ECORRUPT;
    return 0;
}

static int codes(struct inf *s) {
    for (;;) {
        refill(s);
        if (overrun(s)) return IMG_ETRUNC;
        int sym = decode(s, &s->lit);
        if (sym < 0) return IMG_ECORRUPT;
        if (sym < 256) {
            if (s->outpos >= s->outmax) return IMG_ESPACE;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return IMG_ECORRUPT;
            size_t len = lbase[sym] + getbits(s, lext[sym]);
            int d = decode(s, &s->dist);
            if (d < 0 || d >= 30) return IMG_ECORRUPT;
            size_t dist = dbase[d] + getbits(s, dext[d]);
            if (dist > s->outpos) return IMG_ECORRUPT;
            if (len > s->outmax - s->outpos) return IMG_ESPACE;
            uint8_t *o = s->out + s->outpos;
            const uint8_t *src = o - dist;
            s->outpos += len;
            if (dist >= len) memcpy(o, src, len);
            else while (len--) *o++ = *src++;
        }
    }
}

static long inflate_run(struct inf *s, size_t *consumed) {
    int last;
    do {
        refill(s);
        if (overrun(s)) return IMG_ETRUNC;
        last = (int)getbits(s, 1);
        int type = (int)getbits(s, 2);
        int r;
        if (type == 0) {
            int drop = s->bc & 7;
            s->bb >>= drop;
            s->bc -= drop;
            s->pos -= (size_t)(s->bc / 8);
            s->bb = 0;
            s->bc = 0;
            if (s->pos > s->inlen || s->inlen - s->pos < 4) return IMG_ETRUNC;
            unsigned len = rd_le16(s->in + s->pos), nlen = rd_le16(s->in + s->pos + 2);
            if ((len ^ 0xFFFF) != nlen) return IMG_ECORRUPT;
            s->pos += 4;
            if (s->inlen - s->pos < len) return IMG_ETRUNC;
            if (s->outmax - s->outpos < len) return IMG_ESPACE;
            memcpy(s->out + s->outpos, s->in + s->pos, len);
            s->outpos += len;
            s->pos += len;
            continue;
        } else if (type == 1) {
            r = fixed_tables(s);
        } else if (type == 2) {
            r = dynamic_tables(s);
        } else {
            return IMG_ECORRUPT;
        }
        if (r) return r;
        r = codes(s);
        if (r) return r;
    } while (!last);
    if (overrun(s)) return IMG_ETRUNC;
    if (consumed) *consumed = (size_t)(((uint64_t)s->pos * 8 - (uint64_t)s->bc + 7) / 8);
    return (long)s->outpos;
}

static long inflate_raw_part(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *consumed,
                             size_t *produced, const struct img_alloc *a);
long inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *consumed,
                 const struct img_alloc *a) { return inflate_raw_part(in, inlen, out, outmax, consumed, 0, a); }
static long inflate_raw_part(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *consumed,
                             size_t *produced, const struct img_alloc *a) {
    if (!in && inlen) return IMG_EARG;
    if (!out && outmax) return IMG_EARG;
    struct inf *s = ia_alloc(a, sizeof *s);
    if (!s) return IMG_ENOMEM;
    s->in = in; s->inlen = inlen; s->pos = 0;
    s->bb = 0; s->bc = 0;
    s->out = out; s->outmax = outmax; s->outpos = 0;
    long r = inflate_run(s, consumed);
    if (produced) *produced = s->outpos;
    ia_free(a, s);
    return r;
}

long img_zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, int check_adler,
                      const struct img_alloc *a) {
    if (inlen < 2) return IMG_ETRUNC;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 0x20)) return IMG_ECORRUPT;
    size_t used = 0;
    long r = inflate_raw(in + 2, inlen - 2, out, outmax, &used, a);
    if (r < 0 || !check_adler) return r;
    if (inlen - 2 - used < 4) return IMG_ETRUNC;
    if (rd_be32(in + 2 + used) != img_adler32(1, out, (size_t)r)) return IMG_ECHECK;
    return r;
}

long zlib_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, const struct img_alloc *a) {
    return img_zlib_inflate(in, inlen, out, outmax, 1, a);
}

long gzip_size(const uint8_t *in, size_t inlen) {
    if (!in || inlen < 18 || in[0] != 0x1f || in[1] != 0x8b) return IMG_ECORRUPT;
    return (long)rd_le32(in + inlen - 4);
}

long gunzip(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, const struct img_alloc *a) {
    size_t pos = 0, total = 0;
    int members = 0;
    while (pos < inlen) {
        const uint8_t *p = in + pos;
        size_t n = inlen - pos;
        if (members && (n < 2 || p[0] != 0x1f || p[1] != 0x8b)) break;  /* trailing garbage */
        if (n < 10) return IMG_ETRUNC;
        if (p[0] != 0x1f || p[1] != 0x8b) return IMG_EFORMAT;
        if (p[2] != 8) return IMG_EUNSUP;
        unsigned flg = p[3];
        if (flg & 0xE0) return IMG_ECORRUPT;
        size_t i = 10;
        if (flg & 4) {
            if (n - i < 2) return IMG_ETRUNC;
            size_t xl = rd_le16(p + i);
            i += 2;
            if (n - i < xl) return IMG_ETRUNC;
            i += xl;
        }
        for (int f = 8; f <= 16; f <<= 1) {
            if (!(flg & f)) continue;
            while (i < n && p[i]) i++;
            if (i >= n) return IMG_ETRUNC;
            i++;
        }
        if (flg & 2) {
            if (n - i < 2) return IMG_ETRUNC;
            i += 2;
        }
        size_t used = 0;
        long r = inflate_raw(p + i, n - i, out ? out + total : out, outmax - total, &used, a);
        if (r < 0) return r;
        i += used;
        if (n - i < 8) return IMG_ETRUNC;
        if (rd_le32(p + i) != img_crc32(0, out ? out + total : out, (size_t)r)) return IMG_ECHECK;
        if (rd_le32(p + i + 4) != (uint32_t)r) return IMG_ECHECK;
        total += (size_t)r;
        pos += i + 8;
        members++;
    }
    if (!members) return IMG_ETRUNC;
    return (long)total;
}

/* Like gunzip, but the way browsers read web pages: what could be unpacked is kept when the data ends early
 * or a checksum doesn't match, bytes after the last member are ignored. *ok_all tells whether it was all
 * clean. IMG_ESPACE: out is too small (try a bigger one). */
long gunzip_lenient(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, const struct img_alloc *a, int *ok_all) {
    size_t pos = 0, total = 0;
    int members = 0;
    if (ok_all) *ok_all = 1;
    while (pos < inlen) {
        const uint8_t *p = in + pos;
        size_t n = inlen - pos;
        if (n < 10 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) break;     /* not (another) gzip member */
        unsigned flg = p[3];
        size_t i = 10;
        if (flg & 4) { if (n - i < 2) break; size_t xl = rd_le16(p + i); i += 2; if (n - i < xl) break; i += xl; }
        for (int f = 8; f <= 16; f <<= 1) { if (!(flg & f)) continue; while (i < n && p[i]) i++; if (i >= n) break; i++; }
        if (flg & 2) i += 2;
        if (i >= n) break;
        size_t used = 0, made = 0;
        long r = inflate_raw_part(p + i, n - i, out + total, outmax - total, &used, &made, a);
        if (r == IMG_ESPACE) return IMG_ESPACE;
        if (r < 0) { total += made; members++; if (ok_all) *ok_all = 0; break; }     /* keep what came out */
        i += used;
        if (n - i < 8 || rd_le32(p + i) != img_crc32(0, out + total, (size_t)r)) { if (ok_all) *ok_all = 0; }
        total += (size_t)r;
        members++;
        pos += i + 8;
        if (n - i < 8) break;
    }
    if (!members) return IMG_EFORMAT;
    return (long)total;
}
