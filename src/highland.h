/* AXIOM window manager <-> Highland design glue (see highland.c). */
#pragma once
#include <stdint.h>
#define HL_MAX_DOCK 16
struct framebuffer;
const char **hl_icon(const char *name, uint32_t *bg, uint32_t *fg);   /* 16x16 pixel art */
const char **hl_crown_icon(void);
const uint32_t *hl_app_icon(const char *name, int menu);           /* ARGB, 52x52 or (menu) 34x34 */                                     /* 16x12 */
void hl_draw_wallpaper(const struct framebuffer *fb);
void hl_dock_rect(int W, int H, int n, int *x, int *y, int *w, int *h);
void hl_render_dock(const struct framebuffer *fb, const char *const *icons, const int *running, int n);
void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock);
void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused);

#ifndef LOGO_IMG_DEFINED
#define LOGO_IMG_DEFINED
struct logo_img { int w, h; const uint8_t *a; };
#endif
void ax_draw_wallpaper(const struct framebuffer *fb);
void ax_logo(const struct framebuffer *fb, int x, int y, const struct logo_img *l, uint32_t col, int alpha);
const struct logo_img *ax_logo_img(int which);   /* 0 bar lockup, 1 bar mark, 2 mid lockup, 3 splash lockup, 4 big mark, 5 glow */
