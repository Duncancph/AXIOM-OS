/* AXIOM MP3 decoder: ID3v2.2/2.3/2.4 and ID3v1 title/artist/album. */
#include "mp3_int.h"

struct sout { char *p; int n, max, full; };

static void put_cp(struct sout *o, uint32_t c) {
    uint8_t b[4];
    int k;
    if (o->full || !o->p) return;
    if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
    if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
    else if (c < 0x800) { b[0] = (uint8_t)(0xc0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 63)); k = 2; }
    else if (c < 0x10000) { b[0] = (uint8_t)(0xe0 | c >> 12); b[1] = (uint8_t)(0x80 | (c >> 6 & 63)); b[2] = (uint8_t)(0x80 | (c & 63)); k = 3; }
    else { b[0] = (uint8_t)(0xf0 | c >> 18); b[1] = (uint8_t)(0x80 | (c >> 12 & 63)); b[2] = (uint8_t)(0x80 | (c >> 6 & 63)); b[3] = (uint8_t)(0x80 | (c & 63)); k = 4; }
    if (o->n + k > o->max - 1) { o->full = 1; return; }
    for (int i = 0; i < k; i++) o->p[o->n++] = (char)b[i];
    o->p[o->n] = 0;
}

/* Decodes a UTF-8 sequence (invalid bytes become U+FFFD); returns bytes consumed. */
static int get_utf8(const uint8_t *s, long n, uint32_t *cp) {
    uint8_t c = s[0];
    int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (k < 0 || k >= n) { *cp = c < 0x80 ? c : 0xfffd; return 1; }
    uint32_t v = k == 0 ? c : (uint32_t)(c & (0x3f >> k));
    for (int i = 1; i <= k; i++) {
        if ((s[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; }
        v = v << 6 | (s[i] & 63);
    }
    *cp = v;
    return k + 1;
}

static void set_text(char *dst, int max, const uint8_t *s, long n, int enc) {
    struct sout o = { dst, 0, max, 0 };
    if (!dst || max <= 0) return;
    dst[0] = 0;
    if (enc == 0) {
        for (long i = 0; i < n && s[i]; i++) put_cp(&o, s[i]);
    } else if (enc == 3) {
        for (long i = 0; i < n && s[i];) {
            uint32_t c;
            i += get_utf8(s + i, n - i, &c);
            put_cp(&o, c);
        }
    } else if (enc == 1 || enc == 2) {
        int be = enc == 2;
        long i = 0;
        if (enc == 1 && n >= 2) {
            if (s[0] == 0xff && s[1] == 0xfe) { be = 0; i = 2; }
            else if (s[0] == 0xfe && s[1] == 0xff) { be = 1; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = be ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
            if (!u) break;
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {
                uint32_t u2 = be ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
                if (u2 >= 0xdc00 && u2 < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (u2 - 0xdc00); i += 2; }
            }
            put_cp(&o, u);
        }
    }
}

/* Removes ID3 unsynchronisation (0xFF 0x00 -> 0xFF) in place; returns the new length. */
static long unsync(uint8_t *p, long n) {
    long w = 0;
    for (long r = 0; r < n; r++) {
        p[w++] = p[r];
        if (p[r] == 0xff && r + 1 < n && p[r + 1] == 0) r++;
    }
    return w;
}

static long ss32(const uint8_t *p) { return (long)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f); }
static long be32l(const uint8_t *p) { return (long)p[0] << 24 | (long)p[1] << 16 | (long)p[2] << 8 | p[3]; }

static void id3v2(const uint8_t *data, long len, char *title, char *artist, char *album, int max) {
    if (len < 10 || memcmp(data, "ID3", 3)) return;
    int ver = data[3], flags = data[5];
    if (ver < 2 || ver > 4) return;
    long size = ss32(data + 6);
    if (size > len - 10) size = len - 10;
    uint8_t *body = malloc((size_t)size + 1);
    if (!body) return;
    memcpy(body, data + 10, (size_t)size);
    if ((flags & 0x80) && ver < 4) size = unsync(body, size);
    long p = 0;
    if ((flags & 0x40) && ver >= 3 && size >= 4) {
        long ext = ver == 3 ? be32l(body) + 4 : ss32(body);
        p = ext < 0 || ext > size ? size : ext;
    }
    int hl = ver == 2 ? 6 : 10;
    while (p + hl <= size) {
        const uint8_t *f = body + p;
        if (!f[0]) break;   /* padding */
        long fs;
        int fflags = 0;
        if (ver == 2) fs = (long)f[3] << 16 | f[4] << 8 | f[5];
        else if (ver == 3) fs = be32l(f + 4);
        else fs = (f[4] | f[5] | f[6] | f[7]) & 0x80 ? be32l(f + 4) : ss32(f + 4);
        if (ver >= 3) fflags = f[9];
        if (fs < 0 || fs > size - p - hl) break;
        uint8_t *fd = body + p + hl;
        long n = fs;
        int skip = 0;
        if (ver == 3) {
            if (fflags & 0xc0) skip = 1;            /* compressed / encrypted */
            if ((fflags & 0x20) && n > 0) { fd++; n--; }
        } else if (ver == 4) {
            if (fflags & 0x0c) skip = 1;
            if ((fflags & 0x40) && n > 0) { fd++; n--; }
            if ((fflags & 0x01) && n >= 4) { fd += 4; n -= 4; }
            if ((fflags & 0x02) || (flags & 0x80)) n = unsync(fd, n);
        }
        char *dst = 0;
        if (ver == 2) {
            if (!memcmp(f, "TT2", 3)) dst = title;
            else if (!memcmp(f, "TP1", 3)) dst = artist;
            else if (!memcmp(f, "TAL", 3)) dst = album;
        } else {
            if (!memcmp(f, "TIT2", 4)) dst = title;
            else if (!memcmp(f, "TPE1", 4)) dst = artist;
            else if (!memcmp(f, "TALB", 4)) dst = album;
        }
        if (dst && !skip && n >= 1 && !dst[0]) set_text(dst, max, fd + 1, n - 1, fd[0]);
        p += hl + fs;
    }
    free(body);
}

static void v1_field(char *dst, int max, const uint8_t *s) {
    if (dst[0]) return;
    int n = 30;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) if (!s[i]) { n = i; break; }
    set_text(dst, max, s, n, 0);
}

void mp3_tags(const uint8_t *data, long len, char *title, char *artist, char *album, int max) {
    if (max <= 0) return;
    if (title) title[0] = 0;
    if (artist) artist[0] = 0;
    if (album) album[0] = 0;
    if (!data || len <= 0) return;
    id3v2(data, len, title, artist, album, max);
    if (len >= 128 && !memcmp(data + len - 128, "TAG", 3)) {
        const uint8_t *t = data + len - 128;
        if (title) v1_field(title, max, t + 3);
        if (artist) v1_field(artist, max, t + 33);
        if (album) v1_field(album, max, t + 63);
    }
}
