/* IPv4 / IPv6 address parsing and formatting (RFC 4291 text forms, RFC 5952 output). */
#include <stdbool.h>
#include "inet.h"
#include "str.h"

static int parse4(const char *s, uint8_t out[4], const char **end) {
    for (int k = 0; k < 4; k++) {
        if (*s < '0' || *s > '9') return false;
        unsigned v = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s++ - '0'); if (++digits > 3 || v > 255) return false; }
        if (digits > 1 && s[-digits] == '0') return false;   /* "01" could mean octal: refuse it */
        out[k] = (uint8_t)v;
        if (k < 3) { if (*s != '.') return false; s++; }
    }
    *end = s;
    return true;
}

static int has_colon(const char *s) { for (; *s; s++) if (*s == ':') return true; return false; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse6(const char *s, uint8_t out[16]) {
    uint8_t w[16]; int n = 0, gap = -1;          /* n bytes written; gap = byte index of "::" */
    if (s[0] == ':') { if (s[1] != ':') return false; s += 2; gap = 0; if (!*s) goto done; }
    for (;;) {
        /* an embedded IPv4 address may end the address */
        const char *e; uint8_t v4[4];
        const char *p = s; while (hexval(*p) >= 0) p++;
        if (*p == '.') {
            if (n > 12 || !parse4(s, v4, &e) || *e) return false;
            memcpy(w + n, v4, 4); n += 4;
            break;
        }
        int digits = 0; unsigned v = 0;
        while (hexval(*s) >= 0) { v = v << 4 | (unsigned)hexval(*s++); if (++digits > 4) return false; }
        if (!digits || n > 14) return false;
        w[n++] = (uint8_t)(v >> 8); w[n++] = (uint8_t)v;
        if (!*s) break;
        if (*s != ':') return false;
        s++;
        if (*s == ':') {
            if (gap >= 0) return false;          /* only one "::" */
            gap = n; s++;
            if (!*s) break;
        }
    }
done:
    if (gap < 0) { if (n != 16) return false; memcpy(out, w, 16); return true; }
    if (n > 14) return false;                    /* "::" must stand for at least one group */
    memset(out, 0, 16);
    memcpy(out, w, gap);
    memcpy(out + 16 - (n - gap), w + gap, n - gap);
    return true;
}

int ip_parse(const char *s, struct ipaddr *o) {
    struct ipaddr a; memset(&a, 0, sizeof a);
    const char *e;
    if (parse4(s, a.b, &e) && !*e) { *o = a; return true; }
    char buf[IP_STRLEN]; int i = 0;              /* drop a zone ("fe80::1%eth0"), which DuncOS doesn't need */
    for (; s[i] && s[i] != '%' && i < IP_STRLEN - 1; i++) buf[i] = s[i];
    if (s[i] && s[i] != '%') return false;
    buf[i] = 0;
    if (!has_colon(buf) || !parse6(buf, a.b)) return false;
    a.v6 = 1; *o = a;
    return true;
}

int ip_format(const struct ipaddr *a, char *out) {
    if (!a->v6) return ksnprintf(out, IP_STRLEN, "%u.%u.%u.%u", a->b[0], a->b[1], a->b[2], a->b[3]);
    const uint8_t *b = a->b;
    uint16_t g[8]; for (int i = 0; i < 8; i++) g[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    /* IPv4-mapped */
    if (!g[0] && !g[1] && !g[2] && !g[3] && !g[4] && g[5] == 0xFFFF)
        return ksnprintf(out, IP_STRLEN, "::ffff:%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    int best = -1, bestlen = 1;                  /* longest run of zero groups, at least 2 long, first wins */
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i; while (j < 8 && !g[j]) j++;
        if (j - i > bestlen) { best = i; bestlen = j - i; }
        i = j;
    }
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best) { out[o++] = ':'; if (i == 0) out[o++] = ':'; i += bestlen - 1; continue; }
        o += ksnprintf(out + o, IP_STRLEN - o, "%x", g[i]);
        if (i < 7) out[o++] = ':';
    }
    out[o] = 0;
    return o;
}

int ip_eq(const struct ipaddr *a, const struct ipaddr *b) {
    return a->v6 == b->v6 && !memcmp(a->b, b->b, a->v6 ? 16 : 4);
}

struct ipaddr ip_v4(uint32_t n) { struct ipaddr a; memset(&a, 0, sizeof a); memcpy(a.b, &n, 4); return a; }
uint32_t ip_to_v4(const struct ipaddr *a) { uint32_t n; memcpy(&n, a->b, 4); return n; }

int ip_same_prefix(const uint8_t *a, const uint8_t *b, int bits) {
    int k = bits / 8;
    if (memcmp(a, b, k)) return false;
    if (bits % 8) { uint8_t m = (uint8_t)(0xFF << (8 - bits % 8)); if ((a[k] & m) != (b[k] & m)) return false; }
    return true;
}
