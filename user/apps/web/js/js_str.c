/* AXIOM JavaScript engine: strings (Latin-1 / UTF-16 with lazy ropes), atoms, UTF-8, Unicode tables. */
#include "js_int.h"

JSString *str_alloc8(js_rt *rt, uint32_t len) {
    JSString *s = (JSString *)gc_alloc(rt, sizeof(JSString) + len + 1, GC_STR);
    if (!s) return 0;
    s->len = len;
    s->s8 = (uint8_t *)(s + 1);
    s->s8[len] = 0;
    return s;
}
JSString *str_alloc16(js_rt *rt, uint32_t len) {
    JSString *s = (JSString *)gc_alloc(rt, sizeof(JSString) + (size_t)len * 2 + 2, GC_STR);
    if (!s) return 0;
    s->len = len;
    s->h.flags = STR_WIDE;
    s->s16 = (uint16_t *)(s + 1);
    return s;
}
JSString *str_new8(js_rt *rt, const uint8_t *p, uint32_t len) {
    JSString *s = str_alloc8(rt, len);
    if (s && len) memcpy(s->s8, p, len);
    return s;
}
JSString *str_new16(js_rt *rt, const uint16_t *p, uint32_t len) {
    bool narrow = true;
    for (uint32_t i = 0; i < len; i++)
        if (p[i] > 255) {
            narrow = false;
            break;
        }
    if (narrow) {
        JSString *s = str_alloc8(rt, len);
        if (!s) return 0;
        for (uint32_t i = 0; i < len; i++) s->s8[i] = (uint8_t)p[i];
        return s;
    }
    JSString *s = str_alloc16(rt, len);
    if (s) memcpy(s->s16, p, (size_t)len * 2);
    return s;
}
JSString *str_cstr(js_rt *rt, const char *s) { return str_new8(rt, (const uint8_t *)s, (uint32_t)strlen(s)); }

int utf8_decode(const uint8_t *s, size_t n, uint32_t *cp) {
    uint8_t c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int len;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        v = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        v = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        v = c & 0x07;
        min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if ((size_t)len > n) {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = v;
    return len;
}
int utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

JSString *str_utf8(js_rt *rt, const char *p, size_t n) {
    const uint8_t *s = (const uint8_t *)p;
    size_t i = 0;
    bool ascii = true;
    for (i = 0; i < n; i++)
        if (s[i] >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii) return str_new8(rt, s, (uint32_t)n);
    /* count units and width */
    size_t units = 0;
    bool wide = false;
    for (i = 0; i < n;) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, n - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
        if (cp > 255) wide = true;
    }
    if (units > JS_MAX_STRLEN) units = 0;
    JSString *r = wide ? str_alloc16(rt, (uint32_t)units) : str_alloc8(rt, (uint32_t)units);
    if (!r) return 0;
    size_t k = 0;
    for (i = 0; i < n && k < units;) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, n - i, &cp);
        if (!wide)
            r->s8[k++] = (uint8_t)cp;
        else if (cp >= 0x10000) {
            cp -= 0x10000;
            r->s16[k++] = (uint16_t)(0xD800 + (cp >> 10));
            r->s16[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else
            r->s16[k++] = (uint16_t)cp;
    }
    return r;
}

JSString *str_concat(js_rt *rt, JSString *a, JSString *b) {
    if (a->len == 0) return b;
    if (b->len == 0) return a;
    uint64_t tl = (uint64_t)a->len + b->len;
    if (tl > JS_MAX_STRLEN) return 0;
    bool wide = ((a->h.flags | b->h.flags) & STR_WIDE) != 0;
    if (tl <= 24 && !(a->h.flags & STR_ROPE) && !(b->h.flags & STR_ROPE)) {
        JSString *r = wide ? str_alloc16(rt, (uint32_t)tl) : str_alloc8(rt, (uint32_t)tl);
        if (!r) return 0;
        if (!wide) {
            memcpy(r->s8, a->s8, a->len);
            memcpy(r->s8 + a->len, b->s8, b->len);
        } else {
            for (uint32_t i = 0; i < a->len; i++) r->s16[i] = str_at(a, i);
            for (uint32_t i = 0; i < b->len; i++) r->s16[a->len + i] = str_at(b, i);
        }
        return r;
    }
    JSString *r = (JSString *)gc_alloc(rt, sizeof(JSString), GC_STR);
    if (!r) return 0;
    r->len = (uint32_t)tl;
    r->h.flags = STR_ROPE | (wide ? STR_WIDE : 0);
    r->left = a;
    r->right = b;
    return r;
}

void str_flatten(js_rt *rt, JSString *s) {
    if (!(s->h.flags & STR_ROPE)) return;
    bool wide = (s->h.flags & STR_WIDE) != 0;
    uint32_t len = s->len;
    void *buf = malloc(wide ? (size_t)len * 2 + 2 : (size_t)len + 1);
    if (!buf) return;
    /* iterative traversal: write right-to-left using an explicit stack */
    size_t cap = 64, sp = 0;
    JSString **stk = (JSString **)malloc(cap * sizeof(JSString *));
    uint32_t pos = len;
    stk[sp++] = s;
    while (sp) {
        JSString *n = stk[--sp];
        if (n->h.flags & STR_ROPE) {
            if (sp + 2 > cap) {
                cap *= 2;
                stk = (JSString **)realloc(stk, cap * sizeof(JSString *));
            }
            stk[sp++] = n->left;
            stk[sp++] = n->right;
            continue;
        }
        pos -= n->len;
        if (wide) {
            uint16_t *d = (uint16_t *)buf + pos;
            if (n->h.flags & STR_WIDE)
                memcpy(d, n->s16, (size_t)n->len * 2);
            else
                for (uint32_t i = 0; i < n->len; i++) d[i] = n->s8[i];
        } else
            memcpy((uint8_t *)buf + pos, n->s8, n->len);
    }
    free(stk);
    if (!wide) ((uint8_t *)buf)[len] = 0;
    s->h.flags &= (uint16_t)~STR_ROPE;
    s->h.flags |= STR_OWNBUF;
    s->s8 = (uint8_t *)buf;
    s->right = 0;
    rt->bytes += len;
}

JSString *str_sub(js_rt *rt, JSString *s, uint32_t start, uint32_t end) {
    str_flat(rt, s);
    if (start >= end) return vstr(ATOM(empty));
    if (start == 0 && end == s->len) return s;
    if (end - start == 1) return str_char(rt, str_at(s, start));
    if (s->h.flags & STR_WIDE) return str_new16(rt, s->s16 + start, end - start);
    return str_new8(rt, s->s8 + start, end - start);
}

JSString *str_char(js_rt *rt, uint16_t c) {
    if (c < 256) {
        if (!rt->char_str[c]) {
            uint8_t b = (uint8_t)c;
            rt->char_str[c] = str_new8(rt, &b, 1);
        }
        return rt->char_str[c];
    }
    JSString *s = str_alloc16(rt, 1);
    s->s16[0] = c;
    return s;
}

int str_cmp(js_rt *rt, JSString *a, JSString *b) {
    str_flat(rt, a);
    str_flat(rt, b);
    uint32_t n = a->len < b->len ? a->len : b->len;
    if (!((a->h.flags | b->h.flags) & STR_WIDE)) {
        int c = memcmp(a->s8, b->s8, n);
        if (c) return c < 0 ? -1 : 1;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint16_t x = str_at(a, i), y = str_at(b, i);
            if (x != y) return x < y ? -1 : 1;
        }
    }
    return a->len == b->len ? 0 : a->len < b->len ? -1 : 1;
}

bool str_eq(js_rt *rt, JSString *a, JSString *b) {
    if (a == b) return true;
    if (a->len != b->len) return false;
    if ((a->h.flags & STR_ATOM) && (b->h.flags & STR_ATOM)) return false;
    if (a->hash && b->hash && a->hash != b->hash) return false;
    str_flat(rt, a);
    str_flat(rt, b);
    if (!((a->h.flags | b->h.flags) & STR_WIDE)) return memcmp(a->s8, b->s8, a->len) == 0;
    if ((a->h.flags & b->h.flags) & STR_WIDE) return memcmp(a->s16, b->s16, (size_t)a->len * 2) == 0;
    for (uint32_t i = 0; i < a->len; i++)
        if (str_at(a, i) != str_at(b, i)) return false;
    return true;
}

static uint32_t hash_units8(const uint8_t *s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
    return h ? h : 1;
}
uint32_t str_hash(js_rt *rt, JSString *s) {
    if (s->hash) return s->hash;
    str_flat(rt, s);
    uint32_t h;
    if (s->h.flags & STR_WIDE) {
        h = 2166136261u;
        for (uint32_t i = 0; i < s->len; i++) h = (h ^ s->s16[i]) * 16777619u;
        if (!h) h = 1;
    } else
        h = hash_units8(s->s8, s->len);
    s->hash = h;
    return h;
}

#define TOMB ((JSString *)1)
static void atab_grow(js_rt *rt) {
    uint32_t ncap = rt->amask ? (rt->amask + 1) * 2 : 4096;
    if (rt->acount * 4 < (rt->amask + 1)) ncap = rt->amask + 1; /* only tombstones: rehash same size */
    JSString **n = (JSString **)calloc(ncap, sizeof(JSString *));
    if (!n) return;
    for (uint32_t i = 0; rt->atab && i <= rt->amask; i++) {
        JSString *s = rt->atab[i];
        if (!s || s == TOMB) continue;
        uint32_t j = s->hash & (ncap - 1);
        while (n[j]) j = (j + 1) & (ncap - 1);
        n[j] = s;
    }
    free(rt->atab);
    rt->atab = n;
    rt->amask = ncap - 1;
    rt->adel = 0;
}

jsval atomize(js_rt *rt, JSString *s) {
    if (s->h.flags & STR_ATOM) return mkstr(s);
    uint32_t h = str_hash(rt, s);
    if ((rt->acount + rt->adel + 1) * 2 > rt->amask + 1) atab_grow(rt);
    uint32_t j = h & rt->amask, tomb = UINT32_MAX;
    for (;;) {
        JSString *a = rt->atab[j];
        if (!a) break;
        if (a == TOMB) {
            if (tomb == UINT32_MAX) tomb = j;
        } else if (a->hash == h && str_eq(rt, a, s))
            return mkstr(a);
        j = (j + 1) & rt->amask;
    }
    if (tomb != UINT32_MAX) {
        j = tomb;
        rt->adel--;
    }
    rt->atab[j] = s;
    rt->acount++;
    s->h.flags |= STR_ATOM;
    return mkstr(s);
}

jsval atom_n(js_rt *rt, const uint8_t *p, uint32_t len) {
    uint32_t h = hash_units8(p, len);
    if (rt->amask) {
        uint32_t j = h & rt->amask;
        for (;;) {
            JSString *a = rt->atab[j];
            if (!a) break;
            if (a != TOMB && a->hash == h && a->len == len && !(a->h.flags & STR_WIDE) && memcmp(a->s8, p, len) == 0)
                return mkstr(a);
            j = (j + 1) & rt->amask;
        }
    }
    JSString *s = str_new8(rt, p, len);
    return atomize(rt, s);
}
jsval atom_c(js_rt *rt, const char *s) { return atom_n(rt, (const uint8_t *)s, (uint32_t)strlen(s)); }

void atoms_sweep(js_rt *rt) {
    for (uint32_t i = 0; rt->atab && i <= rt->amask; i++) {
        JSString *s = rt->atab[i];
        if (!s || s == TOMB) continue;
        if (!s->h.mark && !(s->h.flags & STR_PERM)) {
            rt->atab[i] = TOMB;
            rt->acount--;
            rt->adel++;
        }
    }
}

int64_t str_index(js_rt *rt, JSString *s) {
    str_flat(rt, s);
    int64_t r = -1;
    uint32_t n = s->len;
    if (n > 0 && n <= 10) {
        uint64_t v = 0;
        bool ok = true;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t c = str_at(s, i);
            if (c < '0' || c > '9' || (i == 0 && c == '0' && n > 1)) {
                ok = false;
                break;
            }
            v = v * 10 + (c - '0');
        }
        if (ok && v < 4294967295ull) r = (int64_t)v;
    }
    if (s->h.flags & STR_ATOM) {
        s->h.flags |= STR_IDXCHK;
        if (r >= 0) {
            s->h.flags |= STR_ISIDX;
            if (!(s->h.flags & STR_ROPE)) s->idx = (uint32_t)r;
        }
    }
    return r;
}

bool str_is_ascii(js_rt *rt, JSString *s) {
    if (s->h.flags & STR_ASCIICHK) return (s->h.flags & STR_ASCII) != 0;
    str_flat(rt, s);
    bool a = true;
    if (s->h.flags & STR_WIDE)
        a = false;
    else
        for (uint32_t i = 0; i < s->len; i++)
            if (s->s8[i] >= 0x80) {
                a = false;
                break;
            }
    s->h.flags |= STR_ASCIICHK | (a ? STR_ASCII : 0);
    return a;
}

char *str_to_utf8(js_rt *rt, JSString *s, size_t *outlen) {
    str_flat(rt, s);
    size_t cap = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = str_at(s, i);
        cap += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    }
    char *o = (char *)malloc(cap + 1);
    if (!o) return 0;
    size_t k = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
            uint32_t d = str_at(s, i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        k += (size_t)utf8_encode(c, o + k);
    }
    o[k] = 0;
    if (outlen) *outlen = k;
    return o;
}

int32_t str_indexof(js_rt *rt, JSString *s, JSString *p, int32_t from) {
    str_flat(rt, s);
    str_flat(rt, p);
    if (from < 0) from = 0;
    if (p->len == 0) return from <= (int32_t)s->len ? from : (int32_t)s->len;
    if (p->len > s->len) return -1;
    uint32_t last = s->len - p->len;
    uint16_t c0 = str_at(p, 0);
    if (!((s->h.flags | p->h.flags) & STR_WIDE)) {
        for (uint32_t i = (uint32_t)from; i <= last; i++) {
            if (s->s8[i] == c0 && memcmp(s->s8 + i, p->s8, p->len) == 0) return (int32_t)i;
        }
        return -1;
    }
    for (uint32_t i = (uint32_t)from; i <= last; i++) {
        if (str_at(s, i) != c0) continue;
        uint32_t k = 1;
        while (k < p->len && str_at(s, i + k) == str_at(p, k)) k++;
        if (k == p->len) return (int32_t)i;
    }
    return -1;
}
int32_t str_lastindexof(js_rt *rt, JSString *s, JSString *p, int32_t from) {
    str_flat(rt, s);
    str_flat(rt, p);
    if (p->len > s->len) return -1;
    int32_t start = (int32_t)(s->len - p->len);
    if (from < start) start = from;
    for (int32_t i = start; i >= 0; i--) {
        uint32_t k = 0;
        while (k < p->len && str_at(s, (uint32_t)i + k) == str_at(p, k)) k++;
        if (k == p->len) return i;
    }
    return -1;
}

/* ---------------- string builder ---------------- */
void sb_init(SB *b, js_rt *rt) {
    memset(b, 0, sizeof *b);
    b->rt = rt;
}
static bool sb_grow(SB *b, uint32_t need) {
    if (b->oom) return false;
    if ((uint64_t)b->len + need > JS_MAX_STRLEN) {
        b->oom = true;
        return false;
    }
    if (b->len + need <= b->cap) return true;
    uint32_t nc = b->cap ? b->cap : 32;
    while (nc < b->len + need) nc = nc * 2 > JS_MAX_STRLEN ? JS_MAX_STRLEN + 1 : nc * 2;
    if (b->wide) {
        uint16_t *n = (uint16_t *)realloc(b->b16, (size_t)nc * 2 + 2);
        if (!n) {
            b->oom = true;
            return false;
        }
        b->b16 = n;
    } else {
        uint8_t *n = (uint8_t *)realloc(b->b8, (size_t)nc + 1);
        if (!n) {
            b->oom = true;
            return false;
        }
        b->b8 = n;
    }
    b->cap = nc;
    return true;
}
static void sb_widen(SB *b) {
    uint32_t cap = b->cap ? b->cap : 32;
    uint16_t *n = (uint16_t *)malloc((size_t)cap * 2 + 2);
    if (!n) {
        b->oom = true;
        return;
    }
    for (uint32_t i = 0; i < b->len; i++) n[i] = b->b8[i];
    free(b->b8);
    b->b8 = 0;
    b->b16 = n;
    b->cap = cap;
    b->wide = true;
}
void sb_putc(SB *b, uint32_t c) {
    if (c > 255 && !b->wide) sb_widen(b);
    if (!sb_grow(b, 1)) return;
    if (b->wide)
        b->b16[b->len++] = (uint16_t)c;
    else
        b->b8[b->len++] = (uint8_t)c;
}
void sb_putcp(SB *b, uint32_t cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        sb_putc(b, 0xD800 + (cp >> 10));
        sb_putc(b, 0xDC00 + (cp & 0x3FF));
    } else
        sb_putc(b, cp);
}
void sb_putn(SB *b, const char *s, uint32_t n) {
    if (!sb_grow(b, n)) return;
    if (b->wide)
        for (uint32_t i = 0; i < n; i++) b->b16[b->len++] = (uint8_t)s[i];
    else {
        memcpy(b->b8 + b->len, s, n);
        b->len += n;
    }
}
void sb_puts(SB *b, const char *s) { sb_putn(b, s, (uint32_t)strlen(s)); }
void sb_putsub(SB *b, JSString *s, uint32_t start, uint32_t end) {
    str_flat(b->rt, s);
    if (end <= start) return;
    if ((s->h.flags & STR_WIDE) && !b->wide) sb_widen(b);
    if (!sb_grow(b, end - start)) return;
    if (b->wide) {
        if (s->h.flags & STR_WIDE)
            memcpy(b->b16 + b->len, s->s16 + start, (size_t)(end - start) * 2);
        else
            for (uint32_t i = start; i < end; i++) b->b16[b->len + i - start] = s->s8[i];
    } else
        memcpy(b->b8 + b->len, s->s8 + start, end - start);
    b->len += end - start;
}
void sb_putstr(SB *b, JSString *s) { sb_putsub(b, s, 0, s->len); }
void sb_pututf8(SB *b, const char *s, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += (size_t)utf8_decode(p + i, n - i, &cp);
        sb_putcp(b, cp);
    }
}
void sb_free(SB *b) {
    free(b->b8);
    free(b->b16);
    b->b8 = 0;
    b->b16 = 0;
}
JSString *sb_finish(SB *b) {
    js_rt *rt = b->rt;
    if (b->oom) {
        sb_free(b);
        throw_error(rt, E_RANGE, "Invalid string length");
        return 0;
    }
    JSString *s;
    if (b->wide) {
        bool narrow = true;
        for (uint32_t i = 0; i < b->len; i++)
            if (b->b16[i] > 255) {
                narrow = false;
                break;
            }
        if (narrow) {
            s = str_alloc8(rt, b->len);
            for (uint32_t i = 0; i < b->len; i++) s->s8[i] = (uint8_t)b->b16[i];
            sb_free(b);
            return s;
        }
        if (b->len < 64) {
            s = str_new16(rt, b->b16, b->len);
            sb_free(b);
            return s;
        }
        s = (JSString *)gc_alloc(rt, sizeof(JSString), GC_STR);
        s->len = b->len;
        s->h.flags = STR_WIDE | STR_OWNBUF;
        s->s16 = b->b16;
        rt->bytes += (size_t)b->len * 2;
        b->b16 = 0;
        return s;
    }
    if (b->len < 64 || !b->b8) {
        s = str_new8(rt, b->b8, b->len);
        sb_free(b);
        return s;
    }
    s = (JSString *)gc_alloc(rt, sizeof(JSString), GC_STR);
    s->len = b->len;
    s->h.flags = STR_OWNBUF;
    b->b8[b->len] = 0;
    s->s8 = b->b8;
    rt->bytes += b->len;
    b->b8 = 0;
    return s;
}

/* ---------------- Unicode ---------------- */
bool uc_is_space(uint32_t c) {
    if (c < 128) return c == 32 || (c >= 9 && c <= 13);
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
bool uc_is_id_start(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    if (uc_is_space(c)) return false;
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
    if (c < 0xC0) return false;
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x300 && c <= 0x36F) return false;
    if (c >= 0x2000 && c <= 0x206F) return false;
    if (c >= 0x2190 && c <= 0x2BFF) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    if (c >= 0xFE00 && c <= 0xFE0F) return false;
    if (c >= 0x1F000 && c <= 0x1FAFF) return false;
    return true;
}
bool uc_is_id_part(uint32_t c) {
    if (c < 128) return uc_is_id_start(c) || (c >= '0' && c <= '9');
    if (c == 0x200C || c == 0x200D) return true;
    if (c >= 0x300 && c <= 0x36F) return true;
    return uc_is_id_start(c);
}

static uint32_t pair_upper(uint32_t c, uint32_t lo, uint32_t hi, int even_upper) {
    if (c < lo || c > hi) return 0;
    if (((c & 1) == 0) == (even_upper != 0)) return c; /* already upper */
    return c - 1;
}
uint32_t uc_upper(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c < 256) {
        if (c == 0xB5) return 0x39C;
        if (c == 0xFF) return 0x178;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        return c;
    }
    uint32_t r;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x180 && c <= 0x24F) {
        if (c == 0x180) return 0x243;
        if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c : c - 1;
        if ((r = pair_upper(c, 0x1DE, 0x1EF, 1))) return r;
        if ((r = pair_upper(c, 0x1F8, 0x21F, 1))) return r;
        if ((r = pair_upper(c, 0x222, 0x233, 1))) return r;
        if ((r = pair_upper(c, 0x246, 0x24F, 1))) return r;
        if (c == 0x1C6 || c == 0x1C9 || c == 0x1CC || c == 0x1F3) return c - 2;
        if (c == 0x1C5 || c == 0x1C8 || c == 0x1CB || c == 0x1F2) return c - 1;
        return c;
    }
    if (c >= 0x370 && c <= 0x3FF) {
        if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? 0x3A3 : c - 32;
        if (c == 0x3AC) return 0x386;
        if (c >= 0x3AD && c <= 0x3AF) return c - 37;
        if (c == 0x3CC) return 0x38C;
        if (c == 0x3CD || c == 0x3CE) return c - 63;
        if ((r = pair_upper(c, 0x3D8, 0x3EF, 1))) return r;
        return c;
    }
    if (c >= 0x400 && c <= 0x52F) {
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if ((r = pair_upper(c, 0x460, 0x481, 1))) return r;
        if ((r = pair_upper(c, 0x48A, 0x4BF, 1))) return r;
        if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
        if (c == 0x4CF) return 0x4C0;
        if ((r = pair_upper(c, 0x4D0, 0x52F, 1))) return r;
        return c;
    }
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0x10D0 && c <= 0x10FA) return c + 3008;
    if (c >= 0x1E00 && c <= 0x1EFF) {
        if (c >= 0x1E96 && c <= 0x1E9F) return c == 0x1E9B ? 0x1E60 : c;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x1F00 && c <= 0x1FFF) {
        uint32_t lo = c & 0xF;
        uint32_t row = c & 0xFFF0;
        if ((row == 0x1F00 || row == 0x1F10 || row == 0x1F20 || row == 0x1F30 || row == 0x1F40 || row == 0x1F60) && lo < 8)
            return c + 8;
        if (row == 0x1F50 && lo < 8 && (lo & 1)) return c + 8;
        return c;
    }
    if (c >= 0x2170 && c <= 0x217F) return c - 16;
    if (c >= 0x24D0 && c <= 0x24E9) return c - 26;
    if (c >= 0x2C30 && c <= 0x2C5F) return c - 48;
    if (c >= 0x2D00 && c <= 0x2D25) return c - 7264;
    if (c >= 0xA640 && c <= 0xA66D) return (c & 1) ? c - 1 : c;
    if (c >= 0xA680 && c <= 0xA69B) return (c & 1) ? c - 1 : c;
    if (c >= 0xA722 && c <= 0xA72F) return (c & 1) ? c - 1 : c;
    if (c >= 0xA732 && c <= 0xA76F) return (c & 1) ? c - 1 : c;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    if (c >= 0x10428 && c <= 0x1044F) return c - 40;
    return c;
}
uint32_t uc_lower(uint32_t c) {
    if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c < 256) {
        if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
        return c;
    }
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x180 && c <= 0x24F) {
        if (c == 0x243) return 0x180;
        if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c + 1 : c;
        if ((c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) || (c >= 0x222 && c <= 0x233) ||
            (c >= 0x246 && c <= 0x24F))
            return (c & 1) ? c : c + 1;
        if (c == 0x1C4 || c == 0x1C7 || c == 0x1CA || c == 0x1F1) return c + 2;
        if (c == 0x1C5 || c == 0x1C8 || c == 0x1CB || c == 0x1F2) return c + 1;
        return c;
    }
    if (c >= 0x370 && c <= 0x3FF) {
        if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
        if (c == 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return c + 37;
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return c + 63;
        if (c >= 0x3D8 && c <= 0x3EF) return (c & 1) ? c : c + 1;
        return c;
    }
    if (c >= 0x400 && c <= 0x52F) {
        if (c >= 0x410 && c <= 0x42F) return c + 32;
        if (c >= 0x400 && c <= 0x40F) return c + 80;
        if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
            return (c & 1) ? c : c + 1;
        if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c + 1 : c;
        if (c == 0x4C0) return 0x4CF;
        return c;
    }
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0x10A0 && c <= 0x10C5) return c + 7264;
    if (c >= 0x1E00 && c <= 0x1EFF) {
        if (c >= 0x1E96 && c <= 0x1E9F) return c == 0x1E9E ? 0xDF : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x1F00 && c <= 0x1FFF) {
        uint32_t lo = c & 0xF;
        uint32_t row = c & 0xFFF0;
        if ((row == 0x1F00 || row == 0x1F10 || row == 0x1F20 || row == 0x1F30 || row == 0x1F40 || row == 0x1F60) && lo >= 8)
            return c - 8;
        if (row == 0x1F50 && lo >= 8 && (lo & 1)) return c - 8;
        return c;
    }
    if (c >= 0x2160 && c <= 0x216F) return c + 16;
    if (c >= 0x24B6 && c <= 0x24CF) return c + 26;
    if (c >= 0x2C00 && c <= 0x2C2F) return c + 48;
    if (c >= 0xA640 && c <= 0xA66D) return (c & 1) ? c : c + 1;
    if (c >= 0xA680 && c <= 0xA69B) return (c & 1) ? c : c + 1;
    if (c >= 0xA722 && c <= 0xA72F) return (c & 1) ? c : c + 1;
    if (c >= 0xA732 && c <= 0xA76F) return (c & 1) ? c : c + 1;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    if (c >= 0x10400 && c <= 0x10427) return c + 40;
    return c;
}
