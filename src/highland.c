/*
 * highland.c - connects the Highland design (desktop_highland.c, unchanged) to the
 * DuncOS window manager, and adds pixel-art icons for the DuncOS apps in the same style.
 */
#include "desktop_highland.c"
#include "highland.h"
#include "icons_data.h"

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
    { "bluetooth", ICON_BLUETOOTH, 0x1E2A44u, 0x8BE3F2u },
    { "calc",      ICON_CALC,      0x2A1F3Du, 0xA78BFAu },
    { "shield",    ICON_SHIELD,    0x3A1A22u, HL_RED    },
    { "crown",     ICON_CROWN16,   0x3A2A12u, HL_GOLD   },
    { "code",      ICON_CODE,      0x2A1F3Du, 0xA78BFAu },
    { "clock",     ICON_CLOCK,     0x1E2A44u, 0x8BE3F2u },
    { "globe",     ICON_GLOBE,     0x1E2A44u, 0x8BE3F2u },
    { "network",   ICON_NETWORK,   0x123326u, HL_GREEN  },
    { "photo",     ICON_PHOTO,     0x3A2A12u, HL_GOLD   },
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
    for (int i = 0; i < n; i++) {                 /* the Highland dock draws the tiles in each icon's edge colour ... */
        const uint32_t *px = hl_app_icon(icons[i], 0);
        items[i].icon = BLANK; items[i].bg = px[(ICON_DOCK / 2) * ICON_DOCK + 1] & 0xFFFFFF; items[i].fg = 0; items[i].running = running[i];
    }
    hl_draw_dock(fb, items, n);
    int x, y, w, h;                               /* ... and the smooth icons go on top */
    hl_dock_rect((int)fb->width, (int)fb->height, n, &x, &y, &w, &h);
    for (int i = 0; i < n; i++)
        blend_icon(fb, x + HL_DOCK_PAD + i * (HL_DOCK_ICON + HL_DOCK_GAP) + (HL_DOCK_ICON - ICON_DOCK) / 2,
                   y + HL_DOCK_PAD + (HL_DOCK_ICON - ICON_DOCK) / 2, hl_app_icon(icons[i], 0), ICON_DOCK);
}

void hl_render_topbar(const struct framebuffer *fb, const char *app, const char *clock) {
    hl_draw_topbar(fb, app, 0, 0, clock);
}

void hl_render_window(const struct framebuffer *fb, int x, int y, int w, int h,
                      const char *title, uint32_t body, int focused) {
    struct hl_window win = { x, y, w, h, title, body, focused };
    hl_draw_window(fb, &win);
}
