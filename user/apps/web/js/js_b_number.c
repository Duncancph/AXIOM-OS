/* AXIOM JavaScript engine: Number, Math, BigInt. */
#include "js_int.h"
#include "js_ops.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

/* ---------------- Number ---------------- */
NATIVE(number_ctor) {
    jsval v = mknum(0);
    if (argc > 0) {
        jsval n;
        if (to_numeric(rt, argv[0], &n) < 0) return JS_EXC;
        v = is_big(n) ? mknum(big_to_double((JSBig *)vptr(n))) : n;
    }
    if (is_undef(rt->new_target)) return v;
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_NumberProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_NUMBER);
    o->u.prim.v = v;
    return mkobj(o);
}
static int this_num(js_rt *rt, jsval t, double *out, const char *fn) {
    if (is_num(t)) {
        *out = vnum(t);
        return 0;
    }
    if (is_cls(t, CLS_NUMBER)) {
        *out = vnum(vobj(t)->u.prim.v);
        return 0;
    }
    throw_error(rt, E_TYPE, "Number.prototype.%s requires that 'this' be a Number", fn);
    return -1;
}
NATIVE(number_is) {
    jsval v = ARG(0);
    int m = MAGIC; /* 0 isInteger 1 isSafeInteger 2 isFinite 3 isNaN */
    if (!is_num(v)) return JS_FALSE;
    double d = vnum(v);
    switch (m) {
    case 0: return mkbool(js_isfinite(d) && js_trunc(d) == d);
    case 1: return mkbool(js_isfinite(d) && js_trunc(d) == d && d <= 9007199254740991.0 && d >= -9007199254740991.0);
    case 2: return mkbool(js_isfinite(d));
    default: return mkbool(d != d);
    }
}
NATIVE(number_to_string) {
    double d;
    if (this_num(rt, this_, &d, "toString") < 0) return JS_EXC;
    int radix = 10;
    if (!is_undef(ARG(0))) {
        double r;
        if (to_int(rt, argv[0], &r) < 0) return JS_EXC;
        if (r < 2 || r > 36) return throw_error(rt, E_RANGE, "toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    if (radix == 10) return mkstr(num_to_str(rt, d));
    char *buf = (char *)malloc(2300);
    if (!buf) return throw_error(rt, E_RANGE, "out of memory");
    int n = num_to_radix(d, radix, buf);
    JSString *s = str_new8(rt, (const uint8_t *)buf, (uint32_t)n);
    free(buf);
    return mkstr(s);
}
NATIVE(number_to_fixed) {
    double d;
    if (this_num(rt, this_, &d, "toFixed") < 0) return JS_EXC;
    double f;
    if (to_int(rt, ARG(0), &f) < 0) return JS_EXC;
    if (f < 0 || f > 100) return throw_error(rt, E_RANGE, "toFixed() digits argument must be between 0 and 100");
    if (!js_isfinite(d)) return mkstr(num_to_str(rt, d));
    char *buf = (char *)malloc(1400);
    int n = num_to_fixed(d, (int)f, buf);
    JSString *s = str_new8(rt, (const uint8_t *)buf, (uint32_t)n);
    free(buf);
    return mkstr(s);
}
NATIVE(number_to_exp) {
    double d;
    if (this_num(rt, this_, &d, "toExponential") < 0) return JS_EXC;
    double f;
    if (to_int(rt, ARG(0), &f) < 0) return JS_EXC;
    if (!js_isfinite(d)) return mkstr(num_to_str(rt, d));
    if (f < 0 || f > 100) return throw_error(rt, E_RANGE, "toExponential() argument must be between 0 and 100");
    char *buf = (char *)malloc(1400);
    int n = num_to_exp(d, is_undef(ARG(0)) ? -1 : (int)f, buf);
    JSString *s = str_new8(rt, (const uint8_t *)buf, (uint32_t)n);
    free(buf);
    return mkstr(s);
}
NATIVE(number_to_prec) {
    double d;
    if (this_num(rt, this_, &d, "toPrecision") < 0) return JS_EXC;
    if (is_undef(ARG(0))) return mkstr(num_to_str(rt, d));
    double p;
    if (to_int(rt, argv[0], &p) < 0) return JS_EXC;
    if (!js_isfinite(d)) return mkstr(num_to_str(rt, d));
    if (p < 1 || p > 100) return throw_error(rt, E_RANGE, "toPrecision() argument must be between 1 and 100");
    char *buf = (char *)malloc(1400);
    int n = num_to_prec(d, (int)p, buf);
    JSString *s = str_new8(rt, (const uint8_t *)buf, (uint32_t)n);
    free(buf);
    return mkstr(s);
}
NATIVE(number_value_of) {
    double d;
    if (this_num(rt, this_, &d, "valueOf") < 0) return JS_EXC;
    return mknum(d);
}

/* en-US style formatting with optional {minimumFractionDigits, maximumFractionDigits, style, currency, useGrouping} */
JSString *format_locale_number(js_rt *rt, double d, jsval opts) {
    int minf = 0, maxf = 3;
    bool grouping = true, percent = false;
    const char *cur = 0;
    if (is_obj(opts)) {
        jsval st = get_c(rt, opts, "style");
        if (st == JS_EXC) return 0;
        if (is_str(st)) {
            JSString *s = vstr(st);
            if (str_eq(rt, s, str_cstr(rt, "percent"))) {
                percent = true;
                maxf = 0;
            } else if (str_eq(rt, s, str_cstr(rt, "currency"))) {
                jsval c = get_c(rt, opts, "currency");
                if (c == JS_EXC) return 0;
                cur = "$";
                if (is_str(c)) {
                    JSString *cs = vstr(c);
                    if (str_eq(rt, cs, str_cstr(rt, "EUR"))) cur = "\xE2\x82\xAC";
                    else if (str_eq(rt, cs, str_cstr(rt, "GBP"))) cur = "\xC2\xA3";
                    else if (str_eq(rt, cs, str_cstr(rt, "JPY"))) cur = "\xC2\xA5";
                }
                minf = maxf = 2;
            }
        }
        jsval v = get_c(rt, opts, "minimumFractionDigits");
        if (v == JS_EXC) return 0;
        if (is_num(v)) {
            minf = (int)vnum(v);
            if (maxf < minf) maxf = minf;
        }
        v = get_c(rt, opts, "maximumFractionDigits");
        if (v == JS_EXC) return 0;
        if (is_num(v)) {
            maxf = (int)vnum(v);
            if (minf > maxf) minf = maxf;
        }
        v = get_c(rt, opts, "useGrouping");
        if (v == JS_EXC) return 0;
        if (!is_undef(v)) grouping = truthy(v);
    }
    if (minf < 0) minf = 0;
    if (maxf > 20) maxf = 20;
    if (d != d) return str_cstr(rt, "NaN");
    SB b;
    sb_init(&b, rt);
    if (js_signbit(d) && !(d == 0 && !percent && 0)) {
        if (d != 0 || js_signbit(d)) sb_puts(&b, "-");
        d = -d;
    }
    if (cur) sb_pututf8(&b, cur, strlen(cur));
    if (d == JS_INF) {
        sb_pututf8(&b, "\xE2\x88\x9E", 3);
    } else {
        if (percent) d *= 100;
        char *buf = (char *)malloc(1400);
        int n = num_to_fixed(d, maxf, buf);
        int dot = -1;
        for (int i = 0; i < n; i++)
            if (buf[i] == '.') dot = i;
        int ip = dot < 0 ? n : dot;
        /* trim fraction to minf */
        int fend = n;
        if (dot >= 0) {
            while (fend > dot + 1 + minf && buf[fend - 1] == '0') fend--;
            if (fend == dot + 1) fend = dot;
        }
        for (int i = 0; i < ip; i++) {
            sb_putc(&b, (uint8_t)buf[i]);
            int rem = ip - 1 - i;
            if (grouping && rem > 0 && rem % 3 == 0) sb_putc(&b, ',');
        }
        if (fend > ip) sb_putn(&b, buf + ip, (uint32_t)(fend - ip));
        else if (minf > 0) {
            sb_putc(&b, '.');
            for (int i = 0; i < minf; i++) sb_putc(&b, '0');
        }
        free(buf);
    }
    if (percent) sb_putc(&b, '%');
    return sb_finish(&b);
}
NATIVE(number_to_locale) {
    double d;
    if (this_num(rt, this_, &d, "toLocaleString") < 0) return JS_EXC;
    JSString *s = format_locale_number(rt, d, ARG(1));
    return s ? mkstr(s) : JS_EXC;
}

void init_number(js_rt *rt) {
    JSObject *np = rt->intr[I_NumberProto];
    JSObject *c = new_ctor(rt, "Number", number_ctor, 1, np);
    def_method(rt, c, "isInteger", number_is, 1, 0);
    def_method(rt, c, "isSafeInteger", number_is, 1, 1);
    def_method(rt, c, "isFinite", number_is, 1, 2);
    def_method(rt, c, "isNaN", number_is, 1, 3);
    def_prop(rt, c, atom_c(rt, "EPSILON"), mknum(2.220446049250313e-16), 0);
    def_prop(rt, c, atom_c(rt, "MAX_SAFE_INTEGER"), mknum(9007199254740991.0), 0);
    def_prop(rt, c, atom_c(rt, "MIN_SAFE_INTEGER"), mknum(-9007199254740991.0), 0);
    def_prop(rt, c, atom_c(rt, "MAX_VALUE"), mknum(1.7976931348623157e308), 0);
    def_prop(rt, c, atom_c(rt, "MIN_VALUE"), mknum(bitsd(1)), 0);
    def_prop(rt, c, atom_c(rt, "NaN"), mknum(JS_NAN), 0);
    def_prop(rt, c, atom_c(rt, "POSITIVE_INFINITY"), mknum(JS_INF), 0);
    def_prop(rt, c, atom_c(rt, "NEGATIVE_INFINITY"), mknum(-JS_INF), 0);
    def_method(rt, np, "toString", number_to_string, 1, 0);
    def_method(rt, np, "toFixed", number_to_fixed, 1, 0);
    def_method(rt, np, "toExponential", number_to_exp, 1, 0);
    def_method(rt, np, "toPrecision", number_to_prec, 1, 0);
    def_method(rt, np, "valueOf", number_value_of, 0, 0);
    def_method(rt, np, "toLocaleString", number_to_locale, 0, 0);
}

/* ---------------- Math ---------------- */
typedef double (*mfn1)(double);
static double m_abs(double x) { return bitsd(dbits(x) & 0x7FFFFFFFFFFFFFFFull); }
static double m_sign(double x) { return x != x ? x : x > 0 ? 1 : x < 0 ? -1 : x; }
static double m_fround(double x) { return (double)(float)x; }
static double m_sqrt(double x) { return js_sqrt(x); }
static double m_clz32(double x) {
    uint32_t u = to_uint32(x);
    return u ? (double)__builtin_clz(u) : 32;
}
static const mfn1 math_fns[] = {m_abs, js_acos, js_acosh, js_asin, js_asinh, js_atan, js_atanh, js_cbrt, js_ceil, m_clz32,
                                js_cos, js_cosh, js_exp, js_expm1, js_floor, m_fround, js_log, js_log1p, js_log10, js_log2,
                                js_round, m_sign, js_sin, js_sinh, m_sqrt, js_tan, js_tanh, js_trunc};
static const char *const math_names[] = {"abs", "acos", "acosh", "asin", "asinh", "atan", "atanh", "cbrt", "ceil", "clz32",
                                         "cos", "cosh", "exp", "expm1", "floor", "fround", "log", "log1p", "log10", "log2",
                                         "round", "sign", "sin", "sinh", "sqrt", "tan", "tanh", "trunc"};
NATIVE(math_1) {
    double d;
    if (to_number(rt, ARG(0), &d) < 0) return JS_EXC;
    return mknum(math_fns[MAGIC](d));
}
NATIVE(math_atan2) {
    double y, x;
    if (to_number(rt, ARG(0), &y) < 0 || to_number(rt, ARG(1), &x) < 0) return JS_EXC;
    return mknum(js_atan2(y, x));
}
NATIVE(math_pow) {
    double x, y;
    if (to_number(rt, ARG(0), &x) < 0 || to_number(rt, ARG(1), &y) < 0) return JS_EXC;
    return mknum(js_pow(x, y));
}
NATIVE(math_imul) {
    double x, y;
    if (to_number(rt, ARG(0), &x) < 0 || to_number(rt, ARG(1), &y) < 0) return JS_EXC;
    return mknum((double)(int32_t)((uint32_t)to_int32(x) * (uint32_t)to_int32(y)));
}
NATIVE(math_minmax) {
    bool mx = MAGIC == 1;
    double r = mx ? -JS_INF : JS_INF;
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        double d;
        if (to_number(rt, argv[i], &d) < 0) return JS_EXC;
        if (d != d) nan = true;
        if (mx) {
            if (d > r || (d == 0 && r == 0 && !js_signbit(d))) r = d;
        } else {
            if (d < r || (d == 0 && r == 0 && js_signbit(d))) r = d;
        }
    }
    return mknum(nan ? JS_NAN : r);
}
NATIVE(math_hypot) {
    double *v = (double *)malloc(sizeof(double) * (size_t)(argc ? argc : 1));
    bool inf = false, nan = false;
    double mx = 0;
    for (int i = 0; i < argc; i++) {
        if (to_number(rt, argv[i], &v[i]) < 0) {
            free(v);
            return JS_EXC;
        }
        double a = m_abs(v[i]);
        v[i] = a;
        if (a == JS_INF) inf = true;
        if (a != a) nan = true;
        if (a > mx) mx = a;
    }
    double r;
    if (inf)
        r = JS_INF;
    else if (nan)
        r = JS_NAN;
    else if (mx == 0)
        r = 0;
    else {
        double sum = 0, comp = 0;
        for (int i = 0; i < argc; i++) {
            double n = v[i] / mx;
            double s = n * n - comp;
            double p = sum + s;
            comp = (p - sum) - s;
            sum = p;
        }
        r = js_sqrt(sum) * mx;
    }
    free(v);
    return mknum(r);
}
double random_double(js_rt *rt) {
    if (!rt->rand_seeded) {
        uint64_t s[2] = {0x9E3779B97F4A7C15ull, 0xBF58476D1CE4E5B9ull};
        if (rt->host.random) rt->host.random(rt->host.ctx, s, sizeof s);
        if (!s[0] && !s[1]) s[0] = 1;
        rt->rand_state[0] = s[0];
        rt->rand_state[1] = s[1];
        rt->rand_seeded = true;
    }
    uint64_t s1 = rt->rand_state[0];
    const uint64_t s0 = rt->rand_state[1];
    rt->rand_state[0] = s0;
    s1 ^= s1 << 23;
    rt->rand_state[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    uint64_t r = (rt->rand_state[1] + s0) >> 11;
    return (double)r * (1.0 / 9007199254740992.0);
}
NATIVE(math_random) { return mknum(random_double(rt)); }

void init_math(js_rt *rt) {
    JSObject *m = new_plain(rt);
    def_value(rt, rt->global, "Math", mkobj(m));
    for (unsigned i = 0; i < sizeof math_fns / sizeof math_fns[0]; i++) def_method(rt, m, math_names[i], math_1, 1, (intptr_t)i);
    def_method(rt, m, "atan2", math_atan2, 2, 0);
    def_method(rt, m, "pow", math_pow, 2, 0);
    def_method(rt, m, "imul", math_imul, 2, 0);
    def_method(rt, m, "max", math_minmax, 2, 1);
    def_method(rt, m, "min", math_minmax, 2, 0);
    def_method(rt, m, "hypot", math_hypot, 2, 0);
    def_method(rt, m, "random", math_random, 0, 0);
    struct {
        const char *n;
        double v;
    } k[] = {{"E", 2.718281828459045}, {"LN10", 2.302585092994046}, {"LN2", 0.6931471805599453},
             {"LOG10E", 0.4342944819032518}, {"LOG2E", 1.4426950408889634}, {"PI", 3.141592653589793},
             {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951}};
    for (int i = 0; i < 8; i++) def_prop(rt, m, atom_c(rt, k[i].n), mknum(k[i].v), 0);
    def_prop(rt, m, SYM(toStringTag), mkstr(str_cstr(rt, "Math")), P_C);
}

/* ---------------- BigInt ---------------- */
static JSBig *bnew(js_rt *rt, uint32_t n) {
    JSBig *b = (JSBig *)gc_alloc(rt, sizeof(JSBig) + (size_t)n * 4, GC_BIG);
    b->n = n;
    return b;
}
static JSBig *bnorm(JSBig *b) {
    while (b->n && b->d[b->n - 1] == 0) b->n--;
    if (!b->n) b->h.flags = 0;
    return b;
}
static bool bneg(JSBig *b) { return (b->h.flags & 1) != 0; }
bool big_is_zero(JSBig *b) { return b->n == 0; }
static jsval mkbig(JSBig *b) { return MKTAG(TAG_BIG, b); }

static int mag_cmp(JSBig *a, JSBig *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = (int)a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static JSBig *mag_add(js_rt *rt, JSBig *a, JSBig *b) {
    uint32_t n = (a->n > b->n ? a->n : b->n) + 1;
    JSBig *r = bnew(rt, n);
    uint64_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = c + (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    return bnorm(r);
}
static JSBig *mag_sub(js_rt *rt, JSBig *a, JSBig *b) { /* |a| >= |b| */
    JSBig *r = bnew(rt, a->n);
    int64_t br = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - br - (i < b->n ? (int64_t)b->d[i] : 0);
        br = t < 0;
        r->d[i] = (uint32_t)(t + (br << 32));
    }
    return bnorm(r);
}
static JSBig *mag_mul(js_rt *rt, JSBig *a, JSBig *b) {
    JSBig *r = bnew(rt, a->n + b->n);
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t c = 0;
        for (uint32_t j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + r->d[i + j] + c;
            r->d[i + j] = (uint32_t)t;
            c = t >> 32;
        }
        r->d[i + b->n] = (uint32_t)c;
    }
    return bnorm(r);
}
static uint32_t bitlen(JSBig *a) { return a->n ? (a->n - 1) * 32 + (32 - (uint32_t)__builtin_clz(a->d[a->n - 1])) : 0; }
static int getbit(JSBig *a, uint32_t i) { return (i / 32 < a->n) ? (int)((a->d[i / 32] >> (i % 32)) & 1) : 0; }
static void mag_divmod(js_rt *rt, JSBig *a, JSBig *b, JSBig **q, JSBig **r) {
    if (mag_cmp(a, b) < 0) {
        if (q) *q = bnew(rt, 0);
        if (r) {
            *r = bnew(rt, a->n);
            memcpy((*r)->d, a->d, a->n * 4);
        }
        return;
    }
    if (b->n == 1) {
        JSBig *qq = bnew(rt, a->n);
        uint64_t rem = 0;
        for (int i = (int)a->n - 1; i >= 0; i--) {
            uint64_t t = (rem << 32) | a->d[i];
            qq->d[i] = (uint32_t)(t / b->d[0]);
            rem = t % b->d[0];
        }
        if (q) *q = bnorm(qq);
        if (r) {
            *r = bnew(rt, 1);
            (*r)->d[0] = (uint32_t)rem;
            bnorm(*r);
        }
        return;
    }
    /* binary long division */
    uint32_t nb = bitlen(a);
    JSBig *qq = bnew(rt, a->n);
    JSBig *rr = bnew(rt, b->n + 1);
    rr->n = 0;
    for (int i = (int)nb - 1; i >= 0; i--) {
        /* rr = rr*2 + bit */
        uint32_t carry = (uint32_t)getbit(a, (uint32_t)i);
        for (uint32_t k = 0; k < rr->n; k++) {
            uint32_t nc = rr->d[k] >> 31;
            rr->d[k] = (rr->d[k] << 1) | carry;
            carry = nc;
        }
        if (carry) rr->d[rr->n++] = carry;
        if (mag_cmp(rr, b) >= 0) {
            int64_t br = 0;
            for (uint32_t k = 0; k < rr->n; k++) {
                int64_t t = (int64_t)rr->d[k] - br - (k < b->n ? (int64_t)b->d[k] : 0);
                br = t < 0;
                rr->d[k] = (uint32_t)(t + (br << 32));
            }
            while (rr->n && rr->d[rr->n - 1] == 0) rr->n--;
            qq->d[i / 32] |= 1u << (i % 32);
        }
    }
    if (q) *q = bnorm(qq);
    if (r) *r = bnorm(rr);
}
static JSBig *with_sign(JSBig *b, bool neg) {
    if (b->n) b->h.flags = neg ? 1 : 0;
    return b;
}
static JSBig *big_add2(js_rt *rt, JSBig *a, JSBig *b, bool negb) {
    bool na = bneg(a), nb = bneg(b) ^ negb;
    if (na == nb) return with_sign(mag_add(rt, a, b), na);
    int c = mag_cmp(a, b);
    if (c == 0) return bnew(rt, 0);
    if (c > 0) return with_sign(mag_sub(rt, a, b), na);
    return with_sign(mag_sub(rt, b, a), nb);
}
JSBig *big_from_double(js_rt *rt, double d) {
    bool neg = d < 0;
    if (neg) d = -d;
    JSBig *b = bnew(rt, 34);
    uint64_t bits = dbits(d);
    int e = (int)((bits >> 52) & 0x7ff);
    uint64_t m = bits & ((1ull << 52) - 1);
    if (e == 0) {
        b->n = 0;
        return b;
    }
    m |= 1ull << 52;
    int sh = e - 1075;
    if (sh < 0) {
        m >>= -sh;
        sh = 0;
    }
    memset(b->d, 0, 34 * 4);
    uint32_t w = (uint32_t)sh / 32, bs = (uint32_t)sh % 32;
    unsigned __int128 v = (unsigned __int128)m << bs;
    for (int i = 0; i < 4 && w + (uint32_t)i < 34; i++) b->d[w + (uint32_t)i] = (uint32_t)(v >> (32 * i));
    bnorm(b);
    return with_sign(b, neg);
}
double big_to_double(JSBig *b) {
    if (!b->n) return 0;
    uint32_t nb = bitlen(b);
    double r;
    if (nb <= 64) {
        uint64_t v = 0;
        for (int i = (int)b->n - 1; i >= 0; i--) v = (v << 32) | b->d[i];
        r = (double)v;
    } else {
        if (nb > 1100) return bneg(b) ? -JS_INF : JS_INF;
        /* top 64 bits with sticky */
        uint64_t top = 0;
        bool sticky = false;
        for (int i = 0; i < 64; i++) top = (top << 1) | (uint64_t)getbit(b, nb - 1 - (uint32_t)i);
        for (uint32_t i = 0; i < nb - 64; i++)
            if (getbit(b, i)) {
                sticky = true;
                break;
            }
        /* round to 53 bits */
        uint64_t mant = top >> 11, rem = top & 0x7FF;
        if (rem > 0x400 || (rem == 0x400 && (sticky || (mant & 1)))) mant++;
        r = js_ldexp((double)mant, (int)nb - 53);
    }
    return bneg(b) ? -r : r;
}
int big_cmp(JSBig *a, JSBig *b) {
    if (bneg(a) != bneg(b)) return bneg(a) ? -1 : 1;
    int c = mag_cmp(a, b);
    return bneg(a) ? -c : c;
}
int big_cmp_num(JSBig *a, double d) {
    if (d != d) return 2;
    if (d == JS_INF) return -1;
    if (d == -JS_INF) return 1;
    double fl = js_floor(d);
    /* compare a with fl using a temporary on the C heap */
    bool neg = fl < 0;
    double ad = neg ? -fl : fl;
    uint64_t bits = dbits(ad);
    int e = (int)((bits >> 52) & 0x7ff);
    uint32_t tmp[40];
    memset(tmp, 0, sizeof tmp);
    uint32_t n = 0;
    if (e) {
        uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);
        int sh = e - 1075;
        if (sh < 0) {
            m >>= -sh;
            sh = 0;
        }
        unsigned __int128 v = (unsigned __int128)m << (sh % 32);
        for (int i = 0; i < 4; i++) tmp[sh / 32 + i] = (uint32_t)(v >> (32 * i));
        n = 40;
        while (n && !tmp[n - 1]) n--;
    }
    struct {
        GCHdr h;
        uint32_t n;
        uint32_t d[40];
    } fb;
    fb.h.flags = (neg && n) ? 1 : 0;
    fb.n = n;
    memcpy(fb.d, tmp, sizeof tmp);
    int c = big_cmp(a, (JSBig *)&fb);
    if (c == 0 && fl != d) return -1;
    return c;
}

JSString *big_to_string(js_rt *rt, JSBig *b, int radix) {
    if (!b->n) return str_cstr(rt, "0");
    uint32_t nb = bitlen(b);
    size_t cap = nb + 4;
    char *buf = (char *)malloc(cap);
    size_t k = 0;
    uint32_t *t = (uint32_t *)malloc(b->n * 4);
    memcpy(t, b->d, b->n * 4);
    uint32_t n = b->n;
    while (n) {
        uint64_t rem = 0;
        for (int i = (int)n - 1; i >= 0; i--) {
            uint64_t x = (rem << 32) | t[i];
            t[i] = (uint32_t)(x / (uint32_t)radix);
            rem = x % (uint32_t)radix;
        }
        buf[k++] = "0123456789abcdefghijklmnopqrstuvwxyz"[rem];
        while (n && !t[n - 1]) n--;
    }
    if (bneg(b)) buf[k++] = '-';
    for (size_t i = 0; i < k / 2; i++) {
        char c = buf[i];
        buf[i] = buf[k - 1 - i];
        buf[k - 1 - i] = c;
    }
    JSString *s = str_new8(rt, (const uint8_t *)buf, (uint32_t)k);
    free(buf);
    free(t);
    return s;
}

jsval big_from_string(js_rt *rt, JSString *s, bool literal) {
    str_flat(rt, s);
    uint32_t a = 0, e = s->len;
    if (!literal) {
        while (a < e && uc_is_space(str_at(s, a))) a++;
        while (e > a && uc_is_space(str_at(s, e - 1))) e--;
    }
    bool neg = false;
    int radix = 10;
    if (a == e) return literal ? JS_UNDEF : mkbig(bnew(rt, 0));
    if (e - a > 2 && str_at(s, a) == '0') {
        uint16_t c = str_at(s, a + 1) | 0x20;
        if (c == 'x') radix = 16;
        else if (c == 'o') radix = 8;
        else if (c == 'b') radix = 2;
        if (radix != 10) a += 2;
    }
    if (radix == 10 && !literal && (str_at(s, a) == '+' || str_at(s, a) == '-')) {
        neg = str_at(s, a) == '-';
        a++;
        if (a == e) return JS_UNDEF;
    }
    uint32_t nd = e - a;
    JSBig *r = bnew(rt, nd / 8 + 2);
    r->n = 0;
    for (uint32_t i = a; i < e; i++) {
        uint32_t c = str_at(s, i), v;
        if (c == '_' && literal) continue;
        if (c >= '0' && c <= '9') v = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') v = (c | 0x20) - 'a' + 10;
        else return JS_UNDEF;
        if (v >= (uint32_t)radix) return JS_UNDEF;
        uint64_t carry = v;
        for (uint32_t k = 0; k < r->n; k++) {
            uint64_t t = (uint64_t)r->d[k] * (uint32_t)radix + carry;
            r->d[k] = (uint32_t)t;
            carry = t >> 32;
        }
        if (carry) r->d[r->n++] = (uint32_t)carry;
    }
    bnorm(r);
    return mkbig(with_sign(r, neg));
}

jsval to_bigint(js_rt *rt, jsval v) {
    v = to_primitive(rt, v, 1);
    if (v == JS_EXC) return JS_EXC;
    if (is_big(v)) return v;
    if (v == JS_TRUE) return mkbig(big_from_double(rt, 1));
    if (v == JS_FALSE) return mkbig(bnew(rt, 0));
    if (is_str(v)) {
        jsval r = big_from_string(rt, vstr(v), false);
        if (!is_big(r)) return throw_error(rt, E_SYNTAX, "Cannot convert %S to a BigInt", vstr(v));
        return r;
    }
    return throw_error(rt, E_TYPE, "Cannot convert %S to a BigInt", val_brief(rt, v));
}

/* two's complement helpers for bitwise ops */
static void to_twos(JSBig *a, uint32_t *out, uint32_t w) {
    for (uint32_t i = 0; i < w; i++) out[i] = i < a->n ? a->d[i] : 0;
    if (bneg(a)) {
        uint64_t c = 1;
        for (uint32_t i = 0; i < w; i++) {
            uint64_t t = (uint64_t)(uint32_t)~out[i] + c;
            out[i] = (uint32_t)t;
            c = t >> 32;
        }
    }
}
static JSBig *from_twos(js_rt *rt, uint32_t *v, uint32_t w) {
    bool neg = (v[w - 1] >> 31) != 0;
    JSBig *r = bnew(rt, w);
    if (neg) {
        uint64_t c = 1;
        for (uint32_t i = 0; i < w; i++) {
            uint64_t t = (uint64_t)(uint32_t)~v[i] + c;
            r->d[i] = (uint32_t)t;
            c = t >> 32;
        }
    } else
        memcpy(r->d, v, w * 4);
    bnorm(r);
    return with_sign(r, neg);
}
static JSBig *big_shift(js_rt *rt, JSBig *a, int64_t sh) {
    if (sh >= 0) {
        if (sh > 1000000) return 0;
        uint32_t w = (uint32_t)(sh / 32), bs = (uint32_t)(sh % 32);
        JSBig *r = bnew(rt, a->n + w + 1);
        for (uint32_t i = 0; i < a->n; i++) {
            uint64_t v = (uint64_t)a->d[i] << bs;
            r->d[i + w] |= (uint32_t)v;
            r->d[i + w + 1] |= (uint32_t)(v >> 32);
        }
        bnorm(r);
        return with_sign(r, bneg(a));
    }
    uint64_t s = (uint64_t)(-sh);
    if (s >= (uint64_t)a->n * 32) return bneg(a) ? with_sign(big_from_double(rt, 1), true) : bnew(rt, 0);
    uint32_t w = (uint32_t)(s / 32), bs = (uint32_t)(s % 32);
    JSBig *r = bnew(rt, a->n - w);
    bool lost = false;
    for (uint32_t i = 0; i < w; i++)
        if (a->d[i]) lost = true;
    if (bs && (a->d[w] & ((1u << bs) - 1))) lost = true;
    for (uint32_t i = 0; i < r->n; i++) {
        uint64_t v = a->d[i + w];
        if (i + w + 1 < a->n) v |= (uint64_t)a->d[i + w + 1] << 32;
        r->d[i] = (uint32_t)(v >> bs);
    }
    bnorm(r);
    if (bneg(a)) {
        with_sign(r, true);
        if (lost) {
            JSBig *one = big_from_double(rt, 1);
            r = big_add2(rt, r, one, true);
        }
    }
    return r;
}

jsval big_from_u64(js_rt *rt, uint64_t v, bool neg) {
    JSBig *b = bnew(rt, 2);
    b->d[0] = (uint32_t)v;
    b->d[1] = (uint32_t)(v >> 32);
    bnorm(b);
    return mkbig(with_sign(b, neg));
}
uint64_t big_to_u64(JSBig *b) {
    uint64_t v = b->n > 0 ? b->d[0] : 0;
    if (b->n > 1) v |= (uint64_t)b->d[1] << 32;
    if (bneg(b)) v = (uint64_t)0 - v;
    return v;
}
jsval big_neg(js_rt *rt, jsval a) {
    JSBig *x = (JSBig *)vptr(a);
    JSBig *r = bnew(rt, x->n);
    memcpy(r->d, x->d, x->n * 4);
    bnorm(r);
    return mkbig(with_sign(r, !bneg(x)));
}

jsval big_binop(js_rt *rt, int op, jsval av, jsval bv) {
    if (!is_big(av) || !is_big(bv)) return throw_error(rt, E_TYPE, "Cannot mix BigInt and other types, use explicit conversions");
    JSBig *a = (JSBig *)vptr(av), *b = (JSBig *)vptr(bv);
    switch (op) {
    case OP_ADD: return mkbig(big_add2(rt, a, b, false));
    case OP_SUB: return mkbig(big_add2(rt, a, b, true));
    case OP_MUL: return mkbig(with_sign(mag_mul(rt, a, b), bneg(a) != bneg(b)));
    case OP_DIV:
    case OP_MOD: {
        if (!b->n) return throw_error(rt, E_RANGE, "Division by zero");
        JSBig *q, *r;
        mag_divmod(rt, a, b, &q, &r);
        if (op == OP_DIV) return mkbig(with_sign(q, bneg(a) != bneg(b)));
        return mkbig(with_sign(r, bneg(a)));
    }
    case OP_POW: {
        if (bneg(b)) return throw_error(rt, E_RANGE, "Exponent must be non-negative");
        if (b->n > 1) {
            if (a->n == 0 || (a->n == 1 && a->d[0] == 1)) return a->n == 0 ? mkbig(bnew(rt, 0)) : mkbig(with_sign(big_from_double(rt, 1), bneg(a) && (b->d[0] & 1)));
            return throw_error(rt, E_RANGE, "Maximum BigInt size exceeded");
        }
        uint32_t e = b->n ? b->d[0] : 0;
        JSBig *r = big_from_double(rt, 1), *base = a;
        jsval *root = push_tmp(rt, mkbig(r));
        push_tmp(rt, mkbig(base));
        while (e) {
            if (e & 1) r = mag_mul(rt, r, base);
            e >>= 1;
            if (e) base = mag_mul(rt, base, base);
            if (bitlen(r) > 1 << 24) {
                rt->sp = root;
                return throw_error(rt, E_RANGE, "Maximum BigInt size exceeded");
            }
        }
        rt->sp = root;
        return mkbig(with_sign(r, bneg(a) && b->n && (b->d[0] & 1)));
    }
    case OP_SHL:
    case OP_SAR: {
        double sh = big_to_double(b);
        if (op == OP_SAR) sh = -sh;
        if (sh > 1e7) return throw_error(rt, E_RANGE, "Maximum BigInt size exceeded");
        if (sh < -1e9) sh = -1e9;
        JSBig *r = big_shift(rt, a, (int64_t)sh);
        if (!r) return throw_error(rt, E_RANGE, "Maximum BigInt size exceeded");
        return mkbig(r);
    }
    case OP_SHR: return throw_error(rt, E_TYPE, "BigInts have no unsigned right shift, use >> instead");
    case OP_BAND:
    case OP_BOR:
    case OP_BXOR:
    case OP_BITNOT: {
        uint32_t w = (a->n > b->n ? a->n : b->n) + 1;
        uint32_t *x = (uint32_t *)malloc(w * 4), *y = (uint32_t *)malloc(w * 4);
        to_twos(a, x, w);
        to_twos(b, y, w);
        for (uint32_t i = 0; i < w; i++) {
            if (op == OP_BAND) x[i] &= y[i];
            else if (op == OP_BOR) x[i] |= y[i];
            else if (op == OP_BXOR) x[i] ^= y[i];
            else x[i] = ~x[i];
        }
        JSBig *r = from_twos(rt, x, w);
        free(x);
        free(y);
        return mkbig(r);
    }
    default: return throw_error(rt, E_TYPE, "Invalid BigInt operation");
    }
}

NATIVE(bigint_ctor) {
    if (!is_undef(rt->new_target)) THROW_TYPE("BigInt is not a constructor");
    jsval v = to_primitive(rt, ARG(0), 1);
    TRY(v);
    if (is_num(v)) {
        double d = vnum(v);
        if (!js_isfinite(d) || js_trunc(d) != d)
            return throw_error(rt, E_RANGE, "The number %S cannot be converted to a BigInt because it is not an integer", num_to_str(rt, d));
        return mkbig(big_from_double(rt, d));
    }
    return to_bigint(rt, v);
}
static jsval this_big(js_rt *rt, jsval t) {
    if (is_big(t)) return t;
    if (is_cls(t, CLS_BIGINT)) return vobj(t)->u.prim.v;
    return throw_error(rt, E_TYPE, "BigInt.prototype method called on incompatible receiver");
}
NATIVE(bigint_to_string) {
    jsval b = this_big(rt, this_);
    TRY(b);
    int radix = 10;
    if (!is_undef(ARG(0))) {
        double r;
        if (to_int(rt, argv[0], &r) < 0) return JS_EXC;
        if (r < 2 || r > 36) return throw_error(rt, E_RANGE, "toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    return mkstr(big_to_string(rt, (JSBig *)vptr(b), radix));
}
NATIVE(bigint_value_of) { return this_big(rt, this_); }
NATIVE(bigint_as_n) {
    double bits;
    if (to_index(rt, ARG(0), (uint64_t *)&(uint64_t){0}) < 0) return JS_EXC;
    if (to_int(rt, ARG(0), &bits) < 0) return JS_EXC;
    jsval x = to_bigint(rt, ARG(1));
    TRY(x);
    JSBig *a = (JSBig *)vptr(x);
    uint32_t nb = (uint32_t)bits;
    if (nb == 0) return mkbig(bnew(rt, 0));
    uint32_t w = (nb + 31) / 32 + (a->n > (nb + 31) / 32 ? a->n : 0) + 1;
    uint32_t *v = (uint32_t *)calloc(w, 4);
    to_twos(a, v, w);
    /* mask to nb bits */
    uint32_t keep = (nb + 31) / 32;
    if (nb % 32) v[keep - 1] &= (1u << (nb % 32)) - 1;
    for (uint32_t i = keep; i < w; i++) v[i] = 0;
    JSBig *r = bnew(rt, w);
    memcpy(r->d, v, w * 4);
    bnorm(r);
    if (MAGIC == 0 && getbit(r, nb - 1)) {
        /* signed: subtract 2^nb */
        JSBig *p = big_shift(rt, big_from_double(rt, 1), nb);
        r = big_add2(rt, r, p, true);
    }
    free(v);
    return mkbig(r);
}
NATIVE(bigint_to_locale) {
    jsval b = this_big(rt, this_);
    TRY(b);
    JSString *s = big_to_string(rt, (JSBig *)vptr(b), 10);
    SB sb;
    sb_init(&sb, rt);
    uint32_t start = 0;
    if (s->len && str_at(s, 0) == '-') {
        sb_putc(&sb, '-');
        start = 1;
    }
    uint32_t nd = s->len - start;
    for (uint32_t i = 0; i < nd; i++) {
        sb_putc(&sb, str_at(s, start + i));
        uint32_t rem = nd - 1 - i;
        if (rem > 0 && rem % 3 == 0) sb_putc(&sb, ',');
    }
    JSString *r = sb_finish(&sb);
    return r ? mkstr(r) : JS_EXC;
}

void init_bigint(js_rt *rt) {
    JSObject *bp = rt->intr[I_BigIntProto];
    JSObject *c = new_ctor(rt, "BigInt", bigint_ctor, 1, bp);
    def_method(rt, c, "asIntN", bigint_as_n, 2, 0);
    def_method(rt, c, "asUintN", bigint_as_n, 2, 1);
    def_method(rt, bp, "toString", bigint_to_string, 0, 0);
    def_method(rt, bp, "valueOf", bigint_value_of, 0, 0);
    def_method(rt, bp, "toLocaleString", bigint_to_locale, 0, 0);
    def_prop(rt, bp, SYM(toStringTag), mkstr(str_cstr(rt, "BigInt")), P_C);
}
