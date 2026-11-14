#include "kernel.h"

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

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_write(buf);
}
