/* String and formatting routines shared by the kernel and DuncOS apps. */
#include <stdint.h>
#include "str.h"

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (uint8_t)*a - (uint8_t)*b; if (!*a) return 0; }
    return 0;
}
char *strncpy0(char *d, const char *s, size_t n) {
    size_t i = 0;
    if (!n) return d;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
long katoi(const char *s) {
    long v = 0; int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

struct out { char *buf; size_t n, pos; };
static void emit(struct out *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }

int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, lng = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }
        char tmp[24]; const char *s = tmp; int len = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
            char r[24]; int k = 0;
            do { r[k++] = '0' + u % 10; u /= 10; } while (u);
            if (v < 0) r[k++] = '-';
            while (k) tmp[len++] = r[--k];
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            uint64_t u;
            int base = (*fmt == 'u') ? 10 : 16;
            if (*fmt == 'p') { u = (uint64_t)va_arg(ap, void *); tmp[len++] = '0'; tmp[len++] = 'x'; }
            else u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            const char *dig = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char r[24]; int k = 0;
            do { r[k++] = dig[u % base]; u /= base; } while (u);
            while (k) tmp[len++] = r[--k];
            break;
        }
        case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = strlen(s); break;
        case '%': tmp[0] = '%'; len = 1; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; len = 2; break;
        }
        int pad = width > len ? width - len : 0;
        if (!left) while (pad-- > 0) emit(&o, zero ? '0' : ' ');
        for (int i = 0; i < len; i++) emit(&o, s[i]);
        if (left) while (pad-- > 0) emit(&o, ' ');
    }
    if (o.n) o.buf[o.pos < o.n ? o.pos : o.n - 1] = 0;
    return (int)o.pos;
}

/* Windows-1252 0x80..0x9F as Unicode (0 = unused) */
static const uint16_t cp1252_hi[32] = { 0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000, 0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178 };

int cp1252_from_unicode(unsigned int u) {
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) return (int)u;
    for (int i = 0; i < 32; i++) if (cp1252_hi[i] == u) return 0x80 + i;
    switch (u) {                                     /* near enough */
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    case 0x2032: return '\'';
    }
    return -1;
}

/* If s (n bytes) is valid UTF-8 with at least one multi-byte character, convert it to Windows-1252 in place
 * (characters it lacks become '?') and return the new length; otherwise leave it and return n. */
long utf8_to_cp1252(char *s, long n) {
    const unsigned char *p = (const unsigned char *)s;
    int multi = 0;
    for (long i = 0; i < n;) {
        unsigned c = p[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n) return n;
        for (int k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return n;
        if (len > 1) multi = 1;
        i += len;
    }
    if (!multi) return n;
    long o = 0;
    for (long i = 0; i < n;) {
        unsigned c = p[i]; uint32_t u; int len;
        if (c < 0x80) { u = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { u = (c & 0x1F) << 6 | (p[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0) { u = (c & 0x0F) << 12 | (p[i + 1] & 0x3F) << 6 | (p[i + 2] & 0x3F); len = 3; }
        else { u = (c & 0x07) << 18 | (p[i + 1] & 0x3F) << 12 | (p[i + 2] & 0x3F) << 6 | (p[i + 3] & 0x3F); len = 4; }
        i += len;
        if (u == 0xFEFF) continue;                   /* byte order mark */
        int b = cp1252_from_unicode(u);
        s[o++] = (char)(b < 0 ? '?' : b);
    }
    return o;
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

