/* AXIOM JavaScript engine: String. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

static JSString *this_str(js_rt *rt, jsval t, const char *name) {
    if (is_str(t)) return str_flat(rt, vstr(t));
    if (is_nullish(t)) {
        throw_error(rt, E_TYPE, "String.prototype.%s called on null or undefined", name);
        return 0;
    }
    JSString *s = to_string(rt, t);
    if (s) str_flat(rt, s);
    return s;
}
#define THIS_STR(name)                              \
    JSString *s = this_str(rt, this_, name);        \
    if (!s) return JS_EXC;                          \
    push_tmp(rt, mkstr(s))

static JSString *arg_str(js_rt *rt, jsval v) {
    JSString *s = to_string(rt, v);
    if (s) {
        str_flat(rt, s);
        push_tmp(rt, mkstr(s));
    }
    return s;
}

NATIVE(string_ctor) {
    JSString *s;
    if (argc == 0)
        s = vstr(ATOM(empty));
    else if (is_undef(rt->new_target) && is_sym(argv[0]))
        return key_to_display(rt, argv[0]);
    else {
        s = to_string(rt, argv[0]);
        if (!s) return JS_EXC;
    }
    if (is_undef(rt->new_target)) return mkstr(s);
    push_tmp(rt, mkstr(s));
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_StringProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_STRING);
    o->u.prim.v = mkstr(s);
    return mkobj(o);
}
NATIVE(string_from_char_code) {
    SB b;
    sb_init(&b, rt);
    for (int i = 0; i < argc; i++) {
        double d;
        if (to_number(rt, argv[i], &d) < 0) {
            sb_free(&b);
            return JS_EXC;
        }
        sb_putc(&b, to_uint32(d) & 0xFFFF);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}
NATIVE(string_from_code_point) {
    SB b;
    sb_init(&b, rt);
    for (int i = 0; i < argc; i++) {
        double d;
        if (to_number(rt, argv[i], &d) < 0) {
            sb_free(&b);
            return JS_EXC;
        }
        if (d != to_integer(d) || d < 0 || d > 0x10FFFF) {
            sb_free(&b);
            return throw_error(rt, E_RANGE, "Invalid code point %S", to_string(rt, argv[i]));
        }
        sb_putcp(&b, (uint32_t)d);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}
NATIVE(string_raw) {
    jsval cooked = to_object(rt, ARG(0));
    TRY(cooked);
    push_tmp(rt, cooked);
    jsval raw = get_p(rt, cooked, ATOM(raw));
    TRY(raw);
    raw = to_object(rt, raw);
    TRY(raw);
    push_tmp(rt, raw);
    uint64_t len;
    if (len_of(rt, raw, &len) < 0) return JS_EXC;
    SB b;
    sb_init(&b, rt);
    for (uint64_t i = 0; i < len; i++) {
        jsval v = get_idx(rt, raw, (uint32_t)i);
        if (v == JS_EXC) goto fail;
        JSString *s = to_string(rt, v);
        if (!s) goto fail;
        sb_putstr(&b, s);
        if (i + 1 < len && (int)(i + 1) < argc) {
            JSString *t = to_string(rt, argv[i + 1]);
            if (!t) goto fail;
            sb_putstr(&b, t);
        }
    }
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
fail:
    sb_free(&b);
    return JS_EXC;
}

static jsval this_string_value(js_rt *rt, jsval t) {
    if (is_str(t)) return t;
    if (is_cls(t, CLS_STRING)) return vobj(t)->u.prim.v;
    return throw_error(rt, E_TYPE, "String.prototype.valueOf requires that 'this' be a String");
}
NATIVE(string_to_string) { return this_string_value(rt, this_); }

NATIVE(string_char_at) {
    THIS_STR("charAt");
    double d;
    if (to_int(rt, ARG(0), &d) < 0) return JS_EXC;
    int m = MAGIC; /* 0 charAt 1 charCodeAt 2 codePointAt 3 at */
    if (m == 3 && d < 0) d += s->len;
    if (d < 0 || d >= s->len) return m == 0 ? ATOM(empty) : m == 1 ? mknum(JS_NAN) : JS_UNDEF;
    uint32_t i = (uint32_t)d;
    uint32_t c = str_at(s, i);
    if (m == 0 || m == 3) return mkstr(str_char(rt, (uint16_t)c));
    if (m == 1) return mknum(c);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
        uint32_t d2 = str_at(s, i + 1);
        if (d2 >= 0xDC00 && d2 <= 0xDFFF) return mknum(0x10000 + ((c - 0xD800) << 10) + (d2 - 0xDC00));
    }
    return mknum(c);
}

NATIVE(string_index_of) {
    THIS_STR("indexOf");
    int m = MAGIC; /* 0 indexOf 1 lastIndexOf 2 includes 3 startsWith 4 endsWith */
    if (m >= 2 && is_cls(ARG(0), CLS_REGEXP)) {
        jsval isr = get_p(rt, argv[0], SYM(match));
        TRY(isr);
        if (is_undef(isr) || truthy(isr))
            THROW_TYPE("First argument to String.prototype.%s must not be a regular expression", m == 2 ? "includes" : m == 3 ? "startsWith" : "endsWith");
    }
    JSString *p = arg_str(rt, ARG(0));
    if (!p) return JS_EXC;
    uint32_t len = s->len;
    if (m == 1) {
        double pos;
        double n;
        if (to_number(rt, ARG(1), &n) < 0) return JS_EXC;
        pos = n != n ? JS_INF : to_integer(n);
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        return mknum(str_lastindexof(rt, s, p, (int32_t)pos));
    }
    double pos;
    if (m == 4) {
        if (is_undef(ARG(1)))
            pos = len;
        else if (to_int(rt, argv[1], &pos) < 0)
            return JS_EXC;
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        uint32_t end = (uint32_t)pos;
        if (p->len > end) return JS_FALSE;
        uint32_t st = end - p->len;
        for (uint32_t i = 0; i < p->len; i++)
            if (str_at(s, st + i) != str_at(p, i)) return JS_FALSE;
        return JS_TRUE;
    }
    if (to_int(rt, ARG(1), &pos) < 0) return JS_EXC;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    if (m == 3) {
        uint32_t st = (uint32_t)pos;
        if ((uint64_t)st + p->len > len) return JS_FALSE;
        for (uint32_t i = 0; i < p->len; i++)
            if (str_at(s, st + i) != str_at(p, i)) return JS_FALSE;
        return JS_TRUE;
    }
    int32_t r = str_indexof(rt, s, p, (int32_t)pos);
    return m == 2 ? mkbool(r >= 0) : mknum(r);
}

NATIVE(string_slice) {
    THIS_STR("slice");
    int m = MAGIC; /* 0 slice 1 substring 2 substr */
    double len = s->len, a, b;
    if (to_int(rt, ARG(0), &a) < 0) return JS_EXC;
    if (m == 2) {
        if (a < 0) a = a + len < 0 ? 0 : a + len;
        if (a > len) a = len;
        double l;
        if (is_undef(ARG(1)))
            l = len - a;
        else if (to_int(rt, argv[1], &l) < 0)
            return JS_EXC;
        if (l < 0) l = 0;
        if (l > len - a) l = len - a;
        return mkstr(str_sub(rt, s, (uint32_t)a, (uint32_t)(a + l)));
    }
    if (is_undef(ARG(1)))
        b = len;
    else if (to_int(rt, argv[1], &b) < 0)
        return JS_EXC;
    if (m == 0) {
        if (a < 0) a = a + len < 0 ? 0 : a + len;
        if (b < 0) b = b + len < 0 ? 0 : b + len;
        if (a > len) a = len;
        if (b > len) b = len;
        if (a >= b) return ATOM(empty);
        return mkstr(str_sub(rt, s, (uint32_t)a, (uint32_t)b));
    }
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > len) a = len;
    if (b > len) b = len;
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    return mkstr(str_sub(rt, s, (uint32_t)a, (uint32_t)b));
}

static bool is_cased_letter(uint32_t c) { return uc_upper(c) != c || uc_lower(c) != c; }

NATIVE(string_case) {
    THIS_STR("toUpperCase");
    bool upper = MAGIC == 1;
    SB b;
    sb_init(&b, rt);
    bool changed = false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        uint32_t cp = c;
        int w = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
            uint32_t d = str_at(s, i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                cp = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                w = 2;
            }
        }
        uint32_t r;
        if (upper) {
            if (cp == 0xDF) {
                sb_puts(&b, "SS");
                changed = true;
                continue;
            }
            if (cp == 0x149) {
                sb_putc(&b, 0x2BC);
                sb_putc(&b, 'N');
                changed = true;
                continue;
            }
            r = uc_upper(cp);
        } else {
            if (cp == 0x130) {
                sb_putc(&b, 'i');
                sb_putc(&b, 0x307);
                changed = true;
                continue;
            }
            if (cp == 0x3A3) {
                /* final sigma */
                bool before = i > 0 && is_cased_letter(str_at(s, i - 1));
                bool after = i + 1 < s->len && is_cased_letter(str_at(s, i + 1));
                r = (before && !after) ? 0x3C2 : 0x3C3;
            } else
                r = uc_lower(cp);
        }
        if (r != cp) changed = true;
        sb_putcp(&b, r);
        if (w == 2) i++;
    }
    if (!changed) {
        sb_free(&b);
        return mkstr(s);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

NATIVE(string_trim) {
    THIS_STR("trim");
    int m = MAGIC; /* 0 both 1 start 2 end */
    uint32_t a = 0, b = s->len;
    if (m != 2)
        while (a < b && uc_is_space(str_at(s, a))) a++;
    if (m != 1)
        while (b > a && uc_is_space(str_at(s, b - 1))) b--;
    return mkstr(str_sub(rt, s, a, b));
}

NATIVE(string_pad) {
    THIS_STR("padStart");
    double ml;
    if (to_int(rt, ARG(0), &ml) < 0) return JS_EXC;
    if (ml <= s->len) return mkstr(s);
    JSString *f = vstr(mkstr(str_cstr(rt, " ")));
    if (!is_undef(ARG(1))) {
        f = arg_str(rt, argv[1]);
        if (!f) return JS_EXC;
    }
    if (f->len == 0) return mkstr(s);
    if (ml > JS_MAX_STRLEN) return throw_error(rt, E_RANGE, "Invalid string length");
    uint32_t fill = (uint32_t)ml - s->len;
    SB b;
    sb_init(&b, rt);
    if (MAGIC == 1) sb_putstr(&b, s);
    for (uint32_t i = 0; i < fill;) {
        uint32_t take = fill - i < f->len ? fill - i : f->len;
        sb_putsub(&b, f, 0, take);
        i += take;
    }
    if (MAGIC == 0) sb_putstr(&b, s);
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

NATIVE(string_repeat) {
    THIS_STR("repeat");
    double n;
    if (to_int(rt, ARG(0), &n) < 0) return JS_EXC;
    if (n < 0 || n == JS_INF) return throw_error(rt, E_RANGE, "Invalid count value: %S", num_to_str(rt, n));
    if (n == 0 || s->len == 0) return ATOM(empty);
    if (n * s->len > JS_MAX_STRLEN) return throw_error(rt, E_RANGE, "Invalid string length");
    SB b;
    sb_init(&b, rt);
    for (uint32_t i = 0; i < (uint32_t)n; i++) sb_putstr(&b, s);
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

NATIVE(string_concat) {
    THIS_STR("concat");
    JSString *r = s;
    for (int i = 0; i < argc; i++) {
        JSString *t = to_string(rt, argv[i]);
        if (!t) return JS_EXC;
        r = str_concat(rt, r, t);
        if (!r) return throw_error(rt, E_RANGE, "Invalid string length");
        push_tmp(rt, mkstr(r));
    }
    return mkstr(r);
}

/* ICU-like root collation for common characters */
static int coll_primary(uint32_t c, int *acc) {
    *acc = 0;
    static const char punct[] = "\t\n\v\f\r _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";
    if (c < 128) {
        if (c >= 'A' && c <= 'Z') return 1000 + (int)(c - 'A') * 4;
        if (c >= 'a' && c <= 'z') return 1000 + (int)(c - 'a') * 4;
        if (c >= '0' && c <= '9') return 900 + (int)(c - '0');
        for (int i = 0; punct[i]; i++)
            if ((uint32_t)(uint8_t)punct[i] == c) return 10 + i;
        return 300 + (int)c;
    }
    /* Latin-1 letters: strip accents */
    static const char base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0 && c <= 0xFF) {
        char bc = base[c - 0xC0];
        if (bc != '*' && bc != '/') {
            *acc = (int)c;
            return coll_primary((uint32_t)(uint8_t)bc, &(int){0});
        }
    }
    return 2000 + (int)uc_lower(c);
}
NATIVE(string_locale_compare) {
    THIS_STR("localeCompare");
    JSString *t = arg_str(rt, ARG(0));
    if (!t) return JS_EXC;
    uint32_t n = s->len < t->len ? s->len : t->len;
    int sec = 0, ter = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = str_at(s, i), b = str_at(t, i);
        if (a == b) continue;
        int aa, ab;
        int pa = coll_primary(a, &aa), pb = coll_primary(b, &ab);
        if (pa != pb) return mknum(pa < pb ? -1 : 1);
        if (!sec && aa != ab) sec = aa < ab ? -1 : 1;
        if (!ter) {
            bool ua = uc_lower(a) != a, ub = uc_lower(b) != b;
            if (ua != ub) ter = ua ? 1 : -1;
        }
    }
    if (s->len != t->len) return mknum(s->len < t->len ? -1 : 1);
    if (sec) return mknum(sec);
    return mknum(ter);
}
/* canonical decompositions of precomposed Latin, Greek and Cyrillic letters (code point, base, mark);
 * normalize() handles these (NFKC/NFKD are treated as NFC/NFD) */
static const uint16_t norm_tab[][3] = {
    {0x00C0,0x0041,0x0300}, {0x00C1,0x0041,0x0301}, {0x00C2,0x0041,0x0302}, {0x00C3,0x0041,0x0303}, {0x00C4,0x0041,0x0308}, {0x00C5,0x0041,0x030A}, {0x00C7,0x0043,0x0327},
    {0x00C8,0x0045,0x0300}, {0x00C9,0x0045,0x0301}, {0x00CA,0x0045,0x0302}, {0x00CB,0x0045,0x0308}, {0x00CC,0x0049,0x0300}, {0x00CD,0x0049,0x0301}, {0x00CE,0x0049,0x0302},
    {0x00CF,0x0049,0x0308}, {0x00D1,0x004E,0x0303}, {0x00D2,0x004F,0x0300}, {0x00D3,0x004F,0x0301}, {0x00D4,0x004F,0x0302}, {0x00D5,0x004F,0x0303}, {0x00D6,0x004F,0x0308},
    {0x00D9,0x0055,0x0300}, {0x00DA,0x0055,0x0301}, {0x00DB,0x0055,0x0302}, {0x00DC,0x0055,0x0308}, {0x00DD,0x0059,0x0301}, {0x00E0,0x0061,0x0300}, {0x00E1,0x0061,0x0301},
    {0x00E2,0x0061,0x0302}, {0x00E3,0x0061,0x0303}, {0x00E4,0x0061,0x0308}, {0x00E5,0x0061,0x030A}, {0x00E7,0x0063,0x0327}, {0x00E8,0x0065,0x0300}, {0x00E9,0x0065,0x0301},
    {0x00EA,0x0065,0x0302}, {0x00EB,0x0065,0x0308}, {0x00EC,0x0069,0x0300}, {0x00ED,0x0069,0x0301}, {0x00EE,0x0069,0x0302}, {0x00EF,0x0069,0x0308}, {0x00F1,0x006E,0x0303},
    {0x00F2,0x006F,0x0300}, {0x00F3,0x006F,0x0301}, {0x00F4,0x006F,0x0302}, {0x00F5,0x006F,0x0303}, {0x00F6,0x006F,0x0308}, {0x00F9,0x0075,0x0300}, {0x00FA,0x0075,0x0301},
    {0x00FB,0x0075,0x0302}, {0x00FC,0x0075,0x0308}, {0x00FD,0x0079,0x0301}, {0x00FF,0x0079,0x0308}, {0x0100,0x0041,0x0304}, {0x0101,0x0061,0x0304}, {0x0102,0x0041,0x0306},
    {0x0103,0x0061,0x0306}, {0x0104,0x0041,0x0328}, {0x0105,0x0061,0x0328}, {0x0106,0x0043,0x0301}, {0x0107,0x0063,0x0301}, {0x0108,0x0043,0x0302}, {0x0109,0x0063,0x0302},
    {0x010A,0x0043,0x0307}, {0x010B,0x0063,0x0307}, {0x010C,0x0043,0x030C}, {0x010D,0x0063,0x030C}, {0x010E,0x0044,0x030C}, {0x010F,0x0064,0x030C}, {0x0112,0x0045,0x0304},
    {0x0113,0x0065,0x0304}, {0x0114,0x0045,0x0306}, {0x0115,0x0065,0x0306}, {0x0116,0x0045,0x0307}, {0x0117,0x0065,0x0307}, {0x0118,0x0045,0x0328}, {0x0119,0x0065,0x0328},
    {0x011A,0x0045,0x030C}, {0x011B,0x0065,0x030C}, {0x011C,0x0047,0x0302}, {0x011D,0x0067,0x0302}, {0x011E,0x0047,0x0306}, {0x011F,0x0067,0x0306}, {0x0120,0x0047,0x0307},
    {0x0121,0x0067,0x0307}, {0x0122,0x0047,0x0327}, {0x0123,0x0067,0x0327}, {0x0124,0x0048,0x0302}, {0x0125,0x0068,0x0302}, {0x0128,0x0049,0x0303}, {0x0129,0x0069,0x0303},
    {0x012A,0x0049,0x0304}, {0x012B,0x0069,0x0304}, {0x012C,0x0049,0x0306}, {0x012D,0x0069,0x0306}, {0x012E,0x0049,0x0328}, {0x012F,0x0069,0x0328}, {0x0130,0x0049,0x0307},
    {0x0134,0x004A,0x0302}, {0x0135,0x006A,0x0302}, {0x0136,0x004B,0x0327}, {0x0137,0x006B,0x0327}, {0x0139,0x004C,0x0301}, {0x013A,0x006C,0x0301}, {0x013B,0x004C,0x0327},
    {0x013C,0x006C,0x0327}, {0x013D,0x004C,0x030C}, {0x013E,0x006C,0x030C}, {0x0143,0x004E,0x0301}, {0x0144,0x006E,0x0301}, {0x0145,0x004E,0x0327}, {0x0146,0x006E,0x0327},
    {0x0147,0x004E,0x030C}, {0x0148,0x006E,0x030C}, {0x014C,0x004F,0x0304}, {0x014D,0x006F,0x0304}, {0x014E,0x004F,0x0306}, {0x014F,0x006F,0x0306}, {0x0150,0x004F,0x030B},
    {0x0151,0x006F,0x030B}, {0x0154,0x0052,0x0301}, {0x0155,0x0072,0x0301}, {0x0156,0x0052,0x0327}, {0x0157,0x0072,0x0327}, {0x0158,0x0052,0x030C}, {0x0159,0x0072,0x030C},
    {0x015A,0x0053,0x0301}, {0x015B,0x0073,0x0301}, {0x015C,0x0053,0x0302}, {0x015D,0x0073,0x0302}, {0x015E,0x0053,0x0327}, {0x015F,0x0073,0x0327}, {0x0160,0x0053,0x030C},
    {0x0161,0x0073,0x030C}, {0x0162,0x0054,0x0327}, {0x0163,0x0074,0x0327}, {0x0164,0x0054,0x030C}, {0x0165,0x0074,0x030C}, {0x0168,0x0055,0x0303}, {0x0169,0x0075,0x0303},
    {0x016A,0x0055,0x0304}, {0x016B,0x0075,0x0304}, {0x016C,0x0055,0x0306}, {0x016D,0x0075,0x0306}, {0x016E,0x0055,0x030A}, {0x016F,0x0075,0x030A}, {0x0170,0x0055,0x030B},
    {0x0171,0x0075,0x030B}, {0x0172,0x0055,0x0328}, {0x0173,0x0075,0x0328}, {0x0174,0x0057,0x0302}, {0x0175,0x0077,0x0302}, {0x0176,0x0059,0x0302}, {0x0177,0x0079,0x0302},
    {0x0178,0x0059,0x0308}, {0x0179,0x005A,0x0301}, {0x017A,0x007A,0x0301}, {0x017B,0x005A,0x0307}, {0x017C,0x007A,0x0307}, {0x017D,0x005A,0x030C}, {0x017E,0x007A,0x030C},
    {0x01A0,0x004F,0x031B}, {0x01A1,0x006F,0x031B}, {0x01AF,0x0055,0x031B}, {0x01B0,0x0075,0x031B}, {0x01CD,0x0041,0x030C}, {0x01CE,0x0061,0x030C}, {0x01CF,0x0049,0x030C},
    {0x01D0,0x0069,0x030C}, {0x01D1,0x004F,0x030C}, {0x01D2,0x006F,0x030C}, {0x01D3,0x0055,0x030C}, {0x01D4,0x0075,0x030C}, {0x01D5,0x00DC,0x0304}, {0x01D6,0x00FC,0x0304},
    {0x01D7,0x00DC,0x0301}, {0x01D8,0x00FC,0x0301}, {0x01D9,0x00DC,0x030C}, {0x01DA,0x00FC,0x030C}, {0x01DB,0x00DC,0x0300}, {0x01DC,0x00FC,0x0300}, {0x01DE,0x00C4,0x0304},
    {0x01DF,0x00E4,0x0304}, {0x01E0,0x0226,0x0304}, {0x01E1,0x0227,0x0304}, {0x01E2,0x00C6,0x0304}, {0x01E3,0x00E6,0x0304}, {0x01E6,0x0047,0x030C}, {0x01E7,0x0067,0x030C},
    {0x01E8,0x004B,0x030C}, {0x01E9,0x006B,0x030C}, {0x01EA,0x004F,0x0328}, {0x01EB,0x006F,0x0328}, {0x01EC,0x01EA,0x0304}, {0x01ED,0x01EB,0x0304}, {0x01EE,0x01B7,0x030C},
    {0x01EF,0x0292,0x030C}, {0x01F0,0x006A,0x030C}, {0x01F4,0x0047,0x0301}, {0x01F5,0x0067,0x0301}, {0x01F8,0x004E,0x0300}, {0x01F9,0x006E,0x0300}, {0x01FA,0x00C5,0x0301},
    {0x01FB,0x00E5,0x0301}, {0x01FC,0x00C6,0x0301}, {0x01FD,0x00E6,0x0301}, {0x01FE,0x00D8,0x0301}, {0x01FF,0x00F8,0x0301}, {0x0200,0x0041,0x030F}, {0x0201,0x0061,0x030F},
    {0x0202,0x0041,0x0311}, {0x0203,0x0061,0x0311}, {0x0204,0x0045,0x030F}, {0x0205,0x0065,0x030F}, {0x0206,0x0045,0x0311}, {0x0207,0x0065,0x0311}, {0x0208,0x0049,0x030F},
    {0x0209,0x0069,0x030F}, {0x020A,0x0049,0x0311}, {0x020B,0x0069,0x0311}, {0x020C,0x004F,0x030F}, {0x020D,0x006F,0x030F}, {0x020E,0x004F,0x0311}, {0x020F,0x006F,0x0311},
    {0x0210,0x0052,0x030F}, {0x0211,0x0072,0x030F}, {0x0212,0x0052,0x0311}, {0x0213,0x0072,0x0311}, {0x0214,0x0055,0x030F}, {0x0215,0x0075,0x030F}, {0x0216,0x0055,0x0311},
    {0x0217,0x0075,0x0311}, {0x0218,0x0053,0x0326}, {0x0219,0x0073,0x0326}, {0x021A,0x0054,0x0326}, {0x021B,0x0074,0x0326}, {0x021E,0x0048,0x030C}, {0x021F,0x0068,0x030C},
    {0x0226,0x0041,0x0307}, {0x0227,0x0061,0x0307}, {0x0228,0x0045,0x0327}, {0x0229,0x0065,0x0327}, {0x022A,0x00D6,0x0304}, {0x022B,0x00F6,0x0304}, {0x022C,0x00D5,0x0304},
    {0x022D,0x00F5,0x0304}, {0x022E,0x004F,0x0307}, {0x022F,0x006F,0x0307}, {0x0230,0x022E,0x0304}, {0x0231,0x022F,0x0304}, {0x0232,0x0059,0x0304}, {0x0233,0x0079,0x0304},
    {0x0386,0x0391,0x0301}, {0x0388,0x0395,0x0301}, {0x0389,0x0397,0x0301}, {0x038A,0x0399,0x0301}, {0x038C,0x039F,0x0301}, {0x038E,0x03A5,0x0301}, {0x038F,0x03A9,0x0301},
    {0x0390,0x03CA,0x0301}, {0x03AA,0x0399,0x0308}, {0x03AB,0x03A5,0x0308}, {0x03AC,0x03B1,0x0301}, {0x03AD,0x03B5,0x0301}, {0x03AE,0x03B7,0x0301}, {0x03AF,0x03B9,0x0301},
    {0x03B0,0x03CB,0x0301}, {0x03CA,0x03B9,0x0308}, {0x03CB,0x03C5,0x0308}, {0x03CC,0x03BF,0x0301}, {0x03CD,0x03C5,0x0301}, {0x03CE,0x03C9,0x0301}, {0x0400,0x0415,0x0300},
    {0x0401,0x0415,0x0308}, {0x0403,0x0413,0x0301}, {0x0407,0x0406,0x0308}, {0x040C,0x041A,0x0301}, {0x040D,0x0418,0x0300}, {0x040E,0x0423,0x0306}, {0x0419,0x0418,0x0306},
    {0x0439,0x0438,0x0306}, {0x0450,0x0435,0x0300}, {0x0451,0x0435,0x0308}, {0x0453,0x0433,0x0301}, {0x0457,0x0456,0x0308}, {0x045C,0x043A,0x0301}, {0x045D,0x0438,0x0300},
    {0x045E,0x0443,0x0306}, {0x0476,0x0474,0x030F}, {0x0477,0x0475,0x030F}, {0x1E00,0x0041,0x0325}, {0x1E01,0x0061,0x0325}, {0x1E02,0x0042,0x0307}, {0x1E03,0x0062,0x0307},
    {0x1E04,0x0042,0x0323}, {0x1E05,0x0062,0x0323}, {0x1E06,0x0042,0x0331}, {0x1E07,0x0062,0x0331}, {0x1E08,0x00C7,0x0301}, {0x1E09,0x00E7,0x0301}, {0x1E0A,0x0044,0x0307},
    {0x1E0B,0x0064,0x0307}, {0x1E0C,0x0044,0x0323}, {0x1E0D,0x0064,0x0323}, {0x1E0E,0x0044,0x0331}, {0x1E0F,0x0064,0x0331}, {0x1E10,0x0044,0x0327}, {0x1E11,0x0064,0x0327},
    {0x1E12,0x0044,0x032D}, {0x1E13,0x0064,0x032D}, {0x1E14,0x0112,0x0300}, {0x1E15,0x0113,0x0300}, {0x1E16,0x0112,0x0301}, {0x1E17,0x0113,0x0301}, {0x1E18,0x0045,0x032D},
    {0x1E19,0x0065,0x032D}, {0x1E1A,0x0045,0x0330}, {0x1E1B,0x0065,0x0330}, {0x1E1C,0x0228,0x0306}, {0x1E1D,0x0229,0x0306}, {0x1E1E,0x0046,0x0307}, {0x1E1F,0x0066,0x0307},
    {0x1E20,0x0047,0x0304}, {0x1E21,0x0067,0x0304}, {0x1E22,0x0048,0x0307}, {0x1E23,0x0068,0x0307}, {0x1E24,0x0048,0x0323}, {0x1E25,0x0068,0x0323}, {0x1E26,0x0048,0x0308},
    {0x1E27,0x0068,0x0308}, {0x1E28,0x0048,0x0327}, {0x1E29,0x0068,0x0327}, {0x1E2A,0x0048,0x032E}, {0x1E2B,0x0068,0x032E}, {0x1E2C,0x0049,0x0330}, {0x1E2D,0x0069,0x0330},
    {0x1E2E,0x00CF,0x0301}, {0x1E2F,0x00EF,0x0301}, {0x1E30,0x004B,0x0301}, {0x1E31,0x006B,0x0301}, {0x1E32,0x004B,0x0323}, {0x1E33,0x006B,0x0323}, {0x1E34,0x004B,0x0331},
    {0x1E35,0x006B,0x0331}, {0x1E36,0x004C,0x0323}, {0x1E37,0x006C,0x0323}, {0x1E38,0x1E36,0x0304}, {0x1E39,0x1E37,0x0304}, {0x1E3A,0x004C,0x0331}, {0x1E3B,0x006C,0x0331},
    {0x1E3C,0x004C,0x032D}, {0x1E3D,0x006C,0x032D}, {0x1E3E,0x004D,0x0301}, {0x1E3F,0x006D,0x0301}, {0x1E40,0x004D,0x0307}, {0x1E41,0x006D,0x0307}, {0x1E42,0x004D,0x0323},
    {0x1E43,0x006D,0x0323}, {0x1E44,0x004E,0x0307}, {0x1E45,0x006E,0x0307}, {0x1E46,0x004E,0x0323}, {0x1E47,0x006E,0x0323}, {0x1E48,0x004E,0x0331}, {0x1E49,0x006E,0x0331},
    {0x1E4A,0x004E,0x032D}, {0x1E4B,0x006E,0x032D}, {0x1E4C,0x00D5,0x0301}, {0x1E4D,0x00F5,0x0301}, {0x1E4E,0x00D5,0x0308}, {0x1E4F,0x00F5,0x0308}, {0x1E50,0x014C,0x0300},
    {0x1E51,0x014D,0x0300}, {0x1E52,0x014C,0x0301}, {0x1E53,0x014D,0x0301}, {0x1E54,0x0050,0x0301}, {0x1E55,0x0070,0x0301}, {0x1E56,0x0050,0x0307}, {0x1E57,0x0070,0x0307},
    {0x1E58,0x0052,0x0307}, {0x1E59,0x0072,0x0307}, {0x1E5A,0x0052,0x0323}, {0x1E5B,0x0072,0x0323}, {0x1E5C,0x1E5A,0x0304}, {0x1E5D,0x1E5B,0x0304}, {0x1E5E,0x0052,0x0331},
    {0x1E5F,0x0072,0x0331}, {0x1E60,0x0053,0x0307}, {0x1E61,0x0073,0x0307}, {0x1E62,0x0053,0x0323}, {0x1E63,0x0073,0x0323}, {0x1E64,0x015A,0x0307}, {0x1E65,0x015B,0x0307},
    {0x1E66,0x0160,0x0307}, {0x1E67,0x0161,0x0307}, {0x1E68,0x1E62,0x0307}, {0x1E69,0x1E63,0x0307}, {0x1E6A,0x0054,0x0307}, {0x1E6B,0x0074,0x0307}, {0x1E6C,0x0054,0x0323},
    {0x1E6D,0x0074,0x0323}, {0x1E6E,0x0054,0x0331}, {0x1E6F,0x0074,0x0331}, {0x1E70,0x0054,0x032D}, {0x1E71,0x0074,0x032D}, {0x1E72,0x0055,0x0324}, {0x1E73,0x0075,0x0324},
    {0x1E74,0x0055,0x0330}, {0x1E75,0x0075,0x0330}, {0x1E76,0x0055,0x032D}, {0x1E77,0x0075,0x032D}, {0x1E78,0x0168,0x0301}, {0x1E79,0x0169,0x0301}, {0x1E7A,0x016A,0x0308},
    {0x1E7B,0x016B,0x0308}, {0x1E7C,0x0056,0x0303}, {0x1E7D,0x0076,0x0303}, {0x1E7E,0x0056,0x0323}, {0x1E7F,0x0076,0x0323}, {0x1E80,0x0057,0x0300}, {0x1E81,0x0077,0x0300},
    {0x1E82,0x0057,0x0301}, {0x1E83,0x0077,0x0301}, {0x1E84,0x0057,0x0308}, {0x1E85,0x0077,0x0308}, {0x1E86,0x0057,0x0307}, {0x1E87,0x0077,0x0307}, {0x1E88,0x0057,0x0323},
    {0x1E89,0x0077,0x0323}, {0x1E8A,0x0058,0x0307}, {0x1E8B,0x0078,0x0307}, {0x1E8C,0x0058,0x0308}, {0x1E8D,0x0078,0x0308}, {0x1E8E,0x0059,0x0307}, {0x1E8F,0x0079,0x0307},
    {0x1E90,0x005A,0x0302}, {0x1E91,0x007A,0x0302}, {0x1E92,0x005A,0x0323}, {0x1E93,0x007A,0x0323}, {0x1E94,0x005A,0x0331}, {0x1E95,0x007A,0x0331}, {0x1E96,0x0068,0x0331},
    {0x1E97,0x0074,0x0308}, {0x1E98,0x0077,0x030A}, {0x1E99,0x0079,0x030A}, {0x1E9B,0x017F,0x0307}, {0x1EA0,0x0041,0x0323}, {0x1EA1,0x0061,0x0323}, {0x1EA2,0x0041,0x0309},
    {0x1EA3,0x0061,0x0309}, {0x1EA4,0x00C2,0x0301}, {0x1EA5,0x00E2,0x0301}, {0x1EA6,0x00C2,0x0300}, {0x1EA7,0x00E2,0x0300}, {0x1EA8,0x00C2,0x0309}, {0x1EA9,0x00E2,0x0309},
    {0x1EAA,0x00C2,0x0303}, {0x1EAB,0x00E2,0x0303}, {0x1EAC,0x1EA0,0x0302}, {0x1EAD,0x1EA1,0x0302}, {0x1EAE,0x0102,0x0301}, {0x1EAF,0x0103,0x0301}, {0x1EB0,0x0102,0x0300},
    {0x1EB1,0x0103,0x0300}, {0x1EB2,0x0102,0x0309}, {0x1EB3,0x0103,0x0309}, {0x1EB4,0x0102,0x0303}, {0x1EB5,0x0103,0x0303}, {0x1EB6,0x1EA0,0x0306}, {0x1EB7,0x1EA1,0x0306},
    {0x1EB8,0x0045,0x0323}, {0x1EB9,0x0065,0x0323}, {0x1EBA,0x0045,0x0309}, {0x1EBB,0x0065,0x0309}, {0x1EBC,0x0045,0x0303}, {0x1EBD,0x0065,0x0303}, {0x1EBE,0x00CA,0x0301},
    {0x1EBF,0x00EA,0x0301}, {0x1EC0,0x00CA,0x0300}, {0x1EC1,0x00EA,0x0300}, {0x1EC2,0x00CA,0x0309}, {0x1EC3,0x00EA,0x0309}, {0x1EC4,0x00CA,0x0303}, {0x1EC5,0x00EA,0x0303},
    {0x1EC6,0x1EB8,0x0302}, {0x1EC7,0x1EB9,0x0302}, {0x1EC8,0x0049,0x0309}, {0x1EC9,0x0069,0x0309}, {0x1ECA,0x0049,0x0323}, {0x1ECB,0x0069,0x0323}, {0x1ECC,0x004F,0x0323},
    {0x1ECD,0x006F,0x0323}, {0x1ECE,0x004F,0x0309}, {0x1ECF,0x006F,0x0309}, {0x1ED0,0x00D4,0x0301}, {0x1ED1,0x00F4,0x0301}, {0x1ED2,0x00D4,0x0300}, {0x1ED3,0x00F4,0x0300},
    {0x1ED4,0x00D4,0x0309}, {0x1ED5,0x00F4,0x0309}, {0x1ED6,0x00D4,0x0303}, {0x1ED7,0x00F4,0x0303}, {0x1ED8,0x1ECC,0x0302}, {0x1ED9,0x1ECD,0x0302}, {0x1EDA,0x01A0,0x0301},
    {0x1EDB,0x01A1,0x0301}, {0x1EDC,0x01A0,0x0300}, {0x1EDD,0x01A1,0x0300}, {0x1EDE,0x01A0,0x0309}, {0x1EDF,0x01A1,0x0309}, {0x1EE0,0x01A0,0x0303}, {0x1EE1,0x01A1,0x0303},
    {0x1EE2,0x01A0,0x0323}, {0x1EE3,0x01A1,0x0323}, {0x1EE4,0x0055,0x0323}, {0x1EE5,0x0075,0x0323}, {0x1EE6,0x0055,0x0309}, {0x1EE7,0x0075,0x0309}, {0x1EE8,0x01AF,0x0301},
    {0x1EE9,0x01B0,0x0301}, {0x1EEA,0x01AF,0x0300}, {0x1EEB,0x01B0,0x0300}, {0x1EEC,0x01AF,0x0309}, {0x1EED,0x01B0,0x0309}, {0x1EEE,0x01AF,0x0303}, {0x1EEF,0x01B0,0x0303},
    {0x1EF0,0x01AF,0x0323}, {0x1EF1,0x01B0,0x0323}, {0x1EF2,0x0059,0x0300}, {0x1EF3,0x0079,0x0300}, {0x1EF4,0x0059,0x0323}, {0x1EF5,0x0079,0x0323}, {0x1EF6,0x0059,0x0309},
    {0x1EF7,0x0079,0x0309}, {0x1EF8,0x0059,0x0303}, {0x1EF9,0x0079,0x0303},
};
#define NORM_N (sizeof norm_tab / sizeof norm_tab[0])
static void nfd_put(SB *b, uint32_t c) {
    if (c >= 0xC0 && c < 0x10000) {
        int lo = 0, hi = (int)NORM_N - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (norm_tab[mid][0] == c) {
                nfd_put(b, norm_tab[mid][1]);
                sb_putc(b, norm_tab[mid][2]);
                return;
            }
            if (norm_tab[mid][0] < c)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    sb_putc(b, c);
}
NATIVE(string_normalize) {
    THIS_STR("normalize");
    bool compose = true;
    if (!is_undef(ARG(0))) {
        JSString *f = arg_str(rt, argv[0]);
        if (!f) return JS_EXC;
        const char *ok[] = {"NFC", "NFD", "NFKC", "NFKD"};
        int form = -1;
        for (int i = 0; i < 4; i++)
            if (str_eq(rt, f, str_cstr(rt, ok[i]))) form = i;
        if (form < 0) return throw_error(rt, E_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
        compose = form == 0 || form == 2;
    }
    bool plain = true;
    for (uint32_t i = 0; i < s->len && plain; i++)
        if (str_at(s, i) >= 0xC0) plain = false;
    if (plain) return mkstr(s);
    SB d;
    sb_init(&d, rt);
    for (uint32_t i = 0; i < s->len; i++) nfd_put(&d, str_at(s, i));
    JSString *ds = sb_finish(&d);
    if (!compose) return mkstr(ds);
    /* compose: a character combines with the preceding output character when a table entry says so */
    uint16_t *out = (uint16_t *)malloc((size_t)(ds->len + 1) * 2);
    if (!out) return throw_error(rt, E_RANGE, "out of memory");
    uint32_t n = 0;
    for (uint32_t i = 0; i < ds->len; i++) {
        uint32_t c = str_at(ds, i);
        if (n && c >= 0x300) {
            bool done = false;
            for (size_t k = 0; k < NORM_N; k++)
                if (norm_tab[k][2] == c && norm_tab[k][1] == out[n - 1]) {
                    out[n - 1] = norm_tab[k][0];
                    done = true;
                    break;
                }
            if (done) continue;
        }
        out[n++] = (uint16_t)c;
    }
    SB r;
    sb_init(&r, rt);
    for (uint32_t i = 0; i < n; i++) sb_putc(&r, out[i]);
    free(out);
    return mkstr(sb_finish(&r));
}
NATIVE(string_is_well_formed) {
    THIS_STR("isWellFormed");
    bool fix = MAGIC == 1;
    SB b;
    sb_init(&b, rt);
    bool ok = true;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) {
            sb_putc(&b, c);
            sb_putc(&b, str_at(s, ++i));
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            ok = false;
            c = 0xFFFD;
        }
        sb_putc(&b, c);
    }
    if (!fix) {
        sb_free(&b);
        return mkbool(ok);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

/* GetSubstitution; caps[0..ncaps) are captures (undefined or string), groups object or undefined */
JSString *get_substitution(js_rt *rt, JSString *matched, JSString *str, uint32_t pos, jsval *caps, int ncaps, jsval groups,
                           JSString *rep) {
    str_flat(rt, rep);
    str_flat(rt, str);
    SB b;
    sb_init(&b, rt);
    uint32_t tail = pos + matched->len;
    for (uint32_t i = 0; i < rep->len; i++) {
        uint32_t c = str_at(rep, i);
        if (c != '$' || i + 1 >= rep->len) {
            sb_putc(&b, c);
            continue;
        }
        uint32_t d = str_at(rep, i + 1);
        if (d == '$') {
            sb_putc(&b, '$');
            i++;
        } else if (d == '&') {
            sb_putstr(&b, matched);
            i++;
        } else if (d == '`') {
            sb_putsub(&b, str, 0, pos);
            i++;
        } else if (d == '\'') {
            if (tail < str->len) sb_putsub(&b, str, tail, str->len);
            i++;
        } else if (d >= '0' && d <= '9') {
            int n = (int)(d - '0');
            uint32_t used = 1;
            if (i + 2 < rep->len) {
                uint32_t e = str_at(rep, i + 2);
                if (e >= '0' && e <= '9') {
                    int n2 = n * 10 + (int)(e - '0');
                    if (n2 >= 1 && n2 <= ncaps) {
                        n = n2;
                        used = 2;
                    }
                }
            }
            if (n >= 1 && n <= ncaps) {
                jsval cv = caps[n - 1];
                if (is_str(cv)) sb_putstr(&b, vstr(cv));
                i += used;
            } else
                sb_putc(&b, '$');
        } else if (d == '<' && !is_undef(groups)) {
            uint32_t j = i + 2;
            while (j < rep->len && str_at(rep, j) != '>') j++;
            if (j >= rep->len) {
                sb_putc(&b, '$');
                continue;
            }
            JSString *name = str_sub(rt, rep, i + 2, j);
            jsval v = get_p(rt, groups, atomize(rt, name));
            if (v == JS_EXC) {
                sb_free(&b);
                return 0;
            }
            if (!is_undef(v)) {
                JSString *vs = to_string(rt, v);
                if (!vs) {
                    sb_free(&b);
                    return 0;
                }
                sb_putstr(&b, vs);
            }
            i = j;
        } else
            sb_putc(&b, '$');
    }
    return sb_finish(&b);
}

/* dispatch to Symbol.replace/split/match/search/matchAll of a regexp-like argument; returns JS_HOLE if not applicable */
static jsval dispatch_sym(js_rt *rt, jsval pattern, jsval sym, jsval thisv, int argc, jsval *args) {
    if (is_nullish(pattern)) return JS_HOLE;
    if (!is_obj(pattern)) return JS_HOLE;
    jsval m = get_p(rt, pattern, sym);
    TRY(m);
    if (is_nullish(m)) return JS_HOLE;
    jsval a[2] = {thisv, argc > 0 ? args[0] : JS_UNDEF};
    return call_fn(rt, m, pattern, argc + 1, a);
}

NATIVE(string_replace) {
    bool all = MAGIC == 1;
    if (is_nullish(this_)) THROW_TYPE("String.prototype.replace called on null or undefined");
    jsval pattern = ARG(0), repl = ARG(1);
    if (!is_nullish(pattern)) {
        if (all && is_cls(pattern, CLS_REGEXP)) {
            jsval fl = get_p(rt, pattern, ATOM(flags));
            TRY(fl);
            JSString *fs = to_string(rt, fl);
            if (!fs) return JS_EXC;
            bool g = false;
            str_flat(rt, fs);
            for (uint32_t i = 0; i < fs->len; i++)
                if (str_at(fs, i) == 'g') g = true;
            if (!g) THROW_TYPE("String.prototype.replaceAll called with a non-global RegExp argument");
        }
        jsval r = dispatch_sym(rt, pattern, SYM(replace), this_, 1, &argv[1]);
        if (r != JS_HOLE) return r;
    }
    JSString *s = this_str(rt, this_, "replace");
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    JSString *p = arg_str(rt, pattern);
    if (!p) return JS_EXC;
    bool fn = is_callable(repl);
    JSString *rs = 0;
    if (!fn) {
        rs = arg_str(rt, repl);
        if (!rs) return JS_EXC;
    }
    SB b;
    sb_init(&b, rt);
    uint32_t last = 0;
    int32_t pos = str_indexof(rt, s, p, 0);
    uint32_t adv = p->len ? p->len : 1;
    while (pos >= 0) {
        sb_putsub(&b, s, last, (uint32_t)pos);
        JSString *piece;
        if (fn) {
            jsval args[3] = {mkstr(p), mknum(pos), mkstr(s)};
            jsval r = call_fn(rt, repl, JS_UNDEF, 3, args);
            if (r == JS_EXC) goto fail;
            piece = to_string(rt, r);
        } else
            piece = get_substitution(rt, p, s, (uint32_t)pos, 0, 0, JS_UNDEF, rs);
        if (!piece) goto fail;
        sb_putstr(&b, piece);
        last = (uint32_t)pos + p->len;
        if (!all) break;
        if ((uint32_t)pos + adv > s->len) break;
        pos = str_indexof(rt, s, p, pos + (int32_t)adv);
    }
    sb_putsub(&b, s, last, s->len);
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
fail:
    sb_free(&b);
    return JS_EXC;
}

NATIVE(string_split) {
    if (is_nullish(this_)) THROW_TYPE("String.prototype.split called on null or undefined");
    jsval sep = ARG(0);
    if (!is_nullish(sep)) {
        jsval r = dispatch_sym(rt, sep, SYM(split), this_, 1, &argv[1]);
        if (r != JS_HOLE) return r;
    }
    JSString *s = this_str(rt, this_, "split");
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    uint32_t lim = 0xFFFFFFFFu;
    if (!is_undef(ARG(1))) {
        double d;
        if (to_number(rt, argv[1], &d) < 0) return JS_EXC;
        lim = to_uint32(d);
    }
    JSString *p = arg_str(rt, sep);
    if (!p) return JS_EXC;
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    if (lim == 0) return mkobj(a);
    if (is_undef(sep)) {
        array_push(rt, a, mkstr(s));
        return mkobj(a);
    }
    if (p->len == 0) {
        for (uint32_t i = 0; i < s->len && a->u.arr.len < lim; i++) array_push(rt, a, mkstr(str_char(rt, str_at(s, i))));
        return mkobj(a);
    }
    if (s->len == 0) {
        array_push(rt, a, mkstr(s));
        return mkobj(a);
    }
    uint32_t last = 0;
    int32_t pos;
    while ((pos = str_indexof(rt, s, p, (int32_t)last)) >= 0) {
        array_push(rt, a, mkstr(str_sub(rt, s, last, (uint32_t)pos)));
        if (a->u.arr.len >= lim) return mkobj(a);
        last = (uint32_t)pos + p->len;
    }
    array_push(rt, a, mkstr(str_sub(rt, s, last, s->len)));
    return mkobj(a);
}

NATIVE(string_match) {
    int m = MAGIC; /* 0 match 1 matchAll 2 search */
    if (is_nullish(this_)) THROW_TYPE("String.prototype.%s called on null or undefined", m == 0 ? "match" : m == 1 ? "matchAll" : "search");
    jsval rx = ARG(0);
    if (!is_nullish(rx)) {
        if (m == 1 && is_cls(rx, CLS_REGEXP)) {
            jsval fl = get_p(rt, rx, ATOM(flags));
            TRY(fl);
            JSString *fs = to_string(rt, fl);
            if (!fs) return JS_EXC;
            bool g = false;
            str_flat(rt, fs);
            for (uint32_t i = 0; i < fs->len; i++)
                if (str_at(fs, i) == 'g') g = true;
            if (!g) THROW_TYPE("String.prototype.matchAll called with a non-global RegExp argument");
        }
        jsval sym = m == 0 ? SYM(match) : m == 1 ? SYM(matchAll) : SYM(search);
        jsval r = dispatch_sym(rt, rx, sym, this_, 0, 0);
        if (r != JS_HOLE) return r;
    }
    JSString *s = this_str(rt, this_, "match");
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    jsval flags = m == 1 ? mkstr(str_cstr(rt, "g")) : JS_UNDEF;
    jsval re = regexp_create(rt, is_undef(rx) ? ATOM(empty) : rx, flags, rt->intr[I_RegExpProto]);
    TRY(re);
    push_tmp(rt, re);
    jsval sym = m == 0 ? SYM(match) : m == 1 ? SYM(matchAll) : SYM(search);
    return call_method(rt, re, sym, 1, &(jsval){mkstr(s)});
}

/* string iterator */
NATIVE(string_iter_next) {
    if (!is_cls(this_, CLS_STRING_ITER)) THROW_TYPE("next method called on incompatible receiver");
    JSObject *it = vobj(this_);
    if (is_undef(it->u.it.a)) return iter_result(rt, JS_UNDEF, true);
    JSString *s = str_flat(rt, vstr(it->u.it.a));
    uint32_t i = it->u.it.i;
    if (i >= s->len) {
        it->u.it.a = JS_UNDEF;
        return iter_result(rt, JS_UNDEF, true);
    }
    uint32_t c = str_at(s, i);
    uint32_t n = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
        uint32_t d = str_at(s, i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) n = 2;
    }
    it->u.it.i = i + n;
    return iter_result(rt, mkstr(str_sub(rt, s, i, i + n)), false);
}
NATIVE(string_iterator) {
    THIS_STR("[Symbol.iterator]");
    JSObject *it = new_object(rt, rt->intr[I_StringIterProto], CLS_STRING_ITER);
    it->u.it.a = mkstr(s);
    it->u.it.i = 0;
    return mkobj(it);
}

NATIVE(string_html) {
    THIS_STR("anchor");
    static const char *const tags[] = {"a", "b", "i", "big", "small", "tt", "sub", "sup", "strike", "font", "blink", "font"};
    static const char *const attrs[] = {"name", 0, 0, 0, 0, 0, 0, 0, 0, "color", 0, "size"};
    int m = MAGIC;
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, "<");
    sb_puts(&b, tags[m]);
    if (attrs[m]) {
        JSString *v = to_string(rt, ARG(0));
        if (!v) {
            sb_free(&b);
            return JS_EXC;
        }
        sb_puts(&b, " ");
        sb_puts(&b, attrs[m]);
        sb_puts(&b, "=\"");
        str_flat(rt, v);
        for (uint32_t i = 0; i < v->len; i++) {
            if (str_at(v, i) == '"')
                sb_puts(&b, "&quot;");
            else
                sb_putc(&b, str_at(v, i));
        }
        sb_puts(&b, "\"");
    }
    sb_puts(&b, ">");
    sb_putstr(&b, s);
    sb_puts(&b, "</");
    sb_puts(&b, tags[m]);
    sb_puts(&b, ">");
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

void init_string(js_rt *rt) {
    JSObject *sp = rt->intr[I_StringProto];
    JSObject *ctor = new_ctor(rt, "String", string_ctor, 1, sp);
    def_method(rt, ctor, "fromCharCode", string_from_char_code, 1, 0);
    def_method(rt, ctor, "fromCodePoint", string_from_code_point, 1, 0);
    def_method(rt, ctor, "raw", string_raw, 1, 0);
    def_method(rt, sp, "toString", string_to_string, 0, 0);
    def_method(rt, sp, "valueOf", string_to_string, 0, 0);
    def_method(rt, sp, "charAt", string_char_at, 1, 0);
    def_method(rt, sp, "charCodeAt", string_char_at, 1, 1);
    def_method(rt, sp, "codePointAt", string_char_at, 1, 2);
    def_method(rt, sp, "at", string_char_at, 1, 3);
    def_method(rt, sp, "indexOf", string_index_of, 1, 0);
    def_method(rt, sp, "lastIndexOf", string_index_of, 1, 1);
    def_method(rt, sp, "includes", string_index_of, 1, 2);
    def_method(rt, sp, "startsWith", string_index_of, 1, 3);
    def_method(rt, sp, "endsWith", string_index_of, 1, 4);
    def_method(rt, sp, "slice", string_slice, 2, 0);
    def_method(rt, sp, "substring", string_slice, 2, 1);
    def_method(rt, sp, "substr", string_slice, 2, 2);
    def_method(rt, sp, "toLowerCase", string_case, 0, 0);
    def_method(rt, sp, "toUpperCase", string_case, 0, 1);
    def_method(rt, sp, "toLocaleLowerCase", string_case, 0, 0);
    def_method(rt, sp, "toLocaleUpperCase", string_case, 0, 1);
    def_method(rt, sp, "trim", string_trim, 0, 0);
    JSObject *ts = def_method(rt, sp, "trimStart", string_trim, 0, 1);
    JSObject *te = def_method(rt, sp, "trimEnd", string_trim, 0, 2);
    def_value(rt, sp, "trimLeft", mkobj(ts));
    def_value(rt, sp, "trimRight", mkobj(te));
    def_method(rt, sp, "padStart", string_pad, 1, 0);
    def_method(rt, sp, "padEnd", string_pad, 1, 1);
    def_method(rt, sp, "repeat", string_repeat, 1, 0);
    def_method(rt, sp, "concat", string_concat, 1, 0);
    def_method(rt, sp, "localeCompare", string_locale_compare, 1, 0);
    def_method(rt, sp, "normalize", string_normalize, 0, 0);
    def_method(rt, sp, "isWellFormed", string_is_well_formed, 0, 0);
    def_method(rt, sp, "toWellFormed", string_is_well_formed, 0, 1);
    def_method(rt, sp, "replace", string_replace, 2, 0);
    def_method(rt, sp, "replaceAll", string_replace, 2, 1);
    def_method(rt, sp, "split", string_split, 2, 0);
    def_method(rt, sp, "match", string_match, 1, 0);
    def_method(rt, sp, "matchAll", string_match, 1, 1);
    def_method(rt, sp, "search", string_match, 1, 2);
    def_method_k(rt, sp, SYM(iterator), "[Symbol.iterator]", string_iterator, 0, 0);
    const char *html[] = {"anchor", "bold", "italics", "big", "small", "fixed", "sub", "sup", "strike", "fontcolor", "blink", "fontsize"};
    for (int i = 0; i < 12; i++) def_method(rt, sp, html[i], string_html, (i == 0 || i == 9 || i == 11) ? 1 : 0, i);
    JSObject *sip = new_object(rt, rt->intr[I_IteratorProto], CLS_OBJECT);
    rt->intr[I_StringIterProto] = sip;
    def_method(rt, sip, "next", string_iter_next, 0, 0);
    def_prop(rt, sip, SYM(toStringTag), mkstr(str_cstr(rt, "String Iterator")), P_C);
}
