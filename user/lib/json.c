/* Minimal JSON helpers for apps: find a member, decode a string, escape text. No allocation.
 * Values are referred to by pointers into the original text; `end` bounds every scan. */
#ifdef JSON_TEST
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#else
#include "dunc.h"
#endif
#include "json.h"

static const char *ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }

static const char *skip_string(const char *p, const char *e) {        /* p at the opening quote */
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return 0;
}

/* the end of the value starting at p (or 0 if it's malformed) */
const char *json_skip(const char *p, const char *e) {
    p = ws(p, e);
    if (p >= e) return 0;
    if (*p == '"') return skip_string(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') { p = skip_string(p, e); if (!p) return 0; p--; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; if (depth < 0) return 0; }
        }
        return 0;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* the value of member `key` of the object at obj (dots go deeper: "delta.text"); 0 if missing */
const char *json_get(const char *obj, const char *e, const char *path) {
    const char *p = ws(obj, e);
    while (*path) {
        const char *dot = path; while (*dot && *dot != '.') dot++;
        size_t kl = (size_t)(dot - path);
        if (p >= e || *p != '{') return 0;
        p++;
        const char *found = 0;
        for (;;) {
            p = ws(p, e);
            if (p >= e || *p == '}') break;
            if (*p != '"') return 0;
            const char *ks = p + 1, *ke = skip_string(p, e);
            if (!ke) return 0;
            bool match = (size_t)(ke - 1 - ks) == kl && !memcmp(ks, path, kl);
            p = ws(ke, e);
            if (p >= e || *p != ':') return 0;
            p = ws(p + 1, e);
            if (match) { found = p; break; }
            p = json_skip(p, e);
            if (!p) return 0;
            p = ws(p, e);
            if (p < e && *p == ',') p++;
        }
        if (!found) return 0;
        p = found;
        path = *dot ? dot + 1 : dot;
    }
    return p;
}

static int hex4(const char *p, const char *e) {
    if (e - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

static long put_utf8(char *out, long o, long max, unsigned u) {
    char b[4]; int n;
    if (u < 0x80) { b[0] = (char)u; n = 1; }
    else if (u < 0x800) { b[0] = (char)(0xC0 | u >> 6); b[1] = (char)(0x80 | (u & 63)); n = 2; }
    else if (u < 0x10000) { b[0] = (char)(0xE0 | u >> 12); b[1] = (char)(0x80 | (u >> 6 & 63)); b[2] = (char)(0x80 | (u & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | u >> 18); b[1] = (char)(0x80 | (u >> 12 & 63)); b[2] = (char)(0x80 | (u >> 6 & 63)); b[3] = (char)(0x80 | (u & 63)); n = 4; }
    for (int i = 0; i < n; i++) { if (o < max) out[o] = b[i]; o++; }
    return o;
}

/* decode the JSON string at v into out (UTF-8, NUL-terminated if it fits); returns the decoded length or -1 */
long json_str(const char *v, const char *e, char *out, long max) {
    v = ws(v, e);
    if (v >= e || *v != '"') return -1;
    long o = 0;
    for (const char *p = v + 1; p < e; p++) {
        char c = *p;
        if (c == '"') { if (o < max) out[o] = 0; else if (max > 0) out[max - 1] = 0; return o; }
        if (c != '\\') { if (o < max) out[o] = c; o++; continue; }
        if (++p >= e) return -1;
        switch (*p) {
        case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
        case 'b': c = '\b'; break; case 'f': c = '\f'; break;
        case 'u': {
            int u = hex4(p + 1, e);
            if (u < 0) return -1;
            p += 4;
            if (u >= 0xD800 && u < 0xDC00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {   /* surrogate pair */
                int lo = hex4(p + 3, e);
                if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            o = put_utf8(out, o, max, (unsigned)u);
            continue;
        }
        default: c = *p;                                    /* \" \\ \/ */
        }
        if (o < max) out[o] = c;
        o++;
    }
    return -1;
}

/* the JSON string form of text (without quotes); returns the length needed */
long json_escape(const char *in, long n, char *out, long max) {
    long o = 0;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        char esc[8]; int k = 0;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; k = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n'; k = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r'; k = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't'; k = 2; }
        else if (c < 0x20) { static const char *hx = "0123456789abcdef"; esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0'; esc[4] = hx[c >> 4]; esc[5] = hx[c & 15]; k = 6; }
        else { esc[0] = (char)c; k = 1; }
        for (int j = 0; j < k; j++) { if (o < max) out[o] = esc[j]; o++; }
    }
    if (o < max) out[o] = 0;
    return o;
}

bool json_is(const char *v, const char *e, const char *s) {           /* is the value at v the string s? */
    v = ws(v, e);
    size_t n = strlen(s);
    return v && v + n + 2 <= e && *v == '"' && !memcmp(v + 1, s, n) && v[n + 1] == '"';
}

long json_int(const char *v, const char *e) {
    v = ws(v, e);
    long sign = 1, x = 0;
    if (v < e && *v == '-') { sign = -1; v++; }
    while (v < e && *v >= '0' && *v <= '9') x = x * 10 + (*v++ - '0');
    return sign * x;
}
