/* AXIOM JavaScript engine: math library (fdlibm algorithms, as used by V8) and exact
 * number <-> string conversion (shortest round-trip dtoa, toFixed/toPrecision/toExponential,
 * correctly rounded strtod) built on a small bignum. */
#include "js_int.h"

#define HI(x) ((int32_t)(dbits(x) >> 32))
#define LO(x) ((uint32_t)dbits(x))
static inline double mkd(int32_t hi, uint32_t lo) { return bitsd(((uint64_t)(uint32_t)hi << 32) | lo); }
#define SET_HI(x, h) ((x) = mkd((h), LO(x)))
#define SET_LO(x, l) ((x) = mkd(HI(x), (l)))
static inline double fabs_(double x) { return bitsd(dbits(x) & 0x7FFFFFFFFFFFFFFFull); }

/* ---------------- rounding ---------------- */
double js_trunc(double x) {
    uint64_t u = dbits(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return x;
    if (e < 0) return bitsd(u & 0x8000000000000000ull);
    uint64_t m = 0x000FFFFFFFFFFFFFull >> e;
    if ((u & m) == 0) return x;
    return bitsd(u & ~m);
}
double js_floor(double x) {
    double t = js_trunc(x);
    return t > x ? t - 1.0 : t;
}
double js_ceil(double x) {
    double t = js_trunc(x);
    if (t < x) return t + 1.0;
    return t;
}
double js_round(double x) {
    if (!js_isfinite(x) || x == 0) return x;
    if (x > 0 && x < 0.5) return 0.0;
    if (x < 0 && x >= -0.5) return -0.0;
    double r = js_floor(x);
    if (x - r >= 0.5) r += 1.0;
    if (r == 0 && x < 0) return -0.0;
    return r;
}
double js_fmod(double x, double y) {
    uint64_t uxi = dbits(x), uyi = dbits(y), i;
    int ex = uxi >> 52 & 0x7ff, ey = uyi >> 52 & 0x7ff, sx = uxi >> 63;
    if (uyi << 1 == 0 || y != y || ex == 0x7ff) return JS_NAN;
    if (uxi << 1 <= uyi << 1) {
        if (uxi << 1 == uyi << 1) return 0 * x;
        return x;
    }
    if (!ex) {
        for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1);
        uxi <<= -ex + 1;
    } else {
        uxi &= -1ULL >> 12;
        uxi |= 1ULL << 52;
    }
    if (!ey) {
        for (i = uyi << 12; i >> 63 == 0; ey--, i <<= 1);
        uyi <<= -ey + 1;
    } else {
        uyi &= -1ULL >> 12;
        uyi |= 1ULL << 52;
    }
    for (; ex > ey; ex--) {
        i = uxi - uyi;
        if (i >> 63 == 0) {
            if (i == 0) return 0 * x;
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uyi;
    if (i >> 63 == 0) {
        if (i == 0) return 0 * x;
        uxi = i;
    }
    for (; uxi >> 52 == 0; uxi <<= 1, ex--);
    if (ex > 0) {
        uxi -= 1ULL << 52;
        uxi |= (uint64_t)ex << 52;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= (uint64_t)sx << 63;
    return bitsd(uxi);
}
double js_ldexp(double x, int n) {
    double y = x;
    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    return y * bitsd((uint64_t)(0x3ff + n) << 52);
}

/* ---------------- exp / log ---------------- */
static const double ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01},
                    ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10},
                    halF[2] = {0.5, -0.5};
static const double invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
                    P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
                    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
static const double o_threshold = 7.09782712893383973096e+02, u_threshold = -7.45133219101941108420e+02;
static const double twom1000 = 9.33263618503218878990e-302;

double js_exp(double x) {
    double y, hi = 0.0, lo = 0.0, c, t, twopk;
    int32_t k = 0, xsb;
    uint32_t hx = (uint32_t)HI(x);
    xsb = (hx >> 31) & 1;
    hx &= 0x7fffffff;
    if (hx >= 0x40862E42) {
        if (hx >= 0x7ff00000) {
            if (((hx & 0xfffff) | LO(x)) != 0) return x + x;
            return (xsb == 0) ? x : 0.0;
        }
        if (x > o_threshold) return JS_INF;
        if (x < u_threshold) return 0.0;
    }
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) {
            if (x == 1.0) return 2.718281828459045; /* as V8: exp(1) is exactly Math.E */
            hi = x - ln2HI[xsb];
            lo = ln2LO[xsb];
            k = 1 - xsb - xsb;
        } else {
            k = (int32_t)(invln2 * x + halF[xsb]);
            t = k;
            hi = x - t * ln2HI[0];
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x3e300000) {
        return 1.0 + x;
    } else
        k = 0;
    t = x * x;
    if (k >= -1021)
        twopk = mkd(0x3ff00000 + (int32_t)((uint32_t)k << 20), 0);
    else
        twopk = mkd(0x3ff00000 + (int32_t)((uint32_t)(k + 1000) << 20), 0);
    c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
    y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
    if (k >= -1021) {
        if (k == 1024) return y * 2.0 * 0x1p1023;
        return y * twopk;
    }
    return y * twopk * twom1000;
}

static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
                    two54 = 1.80143985094819840000e+16, Lg1 = 6.666666666666735130e-01,
                    Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
                    Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;

double js_log(double x) {
    double hfsq, f, s, z, R, w, t1, t2, dk;
    int32_t k, hx, i, j;
    uint32_t lx;
    hx = HI(x);
    lx = LO(x);
    k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | lx) == 0) return -JS_INF;
        if (hx < 0) return JS_NAN;
        k -= 54;
        x *= two54;
        hx = HI(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    i = (hx + 0x95f64) & 0x100000;
    SET_HI(x, hx | (i ^ 0x3ff00000));
    k += (i >> 20);
    f = x - 1.0;
    if ((0x000fffff & (2 + hx)) < 3) {
        if (f == 0.0) {
            if (k == 0) return 0.0;
            dk = (double)k;
            return dk * ln2_hi + dk * ln2_lo;
        }
        R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        dk = (double)k;
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s = f / (2.0 + f);
    dk = (double)k;
    z = s * s;
    i = hx - 0x6147a;
    w = z * z;
    j = 0x6b851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    R = t2 + t1;
    if (i > 0) {
        hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

static inline double k_log1p(double f) {
    double hfsq, s, z, R, w, t1, t2;
    s = f / (2.0 + f);
    z = s * s;
    w = z * z;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    R = t2 + t1;
    hfsq = 0.5 * f * f;
    return s * (hfsq + R);
}

double js_log2(double x) {
    static const double ivln2hi = 1.44269504072144627571e+00, ivln2lo = 1.67517131648865118353e-10;
    double f, hfsq, hi, lo, r, val_hi, val_lo, w, y;
    int32_t i, k, hx;
    uint32_t lx;
    hx = HI(x);
    lx = LO(x);
    k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | lx) == 0) return -JS_INF;
        if (hx < 0) return JS_NAN;
        k -= 54;
        x *= two54;
        hx = HI(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    if (hx == 0x3ff00000 && lx == 0) return 0.0;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    i = (hx + 0x95f64) & 0x100000;
    SET_HI(x, hx | (i ^ 0x3ff00000));
    k += (i >> 20);
    y = (double)k;
    f = x - 1.0;
    hfsq = 0.5 * f * f;
    r = k_log1p(f);
    hi = f - hfsq;
    SET_LO(hi, 0);
    lo = (f - hi) - hfsq + r;
    val_hi = hi * ivln2hi;
    val_lo = (lo + hi) * ivln2lo + lo * ivln2hi;
    w = y + val_hi;
    val_lo += (y - w) + val_hi;
    val_hi = w;
    return val_lo + val_hi;
}

double js_log10(double x) {
    static const double ivln10 = 4.34294481903251816668e-01, log10_2hi = 3.01029995663611771306e-01,
                        log10_2lo = 3.69423907715893078616e-13;
    double y, z;
    int32_t i, k, hx;
    uint32_t lx;
    hx = HI(x);
    lx = LO(x);
    k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | lx) == 0) return -JS_INF;
        if (hx < 0) return JS_NAN;
        k -= 54;
        x *= two54;
        hx = HI(x);
        lx = LO(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    if (hx == 0x3ff00000 && lx == 0) return 0;
    k += (hx >> 20) - 1023;
    i = (int32_t)(((uint32_t)k & 0x80000000) >> 31);
    hx = (hx & 0x000fffff) | ((0x3ff - i) << 20);
    y = (double)(k + i);
    x = mkd(hx, lx);
    z = y * log10_2lo + ivln10 * js_log(x);
    return z + y * log10_2hi;
}

double js_expm1(double x) {
    static const double Q1 = -3.33333333333331316428e-02, Q2 = 1.58730158725481460165e-03,
                        Q3 = -7.93650757867487942473e-05, Q4 = 4.00821782732936239552e-06,
                        Q5 = -2.01099218183624371326e-07;
    double y, hi, lo, c = 0, t, e, hxs, hfx, r1, twopk;
    int32_t k, xsb;
    uint32_t hx = (uint32_t)HI(x);
    xsb = hx & 0x80000000;
    hx &= 0x7fffffff;
    if (hx >= 0x4043687A) {
        if (hx >= 0x40862E42) {
            if (hx >= 0x7ff00000) {
                if (((hx & 0xfffff) | LO(x)) != 0) return x + x;
                return (xsb == 0) ? x : -1.0;
            }
            if (x > o_threshold) return JS_INF;
        }
        if (xsb != 0) return -1.0;
    }
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) {
            if (xsb == 0) {
                hi = x - ln2_hi;
                lo = ln2_lo;
                k = 1;
            } else {
                hi = x + ln2_hi;
                lo = -ln2_lo;
                k = -1;
            }
        } else {
            k = (int32_t)(invln2 * x + ((xsb == 0) ? 0.5 : -0.5));
            t = k;
            hi = x - t * ln2_hi;
            lo = t * ln2_lo;
        }
        x = hi - lo;
        c = (hi - x) - lo;
    } else if (hx < 0x3c900000) {
        return x;
    } else
        k = 0;
    hfx = 0.5 * x;
    hxs = x * hfx;
    r1 = 1.0 + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    t = 3.0 - r1 * hfx;
    e = hxs * ((r1 - t) / (6.0 - x * t));
    if (k == 0) return x - (x * e - hxs);
    twopk = mkd(0x3ff00000 + (int32_t)((uint32_t)k << 20), 0);
    e = (x * (e - c) - c);
    e -= hxs;
    if (k == -1) return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return 1.0 + 2.0 * (x - e);
    }
    if (k <= -2 || k > 56) {
        y = 1.0 - (e - x);
        if (k == 1024)
            y = y * 2.0 * 0x1p1023;
        else
            y = y * twopk;
        return y - 1.0;
    }
    t = 1.0;
    if (k < 20) {
        t = mkd(0x3ff00000 - (0x200000 >> k), 0);
        y = t - (e - x);
        y = y * twopk;
    } else {
        t = mkd(((0x3ff - k) << 20), 0);
        y = x - (e + t);
        y += 1.0;
        y = y * twopk;
    }
    return y;
}

double js_log1p(double x) {
    static const double Lp1 = 6.666666666666735130e-01, Lp2 = 3.999999999940941908e-01,
                        Lp3 = 2.857142874366239149e-01, Lp4 = 2.222219843214978396e-01,
                        Lp5 = 1.818357216161805012e-01, Lp6 = 1.531383769920937332e-01,
                        Lp7 = 1.479819860511658591e-01;
    double hfsq, f = 0, c = 0, s, z, R, u;
    int32_t k, hx, hu = 0, ax;
    hx = HI(x);
    ax = hx & 0x7fffffff;
    k = 1;
    if (hx < 0x3FDA827A) {
        if (ax >= 0x3ff00000) {
            if (x == -1.0) return -JS_INF;
            return JS_NAN;
        }
        if (ax < 0x3e200000) {
            if (ax < 0x3c900000) return x;
            return x - x * x * 0.5;
        }
        if (hx > 0 || hx <= ((int32_t)0xbfd2bec4)) {
            k = 0;
            f = x;
            hu = 1;
        }
    }
    if (hx >= 0x7ff00000) return x + x;
    if (k != 0) {
        if (hx < 0x43400000) {
            u = 1.0 + x;
            hu = HI(u);
            k = (hu >> 20) - 1023;
            c = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
            c /= u;
        } else {
            u = x;
            hu = HI(u);
            k = (hu >> 20) - 1023;
            c = 0;
        }
        hu &= 0x000fffff;
        if (hu < 0x6a09e) {
            SET_HI(u, hu | 0x3ff00000);
        } else {
            k += 1;
            SET_HI(u, hu | 0x3fe00000);
            hu = (0x00100000 - hu) >> 2;
        }
        f = u - 1.0;
    }
    hfsq = 0.5 * f * f;
    if (hu == 0) {
        if (f == 0.0) {
            if (k == 0) return 0.0;
            c += k * ln2_lo;
            return k * ln2_hi + c;
        }
        R = hfsq * (1.0 - 0.66666666666666666 * f);
        if (k == 0) return f - R;
        return k * ln2_hi - ((R - (k * ln2_lo + c)) - f);
    }
    s = f / (2.0 + f);
    z = s * s;
    R = z * (Lp1 + z * (Lp2 + z * (Lp3 + z * (Lp4 + z * (Lp5 + z * (Lp6 + z * Lp7))))));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return k * ln2_hi - ((hfsq - (s * (hfsq + R) + (k * ln2_lo + c))) - f);
}

/* ---------------- pow ---------------- */
double js_pow(double x, double y) {
    static const double bp[] = {1.0, 1.5}, dp_h[] = {0.0, 5.84962487220764160156e-01},
                        dp_l[] = {0.0, 1.35003920212974897128e-08}, two53 = 9007199254740992.0,
                        L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01,
                        L3 = 3.33333329818377432918e-01, L4 = 2.72728123808534006489e-01,
                        L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01,
                        lg2 = 6.93147180559945286227e-01, lg2_h = 6.93147182464599609375e-01,
                        lg2_l = -1.90465429995776804525e-09, ovt = 8.0085662595372944372e-17,
                        cp = 9.61796693925975554329e-01, cp_h = 9.61796700954437255859e-01,
                        cp_l = -7.02846165095275826516e-09, ivln2 = 1.44269504088896338700e+00,
                        ivln2_h = 1.44269502162933349609e+00, ivln2_l = 1.92596299112661746887e-08,
                        huge = 1.0e300, tiny = 1.0e-300;
    double z, ax, z_h, z_l, p_h, p_l;
    double y1, t1, t2, r, s, t, u, v, w;
    int32_t i, j, k, yisint, n;
    int32_t hx, hy, ix, iy;
    uint32_t lx, ly;

    /* JavaScript differences from C pow */
    if (y != y) return JS_NAN;
    if (y == 0) return 1.0;
    if (x != x) return JS_NAN;
    if (!js_isfinite(y) && (x == 1.0 || x == -1.0)) return JS_NAN;

    hx = HI(x);
    lx = LO(x);
    hy = HI(y);
    ly = LO(y);
    ix = hx & 0x7fffffff;
    iy = hy & 0x7fffffff;
    if ((iy | ly) == 0) return 1.0;
    if (hx == 0x3ff00000 && lx == 0) return 1.0;
    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000)
            yisint = 2;
        else if (iy >= 0x3ff00000) {
            k = (iy >> 20) - 0x3ff;
            if (k > 20) {
                j = (int32_t)(ly >> (52 - k));
                if (((uint32_t)j << (52 - k)) == ly) yisint = 2 - (j & 1);
            } else if (ly == 0) {
                j = iy >> (20 - k);
                if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
            }
        }
    }
    if (ly == 0) {
        if (iy == 0x7ff00000) {
            if (((ix - 0x3ff00000) | lx) == 0) return JS_NAN;
            if (ix >= 0x3ff00000) return (hy >= 0) ? y : 0.0;
            return (hy < 0) ? -y : 0.0;
        }
        if (iy == 0x3ff00000) {
            if (hy < 0) return 1.0 / x;
            return x;
        }
        if (hy == 0x40000000) return x * x;
        if (hy == 0x3fe00000) {
            if (hx >= 0) return js_sqrt(x);
        }
    }
    ax = fabs_(x);
    if (lx == 0) {
        if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
            z = ax;
            if (hy < 0) z = 1.0 / z;
            if (hx < 0) {
                if (((ix - 0x3ff00000) | yisint) == 0) {
                    z = JS_NAN;
                } else if (yisint == 1)
                    z = -z;
            }
            return z;
        }
    }
    n = (int32_t)(((uint32_t)hx >> 31) - 1);
    if ((n | yisint) == 0) return JS_NAN;
    s = 1.0;
    if ((n | (yisint - 1)) == 0) s = -1.0;
    if (iy > 0x41e00000) {
        if (iy > 0x43f00000) {
            if (ix <= 0x3fefffff) return (hy < 0) ? huge * huge : tiny * tiny;
            if (ix >= 0x3ff00000) return (hy > 0) ? huge * huge : tiny * tiny;
        }
        if (ix < 0x3fefffff) return (hy < 0) ? s * huge * huge : s * tiny * tiny;
        if (ix > 0x3ff00000) return (hy > 0) ? s * huge * huge : s * tiny * tiny;
        t = ax - 1.0;
        w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        u = ivln2_h * t;
        v = t * ivln2_l - w * ivln2;
        t1 = u + v;
        SET_LO(t1, 0);
        t2 = v - (t1 - u);
    } else {
        double ss, s2, s_h, s_l, t_h, t_l;
        n = 0;
        if (ix < 0x00100000) {
            ax *= two53;
            n -= 53;
            ix = HI(ax);
        }
        n += ((ix) >> 20) - 0x3ff;
        j = ix & 0x000fffff;
        ix = j | 0x3ff00000;
        if (j <= 0x3988E)
            k = 0;
        else if (j < 0xBB67A)
            k = 1;
        else {
            k = 0;
            n += 1;
            ix -= 0x00100000;
        }
        SET_HI(ax, ix);
        u = ax - bp[k];
        v = 1.0 / (ax + bp[k]);
        ss = u * v;
        s_h = ss;
        SET_LO(s_h, 0);
        t_h = mkd(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18), 0);
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l);
        s2 = ss * ss;
        r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = 3.0 + s2 + r;
        SET_LO(t_h, 0);
        t_l = r - ((t_h - 3.0) - s2);
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        p_h = u + v;
        SET_LO(p_h, 0);
        p_l = v - (p_h - u);
        z_h = cp_h * p_h;
        z_l = cp_l * p_h + p_l * cp + dp_l[k];
        t = (double)n;
        t1 = (((z_h + z_l) + dp_h[k]) + t);
        SET_LO(t1, 0);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }
    y1 = y;
    SET_LO(y1, 0);
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    j = HI(z);
    i = (int32_t)LO(z);
    if (j >= 0x40900000) {
        if (((j - 0x40900000) | i) != 0) return s * huge * huge;
        if (p_l + ovt > z - p_h) return s * huge * huge;
    } else if ((j & 0x7fffffff) >= 0x4090cc00) {
        if (((j - (int32_t)0xc090cc00) | i) != 0) return s * tiny * tiny;
        if (p_l <= z - p_h) return s * tiny * tiny;
    }
    i = j & 0x7fffffff;
    k = (i >> 20) - 0x3ff;
    n = 0;
    if (i > 0x3fe00000) {
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 20) - 0x3ff;
        t = mkd(n & ~(0x000fffff >> k), 0);
        n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    t = p_l + p_h;
    SET_LO(t, 0);
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    r = (z * t1) / ((t1 - 2.0) - (w + z * w)); /* sic: V8's variant of fdlibm (results must match node) */
    z = 1.0 - (r - z);
    j = HI(z);
    j += (int32_t)((uint32_t)n << 20);
    if ((j >> 20) <= 0)
        z = js_ldexp(z, n);
    else
        SET_HI(z, j);
    return s * z;
}

/* ---------------- trigonometry ---------------- */
static double k_sin(double x, double y, int iy) {
    static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                        S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                        S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    double z, r, v;
    int32_t ix = HI(x) & 0x7fffffff;
    if (ix < 0x3e400000) {
        if ((int)x == 0) return x;
    }
    z = x * x;
    v = z * x;
    r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}
static double k_cos(double x, double y) {
    static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                        C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                        C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    double a, hz, z, r, qx;
    int32_t ix = HI(x) & 0x7fffffff;
    if (ix < 0x3e400000) {
        if (((int)x) == 0) return 1.0;
    }
    z = x * x;
    r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333) return 1.0 - (0.5 * z - (z * r - x * y));
    if (ix > 0x3fe90000)
        qx = 0.28125;
    else
        qx = mkd(ix - 0x00200000, 0);
    hz = 0.5 * z - qx;
    a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}
static double k_tan(double x, double y, int iy) {
    static const double T[] = {
        3.33333333333334091986e-01, 1.33333333333201242699e-01, 5.39682539762260521377e-02,
        2.18694882948595424599e-02, 8.86323982359930005737e-03, 3.59207910759131235356e-03,
        1.45620945432529025516e-03, 5.88041240820264096874e-04, 2.46463134818469906812e-04,
        7.81794442939557092300e-05, 7.14072491382608190305e-05, -1.85586374855275456654e-05,
        2.59073051863633712884e-05,
    };
    static const double pio4 = 7.85398163397448278999e-01, pio4lo = 3.06161699786838301793e-17;
    double z, r, v, w, s;
    int32_t ix, hx;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix < 0x3e300000) {
        if ((int)x == 0) {
            if (((ix | LO(x)) | (iy + 1)) == 0) return 1.0 / fabs_(x);
            if (iy == 1) return x;
            double a, t;
            z = w = x + y;
            SET_LO(z, 0);
            v = y - (z - x);
            t = a = -1.0 / w;
            SET_LO(t, 0);
            s = 1.0 + t * z;
            return t + a * (s + t * v);
        }
    }
    if (ix >= 0x3FE59428) {
        if (hx < 0) {
            x = -x;
            y = -y;
        }
        z = pio4 - x;
        w = pio4lo - y;
        x = z + w;
        y = 0.0;
    }
    z = x * x;
    w = z * z;
    r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    s = z * x;
    r = y + z * (s * (r + v) + y);
    r += T[0] * s;
    w = x + r;
    if (ix >= 0x3FE59428) {
        v = (double)iy;
        return (double)(1 - ((hx >> 30) & 2)) * (v - 2.0 * (x - (w * w / (w + v) - r)));
    }
    if (iy == 1) return w;
    {
        double a, t;
        z = w;
        SET_LO(z, 0);
        v = r - (z - x);
        t = a = -1.0 / w;
        SET_LO(t, 0);
        s = 1.0 + t * z;
        return t + a * (s + t * v);
    }
}

static const uint32_t two_over_pi32[48] = {
    0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041, 0xFE5163AB, 0xDEBBC561,
    0xB7246E3A, 0x424DD2E0, 0x06492EEA, 0x09D1921C, 0xFE1DEB1C, 0xB129A73E, 0xE88235F5, 0x2EBB4484,
    0xE99C7026, 0xB45F7E41, 0x3991D639, 0x835339F4, 0x9C845F8B, 0xBDF9283B, 0x1FF897FF, 0xDE05980F,
    0xEF2F118B, 0x5A0A6D1F, 0x6D367ECF, 0x27CB09B7, 0x4F463F66, 0x9E5FEA2D, 0x7527BAC7, 0xEBE5F17B,
    0x3D0739F7, 0x8A5292EA, 0x6BFB5FB1, 0x1F8D5D08, 0x56033046, 0xFC7B6BAB, 0xF0CFBC20, 0x9AF4361D,
    0xA9E39161, 0x5EE61B08, 0x6599855F, 0x14A06840, 0x8DFFD880, 0x4D732731, 0x06061556, 0xCA73A8C9,
};
static const int32_t npio2_hw[] = {
    0x3ff921fb, 0x400921fb, 0x4012d97c, 0x401921fb, 0x401f6a7a, 0x4022d97c, 0x4025fdbb, 0x402921fb,
    0x402c463a, 0x402f6a7a, 0x4031475c, 0x4032d97c, 0x40346b9c, 0x4035fdbb, 0x40378fdb, 0x403921fb,
    0x403ab41b, 0x403c463a, 0x403dd85a, 0x403f6a7a, 0x40407e4c, 0x4041475c, 0x4042106c, 0x4042d97c,
    0x4043a28c, 0x40446b9c, 0x404534ac, 0x4045fdbb, 0x4046c6cb, 0x40478fdb, 0x404858eb, 0x404921fb,
};

static void two_prod(double a, double b, double *p, double *e) {
    double c, ah, al, bh, bl;
    *p = a * b;
    c = 134217729.0 * a;
    ah = c - (c - a);
    al = a - ah;
    c = 134217729.0 * b;
    bh = c - (c - b);
    bl = b - bh;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* exact reduction of a huge |x| (>= 2^19*pi/2) modulo pi/2 using the bits of 2/pi */
static int rem_pio2_large(double x, double *y) {
    uint64_t u = dbits(x) & 0x7FFFFFFFFFFFFFFFull;
    int be = (int)(u >> 52);
    uint64_t m = (u & ((1ull << 52) - 1)) | (1ull << 52);
    int e = be - 1075;
    int i0 = e >= 35 ? (e - 35) / 32 + 1 : 0;
    uint32_t L[8], Q[11];
    for (int j = 0; j < 8; j++) L[j] = (i0 + 7 - j) < 48 ? two_over_pi32[i0 + 7 - j] : 0;
    uint32_t mlo = (uint32_t)m, mhi = (uint32_t)(m >> 32);
    memset(Q, 0, sizeof Q);
    for (int j = 0; j < 8; j++) {
        uint64_t c = 0;
        uint64_t t = (uint64_t)L[j] * mlo + Q[j] + c;
        Q[j] = (uint32_t)t;
        c = t >> 32;
        t = (uint64_t)L[j] * mhi + Q[j + 1] + c;
        Q[j + 1] = (uint32_t)t;
        c = t >> 32;
        int k = j + 2;
        while (c) {
            t = (uint64_t)Q[k] + c;
            Q[k] = (uint32_t)t;
            c = t >> 32;
            k++;
        }
    }
    int sh = 32 * (i0 + 8) - e; /* fractional bits in Q */
    /* bit access helper */
#define QBIT(b) ((b) < 0 || (b) >= 352 ? 0 : (Q[(b) >> 5] >> ((b) & 31)) & 1)
    int n = (int)(QBIT(sh) | (QBIT(sh + 1) << 1) | (QBIT(sh + 2) << 2));
    /* take 128 fraction bits below the point */
    uint64_t A = 0, B = 0;
    for (int b = 0; b < 64; b++) A = (A << 1) | QBIT(sh - 1 - b);
    for (int b = 64; b < 128; b++) B = (B << 1) | QBIT(sh - 1 - b);
#undef QBIT
    bool neg = false;
    if (A >> 63) { /* >= 0.5: round up, f = frac - 1 */
        n++;
        neg = true;
        /* 2^128 - (A,B) */
        B = ~B + 1;
        A = ~A + (B == 0 ? 1 : 0);
    }
    int cnt = 0;
    if (A == 0) {
        A = B;
        B = 0;
        cnt += 64;
    }
    if (A == 0) {
        y[0] = y[1] = 0;
        return n & 7;
    }
    int lz = __builtin_clzll(A);
    if (lz) {
        A = (A << lz) | (B >> (64 - lz));
        B <<= lz;
    }
    cnt += lz;
    double hi = (double)(A & ~0x7FFull);
    double lo = (double)(A & 0x7FF) + (double)B * 0x1p-64;
    double sc = js_ldexp(1.0, -64 - cnt);
    hi *= sc;
    lo *= sc;
    static const double PIO2_HI = 1.57079632679489655800e+00, PIO2_LO = 6.12323399573676603587e-17;
    double p, err;
    two_prod(hi, PIO2_HI, &p, &err);
    double y1 = err + hi * PIO2_LO + lo * PIO2_HI;
    double t = p + y1;
    y1 = y1 - (t - p);
    if (neg) {
        t = -t;
        y1 = -y1;
    }
    y[0] = t;
    y[1] = y1;
    return n & 7;
}

static int32_t rem_pio2(double x, double *y) {
    static const double invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                        pio2_1t = 6.07710050650619224932e-11, pio2_2 = 6.07710050630396597660e-11,
                        pio2_2t = 2.02226624879595063154e-21, pio2_3 = 2.02226624871116645580e-21,
                        pio2_3t = 8.47842766036889956997e-32;
    double z, w, t, r, fn;
    int32_t i, j, n, ix, hx;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) {
        if (hx > 0) {
            z = x - pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        } else {
            z = x + pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z + pio2_1t;
                y[1] = (z - y[0]) + pio2_1t;
            } else {
                z += pio2_2;
                y[0] = z + pio2_2t;
                y[1] = (z - y[0]) + pio2_2t;
            }
            return -1;
        }
    }
    if (ix <= 0x413921fb) {
        t = fabs_(x);
        n = (int32_t)(t * invpio2 + 0.5);
        fn = (double)n;
        r = t - fn * pio2_1;
        w = fn * pio2_1t;
        if (n < 32 && ix != npio2_hw[n - 1]) {
            y[0] = r - w;
        } else {
            j = ix >> 20;
            y[0] = r - w;
            i = j - ((HI(y[0]) >> 20) & 0x7ff);
            if (i > 16) {
                t = r;
                w = fn * pio2_2;
                r = t - w;
                w = fn * pio2_2t - ((t - r) - w);
                y[0] = r - w;
                i = j - ((HI(y[0]) >> 20) & 0x7ff);
                if (i > 49) {
                    t = r;
                    w = fn * pio2_3;
                    r = t - w;
                    w = fn * pio2_3t - ((t - r) - w);
                    y[0] = r - w;
                }
            }
        }
        y[1] = (r - y[0]) - w;
        if (hx < 0) {
            y[0] = -y[0];
            y[1] = -y[1];
            return -n;
        }
        return n;
    }
    if (ix >= 0x7ff00000) {
        y[0] = y[1] = x - x;
        return 0;
    }
    n = rem_pio2_large(x, y);
    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

double js_sin(double x) {
    double y[2];
    int32_t n, ix = HI(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return k_sin(x, 0.0, 0);
    if (ix >= 0x7ff00000) return JS_NAN;
    n = rem_pio2(x, y);
    switch (n & 3) {
    case 0: return k_sin(y[0], y[1], 1);
    case 1: return k_cos(y[0], y[1]);
    case 2: return -k_sin(y[0], y[1], 1);
    default: return -k_cos(y[0], y[1]);
    }
}
double js_cos(double x) {
    double y[2];
    int32_t n, ix = HI(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return k_cos(x, 0.0);
    if (ix >= 0x7ff00000) return JS_NAN;
    n = rem_pio2(x, y);
    switch (n & 3) {
    case 0: return k_cos(y[0], y[1]);
    case 1: return -k_sin(y[0], y[1], 1);
    case 2: return -k_cos(y[0], y[1]);
    default: return k_sin(y[0], y[1], 1);
    }
}
double js_tan(double x) {
    double y[2];
    int32_t n, ix = HI(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return k_tan(x, 0.0, 1);
    if (ix >= 0x7ff00000) return JS_NAN;
    n = rem_pio2(x, y);
    return k_tan(y[0], y[1], 1 - ((n & 1) << 1));
}

static const double pS0 = 1.66666666666666657415e-01, pS1 = -3.25565818622400915405e-01,
                    pS2 = 2.01212532134862925881e-01, pS3 = -4.00555345006794114027e-02,
                    pS4 = 7.91534994289814532176e-04, pS5 = 3.47933107596021167570e-05,
                    qS1 = -2.40339491173441421878e+00, qS2 = 2.02094576023350569471e+00,
                    qS3 = -6.88283971605453293030e-01, qS4 = 7.70381505559019352791e-02,
                    pio2_hi = 1.57079632679489655800e+00, pio2_lo = 6.12323399573676603587e-17,
                    pio4_hi = 7.85398163397448278999e-01, pi_d = 3.14159265358979311600e+00;

double js_asin(double x) {
    double t = 0.0, w, p, q, c, r, s;
    int32_t hx, ix;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix >= 0x3ff00000) {
        if (((ix - 0x3ff00000) | LO(x)) == 0) return x * pio2_hi + x * pio2_lo;
        return JS_NAN;
    } else if (ix < 0x3fe00000) {
        if (ix < 0x3e400000) return x;
        t = x * x;
        p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
        q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
        w = p / q;
        return x + x * w;
    }
    w = 1.0 - fabs_(x);
    t = w * 0.5;
    p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    s = js_sqrt(t);
    if (ix >= 0x3FEF3333) {
        w = p / q;
        t = pio2_hi - (2.0 * (s + s * w) - pio2_lo);
    } else {
        w = s;
        SET_LO(w, 0);
        c = (t - w * w) / (s + w);
        r = p / q;
        p = 2.0 * s * r - (pio2_lo - 2.0 * c);
        q = pio4_hi - 2.0 * w;
        t = pio4_hi - (p - q);
    }
    return hx > 0 ? t : -t;
}
double js_acos(double x) {
    double z, p, q, r, w, s, c, df;
    int32_t hx, ix;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix >= 0x3ff00000) {
        if (((ix - 0x3ff00000) | LO(x)) == 0) {
            if (hx > 0) return 0.0;
            return pi_d + 2.0 * pio2_lo;
        }
        return JS_NAN;
    }
    if (ix < 0x3fe00000) {
        if (ix <= 0x3c600000) return pio2_hi + pio2_lo;
        z = x * x;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        r = p / q;
        return pio2_hi - (x - (pio2_lo - x * r));
    } else if (hx < 0) {
        z = (1.0 + x) * 0.5;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        s = js_sqrt(z);
        r = p / q;
        w = r * s - pio2_lo;
        return pi_d - 2.0 * (s + w);
    }
    z = (1.0 - x) * 0.5;
    s = js_sqrt(z);
    df = s;
    SET_LO(df, 0);
    c = (z - df * df) / (s + df);
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    w = r * s + c;
    return 2.0 * (df + w);
}

double js_atan(double x) {
    static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                    9.82793723247329054082e-01, 1.57079632679489655800e+00};
    static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                    1.39033110312309984516e-17, 6.12323399573676603587e-17};
    static const double aT[] = {
        3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
        -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
        6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
        -3.65315727442169155270e-02, 1.62858201153657823623e-02,
    };
    double w, s1, s2, z;
    int32_t ix, hx, id;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix >= 0x44100000) {
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && (LO(x) != 0))) return x + x;
        if (hx > 0) return atanhi[3] + atanlo[3];
        return -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) {
        if (ix < 0x3e300000) return x;
        id = -1;
    } else {
        x = fabs_(x);
        if (ix < 0x3ff30000) {
            if (ix < 0x3fe60000) {
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else {
            if (ix < 0x40038000) {
                id = 2;
                x = (x - 1.5) / (1.0 + 1.5 * x);
            } else {
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return (hx < 0) ? -z : z;
}

double js_atan2(double y, double x) {
    static const double pi_o_4 = 7.8539816339744827900E-01, pi_o_2 = 1.5707963267948965580E+00,
                        pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    double z;
    int32_t k, m, hx, hy, ix, iy;
    uint32_t lx, ly;
    hx = HI(x);
    lx = LO(x);
    ix = hx & 0x7fffffff;
    hy = HI(y);
    ly = LO(y);
    iy = hy & 0x7fffffff;
    if (x != x || y != y) return JS_NAN;
    if ((((uint32_t)hx - 0x3ff00000u) | lx) == 0) return js_atan(y);
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2);
    if ((iy | ly) == 0) {
        switch (m) {
        case 0:
        case 1: return y;
        case 2: return pi;
        default: return -pi;
        }
    }
    if ((ix | lx) == 0) return (hy < 0) ? -pi_o_2 : pi_o_2;
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0: return pi_o_4;
            case 1: return -pi_o_4;
            case 2: return 3.0 * pi_o_4;
            default: return -3.0 * pi_o_4;
            }
        } else {
            switch (m) {
            case 0: return 0.0;
            case 1: return -0.0;
            case 2: return pi;
            default: return -pi;
            }
        }
    }
    if (iy == 0x7ff00000) return (hy < 0) ? -pi_o_2 : pi_o_2;
    k = (iy - ix) >> 20;
    if (k > 60) {
        z = pi_o_2 + 0.5 * pi_lo;
        m &= 1;
    } else if (hx < 0 && k < -60)
        z = 0.0;
    else
        z = js_atan(fabs_(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

/* ---------------- hyperbolic, cbrt, hypot ---------------- */
#define K_OVERFLOW 710.4758600739439
double js_sinh(double x) {
    double t, h;
    int32_t ix, jx;
    jx = HI(x);
    ix = jx & 0x7fffffff;
    if (ix >= 0x7ff00000) return x + x;
    h = 0.5;
    if (jx < 0) h = -h;
    if (ix < 0x40360000) {
        if (ix < 0x3e300000) return x;
        t = js_expm1(fabs_(x));
        if (ix < 0x3ff00000) return h * (2.0 * t - t * t / (t + 1.0));
        return h * (t + t / (t + 1.0));
    }
    if (ix < 0x40862E42) return h * js_exp(fabs_(x));
    if (fabs_(x) <= K_OVERFLOW) {
        double w = js_exp(0.5 * fabs_(x));
        t = h * w;
        return t * w;
    }
    return x * 1.0e307;
}
double js_cosh(double x) {
    double t, w;
    int32_t ix = HI(x) & 0x7fffffff;
    if (ix >= 0x7ff00000) return x * x;
    if (ix < 0x3fd62e43) {
        t = js_expm1(fabs_(x));
        w = 1.0 + t;
        if (ix < 0x3c800000) return w;
        return 1.0 + (t * t) / (w + w);
    }
    if (ix < 0x40360000) {
        t = js_exp(fabs_(x));
        return 0.5 * t + 0.5 / t;
    }
    if (ix < 0x40862E42) return 0.5 * js_exp(fabs_(x));
    if (fabs_(x) <= K_OVERFLOW) {
        w = js_exp(0.5 * fabs_(x));
        t = 0.5 * w;
        return t * w;
    }
    return JS_INF;
}
double js_tanh(double x) {
    double t, z;
    int32_t jx, ix;
    jx = HI(x);
    ix = jx & 0x7fffffff;
    if (ix >= 0x7ff00000) {
        if (x != x) return x;
        return jx >= 0 ? 1.0 : -1.0;
    }
    if (ix < 0x40360000) {
        if (ix < 0x3e300000) return x;
        if (ix >= 0x3ff00000) {
            t = js_expm1(2.0 * fabs_(x));
            z = 1.0 - 2.0 / (t + 2.0);
        } else {
            t = js_expm1(-2.0 * fabs_(x));
            z = -t / (t + 2.0);
        }
    } else {
        z = 1.0;
    }
    return (jx >= 0) ? z : -z;
}
double js_asinh(double x) {
    static const double ln2 = 6.93147180559945286227e-01;
    double t, w;
    int32_t hx, ix;
    hx = HI(x);
    ix = hx & 0x7fffffff;
    if (ix >= 0x7ff00000) return x + x;
    if (ix < 0x3e300000) return x;
    if (ix > 0x41b00000) {
        w = js_log(fabs_(x)) + ln2;
    } else if (ix > 0x40000000) {
        t = fabs_(x);
        w = js_log(2.0 * t + 1.0 / (js_sqrt(x * x + 1.0) + t));
    } else {
        t = x * x;
        w = js_log1p(fabs_(x) + t / (1.0 + js_sqrt(1.0 + t)));
    }
    return hx > 0 ? w : -w;
}
double js_acosh(double x) {
    static const double ln2 = 6.93147180559945286227e-01;
    double t;
    int32_t hx = HI(x);
    uint32_t lx = LO(x);
    if (hx < 0x3ff00000) return JS_NAN;
    if (hx >= 0x41b00000) {
        if (hx >= 0x7ff00000) return x + x;
        return js_log(x) + ln2;
    }
    if ((((uint32_t)hx - 0x3ff00000u) | lx) == 0) return 0.0;
    if (hx > 0x40000000) {
        t = x * x;
        return js_log(2.0 * x - 1.0 / (x + js_sqrt(t - 1.0)));
    }
    t = x - 1.0;
    return js_log1p(t + js_sqrt(2.0 * t + t * t));
}
double js_atanh(double x) {
    double t;
    int32_t hx = HI(x), ix = hx & 0x7fffffff;
    uint32_t lx = LO(x);
    if (x != x) return x;
    if ((ix | ((lx | (0u - lx)) >> 31)) > 0x3ff00000) return JS_NAN;
    if (ix == 0x3ff00000) return hx > 0 ? JS_INF : -JS_INF;
    if (ix < 0x3e300000) return x;
    SET_HI(x, ix);
    if (ix < 0x3fe00000) {
        t = x + x;
        t = 0.5 * js_log1p(t + t * x / (1.0 - x));
    } else
        t = 0.5 * js_log1p((x + x) / (1.0 - x));
    return hx >= 0 ? t : -t;
}
double js_cbrt(double x) {
    static const uint32_t B1 = 715094163, B2 = 696219795;
    static const double P0 = 1.87595182427177009643, P1c = -1.88497979543377169875, P2c = 1.621429720105354466140,
                        P3c = -0.758397934778766047437, P4c = 0.145996192886612446982;
    int32_t hx;
    double r, s, t = 0.0, w;
    uint32_t sign, high, low;
    hx = HI(x);
    low = LO(x);
    sign = (uint32_t)hx & 0x80000000;
    hx ^= (int32_t)sign;
    if (hx >= 0x7ff00000) return (x + x);
    if (hx < 0x00100000) {
        if ((hx | (int32_t)low) == 0) return (x);
        t = mkd(0x43500000, 0);
        t *= x;
        high = (uint32_t)HI(t);
        t = mkd((int32_t)(sign | ((high & 0x7fffffff) / 3 + B2)), 0);
    } else
        t = mkd((int32_t)(sign | ((uint32_t)hx / 3 + B1)), 0);
    r = (t * t) * (t / x);
    t = t * ((P0 + r * (P1c + r * P2c)) + ((r * r) * r) * (P3c + r * P4c));
    uint64_t bits = dbits(t);
    bits = (bits + 0x80000000) & 0xffffffffc0000000ULL;
    t = bitsd(bits);
    s = t * t;
    r = x / s;
    w = t + t;
    r = (r - t) / (w + r);
    t = t + t * r;
    return t;
}
double js_hypot(double x, double y) {
    x = fabs_(x);
    y = fabs_(y);
    if (x == JS_INF || y == JS_INF) return JS_INF;
    if (x != x || y != y) return JS_NAN;
    double mx = x > y ? x : y;
    if (mx == 0) return 0;
    double sum = 0, comp = 0, v[2] = {x, y};
    for (int i = 0; i < 2; i++) {
        double n = v[i] / mx;
        double summand = n * n - comp;
        double pre = sum + summand;
        comp = (pre - sum) - summand;
        sum = pre;
    }
    return js_sqrt(sum) * mx;
}

/* ---------------- integer conversions ---------------- */
int32_t to_int32(double d) {
    if (LIKELY(d >= -2147483648.0 && d <= 2147483647.0)) return (int32_t)d;
    if (!js_isfinite(d)) return 0;
    double t = js_fmod(js_trunc(d), 4294967296.0);
    if (t < 0) t += 4294967296.0;
    return (int32_t)(uint32_t)t;
}
double to_integer(double d) {
    if (d != d) return 0;
    double t = js_trunc(d);
    return t == 0 ? 0 : t;
}

/* ---------------- bignum ---------------- */
#define BN_MAX 150
typedef struct {
    int n;
    uint32_t d[BN_MAX];
} BN;
static void bn_set(BN *a, uint64_t v) {
    a->n = 0;
    while (v) {
        a->d[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}
static void bn_mul_small(BN *a, uint32_t m) {
    uint64_t c = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + c;
        a->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    if (c && a->n < BN_MAX) a->d[a->n++] = (uint32_t)c;
}
static void bn_add_small(BN *a, uint32_t v) {
    uint64_t c = v;
    for (int i = 0; i < a->n && c; i++) {
        uint64_t t = (uint64_t)a->d[i] + c;
        a->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    if (c && a->n < BN_MAX) a->d[a->n++] = (uint32_t)c;
}
static void bn_mul_pow5(BN *a, int k) {
    while (k >= 13) {
        bn_mul_small(a, 1220703125u);
        k -= 13;
    }
    static const uint32_t p5[] = {1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625, 48828125, 244140625};
    if (k > 0) bn_mul_small(a, p5[k]);
}
static void bn_mul_pow10(BN *a, int k) {
    while (k >= 9) {
        bn_mul_small(a, 1000000000u);
        k -= 9;
    }
    static const uint32_t p10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (k > 0) bn_mul_small(a, p10[k]);
}
static void bn_shl(BN *a, int s) {
    if (a->n == 0 || s == 0) return;
    int w = s >> 5, b = s & 31;
    int n = a->n;
    if (n + w + 1 > BN_MAX) w = BN_MAX - n - 1; /* overflow: should not happen */
    if (b) {
        a->d[n + w] = 0;
        for (int i = n - 1; i >= 0; i--) {
            a->d[i + w + 1] |= a->d[i] >> (32 - b);
            a->d[i + w] = a->d[i] << b;
        }
        n += w + 1;
    } else {
        for (int i = n - 1; i >= 0; i--) a->d[i + w] = a->d[i];
        n += w;
    }
    for (int i = 0; i < w; i++) a->d[i] = 0;
    while (n > 0 && a->d[n - 1] == 0) n--;
    a->n = n;
}
static int bn_cmp(const BN *a, const BN *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static void bn_add(BN *r, const BN *a, const BN *b) {
    int n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = c;
        if (i < a->n) t += a->d[i];
        if (i < b->n) t += b->d[i];
        r->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    r->n = n;
    if (c && n < BN_MAX) r->d[r->n++] = (uint32_t)c;
}
static void bn_sub(BN *a, const BN *b) { /* a -= b, a >= b */
    int64_t br = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - br - (i < b->n ? (int64_t)b->d[i] : 0);
        br = t < 0;
        a->d[i] = (uint32_t)(t + (br << 32));
    }
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}
static uint32_t bn_divmod_small(BN *a, uint32_t dv) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t t = (r << 32) | a->d[i];
        a->d[i] = (uint32_t)(t / dv);
        r = t % dv;
    }
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
    return (uint32_t)r;
}
static void bn_copy(BN *d, const BN *s) {
    d->n = s->n;
    memcpy(d->d, s->d, (size_t)s->n * 4);
}
/* decimal digits of a (destroys a); returns count */
static int bn_to_dec(BN *a, char *out) {
    char tmp[1400];
    int n = 0;
    if (a->n == 0) {
        out[0] = '0';
        return 1;
    }
    while (a->n > 0) {
        uint32_t r = bn_divmod_small(a, 1000000000u);
        for (int i = 0; i < 9; i++) {
            tmp[n++] = (char)('0' + r % 10);
            r /= 10;
        }
    }
    while (n > 1 && tmp[n - 1] == '0') n--;
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static void decompose(double v, uint64_t *f, int *e) {
    uint64_t bits = dbits(v);
    int be = (int)((bits >> 52) & 0x7ff);
    *f = bits & ((1ull << 52) - 1);
    if (be == 0)
        *e = -1074;
    else {
        *f |= 1ull << 52;
        *e = be - 1075;
    }
}

/* shortest digits (Steele & White / Burger & Dybvig free-format) for v > 0:
 * value = 0.DIGITS x 10^*kout */
static int dtoa_shortest(double v, char *digits, int *kout) {
    uint64_t f;
    int e;
    decompose(v, &f, &e);
    bool even = (f & 1) == 0;
    bool unequal = (f == (1ull << 52)) && e > -1074;
    BN r, s, mp, mm, t;
    if (e >= 0) {
        bn_set(&r, f);
        bn_shl(&r, e);
        if (!unequal) {
            bn_shl(&r, 1);
            bn_set(&s, 2);
            bn_set(&mp, 1);
            bn_shl(&mp, e);
            bn_copy(&mm, &mp);
        } else {
            bn_shl(&r, 2);
            bn_set(&s, 4);
            bn_set(&mp, 1);
            bn_shl(&mp, e + 1);
            bn_set(&mm, 1);
            bn_shl(&mm, e);
        }
    } else {
        if (!unequal) {
            bn_set(&r, f);
            bn_shl(&r, 1);
            bn_set(&s, 1);
            bn_shl(&s, 1 - e);
            bn_set(&mp, 1);
            bn_set(&mm, 1);
        } else {
            bn_set(&r, f);
            bn_shl(&r, 2);
            bn_set(&s, 1);
            bn_shl(&s, 2 - e);
            bn_set(&mp, 2);
            bn_set(&mm, 1);
        }
    }
    int nbits = 64 - __builtin_clzll(f);
    double est = (e + nbits - 1) * 0.30102999566398114 - 1e-10;
    int k = (int)js_ceil(est);
    if (k >= 0)
        bn_mul_pow10(&s, k);
    else {
        bn_mul_pow10(&r, -k);
        bn_mul_pow10(&mp, -k);
        bn_mul_pow10(&mm, -k);
    }
    bn_add(&t, &r, &mp);
    if (even ? bn_cmp(&t, &s) >= 0 : bn_cmp(&t, &s) > 0) {
        bn_mul_small(&s, 10);
        k++;
    }
    int n = 0;
    for (;;) {
        bn_mul_small(&r, 10);
        bn_mul_small(&mp, 10);
        bn_mul_small(&mm, 10);
        int d = 0;
        while (bn_cmp(&r, &s) >= 0) {
            bn_sub(&r, &s);
            d++;
        }
        bool tc1 = even ? bn_cmp(&r, &mm) <= 0 : bn_cmp(&r, &mm) < 0;
        bn_add(&t, &r, &mp);
        bool tc2 = even ? bn_cmp(&t, &s) >= 0 : bn_cmp(&t, &s) > 0;
        if (!tc1 && !tc2) {
            digits[n++] = (char)('0' + d);
            if (n > 40) break;
            continue;
        }
        if (tc1 && !tc2)
            ;
        else if (!tc1 && tc2)
            d++;
        else {
            bn_add(&t, &r, &r);
            int c = bn_cmp(&t, &s);
            if (c > 0 || (c == 0 && (d & 1))) d++;
        }
        if (d == 10) { /* carry (should not happen) */
            int i = n - 1;
            digits[n++] = '0';
            while (i >= 0 && digits[i] == '9') digits[i--] = '0';
            if (i >= 0)
                digits[i]++;
            else {
                memmove(digits + 1, digits, (size_t)n);
                digits[0] = '1';
                n++;
                k++;
            }
            while (n > 1 && digits[n - 1] == '0') n--;
        } else
            digits[n++] = (char)('0' + d);
        break;
    }
    *kout = k;
    return n;
}

/* exact decimal expansion of v > 0: value = 0.DIGITS x 10^*kout; trailing zeros stripped */
static int dtoa_exact(double v, char *digits, int *kout) {
    uint64_t f;
    int e;
    decompose(v, &f, &e);
    BN a;
    bn_set(&a, f);
    int n;
    if (e >= 0) {
        bn_shl(&a, e);
        n = bn_to_dec(&a, digits);
        *kout = n;
    } else {
        bn_mul_pow5(&a, -e);
        n = bn_to_dec(&a, digits);
        *kout = n + e;
    }
    while (n > 1 && digits[n - 1] == '0') n--;
    return n;
}

/* round digit string to keep digits (half up); returns new count, may bump *k */
static int round_digits(char *d, int n, int keep, int *k) {
    if (keep >= n) return n;
    if (keep < 0) return 0;
    bool up = d[keep] >= '5';
    n = keep;
    if (up) {
        int i = n - 1;
        while (i >= 0 && d[i] == '9') d[i--] = '0';
        if (i >= 0)
            d[i]++;
        else {
            d[0] = '1';
            for (int j = 1; j < n; j++) d[j] = '0';
            if (n == 0) {
                d[0] = '1';
                n = 1;
            }
            (*k)++;
        }
    }
    return n;
}

static int put_int(char *buf, int v) {
    char t[12];
    int n = 0, o = 0;
    if (v < 0) {
        buf[o++] = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) buf[o++] = t[--n];
    return o;
}

/* format digits per Number::toString */
static int format_number(char *buf, bool neg, const char *d, int nd, int n) {
    int o = 0;
    if (neg) buf[o++] = '-';
    if (nd <= n && n <= 21) {
        memcpy(buf + o, d, (size_t)nd);
        o += nd;
        for (int i = nd; i < n; i++) buf[o++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(buf + o, d, (size_t)n);
        o += n;
        buf[o++] = '.';
        memcpy(buf + o, d + n, (size_t)(nd - n));
        o += nd - n;
    } else if (-6 < n && n <= 0) {
        buf[o++] = '0';
        buf[o++] = '.';
        for (int i = 0; i < -n; i++) buf[o++] = '0';
        memcpy(buf + o, d, (size_t)nd);
        o += nd;
    } else {
        buf[o++] = d[0];
        if (nd > 1) {
            buf[o++] = '.';
            memcpy(buf + o, d + 1, (size_t)(nd - 1));
            o += nd - 1;
        }
        buf[o++] = 'e';
        buf[o++] = n - 1 >= 0 ? '+' : '-';
        o += put_int(buf + o, n - 1 >= 0 ? n - 1 : 1 - n);
    }
    buf[o] = 0;
    return o;
}

int num_to_cstr(double v, char *buf) {
    if (v != v) {
        memcpy(buf, "NaN", 4);
        return 3;
    }
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    bool neg = v < 0;
    if (neg) v = -v;
    if (v == JS_INF) {
        if (neg) {
            memcpy(buf, "-Infinity", 10);
            return 9;
        }
        memcpy(buf, "Infinity", 9);
        return 8;
    }
    if (v < 9007199254740992.0 && v == js_trunc(v)) {
        uint64_t u = (uint64_t)v;
        char t[24];
        int n = 0, o = 0;
        while (u) {
            t[n++] = (char)('0' + u % 10);
            u /= 10;
        }
        if (neg) buf[o++] = '-';
        while (n) buf[o++] = t[--n];
        buf[o] = 0;
        return o;
    }
    char d[48];
    int k;
    int nd = dtoa_shortest(v, d, &k);
    return format_number(buf, neg, d, nd, k);
}

int num_to_fixed(double v, int f, char *buf) {
    int o = 0;
    if (v != v) {
        memcpy(buf, "NaN", 4);
        return 3;
    }
    if (v < 0) {
        buf[o++] = '-';
        v = -v;
    }
    if (v >= 1e21) return o + num_to_cstr(v, buf + o) ;
    char d[1200];
    int k = 0, nd = 0;
    if (v != 0) nd = dtoa_exact(v, d, &k);
    /* digits before the cut: k + f */
    int keep = k + f;
    if (v == 0 || keep < 0) {
        nd = 0;
    } else {
        nd = round_digits(d, nd, keep, &k);
    }
    /* integer n = digits d[0..nd) * 10^(k+f-nd) */
    char nbuf[1300];
    int nn = 0;
    if (nd == 0) {
        nbuf[nn++] = '0';
    } else {
        int tot = k + f; /* total digits of n */
        for (int i = 0; i < tot; i++) nbuf[nn++] = i < nd ? d[i] : '0';
        if (nn == 0) nbuf[nn++] = '0';
    }
    if (o == 1 && nn == 1 && nbuf[0] == '0' && f == 0) {
        /* (-0.4).toFixed(0) is "-0" in JS: keep sign */
    }
    if (f == 0) {
        memcpy(buf + o, nbuf, (size_t)nn);
        o += nn;
    } else {
        if (nn <= f) {
            buf[o++] = '0';
            buf[o++] = '.';
            for (int i = nn; i < f; i++) buf[o++] = '0';
            memcpy(buf + o, nbuf, (size_t)nn);
            o += nn;
        } else {
            memcpy(buf + o, nbuf, (size_t)(nn - f));
            o += nn - f;
            buf[o++] = '.';
            memcpy(buf + o, nbuf + nn - f, (size_t)f);
            o += f;
        }
    }
    buf[o] = 0;
    return o;
}

int num_to_exp(double v, int f, char *buf) {
    int o = 0;
    if (v != v) {
        memcpy(buf, "NaN", 4);
        return 3;
    }
    if (v < 0) {
        buf[o++] = '-';
        v = -v;
    }
    if (v == JS_INF) {
        memcpy(buf + o, "Infinity", 9);
        return o + 8;
    }
    char d[1200];
    int k, nd;
    if (v == 0) {
        nd = 1;
        d[0] = '0';
        k = 1;
        if (f > 0) {
            for (int i = 1; i <= f; i++) d[i] = '0';
            nd = f + 1;
        }
    } else if (f < 0) {
        nd = dtoa_shortest(v, d, &k);
    } else {
        nd = dtoa_exact(v, d, &k);
        nd = round_digits(d, nd, f + 1, &k);
        while (nd < f + 1) d[nd++] = '0';
    }
    buf[o++] = d[0];
    if (nd > 1) {
        buf[o++] = '.';
        memcpy(buf + o, d + 1, (size_t)(nd - 1));
        o += nd - 1;
    }
    buf[o++] = 'e';
    int e = v == 0 ? 0 : k - 1;
    buf[o++] = e >= 0 ? '+' : '-';
    o += put_int(buf + o, e >= 0 ? e : -e);
    buf[o] = 0;
    return o;
}

int num_to_prec(double v, int p, char *buf) {
    int o = 0;
    if (v != v) {
        memcpy(buf, "NaN", 4);
        return 3;
    }
    if (v < 0) {
        buf[o++] = '-';
        v = -v;
    }
    if (v == JS_INF) {
        memcpy(buf + o, "Infinity", 9);
        return o + 8;
    }
    char d[1200];
    int k, nd;
    if (v == 0) {
        for (int i = 0; i < p; i++) d[i] = '0';
        nd = p;
        k = 1;
    } else {
        nd = dtoa_exact(v, d, &k);
        nd = round_digits(d, nd, p, &k);
        while (nd < p) d[nd++] = '0';
    }
    int e = k - 1;
    if (e < -6 || e >= p) {
        buf[o++] = d[0];
        if (p > 1) {
            buf[o++] = '.';
            memcpy(buf + o, d + 1, (size_t)(p - 1));
            o += p - 1;
        }
        buf[o++] = 'e';
        buf[o++] = e >= 0 ? '+' : '-';
        o += put_int(buf + o, e >= 0 ? e : -e);
    } else if (e == p - 1) {
        memcpy(buf + o, d, (size_t)p);
        o += p;
    } else if (e >= 0) {
        memcpy(buf + o, d, (size_t)(e + 1));
        o += e + 1;
        buf[o++] = '.';
        memcpy(buf + o, d + e + 1, (size_t)(p - e - 1));
        o += p - e - 1;
    } else {
        buf[o++] = '0';
        buf[o++] = '.';
        for (int i = 0; i < -(e + 1); i++) buf[o++] = '0';
        memcpy(buf + o, d, (size_t)p);
        o += p;
    }
    buf[o] = 0;
    return o;
}

/* V8's DoubleToRadixCString */
int num_to_radix(double value, int radix, char *out) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (value != value) {
        memcpy(out, "NaN", 4);
        return 3;
    }
    if (value == JS_INF || value == -JS_INF) {
        return (int)(value < 0 ? (memcpy(out, "-Infinity", 10), 9) : (memcpy(out, "Infinity", 9), 8));
    }
    if (value == 0) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    enum { kBufferSize = 2200 };
    char *buffer = (char *)malloc(kBufferSize);
    if (!buffer) {
        out[0] = 0;
        return 0;
    }
    int integer_cursor = kBufferSize / 2;
    int fraction_cursor = integer_cursor;
    bool negative = value < 0;
    if (negative) value = -value;
    double integer = js_floor(value);
    double fraction = value - integer;
    double next = bitsd(dbits(value) + 1);
    double delta = 0.5 * (next - value);
    double mind = bitsd(1);
    if (mind > delta) delta = mind;
    if (fraction >= delta) {
        buffer[fraction_cursor++] = '.';
        do {
            fraction *= radix;
            delta *= radix;
            int digit = (int)fraction;
            buffer[fraction_cursor++] = chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    for (;;) {
                        fraction_cursor--;
                        if (fraction_cursor == kBufferSize / 2) {
                            integer += 1;
                            break;
                        }
                        char c = buffer[fraction_cursor];
                        int dg = c > '9' ? (c - 'a' + 10) : (c - '0');
                        if (dg + 1 < radix) {
                            buffer[fraction_cursor++] = chars[dg + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }
    /* integer digits; fill unrepresented digits with zero */
    for (;;) {
        double q = integer / radix;
        uint64_t qb = dbits(q);
        int qe = (int)((qb >> 52) & 0x7ff) - 1075;
        if (!(qe > 0)) break;
        integer = q;
        buffer[--integer_cursor] = '0';
    }
    do {
        double remainder = js_fmod(integer, radix);
        buffer[--integer_cursor] = chars[(int)remainder];
        integer = (integer - remainder) / radix;
    } while (integer > 0);
    if (negative) buffer[--integer_cursor] = '-';
    int n = fraction_cursor - integer_cursor;
    memcpy(out, buffer + integer_cursor, (size_t)n);
    out[n] = 0;
    free(buffer);
    return n;
}

/* ---------------- strtod ---------------- */
#define SD_MAXDIG 780
static double pow10_d(int e) {
    static const double p[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return p[e];
}
/* compare D*10^E with M*2^K exactly */
static int cmp_exact(const BN *D, int E, uint64_t M, int K) {
    BN a, b;
    bn_copy(&a, D);
    bn_set(&b, M);
    if (E >= 0)
        bn_mul_pow5(&a, E);
    else
        bn_mul_pow5(&b, -E);
    int d = E - K;
    if (d > 0)
        bn_shl(&a, d);
    else if (d < 0)
        bn_shl(&b, -d);
    return bn_cmp(&a, &b);
}

double js_strtod(const char *s, size_t n, size_t *used) {
    size_t i = 0;
    char dig[SD_MAXDIG + 2];
    int nd = 0, dexp = 0;
    bool sticky = false, any = false;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        any = true;
        if (nd == 0 && s[i] == '0') {
            i++;
            continue;
        }
        if (nd < SD_MAXDIG)
            dig[nd++] = s[i];
        else {
            if (s[i] != '0') sticky = true;
            dexp++;
        }
        i++;
    }
    if (i < n && s[i] == '.') {
        size_t j = i + 1;
        bool fany = false;
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            fany = true;
            if (nd == 0 && s[j] == '0') {
                dexp--;
                j++;
                continue;
            }
            if (nd < SD_MAXDIG) {
                dig[nd++] = s[j];
                dexp--;
            } else if (s[j] != '0')
                sticky = true;
            j++;
        }
        if (any || fany) {
            i = j;
            any = true;
        }
    }
    if (!any) {
        *used = 0;
        return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        int sg = 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) {
            if (s[j] == '-') sg = -1;
            j++;
        }
        if (j < n && s[j] >= '0' && s[j] <= '9') {
            int ev = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') {
                if (ev < 100000) ev = ev * 10 + (s[j] - '0');
                j++;
            }
            dexp += sg * ev;
            i = j;
        }
    }
    *used = i;
    while (nd > 0 && dig[nd - 1] == '0') {
        nd--;
        dexp++;
    }
    if (nd == 0) return 0.0;
    if (sticky) {
        dig[nd++] = '1';
        dexp--;
    }
    /* value = DIGITS * 10^dexp */
    if (nd + dexp > 310) return JS_INF;
    if (nd + dexp < -326) return 0.0;
    if (nd <= 15) {
        uint64_t m = 0;
        for (int k = 0; k < nd; k++) m = m * 10 + (uint64_t)(dig[k] - '0');
        double dm = (double)m;
        if (dexp == 0) return dm;
        if (dexp > 0 && dexp <= 22) return dm * pow10_d(dexp);
        if (dexp < 0 && dexp >= -22) return dm / pow10_d(-dexp);
        if (dexp > 22 && dexp <= 22 + 15 - nd) return (dm * pow10_d(dexp - 22)) * 1e22;
    }
    /* approximation */
    uint64_t m = 0;
    int k = 0;
    for (; k < nd && k < 19; k++) m = m * 10 + (uint64_t)(dig[k] - '0');
    double z = (double)m;
    int e = dexp + (nd - k);
    if (e > 0) {
        while (e > 22) {
            z *= 1e22;
            e -= 22;
        }
        z *= pow10_d(e);
    } else if (e < 0) {
        e = -e;
        if (e > 300) {
            z /= 1e300;
            e -= 300;
            /* keep precision for subnormals: scale up then down */
        }
        while (e > 22) {
            z /= 1e22;
            e -= 22;
        }
        z /= pow10_d(e);
    }
    if (z == JS_INF || z > 1.7976931348623157e308) z = 1.7976931348623157e308;
    if (z == 0) z = bitsd(1);
    BN D;
    D.n = 0;
    for (int q = 0; q < nd; q++) {
        bn_mul_small(&D, 10);
        bn_add_small(&D, (uint32_t)(dig[q] - '0'));
    }
    for (int iter = 0; iter < 200; iter++) {
        uint64_t f;
        int ee;
        decompose(z, &f, &ee);
        int c = cmp_exact(&D, dexp, 2 * f + 1, ee - 1);
        if (c > 0 || (c == 0 && (f & 1))) {
            if (z == 1.7976931348623157e308) return JS_INF;
            if (c == 0) return bitsd(dbits(z) + 1);
            z = bitsd(dbits(z) + 1);
            continue;
        }
        if (c == 0) return z;
        uint64_t lm;
        int lk;
        if (f == (1ull << 52) && ee > -1074) {
            lm = 4 * f - 1;
            lk = ee - 2;
        } else {
            lm = 2 * f - 1;
            lk = ee - 1;
        }
        c = cmp_exact(&D, dexp, lm, lk);
        if (c < 0 || (c == 0 && (f & 1))) {
            if (dbits(z) == 1) return 0.0;
            if (c == 0) return bitsd(dbits(z) - 1);
            z = bitsd(dbits(z) - 1);
            continue;
        }
        return z;
    }
    return z;
}

static bool is_ws(uint32_t c) { return uc_is_space(c); }

/* StringToNumber */
double str_to_number(js_rt *rt, JSString *s) {
    str_flat(rt, s);
    uint32_t n = s->len, a = 0, b = n;
    while (a < b && is_ws(str_at(s, a))) a++;
    while (b > a && is_ws(str_at(s, b - 1))) b--;
    if (a == b) return 0;
    uint32_t len = b - a;
    if (len > 100000) len = 100000;
    char stackbuf[256];
    char *buf = len < sizeof stackbuf ? stackbuf : (char *)malloc(len + 1);
    if (!buf) return JS_NAN;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t c = str_at(s, a + i);
        buf[i] = c < 128 ? (char)c : '?';
    }
    buf[len] = 0;
    double r = JS_NAN;
    const char *p = buf;
    size_t pl = len;
    if (pl > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' || p[1] == 'b' || p[1] == 'B')) {
        int radix = (p[1] == 'x' || p[1] == 'X') ? 16 : (p[1] == 'o' || p[1] == 'O') ? 8 : 2;
        size_t used;
        double v = parse_int_digits(p + 2, pl - 2, radix, &used);
        if (used == pl - 2) r = v;
        goto done;
    }
    {
        bool neg = false;
        if (p[0] == '+' || p[0] == '-') {
            neg = p[0] == '-';
            p++;
            pl--;
        }
        if (pl == 8 && memcmp(p, "Infinity", 8) == 0) {
            r = neg ? -JS_INF : JS_INF;
            goto done;
        }
        size_t used;
        double v = js_strtod(p, pl, &used);
        if (used == pl && pl > 0) r = neg ? -v : v;
    }
done:
    if (buf != stackbuf) free(buf);
    return r;
}

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* digits in radix; exact rounding for power-of-two radixes and radix 10 */
double parse_int_digits(const char *s, size_t n, int radix, size_t *used) {
    size_t i = 0;
    while (i < n && digit_val((unsigned char)s[i]) < radix) i++;
    *used = i;
    if (i == 0) return JS_NAN;
    if (radix == 10) {
        size_t u;
        return js_strtod(s, i, &u);
    }
    if ((radix & (radix - 1)) == 0) {
        int bits = radix == 2 ? 1 : radix == 4 ? 2 : radix == 8 ? 3 : radix == 16 ? 4 : 5;
        /* accumulate into 64-bit mantissa with sticky, then round to 53 bits */
        uint64_t m = 0;
        int exp = 0;
        bool sticky = false;
        size_t k = 0;
        while (k < i && digit_val((unsigned char)s[k]) == 0) k++;
        for (; k < i; k++) {
            int dv = digit_val((unsigned char)s[k]);
            for (int b = bits - 1; b >= 0; b--) {
                int bit = (dv >> b) & 1;
                if (m >> 63) {
                    if (bit) sticky = true;
                    exp++;
                } else
                    m = (m << 1) | (uint64_t)bit;
            }
        }
        if (m == 0) return 0;
        /* normalize to 64 bits */
        int lz = __builtin_clzll(m);
        if (exp == 0 && lz >= 11) return js_ldexp((double)m, 0);
        int shift = 11 - lz;
        uint64_t top;
        if (shift > 0) {
            uint64_t rem = m & ((1ull << shift) - 1);
            top = m >> shift;
            uint64_t half = 1ull << (shift - 1);
            if (rem > half || (rem == half && (sticky || (top & 1)))) top++;
            else if (rem == half && !sticky && !(top & 1)) {}
        } else {
            top = m;
            shift = 0;
        }
        return js_ldexp((double)top, shift + exp);
    }
    double v = 0;
    for (size_t k = 0; k < i; k++) v = v * radix + digit_val((unsigned char)s[k]);
    return v;
}

JSString *num_to_str(js_rt *rt, double d) {
    if (d >= 0 && d < 256 && d == (int)d && !(d == 0 && js_signbit(d))) {
        int i = (int)d;
        if (!rt->small_int_str[i]) {
            char b[8];
            int n = num_to_cstr(d, b);
            rt->small_int_str[i] = str_new8(rt, (const uint8_t *)b, (uint32_t)n);
            rt->small_int_str[i]->h.flags |= STR_PERM;
        }
        return rt->small_int_str[i];
    }
    char buf[40];
    int n = num_to_cstr(d, buf);
    return str_new8(rt, (const uint8_t *)buf, (uint32_t)n);
}
