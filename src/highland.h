/* DuncOS window manager <-> Highland design glue (see highland.c). */
#pragma once
#include <stdint.h>
#define HL_NAPPS 6
struct framebuffer;
const char **hl_app_icon(int i, uint32_t *bg, uint32_t *fg);
const char **hl_crown_icon(void);       /* 16x12 */
void hl_draw_wallpaper(const struct framebuffer *fb);
void hl_dock_rect(int W, int H, int *x, int *y, int *w, int *h);
void hl_render_dock(const struct framebuffer *fb, const int *running);
void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock);
void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused);
