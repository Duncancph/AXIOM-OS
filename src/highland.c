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

static const char *ICON_CLOCK[16] = {
    "................", ".....######.....", "...##......##...", "..#....#.....#..",
    ".#.....#......#.", ".#.....#......#.", "#......#.......#", "#......####....#",
    "#..............#", "#..............#", ".#............#.", ".#............#.",
    "..#..........#..", "...##......##...", ".....######.....", "................",
};

struct named_icon { const char *name; const char **rows; uint32_t bg, fg; };
static const struct named_icon ICONS[] = {
    { "folder",    ICON_FOLDER,    0x1C3A52u, HL_CYAN   },
    { "terminal",  ICON_TERMINAL,  0x111827u, HL_TEXT   },
    { "monitor",   ICON_MONITOR,   0x10291Fu, HL_GREEN  },
    { "bluetooth", ICON_BLUETOOTH, 0x1E2A44u, 0x8BE3F2u },
    { "calc",      ICON_CALC,      0x2A1F3Du, 0xA78BFAu },
    { "shield",    ICON_SHIELD,    0x3A1A22u, HL_RED    },
    { "crown",     ICON_CROWN16,   0x3A2A12u, HL_GOLD   },
    { "code",      ICON_CODE,      0x2A1F3Du, 0xA78BFAu },
    { "clock",     ICON_CLOCK,     0x1E2A44u, 0x8BE3F2u },
    { "globe",     ICON_GLOBE,     0x1E2A44u, 0x8BE3F2u },
    { "photo",     ICON_PHOTO,     0x3A2A12u, HL_GOLD   },
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

const char **hl_crown_icon(void) { return ICON_CROWN; }

void hl_dock_rect(int W, int H, int n, int *x, int *y, int *w, int *h) {
    *w = n * HL_DOCK_ICON + (n - 1) * HL_DOCK_GAP + 2 * HL_DOCK_PAD;
    *h = HL_DOCK_ICON + 2 * HL_DOCK_PAD;
    *x = (W - *w) / 2;
    *y = H - HL_DOCK_MARGIN - *h;
}

void hl_render_dock(const struct framebuffer *fb, const char *const *icons, const int *running, int n) {
    struct hl_dock_item items[HL_MAX_DOCK];
    if (n > HL_MAX_DOCK) n = HL_MAX_DOCK;
    for (int i = 0; i < n; i++) {
        const struct named_icon *ic = lookup(icons[i]);
        items[i].icon = ic->rows; items[i].bg = ic->bg; items[i].fg = ic->fg; items[i].running = running[i];
    }
    hl_draw_dock(fb, items, n);
}

void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock) {
    hl_draw_topbar(fb, app, 0, 0, clock);
}

void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused) {
    struct hl_window win = { x, y, w, h, title, body, focused };
    hl_draw_window(fb, &win);
}
