/*
 * fb_boot_logo.c - DuncOS graphical boot splash (Thane kernel)
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

struct framebuffer {
    volatile uint32_t *addr;  /* start of framebuffer               */
    uint32_t width;           /* pixels                             */
    uint32_t height;          /* pixels                             */
    uint32_t pitch;           /* BYTES per row (may exceed width*4) */
};

/* Colours as 0x00RRGGBB (standard for 32-bpp BGRX/XRGB framebuffers). */
#define COL_BG_TOP     0x0A1428u   /* deep navy   */
#define COL_BG_BOTTOM  0x000000u   /* black       */
#define COL_GOLD       0xFFC83Du
#define COL_CYAN       0x4FD1E8u
#define COL_WHITE      0xF0F0F0u
#define COL_DIM        0x6B7280u
#define COL_BAR_EMPTY  0x1F2937u

static const char *crown[] = {
    "            .     '     .",
    "        .  /\\    /\\    /\\  .",
    "          /  \\  /  \\  /  \\",
    "         /    \\/    \\/    \\",
    "        |__________________|"
};

static const char *title[] = {
    "    ____                   ____  _____",
    "   / __ \\__  ______  _____/ __ \\/ ___/",
    "  / / / / / / / __ \\/ ___/ / / /\\__ \\",
    " / /_/ / /_/ / / / / /__/ /_/ /___/ /",
    "/_____/\\__,_/_/ /_/\\___/\\____//____/"
};

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

void boot_logo_show(const struct framebuffer *fb) {
    uint32_t s  = pick_scale(fb);
    uint32_t lh = FONT_H * s;                 /* one line height */

    /* total height of everything, so we can centre vertically */
    uint32_t total = (uint32_t)(ARRAY_LEN(crown) + ARRAY_LEN(title)) * lh
                   + 2 * lh        /* gaps after crown and title */
                   + 3 * lh        /* two text lines + gap       */
                   + 6 * s;        /* progress bar               */
    uint32_t y = total < fb->height ? (fb->height - total) / 2 : 0;

    draw_background(fb);

    y = draw_block(fb, crown, ARRAY_LEN(crown), y, s, COL_GOLD);
    y += lh;
    y = draw_block(fb, title, ARRAY_LEN(title), y, s, COL_CYAN);
    y += lh;

    draw_text_centered(fb, "~ Thane Kernel v0.1 ~", y, s, COL_WHITE);
    y += lh;
    draw_text_centered(fb, "Created by Bryan Duncan", y, s, COL_DIM);
    y += 2 * lh;

    g_bar_y = y;
    boot_logo_progress(fb, 0);
}

/* Progress bar under the logo. Call with 0-100 as the kernel boots. */
void boot_logo_progress(const struct framebuffer *fb, unsigned percent) {
    if (percent > 100) percent = 100;
    uint32_t s = pick_scale(fb);
    uint32_t w = fb->width / 3;
    uint32_t h = 6 * s;
    uint32_t x = (fb->width - w) / 2;
    uint32_t filled = w * percent / 100;

    fill_rect(fb, x, g_bar_y, w, h, COL_BAR_EMPTY);
    fill_rect(fb, x, g_bar_y, filled, h, COL_CYAN);
}
