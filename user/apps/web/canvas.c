/* <canvas> 2D drawing for Web's scripts (dom.c calls cv_op for each drawing call of a CanvasRenderingContext2D).
 * Each canvas keeps its pixels (straight-alpha ARGB, shown like a picture), the current path (already
 * transformed to pixels) and a stack of drawing states. Shapes are filled and stroked by the SVG rasteriser. */
#include "web.h"
#include "../../lib/img/img.h"

#define PI 3.14159265358979323846
static double f_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double f_abs(double x) { return x < 0 ? -x : x; }
static double f_sin(double x) {
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double f_cos(double x) { return f_sin(x + PI / 2); }

struct cvstate {
    double m[6];                       /* a b c d e f: x' = a x + c y + e, y' = b x + d y + f */
    uint32_t fill, stroke;             /* alpha << 24 | rgb */
    double lw, galpha;
    int font_px; bool bold;
    int align, baseline;               /* align: 0 start/left 1 center 2 end/right; baseline: 0 alphabetic 1 top 2 middle 3 bottom */
};
struct path {
    double *xy; int npt, cap;          /* points, in pixels */
    int sub[2048]; unsigned char cl[2048]; int nsub;
    bool open;                         /* the last sub-path can go on (lineTo) */
    bool pend; double pendX, pendY;    /* after closePath: where the next sub-path starts */
};
struct cv {
    uint32_t *px; int w, h;
    struct cvstate st, stack[32]; int nstack;
    struct path *p, path;              /* the path (p: the one being built; fillRect uses one of its own) */
};

static void reset_state(struct cvstate *s) {
    double id[6] = { 1, 0, 0, 1, 0, 0 };
    memcpy(s->m, id, sizeof id);
    s->fill = 0xFF000000; s->stroke = 0xFF000000; s->lw = 1; s->galpha = 1;
    s->font_px = 10; s->bold = false; s->align = 0; s->baseline = 0;
}
struct cv *cv_new(int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((long)w * h > 16l * 1024 * 1024) return 0;
    struct cv *c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->w = w; c->h = h;
    c->px = calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
    if (!c->px) { free(c); return 0; }
    reset_state(&c->st); c->p = &c->path;
    return c;
}
void cv_free(struct cv *c) { if (c) { free(c->px); free(c->path.xy); free(c); } }
uint32_t *cv_pixels(struct cv *c, int *w, int *h) { *w = c->w; *h = c->h; return c->px; }
void cv_resize(struct cv *c, int w, int h) {             /* setting width/height clears the canvas and its state */
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((long)w * h > 16l * 1024 * 1024) return;
    uint32_t *p = calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
    if (!p) return;
    free(c->px); c->px = p; c->w = w; c->h = h;
    reset_state(&c->st); c->nstack = 0; c->p->npt = 0; c->p->nsub = 0; c->p->open = c->p->pend = false;
}

/* ---- the path ---- */
static void tx(struct cv *c, double x, double y, double *ox, double *oy) {
    const double *m = c->st.m;
    *ox = m[0] * x + m[2] * y + m[4]; *oy = m[1] * x + m[3] * y + m[5];
}
static void addp(struct cv *c, double X, double Y) {      /* a point already in pixels */
    struct path *p = c->p;
    if (p->npt >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        if (nc > 400000) return;
        double *n = realloc(p->xy, nc * 2 * sizeof *n);
        if (!n) return;
        p->xy = n; p->cap = nc;
    }
    p->xy[p->npt * 2] = X; p->xy[p->npt * 2 + 1] = Y; p->npt++;
}
static void move_px(struct cv *c, double X, double Y) {
    struct path *p = c->p;
    if (p->nsub >= 2048) return;
    p->sub[p->nsub] = p->npt; p->cl[p->nsub] = 0; p->nsub++;
    addp(c, X, Y); p->open = true; p->pend = false;
}
static void line_px(struct cv *c, double X, double Y) {
    struct path *p = c->p;
    if (!p->open) { if (p->pend) move_px(c, p->pendX, p->pendY); else { move_px(c, X, Y); return; } }
    addp(c, X, Y);
}
static bool cur_user(struct cv *c, double *x, double *y) {   /* the current point, back in user space (for curves) */
    struct path *p = c->p; double X, Y;
    if (p->open && p->npt) { X = p->xy[p->npt * 2 - 2]; Y = p->xy[p->npt * 2 - 1]; }
    else if (p->pend) { X = p->pendX; Y = p->pendY; }
    else return false;
    const double *m = c->st.m; double det = m[0] * m[3] - m[1] * m[2];
    if (f_abs(det) < 1e-12) { *x = X; *y = Y; return true; }
    X -= m[4]; Y -= m[5];
    *x = (m[3] * X - m[2] * Y) / det; *y = (-m[1] * X + m[0] * Y) / det;
    return true;
}
static void line_to(struct cv *c, double x, double y) { double X, Y; tx(c, x, y, &X, &Y); line_px(c, X, Y); }
static void move_to(struct cv *c, double x, double y) { double X, Y; tx(c, x, y, &X, &Y); move_px(c, X, Y); }
static void close_path(struct cv *c) {
    struct path *p = c->p;
    if (!p->nsub || !p->open) return;
    p->cl[p->nsub - 1] = 1;
    int f = p->sub[p->nsub - 1];
    p->open = false; p->pend = true; p->pendX = p->xy[f * 2]; p->pendY = p->xy[f * 2 + 1];
}
static void arc(struct cv *c, double cx, double cy, double rx, double ry, double rot, double a0, double a1, bool ccw) {
    double da = a1 - a0;
    if (!ccw) { if (da >= 2 * PI) da = 2 * PI; else { while (da < 0) da += 2 * PI; } }
    else { if (-da >= 2 * PI) da = -2 * PI; else { while (da > 0) da -= 2 * PI; } }
    double r = rx > ry ? rx : ry;
    int steps = (int)(f_abs(da) * (r < 4 ? 4 : r > 200 ? 200 : r) / 4) + 2;
    if (steps > 360) steps = 360;
    double cr = f_cos(rot), sr = f_sin(rot);
    for (int i = 0; i <= steps; i++) {
        double a = a0 + da * i / steps, ex = rx * f_cos(a), ey = ry * f_sin(a);
        line_to(c, cx + ex * cr - ey * sr, cy + ex * sr + ey * cr);
    }
}
static void cubic(struct cv *c, double x1, double y1, double x2, double y2, double x3, double y3) {
    double x0, y0; if (!cur_user(c, &x0, &y0)) { move_to(c, x1, y1); x0 = x1; y0 = y1; }
    for (int i = 1; i <= 16; i++) {
        double t = i / 16.0, u = 1 - t;
        line_to(c, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}
static void quad(struct cv *c, double x1, double y1, double x2, double y2) {
    double x0, y0; if (!cur_user(c, &x0, &y0)) { move_to(c, x1, y1); x0 = x1; y0 = y1; }
    for (int i = 1; i <= 12; i++) {
        double t = i / 12.0, u = 1 - t;
        line_to(c, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
    }
}
static void rect_path(struct cv *c, double x, double y, double w, double h) {
    move_to(c, x, y); line_to(c, x + w, y); line_to(c, x + w, y + h); line_to(c, x, y + h); close_path(c);
}
static double lw_px(struct cv *c) { const double *m = c->st.m; return c->st.lw * f_sqrt(f_abs(m[0] * m[3] - m[1] * m[2])); }
static void paint(struct cv *c, uint32_t col, int mode) {     /* mode 0 nonzero fill, 1 even-odd fill, 2 stroke */
    double a = (col >> 24) / 255.0 * c->st.galpha;
    struct path *p = c->p;
    if (a <= 0 || !p->nsub) return;
    svg_raster(c->px, c->w, c->h, p->xy, p->npt, p->sub, p->cl, p->nsub, col & 0xFFFFFF, a, mode, lw_px(c));
}
static void fill_rect(struct cv *c, double x, double y, double w, double h, uint32_t col, int mode) {   /* with a path of its own */
    struct path *tmp = calloc(1, sizeof *tmp), *old = c->p;
    if (!tmp) return;
    c->p = tmp;
    rect_path(c, x, y, w, h);
    paint(c, col, mode);
    c->p = old; free(tmp->xy); free(tmp);
}
static void clear_rect(struct cv *c, double x, double y, double w, double h) {
    const double *m = c->st.m;
    if (f_abs(m[1]) > 1e-9 || f_abs(m[2]) > 1e-9) { fill_rect(c, x, y, w, h, 0, 0); return; }
    double X0, Y0, X1, Y1; tx(c, x, y, &X0, &Y0); tx(c, x + w, y + h, &X1, &Y1);
    if (X1 < X0) { double t = X0; X0 = X1; X1 = t; }
    if (Y1 < Y0) { double t = Y0; Y0 = Y1; Y1 = t; }
    int x0 = X0 < 0 ? 0 : (int)(X0 + 0.5), y0 = Y0 < 0 ? 0 : (int)(Y0 + 0.5), x1 = X1 > c->w ? c->w : (int)(X1 + 0.5), y1 = Y1 > c->h ? c->h : (int)(Y1 + 0.5);
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) c->px[(long)j * c->w + i] = 0;
}
static void blend(uint32_t *d, uint32_t rgb, uint32_t sa) {   /* source-over, straight alpha */
    if (!sa) return;
    uint32_t p = *d, da = p >> 24, oa = sa + da * (255 - sa) / 255;
    if (!oa) return;
    uint32_t r = ((rgb >> 16 & 255) * sa + (p >> 16 & 255) * da * (255 - sa) / 255) / oa;
    uint32_t g = ((rgb >> 8 & 255) * sa + (p >> 8 & 255) * da * (255 - sa) / 255) / oa;
    uint32_t b = ((rgb & 255) * sa + (p & 255) * da * (255 - sa) / 255) / oa;
    *d = oa << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
static void text(struct cv *c, const char *s, double x, double y, bool stroke) {
    if (!s || !s[0]) return;
    uint32_t col = stroke ? c->st.stroke : c->st.fill;
    double a = (col >> 24) / 255.0 * c->st.galpha;
    if (a <= 0) return;
    const double *m = c->st.m;
    double scale = f_sqrt(f_abs(m[0] * m[3] - m[1] * m[2]));
    int px = (int)(c->st.font_px * scale + 0.5);
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    int tw = web_text_px(px, c->st.bold, s), th = px * 3 / 2 + 4;
    if (tw <= 0 || (long)tw * th > 4l * 1024 * 1024) return;
    uint8_t *cov = calloc((size_t)tw * th, 1);
    if (!cov) return;
    int asc = web_text_coverage(cov, tw, th, px, c->st.bold, s);   /* glyph coverage; returns the baseline's row */
    double X, Y; tx(c, x, y, &X, &Y);
    if (c->st.align == 1) X -= tw / 2.0; else if (c->st.align == 2) X -= tw;
    if (c->st.baseline == 1) Y += px * 0.8; else if (c->st.baseline == 2) Y += px * 0.3; else if (c->st.baseline == 3) Y -= px * 0.2;
    int ox = (int)(X + 0.5), oy = (int)(Y + 0.5) - asc;
    uint32_t A = (uint32_t)(a * 255 + 0.5);
    for (int j = 0; j < th; j++) {
        int yy = oy + j; if (yy < 0 || yy >= c->h) continue;
        for (int i = 0; i < tw; i++) {
            int xx = ox + i; if (xx < 0 || xx >= c->w) continue;
            uint32_t cv = cov[(long)j * tw + i];
            if (cv) blend(&c->px[(long)yy * c->w + xx], col & 0xFFFFFF, cv * A / 255);
        }
    }
    free(cov);
}
/* drawImage: src w x h pixels (sx..: the part of it) into dx, dy, dw, dh (scaled, nearest pixel; no rotation) */
void cv_draw_image(struct cv *c, const uint32_t *src, int sw_, int sh_, bool opaque, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh) {
    if (!src || sw <= 0 || sh <= 0 || dw == 0 || dh == 0) return;
    double X0, Y0, X1, Y1; tx(c, dx, dy, &X0, &Y0); tx(c, dx + dw, dy + dh, &X1, &Y1);
    double fx = (X1 - X0), fy = (Y1 - Y0);
    if (f_abs(fx) < 0.5 || f_abs(fy) < 0.5) return;
    int x0 = (int)(X0 < X1 ? X0 : X1), x1 = (int)(X0 < X1 ? X1 : X0) + 1, y0 = (int)(Y0 < Y1 ? Y0 : Y1), y1 = (int)(Y0 < Y1 ? Y1 : Y0) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    uint32_t ga = (uint32_t)(c->st.galpha * 255 + 0.5);
    for (int j = y0; j < y1; j++) {
        double v = (j + 0.5 - Y0) / fy; if (v < 0 || v >= 1) continue;
        int syy = (int)(sy + v * sh); if (syy < 0 || syy >= sh_) continue;
        for (int i = x0; i < x1; i++) {
            double u = (i + 0.5 - X0) / fx; if (u < 0 || u >= 1) continue;
            int sxx = (int)(sx + u * sw); if (sxx < 0 || sxx >= sw_) continue;
            uint32_t p = src[(long)syy * sw_ + sxx], al = opaque ? 255 : p >> 24;
            blend(&c->px[(long)j * c->w + i], p & 0xFFFFFF, al * ga / 255);
        }
    }
}
/* getImageData: RGBA bytes (malloc) */
uint8_t *cv_get_rgba(struct cv *c, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || (long)w * h > 16l * 1024 * 1024) return 0;
    uint8_t *o = calloc((size_t)w * h, 4);
    if (!o) return 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int X = x + i, Y = y + j; if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
        uint32_t p = c->px[(long)Y * c->w + X]; uint8_t *q = o + ((long)j * w + i) * 4;
        q[0] = p >> 16; q[1] = p >> 8; q[2] = p; q[3] = p >> 24;
    }
    return o;
}
void cv_put_rgba(struct cv *c, const uint8_t *b, long n, int w, int h, int x, int y) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        long k = ((long)j * w + i) * 4; if (k + 3 >= n) return;
        int X = x + i, Y = y + j; if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
        c->px[(long)Y * c->w + X] = (uint32_t)b[k + 3] << 24 | b[k] << 16 | b[k + 1] << 8 | b[k + 2];
    }
}
/* toDataURL: "data:image/png;base64,..." (malloc) */
char *cv_data_url(struct cv *c) {
    int w = c->w ? c->w : 1, h = c->h ? c->h : 1;
    size_t cap = png_encode_bound(w, h);
    uint8_t *png = malloc(cap);
    if (!png) return 0;
    long n = png_encode(c->px, w, h, 0, png, cap);
    if (n < 0) { free(png); return 0; }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(22 + (n + 2) / 3 * 4 + 1);
    if (!o) { free(png); return 0; }
    memcpy(o, "data:image/png;base64,", 22);
    long k = 22;
    for (long i = 0; i < n; i += 3) {
        uint32_t v = png[i] << 16 | (i + 1 < n ? png[i + 1] << 8 : 0) | (i + 2 < n ? png[i + 2] : 0);
        o[k++] = b64[v >> 18 & 63]; o[k++] = b64[v >> 12 & 63];
        o[k++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? b64[v & 63] : '=';
    }
    o[k] = 0; free(png);
    return o;
}

static uint32_t colour(const char *s, uint32_t old) {
    uint32_t v; if (s && css_parse_color(s, strlen(s), &v)) return v;
    return old;
}
static bool eq(const char *a, const char *b) { return !strcmp(a, b); }
/* one drawing call: op is the method or property name, a[] its numbers, s its text (a colour, font, string...).
 * Returns a measurement where there is one (measureText's width). */
double cv_op(struct cv *c, const char *op, const double *a, int na, const char *s) {
    double z[8] = { 0 };
    for (int i = 0; i < na && i < 8; i++) z[i] = a[i] == a[i] ? a[i] : 0;   /* NaN: 0 */
    struct cvstate *st = &c->st;
    if (eq(op, "fillStyle")) st->fill = colour(s, st->fill);
    else if (eq(op, "strokeStyle")) st->stroke = colour(s, st->stroke);
    else if (eq(op, "lineWidth")) { if (z[0] > 0) st->lw = z[0]; }
    else if (eq(op, "globalAlpha")) { if (z[0] >= 0 && z[0] <= 1) st->galpha = z[0]; }
    else if (eq(op, "font")) { if (z[0] > 0) st->font_px = (int)(z[0] + 0.5); st->bold = z[1] >= 600; }
    else if (eq(op, "textAlign")) st->align = s && (eq(s, "center")) ? 1 : s && (eq(s, "right") || eq(s, "end")) ? 2 : 0;
    else if (eq(op, "textBaseline")) st->baseline = !s ? 0 : eq(s, "top") || eq(s, "hanging") ? 1 : eq(s, "middle") ? 2 : eq(s, "bottom") || eq(s, "ideographic") ? 3 : 0;
    else if (eq(op, "save")) { if (c->nstack < 32) c->stack[c->nstack++] = *st; }
    else if (eq(op, "restore")) { if (c->nstack) *st = c->stack[--c->nstack]; }
    else if (eq(op, "reset")) { reset_state(st); c->nstack = 0; c->p->npt = c->p->nsub = 0; c->p->open = c->p->pend = false; clear_rect(c, 0, 0, c->w, c->h); }
    else if (eq(op, "setTransform") || eq(op, "transform")) {
        double n[6] = { z[0], z[1], z[2], z[3], z[4], z[5] };
        if (eq(op, "setTransform")) { if (na < 6) { n[0] = 1; n[1] = 0; n[2] = 0; n[3] = 1; n[4] = 0; n[5] = 0; } memcpy(st->m, n, sizeof n); }
        else {
            double *m = st->m, r[6];
            r[0] = m[0] * n[0] + m[2] * n[1]; r[1] = m[1] * n[0] + m[3] * n[1];
            r[2] = m[0] * n[2] + m[2] * n[3]; r[3] = m[1] * n[2] + m[3] * n[3];
            r[4] = m[0] * n[4] + m[2] * n[5] + m[4]; r[5] = m[1] * n[4] + m[3] * n[5] + m[5];
            memcpy(m, r, sizeof r);
        }
    }
    else if (eq(op, "translate")) { double n[2] = { z[0], z[1] }; double *m = st->m; m[4] += m[0] * n[0] + m[2] * n[1]; m[5] += m[1] * n[0] + m[3] * n[1]; }
    else if (eq(op, "scale")) { double *m = st->m; m[0] *= z[0]; m[1] *= z[0]; m[2] *= z[1]; m[3] *= z[1]; }
    else if (eq(op, "rotate")) {
        double cs = f_cos(z[0]), sn = f_sin(z[0]), *m = st->m, r[4];
        r[0] = m[0] * cs + m[2] * sn; r[1] = m[1] * cs + m[3] * sn; r[2] = -m[0] * sn + m[2] * cs; r[3] = -m[1] * sn + m[3] * cs;
        m[0] = r[0]; m[1] = r[1]; m[2] = r[2]; m[3] = r[3];
    }
    else if (eq(op, "beginPath")) { c->p->npt = 0; c->p->nsub = 0; c->p->open = c->p->pend = false; }
    else if (eq(op, "moveTo")) move_to(c, z[0], z[1]);
    else if (eq(op, "lineTo")) line_to(c, z[0], z[1]);
    else if (eq(op, "closePath")) close_path(c);
    else if (eq(op, "rect")) rect_path(c, z[0], z[1], z[2], z[3]);
    else if (eq(op, "arc")) arc(c, z[0], z[1], z[2], z[2], 0, z[3], z[4], z[5] != 0);
    else if (eq(op, "ellipse")) arc(c, z[0], z[1], z[2], z[3], z[4], z[5], z[6], z[7] != 0);
    else if (eq(op, "bezierCurveTo")) cubic(c, z[0], z[1], z[2], z[3], z[4], z[5]);
    else if (eq(op, "quadraticCurveTo")) quad(c, z[0], z[1], z[2], z[3]);
    else if (eq(op, "arcTo")) line_to(c, z[0], z[1]);
    else if (eq(op, "roundRect")) rect_path(c, z[0], z[1], z[2], z[3]);
    else if (eq(op, "fill")) paint(c, st->fill, s && eq(s, "evenodd") ? 1 : 0);
    else if (eq(op, "stroke")) paint(c, st->stroke, 2);
    else if (eq(op, "fillRect")) fill_rect(c, z[0], z[1], z[2], z[3], st->fill, 0);
    else if (eq(op, "strokeRect")) fill_rect(c, z[0], z[1], z[2], z[3], st->stroke, 2);
    else if (eq(op, "clearRect")) clear_rect(c, z[0], z[1], z[2], z[3]);
    else if (eq(op, "fillText")) text(c, s, z[0], z[1], false);
    else if (eq(op, "strokeText")) text(c, s, z[0], z[1], true);
    else if (eq(op, "measureText")) {
        if (!s) return 0;
        return web_text_px(st->font_px < 6 ? 6 : st->font_px > 96 ? 96 : st->font_px, st->bold, s) * (st->font_px < 6 || st->font_px > 96 ? st->font_px / (st->font_px < 6 ? 6.0 : 96.0) : 1);
    }
    return 0;
}
