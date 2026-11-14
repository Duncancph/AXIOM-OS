/*
 * gfx.h - AXIOM 2D drawing primitives for a 32-bpp linear framebuffer
 * Created by Bryan Duncan
 *
 * Freestanding (no libc). Colours are 0x00RRGGBB.
 * Include from exactly ONE .c file per primitive set, or keep as-is:
 * everything is static inline.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stddef.h>
#include "font8x16.h"

struct framebuffer {
    volatile uint32_t *addr;  /* start of framebuffer               */
    uint32_t width;           /* pixels                             */
    uint32_t height;          /* pixels                             */
    uint32_t pitch;           /* BYTES per row (may exceed width*4) */
};

static inline volatile uint32_t *gfx_row(const struct framebuffer *fb, uint32_t y) {
    return (volatile uint32_t *)((volatile uint8_t *)fb->addr + (size_t)y * fb->pitch);
}

static inline void put_pixel(const struct framebuffer *fb, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    gfx_row(fb, (uint32_t)y)[x] = c;
}

/* Mix colour c over existing pixel with alpha a (0-255). */
static inline uint32_t gfx_mix(uint32_t dst, uint32_t c, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = ((c >> 16 & 0xFF) * a + (dst >> 16 & 0xFF) * ia) / 255;
    uint32_t g = ((c >> 8  & 0xFF) * a + (dst >> 8  & 0xFF) * ia) / 255;
    uint32_t b = ((c       & 0xFF) * a + (dst       & 0xFF) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

static inline void blend_pixel(const struct framebuffer *fb, int x, int y,
                               uint32_t c, uint32_t a) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    volatile uint32_t *p = &gfx_row(fb, (uint32_t)y)[x];
    *p = (a >= 255) ? c : gfx_mix(*p, c, a);
}

static inline void fill_rect_a(const struct framebuffer *fb, int x, int y,
                               int w, int h, uint32_t c, uint32_t a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            blend_pixel(fb, x + i, y + j, c, a);
}

static inline void fill_rect(const struct framebuffer *fb, int x, int y,
                             int w, int h, uint32_t c) {
    fill_rect_a(fb, x, y, w, h, c, 255);
}

/* Is (px,py) inside a rounded rect of size w x h with radius r? */
static inline int in_round(int px, int py, int w, int h, int r) {
    int cx = px < r ? r : (px >= w - r ? w - r - 1 : px);
    int cy = py < r ? r : (py >= h - r ? h - r - 1 : py);
    int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

static inline void fill_round_rect_a(const struct framebuffer *fb, int x, int y,
                                     int w, int h, int r, uint32_t c, uint32_t a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_round(i, j, w, h, r))
                blend_pixel(fb, x + i, y + j, c, a);
}

/* 1-px border of a rounded rect. */
static inline void stroke_round_rect(const struct framebuffer *fb, int x, int y,
                                     int w, int h, int r, uint32_t c, uint32_t a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_round(i, j, w, h, r) &&
                !in_round(i - 1, j - 1, w - 2, h - 2, r > 1 ? r - 1 : 0))
                blend_pixel(fb, x + i, y + j, c, a);
}

static inline void fill_circle_a(const struct framebuffer *fb, int cx, int cy,
                                 int r, uint32_t c, uint32_t a) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                blend_pixel(fb, cx + dx, cy + dy, c, a);
}

static inline size_t k_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static inline void draw_char(const struct framebuffer *fb, char ch, int x, int y,
                             int scale, uint32_t c) {
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++)
        for (int col = 0; col < FONT_W; col++)
            if (g[row] & (0x80 >> col))
                fill_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
}

/* Returns the x just past the last character drawn. */
static inline int draw_text(const struct framebuffer *fb, const char *s, int x, int y,
                            int scale, uint32_t c) {
    for (size_t i = 0; s[i]; i++, x += FONT_W * scale)
        draw_char(fb, s[i], x, y, scale, c);
    return x;
}

static inline int text_width(const char *s, int scale) {
    return (int)k_strlen(s) * FONT_W * scale;
}

#endif
