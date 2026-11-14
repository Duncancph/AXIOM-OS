/* Software 2D graphics shared by the Thane kernel compositor and DuncOS apps.
 * Pixels are 0x00RRGGBB. */
#pragma once
#include <stdint.h>

struct glyph { int8_t xoff, yoff; uint8_t w, h, adv; uint32_t off; };
struct font { int size, ascent, descent; const struct glyph *g; const uint8_t *bits; };
extern const struct font font_ui, font_ui_bold, font_title, font_big;

struct canvas {
    uint32_t *px;
    int w, h, stride;          /* stride in pixels */
    int cx0, cy0, cx1, cy1;    /* clip rectangle (exclusive end) */
};

void gfx_init(struct canvas *c, uint32_t *px, int w, int h, int stride);
void gfx_clip(struct canvas *c, int x, int y, int w, int h);   /* clip = bounds ∩ rect */
void gfx_noclip(struct canvas *c);

static inline uint32_t gfx_mix(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a) + 0x800080) >> 8;
    uint32_t g  = ((s & 0x00FF00) * a + (d & 0x00FF00) * (255 - a) + 0x008000) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}
uint32_t gfx_lerp(uint32_t a, uint32_t b, int t, int max);

void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t col);
void gfx_blend(struct canvas *c, int x, int y, int w, int h, uint32_t col, int alpha);
void gfx_vgrad(struct canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int alpha);
void gfx_rrect_vgrad(struct canvas *c, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom);
void gfx_rrect_border(struct canvas *c, int x, int y, int w, int h, int r, uint32_t border, uint32_t fill);
void gfx_circle(struct canvas *c, int cx, int cy, int r, uint32_t col, int alpha);
void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, uint32_t col, int thick);
void gfx_blit(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int sstride);
void gfx_blit_rr(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int sstride, int rx, int ry, int rw, int rh, int r);
void gfx_rrect_outline(struct canvas *c, int x, int y, int w, int h, int r, uint32_t col, int alpha);
int  gfx_rr_cov(int i, int j, int x, int y, int w, int h, int r);

int  gfx_text(struct canvas *c, const struct font *f, int x, int baseline, uint32_t col, const char *s);
int  gfx_text_width(const struct font *f, const char *s);
void gfx_text_center(struct canvas *c, const struct font *f, int x, int w, int baseline, uint32_t col, const char *s);
void gfx_mono(struct canvas *c, int x, int y, char ch, uint32_t fg);   /* Unifont 8x16 */
void gfx_mono_text(struct canvas *c, int x, int y, const char *s, uint32_t fg);
int  gfx_isqrt(int v);
