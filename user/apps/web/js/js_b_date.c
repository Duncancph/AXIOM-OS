/* AXIOM JavaScript engine: Date (local time zone is UTC). */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)
#define MS_DAY 86400000.0

static const char *const wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

double date_now(js_rt *rt) { return rt->host.now_ms ? (double)rt->host.now_ms(rt->host.ctx) : 0; }

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

static double make_day(double y, double m, double d) {
    if (!js_isfinite(y) || !js_isfinite(m) || !js_isfinite(d)) return JS_NAN;
    y = to_integer(y);
    m = to_integer(m);
    d = to_integer(d);
    double ym = y + js_floor(m / 12);
    double mn = js_fmod(m, 12);
    if (mn < 0) mn += 12;
    if (ym > 400000 || ym < -400000) return JS_NAN;
    return (double)days_from_civil((int64_t)ym, (int64_t)mn + 1, 1) + d - 1;
}
static double make_time(double h, double mi, double s, double ms) {
    if (!js_isfinite(h) || !js_isfinite(mi) || !js_isfinite(s) || !js_isfinite(ms)) return JS_NAN;
    return to_integer(h) * 3600000.0 + to_integer(mi) * 60000.0 + to_integer(s) * 1000.0 + to_integer(ms);
}
static double make_date(double day, double t) {
    if (!js_isfinite(day) || !js_isfinite(t)) return JS_NAN;
    return day * MS_DAY + t;
}
static double time_clip(double t) {
    if (!js_isfinite(t) || (t < 0 ? -t : t) > 8.64e15) return JS_NAN;
    return to_integer(t) + 0.0;
}

typedef struct DT {
    int64_t year;
    int mon, day, wday, h, mi, s, ms;
} DT;
static void split_time(double t, DT *o) {
    double days = js_floor(t / MS_DAY);
    double msd = t - days * MS_DAY;
    civil_from_days((int64_t)days, &o->year, &o->mon, &o->day);
    o->mon -= 1;
    int64_t wd = ((int64_t)days + 4) % 7;
    if (wd < 0) wd += 7;
    o->wday = (int)wd;
    int64_t m = (int64_t)msd;
    o->ms = (int)(m % 1000);
    o->s = (int)(m / 1000 % 60);
    o->mi = (int)(m / 60000 % 60);
    o->h = (int)(m / 3600000);
}

static double this_time(js_rt *rt, jsval t, bool *ok) {
    if (!is_cls(t, CLS_DATE)) {
        throw_error(rt, E_TYPE, "this is not a Date object.");
        *ok = false;
        return 0;
    }
    *ok = true;
    return vnum(vobj(t)->u.prim.v);
}

/* ---------------- parsing ---------------- */
static int month_index(const char *w) {
    static const char *const full[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
    size_t n = strlen(w);
    if (n < 3) return -1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(full[i], w, n) == 0) return i;
    }
    return -1;
}

static bool dig(int c) { return c >= '0' && c <= '9'; }

static double parse_iso(const char *s, size_t n, bool *ok) {
    *ok = false;
    size_t i = 0;
    int64_t y;
    int sign = 1;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        sign = s[i] == '-' ? -1 : 1;
        i++;
        if (i + 6 > n) return JS_NAN;
        y = 0;
        for (int k = 0; k < 6; k++) {
            if (!dig(s[i])) return JS_NAN;
            y = y * 10 + (s[i++] - '0');
        }
        if (sign < 0 && y == 0) return JS_NAN;
        y *= sign;
    } else {
        if (i + 4 > n) return JS_NAN;
        y = 0;
        for (int k = 0; k < 4; k++) {
            if (!dig(s[i])) return JS_NAN;
            y = y * 10 + (s[i++] - '0');
        }
    }
    int mon = 1, day = 1, h = 0, mi = 0, sec = 0, ms = 0;
    bool date_only = true, has_tz = false;
    double tzoff = 0;
#define TWO(var)                                         \
    do {                                                 \
        if (i + 2 > n || !dig(s[i]) || !dig(s[i + 1])) return JS_NAN; \
        var = (s[i] - '0') * 10 + (s[i + 1] - '0');      \
        i += 2;                                          \
    } while (0)
    if (i < n && s[i] == '-') {
        i++;
        TWO(mon);
        if (i < n && s[i] == '-') {
            i++;
            TWO(day);
        }
    }
    if (i < n && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        i++;
        date_only = false;
        TWO(h);
        if (i >= n || s[i] != ':') return JS_NAN;
        i++;
        TWO(mi);
        if (i < n && s[i] == ':') {
            i++;
            TWO(sec);
            if (i < n && (s[i] == '.' || s[i] == ',')) {
                i++;
                int k = 0;
                ms = 0;
                while (i < n && dig(s[i])) {
                    if (k < 3) ms = ms * 10 + (s[i] - '0');
                    k++;
                    i++;
                }
                if (!k) return JS_NAN;
                while (k < 3) {
                    ms *= 10;
                    k++;
                }
            }
        }
        if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
            has_tz = true;
            i++;
        } else if (i < n && (s[i] == '+' || s[i] == '-')) {
            int sg = s[i] == '-' ? -1 : 1;
            i++;
            int th, tm;
            TWO(th);
            if (i < n && s[i] == ':') i++;
            TWO(tm);
            has_tz = true;
            tzoff = sg * (th * 60 + tm);
        }
    }
    if (i != n) return JS_NAN;
#undef TWO
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || h > 24 || mi > 59 || sec > 59) return JS_NAN;
    if (h == 24 && (mi || sec || ms)) return JS_NAN;
    (void)date_only;
    double t = make_date(make_day((double)y, mon - 1, day), make_time(h, mi, sec, ms));
    if (has_tz) t -= tzoff * 60000.0;
    *ok = true;
    return t;
}

static double parse_loose(const char *s, size_t n) {
    int64_t nums[8];
    int nlen[8], nn = 0;
    char seps[8];
    int month = -1, h = -1, mi = 0, sec = 0, ms = 0;
    bool pm = false, am = false, has_tz = false;
    double tz = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '(') {
            int depth = 0;
            while (i < n) {
                if (s[i] == '(') depth++;
                if (s[i] == ')' && --depth == 0) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            char w[16];
            int k = 0;
            while (i < n && (((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') || s[i] == '.')) {
                if (k < 15 && s[i] != '.') w[k++] = (char)(s[i] | 0x20);
                i++;
            }
            w[k] = 0;
            if (!strcmp(w, "am")) am = true;
            else if (!strcmp(w, "pm")) pm = true;
            else if (!strcmp(w, "gmt") || !strcmp(w, "utc") || !strcmp(w, "ut") || !strcmp(w, "z")) {
                has_tz = true;
                if (i < n && (s[i] == '+' || s[i] == '-')) {
                    int sg = s[i] == '-' ? -1 : 1;
                    i++;
                    int v = 0, d = 0;
                    while (i < n && dig(s[i])) {
                        v = v * 10 + (s[i++] - '0');
                        d++;
                    }
                    if (i < n && s[i] == ':') {
                        i++;
                        int m2 = 0;
                        while (i < n && dig(s[i])) m2 = m2 * 10 + (s[i++] - '0');
                        tz = sg * (v * 60 + m2);
                    } else
                        tz = d <= 2 ? sg * v * 60 : sg * ((v / 100) * 60 + v % 100);
                }
            } else if (!strcmp(w, "t") && nn >= 3) {
            } else {
                int mi2 = month_index(w);
                if (mi2 >= 0)
                    month = mi2;
                else {
                    bool wd = false;
                    for (int q = 0; q < 7; q++)
                        if (!strncmp(w, "sunmontuewedthufrisat" + q * 3, 3)) wd = true;
                    if (!wd) return JS_NAN;
                }
            }
            continue;
        }
        if (dig(c)) {
            int64_t v = 0;
            int d = 0;
            while (i < n && dig(s[i])) {
                v = v * 10 + (s[i++] - '0');
                d++;
            }
            if (i < n && s[i] == ':' && h < 0) {
                h = (int)v;
                i++;
                int m2 = 0;
                while (i < n && dig(s[i])) m2 = m2 * 10 + (s[i++] - '0');
                mi = m2;
                if (i < n && s[i] == ':') {
                    i++;
                    int s2 = 0;
                    while (i < n && dig(s[i])) s2 = s2 * 10 + (s[i++] - '0');
                    sec = s2;
                    if (i < n && s[i] == '.') {
                        i++;
                        int k = 0, msv = 0;
                        while (i < n && dig(s[i])) {
                            if (k < 3) msv = msv * 10 + (s[i] - '0');
                            k++;
                            i++;
                        }
                        while (k < 3) {
                            msv *= 10;
                            k++;
                        }
                        ms = msv;
                    }
                }
                continue;
            }
            if (nn < 8) {
                nums[nn] = v;
                nlen[nn] = d;
                seps[nn] = i < n ? s[i] : 0;
                nn++;
            }
            continue;
        }
        if ((c == '+' || c == '-') && h >= 0 && i + 1 < n && dig(s[i + 1])) {
            int sg = c == '-' ? -1 : 1;
            i++;
            int v = 0, d = 0;
            while (i < n && dig(s[i])) {
                v = v * 10 + (s[i++] - '0');
                d++;
            }
            if (i < n && s[i] == ':') {
                i++;
                int m2 = 0;
                while (i < n && dig(s[i])) m2 = m2 * 10 + (s[i++] - '0');
                tz = sg * (v * 60 + m2);
            } else
                tz = d <= 2 ? sg * v * 60 : sg * ((v / 100) * 60 + v % 100);
            has_tz = true;
            continue;
        }
        i++;
    }
    int64_t y = -1;
    int mon = month, day = -1;
    if (month >= 0) {
        for (int k = 0; k < nn; k++) {
            if (day < 0 && nlen[k] <= 2 && nums[k] >= 1 && nums[k] <= 31)
                day = (int)nums[k];
            else if (y < 0)
                y = nums[k];
        }
        if (day < 0) day = 1;
    } else {
        if (nn < 3) {
            if (nn == 1 && nlen[0] >= 4 && h < 0) {
                y = nums[0];
                mon = 0;
                day = 1;
            } else
                return JS_NAN;
        } else if (nlen[0] >= 3) {
            y = nums[0];
            mon = (int)nums[1] - 1;
            day = (int)nums[2];
        } else {
            mon = (int)nums[0] - 1;
            day = (int)nums[1];
            y = nums[2];
        }
        (void)seps;
    }
    if (y < 0) return JS_NAN;
    if (y < 50 && month < 0 && nn >= 3 && nlen[2] <= 2) y += 2000;
    else if (y < 100 && (month >= 0 || nn >= 3)) y += 1900;
    if (h < 0) h = 0;
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    if (mon < 0 || mon > 11 || day < 1 || day > 31 || h > 24 || mi > 59 || sec > 59) return JS_NAN;
    double t = make_date(make_day((double)y, mon, day), make_time(h, mi, sec, ms));
    if (has_tz) t -= tz * 60000.0;
    return t;
}

static double parse_date(js_rt *rt, JSString *str) {
    char *u = str_to_utf8(rt, str, 0);
    if (!u) return JS_NAN;
    size_t n = strlen(u);
    while (n && (u[n - 1] == ' ' || u[n - 1] == '\t' || u[n - 1] == '\n')) n--;
    size_t a = 0;
    while (a < n && (u[a] == ' ' || u[a] == '\t' || u[a] == '\n')) a++;
    bool ok;
    double t = parse_iso(u + a, n - a, &ok);
    if (!ok) t = parse_loose(u + a, n - a);
    free(u);
    return time_clip(t);
}

/* ---------------- formatting ---------------- */
static void put2(SB *b, int v) {
    sb_putc(b, (uint32_t)('0' + v / 10 % 10));
    sb_putc(b, (uint32_t)('0' + v % 10));
}
static void put_year(SB *b, js_rt *rt, int64_t y, bool iso) {
    if (iso && (y < 0 || y > 9999)) {
        sb_putc(b, y < 0 ? '-' : '+');
        int64_t a = y < 0 ? -y : y;
        char t[8];
        for (int k = 5; k >= 0; k--) {
            t[k] = (char)('0' + a % 10);
            a /= 10;
        }
        sb_putn(b, t, 6);
        return;
    }
    if (y < 0) {
        sb_putc(b, '-');
        y = -y;
    }
    char t[24];
    int k = 0;
    do {
        t[k++] = (char)('0' + y % 10);
        y /= 10;
    } while (y);
    while (k < 4) t[k++] = '0';
    while (k) sb_putc(b, (uint8_t)t[--k]);
    (void)rt;
}

static JSString *fmt_date(js_rt *rt, double t, int kind) {
    if (t != t) {
        if (kind == 3) return 0;
        return str_cstr(rt, "Invalid Date");
    }
    DT d;
    split_time(t, &d);
    SB b;
    sb_init(&b, rt);
    switch (kind) {
    case 0: /* toString */
    case 1: /* toDateString */
    case 2: /* toTimeString */
        if (kind != 2) {
            sb_puts(&b, wdays[d.wday]);
            sb_putc(&b, ' ');
            sb_puts(&b, months[d.mon]);
            sb_putc(&b, ' ');
            put2(&b, d.day);
            sb_putc(&b, ' ');
            put_year(&b, rt, d.year, false);
        }
        if (kind == 0) sb_putc(&b, ' ');
        if (kind != 1) {
            put2(&b, d.h);
            sb_putc(&b, ':');
            put2(&b, d.mi);
            sb_putc(&b, ':');
            put2(&b, d.s);
            sb_puts(&b, " GMT+0000 (Coordinated Universal Time)");
        }
        break;
    case 3: /* ISO */
        put_year(&b, rt, d.year, true);
        sb_putc(&b, '-');
        put2(&b, d.mon + 1);
        sb_putc(&b, '-');
        put2(&b, d.day);
        sb_putc(&b, 'T');
        put2(&b, d.h);
        sb_putc(&b, ':');
        put2(&b, d.mi);
        sb_putc(&b, ':');
        put2(&b, d.s);
        sb_putc(&b, '.');
        sb_putc(&b, (uint32_t)('0' + d.ms / 100));
        put2(&b, d.ms % 100);
        sb_putc(&b, 'Z');
        break;
    case 4: /* UTC */
        sb_puts(&b, wdays[d.wday]);
        sb_puts(&b, ", ");
        put2(&b, d.day);
        sb_putc(&b, ' ');
        sb_puts(&b, months[d.mon]);
        sb_putc(&b, ' ');
        put_year(&b, rt, d.year, false);
        sb_putc(&b, ' ');
        put2(&b, d.h);
        sb_putc(&b, ':');
        put2(&b, d.mi);
        sb_putc(&b, ':');
        put2(&b, d.s);
        sb_puts(&b, " GMT");
        break;
    case 5: /* toLocaleDateString */
    case 6: /* toLocaleTimeString */
    case 7: /* toLocaleString */
        if (kind != 6) {
            sb_putstr(&b, num_to_str(rt, d.mon + 1));
            sb_putc(&b, '/');
            sb_putstr(&b, num_to_str(rt, d.day));
            sb_putc(&b, '/');
            put_year(&b, rt, d.year, false);
        }
        if (kind == 7) sb_puts(&b, ", ");
        if (kind != 5) {
            int h12 = d.h % 12;
            if (h12 == 0) h12 = 12;
            sb_putstr(&b, num_to_str(rt, h12));
            sb_putc(&b, ':');
            put2(&b, d.mi);
            sb_putc(&b, ':');
            put2(&b, d.s);
            sb_puts(&b, d.h < 12 ? " AM" : " PM");
        }
        break;
    }
    return sb_finish(&b);
}

/* ---------------- constructor & statics ---------------- */
NATIVE(date_ctor) {
    if (is_undef(rt->new_target)) return mkstr(fmt_date(rt, date_now(rt), 0));
    double t;
    if (argc == 0)
        t = date_now(rt);
    else if (argc == 1) {
        jsval v = argv[0];
        if (is_cls(v, CLS_DATE))
            t = vnum(vobj(v)->u.prim.v);
        else {
            v = to_primitive(rt, v, 0);
            TRY(v);
            if (is_str(v))
                t = parse_date(rt, vstr(v));
            else {
                if (to_number(rt, v, &t) < 0) return JS_EXC;
                t = time_clip(t);
            }
        }
    } else {
        double f[7] = {0, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < argc && i < 7; i++)
            if (to_number(rt, argv[i], &f[i]) < 0) return JS_EXC;
        if (f[0] == f[0]) {
            double yi = to_integer(f[0]);
            if (yi >= 0 && yi <= 99) f[0] = 1900 + yi;
        }
        t = time_clip(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6])));
    }
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_DateProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_DATE);
    o->u.prim.v = mknum(t);
    return mkobj(o);
}
NATIVE(date_now_n) { return mknum(date_now(rt)); }
NATIVE(date_parse) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    return mknum(parse_date(rt, s));
}
NATIVE(date_utc) {
    double f[7] = {JS_NAN, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < argc && i < 7; i++)
        if (to_number(rt, argv[i], &f[i]) < 0) return JS_EXC;
    if (f[0] == f[0]) {
        double yi = to_integer(f[0]);
        if (yi >= 0 && yi <= 99) f[0] = 1900 + yi;
    }
    return mknum(time_clip(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}

/* getters: magic selects field */
enum { G_TIME, G_YEAR, G_MONTH, G_DATE, G_DAY, G_HOURS, G_MIN, G_SEC, G_MS, G_TZ, G_YEAR2 };
NATIVE(date_get) {
    bool ok;
    double t = this_time(rt, this_, &ok);
    if (!ok) return JS_EXC;
    int f = MAGIC;
    if (f == G_TIME) return mknum(t);
    if (t != t) return mknum(JS_NAN);
    if (f == G_TZ) return mknum(0);
    DT d;
    split_time(t, &d);
    switch (f) {
    case G_YEAR: return mknum((double)d.year);
    case G_YEAR2: return mknum((double)d.year - 1900);
    case G_MONTH: return mknum(d.mon);
    case G_DATE: return mknum(d.day);
    case G_DAY: return mknum(d.wday);
    case G_HOURS: return mknum(d.h);
    case G_MIN: return mknum(d.mi);
    case G_SEC: return mknum(d.s);
    case G_MS: return mknum(d.ms);
    default: return JS_UNDEF;
    }
}

enum { S_MS, S_SEC, S_MIN, S_HOURS, S_DATE, S_MONTH, S_YEAR, S_TIME };
NATIVE(date_set) {
    bool ok;
    double t = this_time(rt, this_, &ok);
    if (!ok) return JS_EXC;
    int f = MAGIC;
    double a[4];
    int na = argc > 4 ? 4 : argc;
    for (int i = 0; i < na; i++)
        if (to_number(rt, argv[i], &a[i]) < 0) return JS_EXC;
    if (na == 0) a[0] = JS_NAN;
    double r;
    if (f == S_TIME) {
        r = time_clip(a[0]);
    } else {
        if (t != t && f != S_YEAR) return mknum(JS_NAN);
        DT d;
        if (t != t) {
            t = 0;
        }
        split_time(t, &d);
        double y = (double)d.year, mo = d.mon, dd = d.day, h = d.h, mi = d.mi, s = d.s, ms = d.ms;
        switch (f) {
        case S_MS: ms = a[0]; break;
        case S_SEC:
            s = a[0];
            if (na > 1) ms = a[1];
            break;
        case S_MIN:
            mi = a[0];
            if (na > 1) s = a[1];
            if (na > 2) ms = a[2];
            break;
        case S_HOURS:
            h = a[0];
            if (na > 1) mi = a[1];
            if (na > 2) s = a[2];
            if (na > 3) ms = a[3];
            break;
        case S_DATE: dd = a[0]; break;
        case S_MONTH:
            mo = a[0];
            if (na > 1) dd = a[1];
            break;
        case S_YEAR:
            y = a[0];
            if (na > 1) mo = a[1];
            if (na > 2) dd = a[2];
            break;
        }
        r = time_clip(make_date(make_day(y, mo, dd), make_time(h, mi, s, ms)));
    }
    vobj(this_)->u.prim.v = mknum(r);
    return mknum(r);
}

NATIVE(date_to_string) {
    bool ok;
    double t = this_time(rt, this_, &ok);
    if (!ok) return JS_EXC;
    int k = MAGIC;
    if (k == 3 && t != t) return throw_error(rt, E_RANGE, "Invalid time value");
    return mkstr(fmt_date(rt, t, k));
}
NATIVE(date_to_json) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    jsval tv = to_primitive(rt, o, 1);
    TRY(tv);
    if (is_num(tv) && !js_isfinite(vnum(tv))) return JS_NULL;
    return call_method(rt, o, atom_c(rt, "toISOString"), 0, 0);
}
NATIVE(date_to_prim) {
    if (!is_obj(this_)) THROW_TYPE("Date.prototype[Symbol.toPrimitive] called on non-object");
    jsval h = ARG(0);
    int hint;
    if (h == ATOM(string) || h == ATOM(default))
        hint = 2;
    else if (h == ATOM(number))
        hint = 1;
    else
        THROW_TYPE("Invalid hint: %S", val_brief(rt, h));
    jsval m1 = hint == 2 ? ATOM(toString) : ATOM(valueOf);
    jsval m2 = hint == 2 ? ATOM(valueOf) : ATOM(toString);
    for (int i = 0; i < 2; i++) {
        jsval f = get_p(rt, this_, i ? m2 : m1);
        TRY(f);
        if (is_callable(f)) {
            jsval r = call_fn(rt, f, this_, 0, 0);
            TRY(r);
            if (!is_obj(r)) return r;
        }
    }
    THROW_TYPE("Cannot convert object to primitive value");
}

void init_date(js_rt *rt) {
    JSObject *dp = new_object(rt, rt->intr[I_ObjectProto], CLS_OBJECT);
    rt->intr[I_DateProto] = dp;
    JSObject *c = new_ctor(rt, "Date", date_ctor, 7, dp);
    def_method(rt, c, "now", date_now_n, 0, 0);
    def_method(rt, c, "parse", date_parse, 1, 0);
    def_method(rt, c, "UTC", date_utc, 7, 0);
    static const struct {
        const char *n;
        int m;
    } g[] = {{"getTime", G_TIME}, {"valueOf", G_TIME}, {"getFullYear", G_YEAR}, {"getUTCFullYear", G_YEAR}, {"getMonth", G_MONTH},
             {"getUTCMonth", G_MONTH}, {"getDate", G_DATE}, {"getUTCDate", G_DATE}, {"getDay", G_DAY}, {"getUTCDay", G_DAY},
             {"getHours", G_HOURS}, {"getUTCHours", G_HOURS}, {"getMinutes", G_MIN}, {"getUTCMinutes", G_MIN},
             {"getSeconds", G_SEC}, {"getUTCSeconds", G_SEC}, {"getMilliseconds", G_MS}, {"getUTCMilliseconds", G_MS},
             {"getTimezoneOffset", G_TZ}, {"getYear", G_YEAR2}};
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) def_method(rt, dp, g[i].n, date_get, 0, g[i].m);
    static const struct {
        const char *n;
        int m, len;
    } s[] = {{"setTime", S_TIME, 1}, {"setMilliseconds", S_MS, 1}, {"setUTCMilliseconds", S_MS, 1}, {"setSeconds", S_SEC, 2},
             {"setUTCSeconds", S_SEC, 2}, {"setMinutes", S_MIN, 3}, {"setUTCMinutes", S_MIN, 3}, {"setHours", S_HOURS, 4},
             {"setUTCHours", S_HOURS, 4}, {"setDate", S_DATE, 1}, {"setUTCDate", S_DATE, 1}, {"setMonth", S_MONTH, 2},
             {"setUTCMonth", S_MONTH, 2}, {"setFullYear", S_YEAR, 3}, {"setUTCFullYear", S_YEAR, 3}};
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++) def_method(rt, dp, s[i].n, date_set, s[i].len, s[i].m);
    def_method(rt, dp, "toString", date_to_string, 0, 0);
    def_method(rt, dp, "toDateString", date_to_string, 0, 1);
    def_method(rt, dp, "toTimeString", date_to_string, 0, 2);
    def_method(rt, dp, "toISOString", date_to_string, 0, 3);
    JSObject *utc = def_method(rt, dp, "toUTCString", date_to_string, 0, 4);
    def_value(rt, dp, "toGMTString", mkobj(utc));
    def_method(rt, dp, "toLocaleDateString", date_to_string, 0, 5);
    def_method(rt, dp, "toLocaleTimeString", date_to_string, 0, 6);
    def_method(rt, dp, "toLocaleString", date_to_string, 0, 7);
    def_method(rt, dp, "toJSON", date_to_json, 1, 0);
    JSObject *tp = new_native(rt, "[Symbol.toPrimitive]", date_to_prim, 1, 0, false);
    def_prop(rt, dp, SYM(toPrimitive), mkobj(tp), P_C);
}
