/*
 * fb_boot_logo.c - AXIOM graphical boot splash (Thane kernel)
 * Created by Bryan Duncan
 *
 * Draws to a linear 32-bpp framebuffer (e.g. from Multiboot2 / Limine / UEFI GOP).
 * Freestanding: no libc. Needs font8x16.h alongside it.
 *
 * Usage in kmain():
 *     struct framebuffer fb = { addr, width, height, pitch };   // from bootloader
 *     boot_logo_show(&fb);
 *     boot_logo_progress(&fb, 25);  ...  boot_logo_progress(&fb, 100);
 */

#include <stdint.h>
#include <stddef.h>
#include "font8x16.h"
#include "logo_data.h"

struct framebuffer {
    volatile uint32_t *addr;  /* start of framebuffer               */
    uint32_t width;           /* pixels                             */
    uint32_t height;          /* pixels                             */
    uint32_t pitch;           /* BYTES per row (may exceed width*4) */
};

/* Colours as 0x00RRGGBB (standard for 32-bpp BGRX/XRGB framebuffers). */
#define COL_BG_TOP     0x000000u   /* AXIOM: black ...  */
#define COL_BG_BOTTOM  0x000000u
#define COL_GOLD       0xF4F1DEu   /* ... and cream     */
#define COL_CYAN       0xF4F1DEu
#define COL_WHITE      0xF4F1DEu
#define COL_DIM        0x6E6B60u
#define COL_BAR_EMPTY  0x1C1C1Au



#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------- primitives ---------- */

static inline void put_pixel(const struct framebuffer *fb,
                             uint32_t x, uint32_t y, uint32_t c) {
    if (x >= fb->width || y >= fb->height) return;
    volatile uint32_t *row =
        (volatile uint32_t *)((volatile uint8_t *)fb->addr + (size_t)y * fb->pitch);
    row[x] = c;
}

static void fill_rect(const struct framebuffer *fb, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t c) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            put_pixel(fb, x + i, y + j, c);
}

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t t, uint32_t max) {
    uint32_t r = ((a >> 16 & 0xFF) * (max - t) + (b >> 16 & 0xFF) * t) / max;
    uint32_t g = ((a >> 8  & 0xFF) * (max - t) + (b >> 8  & 0xFF) * t) / max;
    uint32_t bl= ((a       & 0xFF) * (max - t) + (b       & 0xFF) * t) / max;
    return (r << 16) | (g << 8) | bl;
}

static void draw_background(const struct framebuffer *fb) {
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t c = lerp_color(COL_BG_TOP, COL_BG_BOTTOM, y, fb->height);
        for (uint32_t x = 0; x < fb->width; x++)
            put_pixel(fb, x, y, c);
    }
}

static size_t k_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* Draw one character, each font pixel becomes a scale x scale block. */
static void draw_char(const struct framebuffer *fb, char ch, uint32_t x,
                      uint32_t y, uint32_t scale, uint32_t c) {
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (uint32_t row = 0; row < FONT_H; row++)
        for (uint32_t col = 0; col < FONT_W; col++)
            if (g[row] & (0x80 >> col))
                fill_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
}

static void draw_text(const struct framebuffer *fb, const char *s, uint32_t x,
                      uint32_t y, uint32_t scale, uint32_t c) {
    for (size_t i = 0; s[i]; i++)
        draw_char(fb, s[i], x + (uint32_t)i * FONT_W * scale, y, scale, c);
}

static void draw_text_centered(const struct framebuffer *fb, const char *s,
                               uint32_t y, uint32_t scale, uint32_t c) {
    uint32_t w = (uint32_t)k_strlen(s) * FONT_W * scale;
    uint32_t x = w < fb->width ? (fb->width - w) / 2 : 0;
    draw_text(fb, s, x, y, scale, c);
}

/* Centre a multi-line block as a unit so the ASCII art keeps its shape. */
static uint32_t draw_block(const struct framebuffer *fb, const char **lines,
                           size_t n, uint32_t y, uint32_t scale, uint32_t c) {
    size_t widest = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = k_strlen(lines[i]);
        if (l > widest) widest = l;
    }
    uint32_t w = (uint32_t)widest * FONT_W * scale;
    uint32_t x = w < fb->width ? (fb->width - w) / 2 : 0;
    for (size_t i = 0; i < n; i++)
        draw_text(fb, lines[i], x, y + (uint32_t)i * FONT_H * scale, scale, c);
    return y + (uint32_t)n * FONT_H * scale;
}

/* Pick a scale so the logo looks right from 800x600 up to 4K. */
static uint32_t pick_scale(const struct framebuffer *fb) {
    if (fb->width >= 2560) return 3;
    if (fb->width >= 1024) return 2;
    return 1;
}

/* ---------- public API ---------- */

static uint32_t g_bar_y;   /* remembered for boot_logo_progress() */

void boot_logo_show(const struct framebuffer *fb);
void boot_logo_progress(const struct framebuffer *fb, unsigned percent);

/* the AXIOM logo, scaled (bilinear) to height h, in one colour, centred at y */
static void draw_logo(const struct framebuffer *fb, const struct logo_img *l, uint32_t y, uint32_t h, uint32_t col) {
    uint32_t w = (uint32_t)l->w * h / (uint32_t)l->h, x0 = (fb->width - w) / 2;
    for (uint32_t j = 0; j < h && y + j < fb->height; j++) {
        uint32_t fy = j * 256 * (uint32_t)(l->h - 1) / (h ? h - 1 : 1), sy = fy >> 8, ty = fy & 255;
        volatile uint32_t *row = (volatile uint32_t *)((volatile uint8_t *)fb->addr + (y + j) * fb->pitch);
        for (uint32_t i = 0; i < w; i++) {
            uint32_t fx = i * 256 * (uint32_t)(l->w - 1) / (w ? w - 1 : 1), sx = fx >> 8, tx = fx & 255;
            uint32_t sx1 = sx + 1 < (uint32_t)l->w ? sx + 1 : sx, sy1 = sy + 1 < (uint32_t)l->h ? sy + 1 : sy;
            uint32_t a00 = l->a[sy * l->w + sx], a01 = l->a[sy * l->w + sx1], a10 = l->a[sy1 * l->w + sx], a11 = l->a[sy1 * l->w + sx1];
            uint32_t a = ((a00 * (256 - tx) + a01 * tx) * (256 - ty) + (a10 * (256 - tx) + a11 * tx) * ty) >> 16;
            if (!a) continue;
            uint32_t d = row[x0 + i];
            uint32_t r = ((col >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
            uint32_t g = ((col >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
            uint32_t b = ((col & 255) * a + (d & 255) * (255 - a)) / 255;
            row[x0 + i] = r << 16 | g << 8 | b;
        }
    }
}

void boot_logo_show(const struct framebuffer *fb) {
    uint32_t s  = pick_scale(fb);
    uint32_t lh = FONT_H * s;
    uint32_t logo_h = fb->height / 12; if (logo_h < 40) logo_h = 40;      /* the AXIOM lockup, like the artwork */
    uint32_t total = logo_h + 3 * lh + 4 * s;
    uint32_t y = total < fb->height ? (fb->height - total) / 2 : 0;
    draw_background(fb);
    draw_logo(fb, logo_h > 64 ? &logo_splash : &logo_mid, y, logo_h, COL_WHITE);
    y += logo_h + 2 * lh;
    g_bar_y = y;
    boot_logo_progress(fb, 0);
    draw_text_centered(fb, "Thane kernel", y + lh, 1, COL_DIM);
}

/* Progress bar under the logo. Call with 0-100 as the kernel boots. */
void boot_logo_progress(const struct framebuffer *fb, unsigned percent) {
    if (percent > 100) percent = 100;
    uint32_t s = pick_scale(fb);
    uint32_t w = fb->width / 5;
    uint32_t h = 2 * s;
    uint32_t x = (fb->width - w) / 2;
    uint32_t filled = w * percent / 100;

    fill_rect(fb, x, g_bar_y, w, h, COL_BAR_EMPTY);
    fill_rect(fb, x, g_bar_y, filled, h, COL_CYAN);
    (void)s;
}
