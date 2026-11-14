#include "gfx.h"
typedef int bool;
#include "../src/font8x16.h"
#include "uifont_data.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void gfx_init(struct canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    gfx_noclip(c);
}
void gfx_noclip(struct canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
void gfx_clip(struct canvas *c, int x, int y, int w, int h) {
    c->cx0 = imax(0, x); c->cy0 = imax(0, y);
    c->cx1 = imin(c->w, x + w); c->cy1 = imin(c->h, y + h);
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

uint32_t gfx_lerp(uint32_t a, uint32_t b, int t, int max) {
    if (max <= 0) return a;
    int r = ((a >> 16 & 255) * (max - t) + (b >> 16 & 255) * t) / max;
    int g = ((a >> 8 & 255) * (max - t) + (b >> 8 & 255) * t) / max;
    int bl = ((a & 255) * (max - t) + (b & 255) * t) / max;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

int gfx_isqrt(int v) {
    if (v <= 0) return 0;
    unsigned r = 0, b = 1u << 30, n = (unsigned)v;
    while (b > n) b >>= 2;
    while (b) { if (n >= r + b) { n -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (int)r;
}

#define CLIPRECT int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0), x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1)

void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t col) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) { uint32_t *p = c->px + j * c->stride; for (int i = x0; i < x1; i++) p[i] = col; }
}

void gfx_blend(struct canvas *c, int x, int y, int w, int h, uint32_t col, int a) {
    if (a >= 255) { gfx_fill(c, x, y, w, h, col); return; }
    if (a <= 0) return;
    CLIPRECT;
    for (int j = y0; j < y1; j++) { uint32_t *p = c->px + j * c->stride; for (int i = x0; i < x1; i++) p[i] = gfx_mix(p[i], col, a); }
}

void gfx_vgrad(struct canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        uint32_t col = gfx_lerp(top, bot, j - y, h > 1 ? h - 1 : 1);
        uint32_t *p = c->px + j * c->stride;
        for (int i = x0; i < x1; i++) p[i] = col;
    }
}

/* Coverage (0..255) of pixel (i,j) for a rounded rectangle; fast path inside. */
int gfx_rr_cov(int i, int j, int x, int y, int w, int h, int r) {
    int dx = 0, dy = 0;
    if (i < x + r) dx = x + r - i; else if (i >= x + w - r) dx = i - (x + w - r - 1);
    if (j < y + r) dy = y + r - j; else if (j >= y + h - r) dy = j - (y + h - r - 1);
    if (!dx || !dy) return 255;
    /* distance from corner centre, 4x fixed point, sampled at pixel centre */
    int fx = dx * 4 - 2, fy = dy * 4 - 2;
    int d = gfx_isqrt(fx * fx + fy * fy);
    int cov = (r * 4 + 2 - d) * 64;
    return cov < 0 ? 0 : cov > 255 ? 255 : cov;
}

static void rrect_impl(struct canvas *c, int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int alpha) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        uint32_t col = top == bot ? top : gfx_lerp(top, bot, j - y, h > 1 ? h - 1 : 1);
        uint32_t *p = c->px + j * c->stride;
        int edge = (j < y + r || j >= y + h - r);
        for (int i = x0; i < x1; i++) {
            int a = alpha;
            if (edge || i < x + r || i >= x + w - r) {
                int cov = gfx_rr_cov(i, j, x, y, w, h, r);
                if (!cov) continue;
                a = a * cov / 255;
            }
            p[i] = a >= 255 ? col : gfx_mix(p[i], col, a);
        }
    }
}
void gfx_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int alpha) { rrect_impl(c, x, y, w, h, r, col, col, alpha); }
void gfx_rrect_vgrad(struct canvas *c, int x, int y, int w, int h, int r, uint32_t t, uint32_t b) { rrect_impl(c, x, y, w, h, r, t, b, 255); }
void gfx_rrect_border(struct canvas *c, int x, int y, int w, int h, int r, uint32_t border, uint32_t fill) {
    rrect_impl(c, x, y, w, h, r, border, border, 255);
    rrect_impl(c, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill, fill, 255);
}

void gfx_circle(struct canvas *c, int cx, int cy, int r, uint32_t col, int alpha) {
    rrect_impl(c, cx - r, cy - r, 2 * r, 2 * r, r, col, col, alpha);
}

void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, uint32_t col, int thick) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, t0 = thick / 2;
    for (;;) {
        gfx_fill(c, x0 - t0, y0 - t0, thick, thick, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_blit(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int ss) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        const uint32_t *s = src + (j - y) * ss + (x0 - x);
        uint32_t *p = c->px + j * c->stride + x0;
        for (int i = x0; i < x1; i++) *p++ = *s++;
    }
}

int gfx_text(struct canvas *c, const struct font *f, int x, int base, uint32_t col, const char *s) {
    int sx = x;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const struct glyph *g = &f->g[ch - 32];
        int gx = x + g->xoff, gy = base + g->yoff;
        if (gx + g->w > c->cx0 && gx < c->cx1 && gy + g->h > c->cy0 && gy < c->cy1) {
            const uint8_t *b = f->bits + g->off;
            for (int j = 0; j < g->h; j++) {
                int py = gy + j;
                if (py < c->cy0 || py >= c->cy1) continue;
                uint32_t *p = c->px + py * c->stride;
                for (int i = 0; i < g->w; i++) {
                    int px = gx + i, a = b[j * g->w + i];
                    if (!a || px < c->cx0 || px >= c->cx1) continue;
                    p[px] = a == 255 ? col : gfx_mix(p[px], col, a);
                }
            }
        }
        x += g->adv;
    }
    return x - sx;
}

int gfx_text_width(const struct font *f, const char *s) {
    int w = 0;
    for (; *s; s++) { unsigned ch = (unsigned char)*s; if (ch < 32 || ch > 126) ch = '?'; w += f->g[ch - 32].adv; }
    return w;
}

void gfx_text_center(struct canvas *c, const struct font *f, int x, int w, int base, uint32_t col, const char *s) {
    gfx_text(c, f, x + (w - gfx_text_width(f, s)) / 2, base, col, s);
}

void gfx_mono(struct canvas *c, int x, int y, char ch, uint32_t fg) {
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < c->cy0 || py >= c->cy1 || !g[r]) continue;
        uint32_t *p = c->px + py * c->stride;
        for (int b = 0; b < 8; b++)
            if ((g[r] & (0x80 >> b)) && x + b >= c->cx0 && x + b < c->cx1) p[x + b] = fg;
    }
}
void gfx_mono_text(struct canvas *c, int x, int y, const char *s, uint32_t fg) {
    for (; *s; s++, x += 8) gfx_mono(c, x, y, *s, fg);
}

/* Blit, masking pixels by the rounded rectangle (rx,ry,rw,rh,r) (for window corners). */
void gfx_blit_rr(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int ss,
                 int rx, int ry, int rw, int rh, int r) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        const uint32_t *s = src + (j - y) * ss;
        uint32_t *p = c->px + j * c->stride;
        bool corner_row = j < ry + r || j >= ry + rh - r;
        for (int i = x0; i < x1; i++) {
            if (corner_row && (i < rx + r || i >= rx + rw - r)) {
                int cov = gfx_rr_cov(i, j, rx, ry, rw, rh, r);
                if (cov) p[i] = cov == 255 ? s[i - x] : gfx_mix(p[i], s[i - x], cov);
            } else p[i] = s[i - x];
        }
    }
}

/* 1-pixel anti-aliased outline of a rounded rectangle. */
void gfx_rrect_outline(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int alpha) {
    CLIPRECT;
    for (int j = y0; j < y1; j++) {
        uint32_t *p = c->px + j * c->stride;
        bool edge_row = j <= y + r || j >= y + h - 1 - r;
        for (int i = x0; i < x1; i++) {
            if (!edge_row && i != x && i != x + w - 1) { if (i < x + w - 1) i = x + w - 2; continue; }
            int outer = gfx_rr_cov(i, j, x, y, w, h, r);
            int inner = (i > x && i < x + w - 1 && j > y && j < y + h - 1) ? gfx_rr_cov(i, j, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0) : 0;
            int a = (outer - inner) * alpha / 255;
            if (a > 0) p[i] = gfx_mix(p[i], col, a);
        }
    }
}
