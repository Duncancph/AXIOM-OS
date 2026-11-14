/*
 * highland.c - connects the Highland design (desktop_highland.c, unchanged) to the
 * DuncOS window manager, and adds pixel-art icons for the DuncOS apps in the same style.
 */
#include "desktop_highland.c"
#include "highland.h"

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

/* Same order as the desktop's app list: terminal, monitor, bluetooth, calc, sandbox, about */
static const struct hl_dock_item APPS[HL_NAPPS] = {
    { ICON_TERMINAL,  0x111827u, HL_TEXT,   0 },
    { ICON_MONITOR,   0x10291Fu, HL_GREEN,  0 },
    { ICON_BLUETOOTH, 0x1E2A44u, 0x8BE3F2u, 0 },
    { ICON_CALC,      0x2A1F3Du, 0xA78BFAu, 0 },
    { ICON_SHIELD,    0x3A1A22u, HL_RED,    0 },
    { ICON_CROWN16,   0x3A2A12u, HL_GOLD,   0 },
};

const char **hl_app_icon(int i, uint32_t *bg, uint32_t *fg) {
    if (bg) *bg = APPS[i].bg;
    if (fg) *fg = APPS[i].fg;
    return APPS[i].icon;
}

const char **hl_crown_icon(void) { return ICON_CROWN; }

void hl_dock_rect(int W, int H, int *x, int *y, int *w, int *h) {
    *w = HL_NAPPS * HL_DOCK_ICON + (HL_NAPPS - 1) * HL_DOCK_GAP + 2 * HL_DOCK_PAD;
    *h = HL_DOCK_ICON + 2 * HL_DOCK_PAD;
    *x = (W - *w) / 2;
    *y = H - HL_DOCK_MARGIN - *h;
}

void hl_render_dock(const struct framebuffer *fb, const int *running) {
    struct hl_dock_item items[HL_NAPPS];
    for (int i = 0; i < HL_NAPPS; i++) { items[i] = APPS[i]; items[i].running = running[i]; }
    hl_draw_dock(fb, items, HL_NAPPS);
}

void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock) {
    hl_draw_topbar(fb, app, 0, 0, clock);
}

void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused) {
    struct hl_window win = { x, y, w, h, title, body, focused };
    hl_draw_window(fb, &win);
}
