/*
 * desktop_highland.c - DuncOS "Highland" desktop renderer
 * Created by Bryan Duncan
 *
 * Draws the wallpaper, top bar, windows and dock in the Highland theme.
 * Needs: gfx.h, font8x16.h, theme_highland.h. Freestanding C.
 *
 * Quick start (in kmain, after the boot logo):
 *     hl_draw_desktop_demo(&fb);
 */

#include "gfx.h"
#include "theme_highland.h"

/* ------------------------------------------------------------------ */
/* Pixel-art icons: 16x16, '#' = foreground. Drawn at 2x in the dock.  */
/* ------------------------------------------------------------------ */

static const char *ICON_CROWN[12] = {
    "................",
    ".......##.......",
    ".#.....##.....#.",
    ".##...####...##.",
    ".###..#..#..###.",
    ".#.##.#..#.##.#.",
    ".#..###..###..#.",
    ".#...#....#...#.",
    ".#............#.",
    ".##############.",
    ".##############.",
    "................",
};

static const char *ICON_FOLDER[16] = {
    "................", "................", ".#####..........", "#.....#.........",
    "#......#######..", "#.............#.", "#.............#.", "#.............#.",
    "#.............#.", "#.............#.", "#.............#.", "#.............#.",
    "#.............#.", ".#############..", "................", "................",
};
static const char *ICON_TERMINAL[16] = {
    "................", "................", "................", ".##.............",
    "..##............", "...##...........", "....##..........", ".....##.........",
    "....##..........", "...##...........", "..##............", ".##.....#######.",
    "................", "................", "................", "................",
};
static const char *ICON_GLOBE[16] = {
    "................", ".....######.....", "...##..##..##...", "..#...#..#...#..",
    ".#...#....#...#.", ".#...#....#...#.", "################", ".#...#....#...#.",
    ".#...#....#...#.", ".#...#....#...#.", "..#...#..#...#..", "...##..##..##...",
    ".....######.....", "................", "................", "................",
};
static const char *ICON_CODE[16] = {
    "................", "................", "..........#.....", "...#......#..#..",
    "..#......#....#.", ".#.......#.....#", "#........#.....#", ".#......#.....#.",
    "..#.....#....#..", "...#....#...#...", ".......#........", ".......#........",
    "................", "................", "................", "................",
};
static const char *ICON_PHOTO[16] = {
    "................", "................", "################", "#..............#",
    "#..........##..#", "#..........##..#", "#..............#", "#....#.........#",
    "#...###....#...#", "#..#####..###..#", "#.#######.####.#", "################",
    "................", "................", "................", "................",
};
static const char *ICON_GEAR[16] = {
    "................", ".......##.......", "...#...##...#...", "....########....",
    "...##......##...", "...#...##...#...", ".###..####..###.", ".###..####..###.",
    "...#...##...#...", "...##......##...", "....########....", "...#...##...#...",
    ".......##.......", "................", "................", "................",
};

static void draw_icon(const struct framebuffer *fb, const char **rows, int nrows,
                      int x, int y, int scale, uint32_t c) {
    for (int r = 0; r < nrows; r++)
        for (int col = 0; rows[r][col]; col++)
            if (rows[r][col] == '#')
                fill_rect(fb, x + col * scale, y + r * scale, scale, scale, c);
}

/* ------------------------------------------------------------------ */
/* Wallpaper: three mountain ridges, defined on a 1440x900 design grid */
/* and scaled to the real screen.                                      */
/* ------------------------------------------------------------------ */

struct pt { int x, y; };

static const struct pt RIDGE_FAR[]  = {{0,560},{180,420},{330,500},{520,360},{700,470},
                                       {900,330},{1090,450},{1260,380},{1440,470}};
static const struct pt RIDGE_MID[]  = {{0,660},{220,540},{410,620},{610,500},{820,600},
                                       {1010,520},{1220,610},{1440,540}};
static const struct pt RIDGE_NEAR[] = {{0,760},{260,660},{500,740},{760,650},{1010,730},
                                       {1250,670},{1440,720}};

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void draw_ridge(const struct framebuffer *fb, const struct pt *p, int n, uint32_t c) {
    int W = (int)fb->width, H = (int)fb->height;
    for (int x = 0; x < W; x++) {
        int dx = x * 1440 / W;                 /* back to design units */
        int i = 0;
        while (i < n - 2 && p[i + 1].x < dx) i++;
        int x0 = p[i].x, y0 = p[i].y, x1 = p[i + 1].x, y1 = p[i + 1].y;
        int dy = y0 + (y1 - y0) * (dx - x0) / (x1 - x0);
        int top = dy * H / 900;
        for (int y = top; y < H; y++)
            gfx_row(fb, (uint32_t)y)[x] = c;
    }
}

void hl_draw_wallpaper(const struct framebuffer *fb) {
    int W = (int)fb->width, H = (int)fb->height;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            gfx_row(fb, (uint32_t)y)[x] = HL_NAVY;
    fill_circle_a(fb, W * 1120 / 1440, H * 210 / 900, H * 70 / 900, HL_MOON, 24);
    draw_ridge(fb, RIDGE_FAR,  LEN(RIDGE_FAR),  HL_RIDGE_FAR);
    draw_ridge(fb, RIDGE_MID,  LEN(RIDGE_MID),  HL_RIDGE_MID);
    draw_ridge(fb, RIDGE_NEAR, LEN(RIDGE_NEAR), HL_RIDGE_NEAR);
}

/* ------------------------------------------------------------------ */
/* Top bar                                                             */
/* ------------------------------------------------------------------ */

void hl_draw_topbar(const struct framebuffer *fb, const char *app_name,
                    const char **menus, int nmenus, const char *clock) {
    int W = (int)fb->width;
    int ty = (HL_BAR_H - FONT_H) / 2;

    fill_rect_a(fb, 0, 0, W, HL_BAR_H, HL_BAR_BG, HL_BAR_ALPHA);
    fill_rect_a(fb, 0, HL_BAR_H - 1, W, 1, 0xFFFFFFu, 16);

    draw_icon(fb, ICON_CROWN, 12, 16, (HL_BAR_H - 12) / 2, 1, HL_GOLD);
    int x = draw_text(fb, "DuncOS", 38, ty, 1, HL_ACCENT) + 20;

    x = draw_text(fb, app_name, x, ty, 1, HL_TEXT) + 16;
    for (int i = 0; i < nmenus; i++)
        x = draw_text(fb, menus[i], x, ty, 1, HL_TEXT_DIM) + 16;

    draw_text(fb, clock, W - 16 - text_width(clock, 1), ty, 1, HL_TEXT);
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

struct hl_window {
    int x, y, w, h;
    const char *title;
    uint32_t body;          /* HL_WIN_BG or HL_TERM_BG */
    int focused;
};

/* Draws frame + title bar. Returns the y where content starts. */
int hl_draw_window(const struct framebuffer *fb, const struct hl_window *win) {
    int r = HL_WIN_RADIUS;

    /* soft shadow: a few offset, low-alpha layers */
    for (int s = 3; s >= 1; s--)
        fill_round_rect_a(fb, win->x - s * 2, win->y + HL_SHADOW_OFF - s * 2 + 6,
                          win->w + s * 4, win->h + s * 4, r + s * 2,
                          HL_SHADOW, HL_SHADOW_ALPHA / 2);

    fill_round_rect_a(fb, win->x, win->y, win->w, win->h, r, win->body, HL_WIN_ALPHA);
    stroke_round_rect(fb, win->x, win->y, win->w, win->h, r,
                      win->focused ? HL_ACCENT : 0xFFFFFFu, win->focused ? 90 : 24);

    /* title bar divider */
    fill_rect_a(fb, win->x + 1, win->y + HL_TITLE_H, win->w - 2, 1, 0xFFFFFFu, 16);

    /* close / minimise / maximise */
    int cy = win->y + HL_TITLE_H / 2;
    fill_circle_a(fb, win->x + 20, cy, HL_DOT_R, HL_RED,   255);
    fill_circle_a(fb, win->x + 40, cy, HL_DOT_R, HL_GOLD,  255);
    fill_circle_a(fb, win->x + 60, cy, HL_DOT_R, HL_GREEN, 255);

    int tw = text_width(win->title, 1);
    draw_text(fb, win->title, win->x + (win->w - tw) / 2, cy - FONT_H / 2, 1,
              win->focused ? HL_TEXT : HL_TEXT_DIM);

    return win->y + HL_TITLE_H + 1;
}

/* ------------------------------------------------------------------ */
/* Dock                                                                */
/* ------------------------------------------------------------------ */

struct hl_dock_item {
    const char **icon;
    uint32_t bg, fg;
    int running;            /* draws the dot under the icon */
};

void hl_draw_dock(const struct framebuffer *fb, const struct hl_dock_item *items, int n) {
    int W = (int)fb->width, H = (int)fb->height;
    int dw = n * HL_DOCK_ICON + (n - 1) * HL_DOCK_GAP + 2 * HL_DOCK_PAD;
    int dh = HL_DOCK_ICON + 2 * HL_DOCK_PAD;
    int dx = (W - dw) / 2;
    int dy = H - HL_DOCK_MARGIN - dh;

    fill_round_rect_a(fb, dx, dy + 8, dw, dh, HL_DOCK_RADIUS, HL_SHADOW, 60);
    fill_round_rect_a(fb, dx, dy, dw, dh, HL_DOCK_RADIUS, HL_DOCK_BG, HL_DOCK_ALPHA);
    stroke_round_rect(fb, dx, dy, dw, dh, HL_DOCK_RADIUS, 0xFFFFFFu, 26);

    int x = dx + HL_DOCK_PAD, y = dy + HL_DOCK_PAD;
    for (int i = 0; i < n; i++, x += HL_DOCK_ICON + HL_DOCK_GAP) {
        fill_round_rect_a(fb, x, y, HL_DOCK_ICON, HL_DOCK_ICON, 14, items[i].bg, 255);
        draw_icon(fb, items[i].icon, 16, x + (HL_DOCK_ICON - 32) / 2,
                  y + (HL_DOCK_ICON - 32) / 2, 2, items[i].fg);
        if (items[i].running)
            fill_circle_a(fb, x + HL_DOCK_ICON / 2, y + HL_DOCK_ICON + 5, 2, HL_TEXT, 255);
    }
}

/* ------------------------------------------------------------------ */
/* Demo: the Highland mockup, drawn for real                           */
/* ------------------------------------------------------------------ */

void hl_draw_desktop_demo(const struct framebuffer *fb) {
    static const char *menus[] = { "File", "Edit", "View", "Help" };

    hl_draw_wallpaper(fb);
    hl_draw_topbar(fb, "Terminal", menus, 4, "Fri 9:12 PM");

    /* Files window (behind) */
    struct hl_window files = { (int)fb->width - 700 - 60, 230, 700, 440,
                               "Files - ~/Projects/thane", HL_WIN_BG, 0 };
    int fy = hl_draw_window(fb, &files);
    fill_rect_a(fb, files.x + 1, fy, 170, files.h - HL_TITLE_H - 2, 0x000000u, 40);
    static const char *places[] = { "Home", "Documents", "Projects", "Downloads", "Trash" };
    for (int i = 0; i < 5; i++) {
        int py = fy + 16 + i * 30;
        if (i == 2) fill_round_rect_a(fb, files.x + 10, py - 7, 150, 30, 8, HL_ACCENT, 36);
        draw_text(fb, places[i], files.x + 22, py, 1, i == 2 ? HL_TEXT : HL_TEXT_DIM);
    }
    static const char *names[] = { "kernel", "drivers", "boot", "fonts", "docs" };
    for (int i = 0; i < 5; i++) {
        int ix = files.x + 200 + i * 96, iy = fy + 24;
        draw_icon(fb, ICON_FOLDER, 16, ix + 8, iy, 3, HL_ACCENT);
        draw_text(fb, names[i], ix + 32 - text_width(names[i], 1) / 2, iy + 54, 1, HL_TEXT);
    }

    /* Terminal window (focused, in front) */
    struct hl_window term = { 80, 110, 580, 400, "castle - ~", HL_TERM_BG, 1 };
    int ty = hl_draw_window(fb, &term) + 18;
    int tx = term.x + 22;
    draw_icon(fb, ICON_CROWN, 12, tx, ty, 4, HL_GOLD);
    int kx = tx + 90;
    static const char *keys[] = { "OS", "Kernel", "Shell", "Pkgs", "Font" };
    static const char *vals[] = { "DuncOS 0.1", "Thane 0.1", "castle", "clan", "font8x16" };
    int lx = draw_text(fb, "bryan", kx, ty, 1, HL_ACCENT);
    lx = draw_text(fb, "@", lx, ty, 1, HL_TEXT);
    draw_text(fb, "duncos", lx, ty, 1, HL_ACCENT);
    draw_text(fb, "-------------", kx, ty + 22, 1, HL_TEXT_FAINT);
    for (int i = 0; i < 5; i++) {
        draw_text(fb, keys[i], kx, ty + 44 + i * 22, 1, HL_GOLD);
        draw_text(fb, vals[i], kx + 72, ty + 44 + i * 22, 1, HL_TEXT);
    }
    int py = ty + 180;
    lx = draw_text(fb, "~ > ", tx, py, 1, HL_GREEN);
    draw_text(fb, "clan install editor", lx, py, 1, HL_TEXT);
    draw_text(fb, "resolving... done - installed editor 0.3", tx, py + 22, 1, HL_TEXT_DIM);
    lx = draw_text(fb, "~ > ", tx, py + 44, 1, HL_GREEN);
    fill_rect(fb, lx, py + 44, 8, 16, HL_TEXT);     /* cursor */

    /* Dock */
    struct hl_dock_item dock[] = {
        { ICON_FOLDER,   0x1C3A52u, HL_CYAN,   1 },
        { ICON_TERMINAL, 0x111827u, HL_TEXT,   1 },
        { ICON_GLOBE,    0x1E2A44u, 0x8BE3F2u, 0 },
        { ICON_CODE,     0x2A1F3Du, 0xA78BFAu, 0 },
        { ICON_PHOTO,    0x3A2A12u, HL_GOLD,   0 },
        { ICON_GEAR,     0x1F2937u, 0xCBD5E1u, 0 },
    };
    hl_draw_dock(fb, dock, LEN(dock));
}
