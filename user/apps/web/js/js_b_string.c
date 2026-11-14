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
NATIVE(string_normalize) {
    THIS_STR("normalize");
    if (!is_undef(ARG(0))) {
        JSString *f = arg_str(rt, argv[0]);
        if (!f) return JS_EXC;
        const char *ok[] = {"NFC", "NFD", "NFKC", "NFKD"};
        bool good = false;
        for (int i = 0; i < 4; i++)
            if (str_eq(rt, f, str_cstr(rt, ok[i]))) good = true;
        if (!good) return throw_error(rt, E_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    return mkstr(s);
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
