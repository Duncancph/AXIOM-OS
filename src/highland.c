/*
 * highland.c - the desktop look: the AXIOM theme (black and cream, the AXIOM logo), built on the
 * drawing helpers of the original Highland design (desktop_highland.c, unchanged), for the window manager, and adds pixel-art icons for the AXIOM apps in the same style.
 */
#include "desktop_highland.c"
#include "highland.h"
#include "icons_data.h"
#include "logo_data.h"

/* the smooth app icons (tools/genicons.py); unknown names get the gear */
const uint32_t *hl_app_icon(const char *name, int menu) {
    int n = sizeof app_icons / sizeof app_icons[0], g = 0;
    for (int i = 0; i < n; i++) {
        const char *a = app_icons[i].name, *b = name;
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return menu ? app_icons[i].menu : app_icons[i].dock;
        if (app_icons[i].name[0] == 'g' && app_icons[i].name[1] == 'e') g = i;
    }
    return menu ? app_icons[g].menu : app_icons[g].dock;
}

/* draw an ARGB icon (straight alpha) onto the frame buffer */
static void blend_icon(const struct framebuffer *fb, int x, int y, const uint32_t *px, int size) {
    for (int j = 0; j < size; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= (int)fb->height) continue;
        volatile uint32_t *row = gfx_row(fb, (uint32_t)yy);
        for (int i = 0; i < size; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= (int)fb->width) continue;
            uint32_t s = px[j * size + i], a = s >> 24;
            if (!a) continue;
            if (a == 255) { row[xx] = s & 0xFFFFFF; continue; }
            uint32_t d = row[xx];
            uint32_t r = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
            uint32_t g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
            uint32_t b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
            row[xx] = r << 16 | g << 8 | b;
        }
    }
}

static const char *ICON_CROWN16[16] = {
    "................", "................",
    "................", ".......##.......", ".#.....##.....#.", ".##...####...##.",
    ".###..#..#..###.", ".#.##.#..#.##.#.", ".#..###..###..#.", ".#...#....#...#.",
    ".#............#.", ".##############.", ".##############.", "................",
    "................", "................",
};
static const char *ICON_MONITOR[16] = {
    "................", "................", "...........##...", "...........##...",
    "...........##...", ".......##..##...", ".......##..##...", ".......##..##...",
    "...##..##..##...", "...##..##..##...", "...##..##..##...", "...##..##..##...",
    "..############..", "................", "................", "................",
};
static const char *ICON_BLUETOOTH[16] = {
    "................", ".......#........", ".......##.......", ".......#.#......",
    ".......#..#.....", "...#...#...#....", "....##.#.##.....", "......###.......",
    "......###.......", "....##.#.##.....", "...#...#...#....", ".......#..#.....",
    ".......#.#......", ".......##.......", ".......#........", "................",
};
static const char *ICON_CALC[16] = {
    "................", "..############..", "..#..........#..", "..#.########.#..",
    "..#.#......#.#..", "..#.########.#..", "..#..........#..", "..#.##.##.##.#..",
    "..#.##.##.##.#..", "..#..........#..", "..#.##.##.##.#..", "..#.##.##.##.#..",
    "..#..........#..", "..############..", "................", "................",
};
static const char *ICON_SHIELD[16] = {
    "................", ".......##.......", ".....##..##.....", "...##......##...",
    "..#....##....#..", "..#....##....#..", "..#....##....#..", "..#....##....#..",
    "..#..........#..", "...#...##...#...", "...#...##...#...", "....#......#....",
    ".....##..##.....", ".......##.......", "................", "................",
};

static const char *ICON_CLOCK[16] = {
    "................", ".....######.....", "...##......##...", "..#....#.....#..",
    ".#.....#......#.", ".#.....#......#.", "#......#.......#", "#......####....#",
    "#..............#", "#..............#", ".#............#.", ".#............#.",
    "..#..........#..", "...##......##...", ".....######.....", "................",
};

static const char *ICON_NETWORK[16] = {
    "................", "......####......", "......#..#......", "......####......",
    ".......##.......", ".......##.......", "..############..", "..#....##....#..",
    "..#.........#...", ".####......####.", ".#..#......#..#.", ".####......####.",
    "................", "................", "................", "................",
};

struct named_icon { const char *name; const char **rows; uint32_t bg, fg; };
static const struct named_icon ICONS[] = {
    { "folder",    ICON_FOLDER,    0x1C3A52u, HL_CYAN   },
    { "terminal",  ICON_TERMINAL,  0x111827u, HL_TEXT   },
    { "monitor",   ICON_MONITOR,   0x10291Fu, HL_GREEN  },
    { "bluetooth", ICON_BLUETOOTH, 0x232321u, 0xF4F1DEu },
    { "calc",      ICON_CALC,      0x232321u, 0xD8D3B8u },
    { "shield",    ICON_SHIELD,    0x3A1A22u, HL_RED    },
    { "crown",     ICON_CROWN16,   0x2A2620u, HL_GOLD   },
    { "code",      ICON_CODE,      0x232321u, 0xD8D3B8u },
    { "clock",     ICON_CLOCK,     0x232321u, 0xF4F1DEu },
    { "globe",     ICON_GLOBE,     0x232321u, 0xF4F1DEu },
    { "arc",       ICON_GLOBE,     0x232321u, 0xF4F1DEu },
    { "network",   ICON_NETWORK,   0x123326u, HL_GREEN  },
    { "photo",     ICON_PHOTO,     0x2A2620u, HL_GOLD   },
    { "chat",      ICON_CODE,      0x3A2016u, 0xF0A07Eu },
    { "gear",      ICON_GEAR,      0x1F2937u, 0xCBD5E1u },
};

static const struct named_icon *lookup(const char *name) {
    for (unsigned i = 0; i < sizeof ICONS / sizeof ICONS[0]; i++) {
        const char *a = ICONS[i].name, *b = name;
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return &ICONS[i];
    }
    return &ICONS[sizeof ICONS / sizeof ICONS[0] - 1];   /* gear */
}

const char **hl_icon(const char *name, uint32_t *bg, uint32_t *fg) {
    const struct named_icon *ic = lookup(name);
    if (bg) *bg = ic->bg;
    if (fg) *fg = ic->fg;
    return ic->rows;
}

void hl_dock_rect(int W, int H, int n, int *x, int *y, int *w, int *h);

/* ================= AXIOM look: black and cream, with the AXIOM logo ================= */
static inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t r = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
    uint32_t g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
    uint32_t b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return r << 16 | g << 8 | b;
}
/* the logo (an alpha map) in one colour; alpha 0..255 scales it */
void ax_logo(const struct framebuffer *fb, int x, int y, const struct logo_img *l, uint32_t col, int alpha) {
    for (int j = 0; j < l->h; j++) {
        int yy = y + j; if (yy < 0 || yy >= (int)fb->height) continue;
        volatile uint32_t *row = gfx_row(fb, (uint32_t)yy);
        for (int i = 0; i < l->w; i++) {
            int xx = x + i; if (xx < 0 || xx >= (int)fb->width) continue;
            uint32_t a = l->a[j * l->w + i] * (uint32_t)alpha / 255;
            if (a) row[xx] = mix(row[xx], col, a);
        }
    }
}
const struct logo_img *ax_logo_img(int which) {
    static const struct logo_img *L[] = { &logo_bar, &logo_mark_bar, &logo_mid, &logo_splash, &logo_mark_big, &logo_glow };
    return L[which];
}

/* near-black with a soft light in the middle and a large, blurred AXIOM mark glowing through */
void ax_draw_wallpaper(const struct framebuffer *fb) {
    int W = (int)fb->width, H = (int)fb->height;
    int cx = W / 2, cy = H * 46 / 100;
    long rr = (long)W * W / 4 + (long)H * H / 4;
    for (int y = 0; y < H; y++) {
        volatile uint32_t *row = gfx_row(fb, (uint32_t)y);
        for (int x = 0; x < W; x++) {
            long dx = x - cx, dy = (y - cy) * 13 / 10;
            long d = (dx * dx + dy * dy) * 256 / rr;          /* 0 in the middle .. ~256 at the corners */
            int v = 17 - (int)(d * 14 / 256); if (v < 3) v = 3;
            uint32_t hsh = (uint32_t)(x * 374761393u + y * 668265263u); hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
            v += (hsh >> 28) < 5;                                /* a little noise against banding */
            row[x] = (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)(v * 92 / 100);
        }
    }
    /* the glow: the blurred mark scaled to ~60% of the screen height */
    const struct logo_img *g = &logo_glow;
    int gh = H * 62 / 100, gw = g->w * gh / g->h;
    int gx = cx - gw / 2, gy = cy - gh * 52 / 100;
    for (int y = 0; y < gh; y++) {
        int yy = gy + y; if (yy < 0 || yy >= H) continue;
        volatile uint32_t *row = gfx_row(fb, (uint32_t)yy);
        int sy = y * g->h / gh;
        for (int x = 0; x < gw; x++) {
            int xx = gx + x; if (xx < 0 || xx >= W) continue;
            uint32_t a = g->a[sy * g->w + x * g->w / gw] * 26 / 255;
            if (a) row[xx] = mix(row[xx], HL_CREAM, a);
        }
    }
}

void ax_draw_topbar(const struct framebuffer *fb, const char *app_name, const char *clock) {
    int W = (int)fb->width;
    int ty = (HL_BAR_H - FONT_H) / 2;
    fill_rect_a(fb, 0, 0, W, HL_BAR_H, HL_BAR_BG, HL_BAR_ALPHA);
    fill_rect_a(fb, 0, HL_BAR_H - 1, W, 1, HL_CREAM, 20);
    ax_logo(fb, 16, (HL_BAR_H - logo_bar.h) / 2, &logo_bar, HL_CREAM, 255);
    int x = 16 + logo_bar.w + 24;
    fill_rect_a(fb, x - 12, 11, 1, HL_BAR_H - 22, HL_CREAM, 50);
    draw_text(fb, app_name, x, ty, 1, HL_TEXT_DIM);
    draw_text(fb, clock, W - 16 - text_width(clock, 1), ty, 1, HL_TEXT);
}

int ax_draw_window(const struct framebuffer *fb, const struct hl_window *win) {
    int r = HL_WIN_RADIUS;
    for (int s = 4; s >= 1; s--)
        fill_round_rect_a(fb, win->x - s * 3, win->y + HL_SHADOW_OFF - s * 3 + 4, win->w + s * 6, win->h + s * 6, r + s * 3, HL_SHADOW, HL_SHADOW_ALPHA / 3);
    fill_round_rect_a(fb, win->x, win->y, win->w, win->h, r, win->body, HL_WIN_ALPHA);
    fill_round_rect_a(fb, win->x, win->y, win->w, HL_TITLE_H + r, r, 0x181817u, 255);    /* title bar, a shade lighter */
    fill_rect_a(fb, win->x, win->y + HL_TITLE_H, win->w, r, win->body, 255);
    stroke_round_rect(fb, win->x, win->y, win->w, win->h, r, HL_CREAM, win->focused ? 70 : 22);
    fill_rect_a(fb, win->x + 1, win->y + HL_TITLE_H, win->w - 2, 1, HL_CREAM, 18);
    /* the title and the minimise/maximise/close buttons are drawn by the compositor (desktop.c) */
    return win->y + HL_TITLE_H + 1;
}

void ax_draw_dock(const struct framebuffer *fb, int n, const int *running) {
    int x, y, w, h;
    hl_dock_rect((int)fb->width, (int)fb->height, n, &x, &y, &w, &h);
    fill_round_rect_a(fb, x - 2, y + 10, w + 4, h, HL_DOCK_RADIUS, HL_SHADOW, 90);
    fill_round_rect_a(fb, x, y, w, h, HL_DOCK_RADIUS, HL_DOCK_BG, HL_DOCK_ALPHA);
    stroke_round_rect(fb, x, y, w, h, HL_DOCK_RADIUS, HL_CREAM, 30);
    for (int i = 0; i < n; i++)
        if (running[i]) fill_circle_a(fb, x + HL_DOCK_PAD + i * (HL_DOCK_ICON + HL_DOCK_GAP) + HL_DOCK_ICON / 2, y + HL_DOCK_PAD + HL_DOCK_ICON + 5, 2, HL_CREAM, 255);
}

const char **hl_crown_icon(void) { return ICON_CROWN; }

void hl_dock_rect(int W, int H, int n, int *x, int *y, int *w, int *h) {
    *w = n * HL_DOCK_ICON + (n - 1) * HL_DOCK_GAP + 2 * HL_DOCK_PAD;
    *h = HL_DOCK_ICON + 2 * HL_DOCK_PAD;
    *x = (W - *w) / 2;
    *y = H - HL_DOCK_MARGIN - *h;
}

void hl_render_dock(const struct framebuffer *fb, const char *const *icons, const int *running, int n) {
    static const char *BLANK[16] = { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" };
    struct hl_dock_item items[HL_MAX_DOCK] = { 0 };
    if (n > HL_MAX_DOCK) n = HL_MAX_DOCK;
    (void)BLANK; (void)items;
    ax_draw_dock(fb, n, running);
    int x, y, w, h;                               /* the smooth icons go on top */
    hl_dock_rect((int)fb->width, (int)fb->height, n, &x, &y, &w, &h);
    for (int i = 0; i < n; i++)
        blend_icon(fb, x + HL_DOCK_PAD + i * (HL_DOCK_ICON + HL_DOCK_GAP) + (HL_DOCK_ICON - ICON_DOCK) / 2,
                   y + HL_DOCK_PAD + (HL_DOCK_ICON - ICON_DOCK) / 2, hl_app_icon(icons[i], 0), ICON_DOCK);
}

void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock) {
    ax_draw_topbar(fb, app, clock);
}

void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused) {
    struct hl_window win = { x, y, w, h, title, body, focused };
    ax_draw_window(fb, &win);
}
