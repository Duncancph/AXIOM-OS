/* Web: SVG pictures. An <svg> element (in the page, made by a script, or a .svg file parsed into a
 * little tree) is drawn into a pixel buffer at the size it is shown at: paths (lines, curves, arcs),
 * rect, circle, ellipse, line, polyline, polygon, groups with transforms, fills and strokes with
 * opacity, anti-aliased. Not done: gradients (their first colour is used), text, filters, masks,
 * <use>, and fill colours that come from style sheets rather than the element itself.
 * Compiled with floating point. */
#include "web.h"

/* ---- a little maths (no C library here) ---- */
static double f_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double f_abs(double x) { return x < 0 ? -x : x; }
#define PI 3.14159265358979323846
static double f_sin(double x) {
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    if (x > PI / 2) x = PI - x; else if (x < -PI / 2) x = -PI - x;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static double f_cos(double x) { return f_sin(x + PI / 2); }
static double f_atan(double x) {
    if (x < 0) return -f_atan(-x);
    if (x > 1) return PI / 2 - f_atan(1 / x);
    if (x > 0.4142135623730951) return PI / 4 + f_atan((x - 1) / (x + 1));
    double x2 = x * x;
    return x * (1 - x2 * (1.0 / 3 - x2 * (1.0 / 5 - x2 * (1.0 / 7 - x2 * (1.0 / 9 - x2 / 11)))));
}
static double f_atan2(double y, double x) {
    if (x > 0) return f_atan(y / x);
    if (x < 0) return f_atan(y / x) + (y >= 0 ? PI : -PI);
    return y > 0 ? PI / 2 : y < 0 ? -PI / 2 : 0;
}

/* ---- numbers in attributes ---- */
static bool sp_(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ','; }
static bool num(const char **pp, double *out) {
    const char *p = *pp;
    while (sp_(*p)) p++;
    bool neg = false, any = false;
    if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
    double v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = true; }
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f /= 10; any = true; } }
    if (!any) return false;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1; bool en = false; int e = 0;
        if (*q == '-') { en = true; q++; } else if (*q == '+') q++;
        if (*q >= '0' && *q <= '9') { while (*q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0'); if (e > 30) e = 30; while (e--) v = en ? v / 10 : v * 10; p = q; }
    }
    *out = neg ? -v : v; *pp = p;
    return true;
}
static double attr_num(const struct node *n, const char *name, double def) {
    const char *a = node_attr(n, name); double v;
    if (!a || !num(&a, &v)) return def;
    return v;
}

/* ---- transforms: x' = a x + c y + e, y' = b x + d y + f ---- */
struct mtx { double a, b, c, d, e, f; };
static struct mtx mul(struct mtx m, struct mtx n) {       /* apply n first, then m */
    return (struct mtx){ m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                         m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
}
static bool pre(const char *s, const char *w) { for (; *w; s++, w++) if (*s != *w) return false; return true; }
static struct mtx parse_transform(const char *t, struct mtx m) {
    while (t && *t) {
        while (sp_(*t)) t++;
        const char *o = t; while (*o && *o != '(') o++;
        if (!*o) break;
        const char *p = o + 1; double v[6]; int k = 0;
        while (k < 6 && num(&p, &v[k])) k++;
        struct mtx n = { 1, 0, 0, 1, 0, 0 };
        if (pre(t, "translate")) { n.e = k > 0 ? v[0] : 0; n.f = k > 1 ? v[1] : 0; }
        else if (pre(t, "scale")) { n.a = k > 0 ? v[0] : 1; n.d = k > 1 ? v[1] : n.a; }
        else if (pre(t, "rotate")) {
            double r = (k > 0 ? v[0] : 0) * PI / 180, cs = f_cos(r), sn = f_sin(r);
            n = (struct mtx){ cs, sn, -sn, cs, 0, 0 };
            if (k >= 3) n = mul(mul((struct mtx){ 1, 0, 0, 1, v[1], v[2] }, n), (struct mtx){ 1, 0, 0, 1, -v[1], -v[2] });
        }
        else if (pre(t, "matrix") && k == 6) n = (struct mtx){ v[0], v[1], v[2], v[3], v[4], v[5] };
        m = mul(m, n);
        while (*p && *p != ')') p++;
        t = *p ? p + 1 : p;
    }
    return m;
}

/* ---- the shape being built: line segments in device pixels ---- */
struct seg { float x0, y0, x1, y1; };
static struct seg *segs; static int nseg, capseg;
static uint32_t *pix; static int PW, PH;
static float *cov;
static void seg_add(double x0, double y0, double x1, double y1) {
    if (nseg >= capseg) {
        int nc = capseg ? capseg * 2 : 256;
        if (nc > 400000) return;
        struct seg *ns = realloc(segs, nc * sizeof *ns);
        if (!ns) return;
        segs = ns; capseg = nc;
    }
    segs[nseg++] = (struct seg){ (float)x0, (float)y0, (float)x1, (float)y1 };
}
/* fill what is in segs (nonzero or even-odd) with a colour, straight alpha, over pix */
static void fill_segs(uint32_t rgb, double alpha, bool evenodd) {
    if (!nseg || alpha <= 0) return;
    float miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < nseg; i++) { if (segs[i].y0 < miny) miny = segs[i].y0; if (segs[i].y1 < miny) miny = segs[i].y1; if (segs[i].y0 > maxy) maxy = segs[i].y0; if (segs[i].y1 > maxy) maxy = segs[i].y1; }
    int y0 = (int)miny - 1, y1 = (int)maxy + 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= PH) y1 = PH - 1;
    static struct { float x; int dir; } xs[512];
    for (int y = y0; y <= y1; y++) {
        bool any = false;
        for (int s = 0; s < 4; s++) {
            float sy = y + (s + 0.5f) / 4;
            int nx = 0;
            for (int i = 0; i < nseg && nx < 512; i++) {
                float ya = segs[i].y0, yb = segs[i].y1;
                if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy)) {
                    float x = segs[i].x0 + (sy - ya) * (segs[i].x1 - segs[i].x0) / (yb - ya);
                    int j = nx++;
                    while (j > 0 && xs[j - 1].x > x) { xs[j] = xs[j - 1]; j--; }
                    xs[j].x = x; xs[j].dir = yb > ya ? 1 : -1;
                }
            }
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += xs[i].dir;
                bool in = evenodd ? ((i + 1) & 1) : wind != 0;
                if (!in) continue;
                float a = xs[i].x, b = xs[i + 1].x;
                if (b <= 0 || a >= PW || b <= a) continue;
                if (a < 0) a = 0;
                if (b > PW) b = (float)PW;
                if (!any) { for (int x = 0; x < PW; x++) cov[x] = 0; any = true; }
                int ia = (int)a, ib = (int)b;
                if (ia == ib) cov[ia] += (b - a) / 4;
                else {
                    cov[ia] += (ia + 1 - a) / 4;
                    for (int x = ia + 1; x < ib; x++) cov[x] += 0.25f;
                    if (ib < PW) cov[ib] += (b - ib) / 4;
                }
            }
        }
        if (!any) continue;
        uint32_t *row = pix + (long)y * PW;
        uint32_t sr = rgb >> 16 & 255, sg = rgb >> 8 & 255, sb = rgb & 255;
        for (int x = 0; x < PW; x++) {
            float cvr = cov[x]; if (cvr <= 0.002f) continue;
            if (cvr > 1) cvr = 1;
            uint32_t sa = (uint32_t)(cvr * alpha * 255 + 0.5);
            if (!sa) continue;
            uint32_t d = row[x], da = d >> 24;
            uint32_t oa = sa + da * (255 - sa) / 255;
            if (!oa) continue;
            uint32_t r = (sr * sa + (d >> 16 & 255) * da * (255 - sa) / 255) / oa, g = (sg * sa + (d >> 8 & 255) * da * (255 - sa) / 255) / oa,
                     b = (sb * sa + (d & 255) * da * (255 - sa) / 255) / oa;
            row[x] = oa << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
}

/* ---- paths: flattened into points (several sub-paths), in user space, then transformed ---- */
struct pt { double x, y; };
static struct pt *pts; static int npt, cappt;
static int subs[2048], nsub; static bool closed[2048];     /* subs[i]: index of the first point of sub-path i */
static void pt_add(double x, double y) {
    if (npt >= cappt) {
        int nc = cappt ? cappt * 2 : 512;
        if (nc > 400000) return;
        struct pt *np = realloc(pts, nc * sizeof *np);
        if (!np) return;
        pts = np; cappt = nc;
    }
    pts[npt++] = (struct pt){ x, y };
}
static void sub_start(double x, double y) { if (nsub < 2048) { subs[nsub] = npt; closed[nsub] = false; nsub++; } pt_add(x, y); }
static void cubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
    for (int i = 1; i <= 12; i++) {
        double t = i / 12.0, u = 1 - t;
        pt_add(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}
static void arc_to(double x0, double y0, double rx, double ry, double rot, bool large, bool sweep, double x, double y) {
    if (rx == 0 || ry == 0 || (x0 == x && y0 == y)) { pt_add(x, y); return; }
    rx = f_abs(rx); ry = f_abs(ry);
    double ph = rot * PI / 180, cp = f_cos(ph), sp = f_sin(ph);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    double lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) { double s = f_sqrt(lam); rx *= s; ry *= s; }
    double nume = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double co = den > 0 && nume > 0 ? f_sqrt(nume / den) : 0;
    if (large == sweep) co = -co;
    double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    double a0 = f_atan2((y1 - cyp) / ry, (x1 - cxp) / rx), a1 = f_atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx);
    double da = a1 - a0;
    if (sweep && da < 0) da += 2 * PI; else if (!sweep && da > 0) da -= 2 * PI;
    int steps = (int)(f_abs(da) / (PI / 16)) + 1;
    for (int i = 1; i <= steps; i++) {
        double a = a0 + da * i / steps, ca = f_cos(a), sa = f_sin(a);
        pt_add(cx + cp * rx * ca - sp * ry * sa, cy + sp * rx * ca + cp * ry * sa);
    }
}
static void path_d(const char *d) {
    double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;  /* current, sub-path start, last control point */
    char cmd = 0, prev = 0;
    const char *p = d;
    for (int guard = 0; guard < 200000; guard++) {
        while (sp_(*p)) p++;
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
        else if (!cmd) break;
        bool rel = cmd >= 'a';
        char c = rel ? cmd - 32 : cmd;
        double v[7];
        if (c == 'Z') { if (nsub) closed[nsub - 1] = true; cx = sx; cy = sy; prev = c; cmd = 0; continue; }
        int need = c == 'H' || c == 'V' ? 1 : c == 'M' || c == 'L' || c == 'T' ? 2 : c == 'S' || c == 'Q' ? 4 : c == 'C' ? 6 : c == 'A' ? 7 : 0;
        if (!need) break;
        bool ok = true;
        for (int i = 0; i < need && ok; i++) {
            if (c == 'A' && (i == 3 || i == 4)) { while (sp_(*p)) p++; if (*p == '0' || *p == '1') v[i] = *p++ - '0'; else ok = false; }
            else ok = num(&p, &v[i]);
        }
        if (!ok) break;
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        if (c == 'M') { cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy; sub_start(cx, cy); cmd = rel ? 'l' : 'L'; }
        else {
            if (!nsub || closed[nsub - 1]) sub_start(cx, cy);
            if (c == 'L') { cx = v[0] + ox; cy = v[1] + oy; pt_add(cx, cy); }
            else if (c == 'H') { cx = v[0] + ox; pt_add(cx, cy); }
            else if (c == 'V') { cy = v[0] + oy; pt_add(cx, cy); }
            else if (c == 'C') { cubic(cx, cy, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy); lx = v[2] + ox; ly = v[3] + oy; cx = v[4] + ox; cy = v[5] + oy; }
            else if (c == 'S') {
                double x1 = prev == 'C' || prev == 'S' ? 2 * cx - lx : cx, y1 = prev == 'C' || prev == 'S' ? 2 * cy - ly : cy;
                cubic(cx, cy, x1, y1, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy); lx = v[0] + ox; ly = v[1] + oy; cx = v[2] + ox; cy = v[3] + oy;
            }
            else if (c == 'Q' || c == 'T') {
                double qx, qy, ex, ey;
                if (c == 'Q') { qx = v[0] + ox; qy = v[1] + oy; ex = v[2] + ox; ey = v[3] + oy; }
                else { qx = prev == 'Q' || prev == 'T' ? 2 * cx - lx : cx; qy = prev == 'Q' || prev == 'T' ? 2 * cy - ly : cy; ex = v[0] + ox; ey = v[1] + oy; }
                cubic(cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey);
                lx = qx; ly = qy; cx = ex; cy = ey;
            }
            else if (c == 'A') { arc_to(cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, v[5] + ox, v[6] + oy); cx = v[5] + ox; cy = v[6] + oy; }
        }
        prev = c;
    }
}
static void ellipse_pts(double cx, double cy, double rx, double ry) {
    sub_start(cx + rx, cy);
    for (int i = 1; i < 48; i++) { double a = 2 * PI * i / 48; pt_add(cx + rx * f_cos(a), cy + ry * f_sin(a)); }
    closed[nsub - 1] = true;
}

/* ---- paint ---- */
struct paint { uint32_t fill, stroke; bool has_fill, has_stroke, evenodd; double sw, op, fop, sop; uint32_t cur; };
static void color_attr(const char *v, int n, uint32_t cur, uint32_t *col, bool *has) {
    while (n && *v == ' ') { v++; n--; }
    while (n && v[n - 1] == ' ') n--;
    if (!n) return;
    if (n == 4 && pre(v, "none")) { *has = false; return; }
    if (n >= 12 && (pre(v, "currentColor") || pre(v, "currentcolor"))) { *col = cur; *has = true; return; }
    if (pre(v, "url(")) { *col = 0x808080; *has = true; return; }          /* gradients and patterns: grey */
    uint32_t c;
    if (css_parse_color(v, n, &c)) { if (!c) *has = false; else { *col = c & 0xFFFFFF; *has = true; } }
}
static void paint_prop(struct paint *p, const char *k, int kl, const char *v, int vl) {
    const char *q = v; double d;
    if (kl == 4 && pre(k, "fill")) color_attr(v, vl, p->cur, &p->fill, &p->has_fill);
    else if (kl == 6 && pre(k, "stroke")) color_attr(v, vl, p->cur, &p->stroke, &p->has_stroke);
    else if (kl == 12 && pre(k, "stroke-width")) { if (num(&q, &d)) p->sw = d; }
    else if (kl == 9 && pre(k, "fill-rule")) p->evenodd = vl >= 7 && pre(v, "evenodd");
    else if (kl == 7 && pre(k, "opacity")) { if (num(&q, &d)) p->op *= d < 0 ? 0 : d > 1 ? 1 : d; }
    else if (kl == 12 && pre(k, "fill-opacity")) { if (num(&q, &d)) p->fop = d < 0 ? 0 : d > 1 ? 1 : d; }
    else if (kl == 14 && pre(k, "stroke-opacity")) { if (num(&q, &d)) p->sop = d < 0 ? 0 : d > 1 ? 1 : d; }
    else if (kl == 5 && pre(k, "color")) { uint32_t c; if (css_parse_color(v, vl, &c) && c) p->cur = c & 0xFFFFFF; }
}
static struct paint node_paint(const struct node *n, struct paint p) {
    static const char *const names[] = { "color", "fill", "stroke", "stroke-width", "fill-rule", "opacity", "fill-opacity", "stroke-opacity" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) { const char *a = node_attr(n, names[i]); if (a) paint_prop(&p, names[i], strlen(names[i]), a, strlen(a)); }
    const char *s = node_attr(n, "style");
    while (s && *s) {                                     /* fill: red; stroke: none */
        while (*s == ' ' || *s == ';') s++;
        const char *k = s; while (*s && *s != ':' && *s != ';') s++;
        if (*s != ':') continue;
        int kl = (int)(s - k); while (kl && k[kl - 1] == ' ') kl--;
        s++; while (*s == ' ') s++;
        const char *v = s; while (*s && *s != ';') s++;
        paint_prop(&p, k, kl, v, (int)(s - v));
    }
    return p;
}

/* draw the points gathered (user space) through m */
static void emit(struct mtx m, const struct paint *p) {
    if (p->has_fill) {
        nseg = 0;
        for (int s = 0; s < nsub; s++) {
            int a = subs[s], b = s + 1 < nsub ? subs[s + 1] : npt;
            for (int i = a; i < b; i++) {
                int j = i + 1 < b ? i + 1 : a;                /* fills always close */
                seg_add(m.a * pts[i].x + m.c * pts[i].y + m.e, m.b * pts[i].x + m.d * pts[i].y + m.f, m.a * pts[j].x + m.c * pts[j].y + m.e, m.b * pts[j].x + m.d * pts[j].y + m.f);
            }
        }
        fill_segs(p->fill, p->op * p->fop, p->evenodd);
    }
    if (p->has_stroke && p->sw > 0) {
        double scale = f_sqrt(f_abs(m.a * m.d - m.b * m.c)), hw = p->sw * scale / 2;
        if (hw < 0.35) hw = 0.35;
        nseg = 0;
        for (int s = 0; s < nsub; s++) {
            int a = subs[s], b = s + 1 < nsub ? subs[s + 1] : npt, cnt = b - a;
            int last = closed[s] ? cnt : cnt - 1;
            for (int i = 0; i < last; i++) {
                struct pt A = pts[a + i], B = pts[a + (i + 1) % cnt];
                double ax = m.a * A.x + m.c * A.y + m.e, ay = m.b * A.x + m.d * A.y + m.f, bx = m.a * B.x + m.c * B.y + m.e, by = m.b * B.x + m.d * B.y + m.f;
                double dx = bx - ax, dy = by - ay, len = f_sqrt(dx * dx + dy * dy);
                if (len < 1e-6) continue;
                double nx = -dy / len * hw, ny = dx / len * hw;
                seg_add(ax + nx, ay + ny, bx + nx, by + ny); seg_add(bx + nx, by + ny, bx - nx, by - ny);
                seg_add(bx - nx, by - ny, ax - nx, ay - ny); seg_add(ax - nx, ay - ny, ax + nx, ay + ny);
                if (hw > 0.8) for (int k = 0; k < 8; k++) {         /* a round join at the far end */
                    double a0 = 2 * PI * k / 8, a1 = 2 * PI * (k + 1) / 8;
                    seg_add(bx + hw * f_cos(a1), by + hw * f_sin(a1), bx + hw * f_cos(a0), by + hw * f_sin(a0));   /* the same way round as the pieces */
                }
            }
        }
        fill_segs(p->stroke, p->op * p->sop, false);
    }
}

static bool is(const struct node *n, const char *name) { return n->name && !strcmp(n->name, name); }
static void draw_node(struct node *n, struct mtx m, struct paint p, int depth) {
    if (depth > 40 || n->type != N_ELEM) return;
    if (is(n, "defs") || is(n, "title") || is(n, "desc") || is(n, "style") || is(n, "clippath") || is(n, "clipPath") || is(n, "mask") || is(n, "symbol") ||
        is(n, "lineargradient") || is(n, "linearGradient") || is(n, "radialgradient") || is(n, "radialGradient") || is(n, "filter") || is(n, "metadata")) return;
    const char *disp = node_attr(n, "display"); if (disp && pre(disp, "none")) return;
    p = node_paint(n, p);
    const char *tr = node_attr(n, "transform");
    if (tr) m = parse_transform(tr, m);
    npt = 0; nsub = 0;
    if (is(n, "path")) { const char *d = node_attr(n, "d"); if (d) path_d(d); }
    else if (is(n, "rect")) {
        double x = attr_num(n, "x", 0), y = attr_num(n, "y", 0), w = attr_num(n, "width", 0), h = attr_num(n, "height", 0);
        double rx = attr_num(n, "rx", -1), ry = attr_num(n, "ry", -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (w > 0 && h > 0) {
            if (rx <= 0 || ry <= 0) { sub_start(x, y); pt_add(x + w, y); pt_add(x + w, y + h); pt_add(x, y + h); }
            else {
                sub_start(x + rx, y);
                static const int qx[4] = { 1, 1, 0, 0 }, qy[4] = { 0, 1, 1, 0 };
                for (int q = 0; q < 4; q++) {
                    double ccx = qx[q] ? x + w - rx : x + rx, ccy = qy[q] ? y + h - ry : y + ry;
                    for (int i = 0; i <= 6; i++) { double a = -PI / 2 + PI / 2 * q + PI / 2 * i / 6; pt_add(ccx + rx * f_cos(a), ccy + ry * f_sin(a)); }
                }
            }
            closed[nsub - 1] = true;
        }
    }
    else if (is(n, "circle")) { double r = attr_num(n, "r", 0); if (r > 0) ellipse_pts(attr_num(n, "cx", 0), attr_num(n, "cy", 0), r, r); }
    else if (is(n, "ellipse")) { double rx = attr_num(n, "rx", 0), ry = attr_num(n, "ry", 0); if (rx > 0 && ry > 0) ellipse_pts(attr_num(n, "cx", 0), attr_num(n, "cy", 0), rx, ry); }
    else if (is(n, "line")) { sub_start(attr_num(n, "x1", 0), attr_num(n, "y1", 0)); pt_add(attr_num(n, "x2", 0), attr_num(n, "y2", 0)); }
    else if (is(n, "polyline") || is(n, "polygon")) {
        const char *q = node_attr(n, "points"); double x, y; bool first = true;
        while (q && num(&q, &x) && num(&q, &y)) { if (first) { sub_start(x, y); first = false; } else pt_add(x, y); }
        if (nsub && is(n, "polygon")) closed[nsub - 1] = true;
    }
    if (npt) emit(m, &p);
    for (struct node *c = n->first; c; c = c->next) if (c->type == N_ELEM) draw_node(c, m, p, depth + 1);
}

/* the size an <svg> asks for (width/height, else its viewBox, else 300x150) */
static bool view_box(const struct node *svg, double *vb) {
    const char *a = node_attr(svg, "viewBox"); if (!a) a = node_attr(svg, "viewbox");
    if (!a) return false;
    for (int i = 0; i < 4; i++) if (!num(&a, &vb[i])) return false;
    return vb[2] > 0 && vb[3] > 0;
}
static bool px_attr(const struct node *svg, const char *name, double *out) {
    const char *a = node_attr(svg, name);
    if (!a || !num(&a, out)) return false;
    while (*a == ' ') a++;
    if (*a == '%') return false;
    if (pre(a, "em")) *out *= 16; else if (pre(a, "pt")) *out *= 4.0 / 3;
    return *out > 0;
}
void svg_size(const struct node *svg, int *w, int *h) {
    double vb[4], W, H; bool hv = view_box(svg, vb), hw = px_attr(svg, "width", &W), hh = px_attr(svg, "height", &H);
    if (hw && !hh) H = hv ? W * vb[3] / vb[2] : W;
    else if (hh && !hw) W = hv ? H * vb[2] / vb[3] : H;
    else if (!hw && !hh) { if (hv) { W = vb[2]; H = vb[3]; } else { W = 300; H = 150; } }
    if (W > 4096) W = 4096;
    if (H > 4096) H = 4096;
    *w = (int)(W + 0.5); *h = (int)(H + 0.5);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
/* draw svg into a new w x h buffer (transparent where nothing is drawn); cur: the text colour (currentColor) */
uint32_t *svg_render(struct node *svg, int w, int h, uint32_t cur) {
    if (w < 1 || h < 1 || (long)w * h > 16l * 1024 * 1024) return 0;
    uint32_t *out = calloc((size_t)w * h, 4);
    float *cv = malloc((size_t)w * sizeof *cv);
    if (!out || !cv) { free(out); free(cv); return 0; }
    pix = out; PW = w; PH = h; cov = cv;
    struct mtx m = { 1, 0, 0, 1, 0, 0 };
    double vb[4];
    if (view_box(svg, vb)) {                                /* fit the viewBox, centred (xMidYMid meet) */
        double sx = w / vb[2], sy = h / vb[3], s = sx < sy ? sx : sy;
        const char *par = node_attr(svg, "preserveAspectRatio");
        if (par && pre(par, "none")) m = (struct mtx){ sx, 0, 0, sy, -vb[0] * sx, -vb[1] * sy };
        else m = (struct mtx){ s, 0, 0, s, (w - vb[2] * s) / 2 - vb[0] * s, (h - vb[3] * s) / 2 - vb[1] * s };
    } else {
        int iw, ih; svg_size(svg, &iw, &ih);
        m.a = (double)w / iw; m.d = (double)h / ih;
    }
    struct paint p = { 0x000000, 0, true, false, false, 1, 1, 1, 1, cur & 0xFFFFFF };
    p = node_paint(svg, p);
    for (struct node *c = svg->first; c; c = c->next) if (c->type == N_ELEM) draw_node(c, m, p, 0);
    free(cv); cov = 0; pix = 0;
    return out;
}
