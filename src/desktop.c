/* AXIOM desktop: window manager + compositor (kernel thread), in the Highland theme.
 * The look comes from Bryan Duncan's Highland design (desktop_highland.c, theme_highland.h):
 * wallpaper, top bar, window frames and dock are drawn by that code. This file handles
 * window stacking, input, damage tracking and composition. */
#include "kernel.h"
#include "../common/gfx.h"
#include "../common/theme_highland.h"
#include "font8x16.h"
#include "highland.h"
#include "net.h"

uint32_t hl_accent = HL_CREAM;          /* Settings > Personalise */

#define TITLE (HL_TITLE_H + 1)          /* content starts below the title bar + divider */
#define MAX_WIN 32

struct framebuffer { volatile uint32_t *addr; uint32_t width, height, pitch; };

struct window {
    bool used, minimized;
    int id, x, y, w, h;                 /* x,y = frame origin; w,h = content size */
    struct task *owner;
    char title[64];
    uint32_t *buf; int pages; uint64_t uva;
    uint64_t close_req;
    bool resizable, maximized, resize_sent;
    int want_w, want_h;                 /* size requested from the app */
    int rx, ry, rw, rh;                 /* position/size to restore after maximise */
    uint64_t last_title_click;
    bool snapped;                       /* filling half the screen (dragged to an edge) */
    bool popup;                         /* a menu, list or tooltip (WIN_POPUP): no frame, never focused, above the rest;
                                           minimized = hidden */
    bool move_events;                   /* WIN_MOVE_EVENTS: tell the app where its content is when it moves */
    int sent_x, sent_y;                 /* (the last position told) */
    int cursor;                         /* the pointer's shape over its content (WM_CURSOR_*) */
    char app_id[24], app_name[32];      /* whose it really is, when the owner shows it for another program
                                           (AXIOM's Wayland compositor: wm_set_app); "": the owner's */
};
static const char *win_app(const struct window *w) { return w->app_id[0] ? w->app_id : w->owner ? w->owner->app : 0; }
static const char *win_name(const struct window *w) { return w->app_name[0] ? w->app_name : w->owner ? w->owner->name : w->title; }
static inline int fw(struct window *w) { return w->popup ? w->w : w->w + 2; }
static inline int fh(struct window *w) { return w->popup ? w->h : w->h + TITLE + 1; }
static inline int cx0(struct window *w) { return w->popup ? w->x : w->x + 1; }       /* where its content starts */
static inline int cy0(struct window *w) { return w->popup ? w->y : w->y + TITLE; }

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN], nz, next_wid = 1;
static uint32_t *fb32, *bb, *wall;
static int W, H, fb_stride;
/* Scaling: when Settings > Display asks for a smaller size than the graphics card gave us (old firmware often
 * can't switch), the desktop is drawn at that size (W x H) and stretched to fill the screen (PW x PH). */
static bool scaled;
static int PW, PH, ox, oy, dw, dh;
static uint16_t *sx_i, *sy_i; static uint8_t *sx_f, *sy_f;     /* for each screen column/row: source pixel and fraction */
static bool fb_xrgb, splash_shown, desktop_ok;
static struct canvas cv;
static struct framebuffer full;

static int mx, my, mbtn, drag_win = -1, drag_ox, drag_oy, capture_win = -1, hover_win = -1;
static int snap_zone;                    /* while dragging a window: 1 left half, 2 right half, 3 full screen */
static int sw_list[32], sw_n, sw_sel;    /* Alt+Tab: the windows in the order they were last used, and the choice */
static uint64_t sw_until;
static int resize_win = -1, resize_mode, resize_ox, resize_oy, resize_w0, resize_h0;   /* mode: 1 right, 2 bottom, 3 corner */
#define MIN_W 280
#define MIN_H 160
static int hover_ctl, hx, hy, hw, hh;
static bool menu_open;
static char bar_sig[96];
static struct rect dock_area_cached;
static int run_sig = -1;

struct toast { bool used; char title[64], msg[160]; uint64_t until; };
static struct toast toasts[4];

/* apps come from the registry (built-in + installed with clan) */
#define NVIS apps_visible()
static const char *app_name(int i) { return apps_get(i)->name; }

/* The dock: the apps you chose (setting "dock.apps", a comma-separated list of app ids), then any
 * other app that is running. */
static int dock_app[HL_MAX_DOCK], ndock; static bool dock_pinned[HL_MAX_DOCK];
static bool dock_dirty = true;
static bool app_running(int a);
static int app_by_id(const char *id, int len) {
    for (int i = 0; i < NVIS; i++) if ((int)strlen(apps_get(i)->id) == len && !memcmp(apps_get(i)->id, id, len)) return i;
    return -1;
}
static bool is_pinned(int a) { for (int i = 0; i < ndock; i++) if (dock_app[i] == a && dock_pinned[i]) return true; return false; }
static void dock_rebuild(void) {
    ndock = 0;
    const char *s = settings_get("dock.apps");
    while (s && *s && ndock < HL_MAX_DOCK) {
        const char *e = s; while (*e && *e != ',') e++;
        int a = app_by_id(s, e - s);
        bool dup = false; for (int i = 0; i < ndock; i++) if (dock_app[i] == a) dup = true;
        if (a >= 0 && !dup) { dock_app[ndock] = a; dock_pinned[ndock++] = true; }
        s = *e ? e + 1 : e;
    }
    for (int a = 0; a < NVIS && ndock < HL_MAX_DOCK; a++)
        if (!is_pinned(a) && app_running(a)) { dock_app[ndock] = a; dock_pinned[ndock++] = false; }
}
void desktop_dock_changed(void) { dock_dirty = true; }

/* ---------------- geometry ---------------- */
struct rect { int x, y, w, h; };
static bool overlap(struct rect a, struct rect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static bool in_rect(struct rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static struct rect runion(struct rect a, struct rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}
static bool contains(struct rect a, struct rect b) { return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h; }

static struct rect bar_rect(void) { return (struct rect){ 0, 0, W, HL_BAR_H }; }
static struct rect dock_rect(void) { int x, y, w, h; hl_dock_rect(W, H, ndock ? ndock : 1, &x, &y, &w, &h); return (struct rect){ x, y, w, h }; }
static struct rect dock_area(void) { struct rect d = dock_rect(); return (struct rect){ d.x - 1, d.y - 1, d.w + 2, d.h + 10 }; }  /* incl. shadow */
static struct rect dock_item(int i) { struct rect d = dock_rect(); return (struct rect){ d.x + HL_DOCK_PAD + i * (HL_DOCK_ICON + HL_DOCK_GAP), d.y + HL_DOCK_PAD, HL_DOCK_ICON, HL_DOCK_ICON }; }
static struct rect frame_area(struct window *w) {   /* incl. the shadow */
    if (w->popup) return (struct rect){ w->x, w->y, w->w, w->h };
    return (struct rect){ w->x - 16, w->y - 12, fw(w) + 32, fh(w) + 44 };
}
static struct rect crown_btn(void) { return (struct rect){ 6, 3, 132, HL_BAR_H - 6 }; }
/* The AXIOM menu: apps in two columns, in groups (everyday apps, system tools, apps added with clan).
 * The square at the end of each row puts the app in the dock or takes it out. */
#define MENU_W 560
#define MENU_ROW 44
static int mn, mapp[32], nsec, sec_y[3]; static const char *sec_name[3]; static int menu_h = 200;
static struct rect mrect[32];
static int app_cat(int a) {
    const struct app_entry *e = apps_get(a);
    if (e->installed) return 2;
    static const char *sys[] = { "settings", "terminal", "monitor", "network", "bluetooth", "about" };
    for (unsigned i = 0; i < sizeof sys / sizeof sys[0]; i++) if (!strcmp(e->id, sys[i])) return 1;
    return 0;
}
static int app_order(int a) {                      /* the order within a group */
    static const char *ord[] = { "files", "web", "notes", "calendar", "music", "video", "viewer", "calc",
                                 "settings", "terminal", "monitor", "network", "bluetooth", "about" };
    for (unsigned i = 0; i < sizeof ord / sizeof ord[0]; i++) if (!strcmp(apps_get(a)->id, ord[i])) return i;
    return 100 + a;
}
static void menu_layout(void) {
    static const char *names[3] = { "Everyday", "System", "Added apps" };
    int x0 = 8, y0 = HL_BAR_H + 8, y = y0 + 80, colw = (MENU_W - 30) / 2;
    mn = 0; nsec = 0;
    for (int c = 0; c < 3; c++) {
        int list[32], n = 0;
        for (int a = 0; a < NVIS && n < 32; a++) if (app_cat(a) == c) list[n++] = a;
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && app_order(list[j]) < app_order(list[j - 1]); j--) { int t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
        if (!n) continue;
        sec_y[nsec] = y; sec_name[nsec++] = names[c]; y += 24;
        for (int k = 0; k < n && mn < 32; k++) {
            mapp[mn] = list[k];
            mrect[mn++] = (struct rect){ x0 + 10 + (k % 2) * (colw + 10), y + (k / 2) * MENU_ROW, colw, MENU_ROW - 4 };
        }
        y += (n + 1) / 2 * MENU_ROW + 6;
    }
    menu_h = y - y0 + 62;
}
static struct rect menu_rect(void) { return (struct rect){ 8, HL_BAR_H + 8, MENU_W, menu_h }; }
static struct rect menu_item(int i) { return mrect[i]; }
static struct rect menu_pin(int i) { struct rect r = mrect[i]; return (struct rect){ r.x + r.w - 34, r.y + 7, 26, 26 }; }
static struct rect menu_btn(int k) { struct rect m = menu_rect(); return (struct rect){ m.x + 14 + k * 104, m.y + m.h - 50, 96, 34 }; }   /* Sleep, Restart, Shut down */
/* right-click on a dock icon: a small "Remove from dock" / "Keep in dock" button above it */
static int ctx_app = -1, ctx_slot;
static struct rect dock_item(int i);
static struct rect ctx_rect(void) { struct rect d = dock_item(ctx_slot); return (struct rect){ d.x + d.w / 2 - 85, d.y - 54, 170, 36 }; }
static struct rect toast_rect(int i) { return (struct rect){ W - 376, HL_BAR_H + 12 + i * 102, 360, 92 }; }
/* the window buttons, on the right of the title bar: k = 0 close, 1 minimise, 2 maximise (only on resizable windows) */
static int btn_cx(struct window *w, int k) {
    int right = w->x + fw(w);
    if (k == 0) return right - 24;
    if (k == 2) return right - 64;
    return right - (w->resizable ? 104 : 64);
}
static struct rect dot(struct window *w, int k) {
    if (k == 2 && !w->resizable) return (struct rect){ 0, 0, 0, 0 };
    return (struct rect){ btn_cx(w, k) - 18, w->y + 6, 36, HL_TITLE_H - 12 };
}

/* ---------------- damage tracking ---------------- */
static struct rect dmg[32]; static int ndmg;

/* The Highland bar and dock are drawn whole, so any damage touching them grows to cover them. */
static struct rect expand(struct rect r) {
    for (bool again = true; again;) {
        again = false;
        struct rect b = bar_rect(), d = dock_area();
        if (overlap(r, b) && !contains(r, b)) { r = runion(r, b); again = true; }
        if (overlap(r, d) && !contains(r, d)) { r = runion(r, d); again = true; }
    }
    return r;
}

static void damage(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    struct rect r = expand((struct rect){ x, y, w, h });
    for (int i = 0; i < ndmg; i++)
        if (overlap(dmg[i], r)) { r = expand(runion(r, dmg[i])); dmg[i] = dmg[--ndmg]; i = -1; }
    if (ndmg == 32) { for (int i = 1; i < ndmg; i++) dmg[0] = runion(dmg[0], dmg[i]); ndmg = 1; dmg[0] = runion(dmg[0], r); return; }
    dmg[ndmg++] = r;
}
static void damage_r(struct rect r) { damage(r.x, r.y, r.w, r.h); }
static void damage_frame(struct window *w) { damage_r(frame_area(w)); }

/* ---------------- helpers ---------------- */
static int top_window(void) {                    /* the focused one (popups never are) */
    for (int i = nz - 1; i >= 0; i--) if (!wins[zorder[i]].minimized && !wins[zorder[i]].popup) return zorder[i];
    return -1;
}
static int app_index(const char *id) {
    for (int i = 0; id && i < NVIS; i++) if (!strcmp(apps_get(i)->id, id)) return i;
    return -1;
}
static bool app_running(int a) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].app && !strcmp(tasks[i].app, apps_get(a)->id)) return true;
    return false;
}

static void pixel_icon(struct canvas *c, const char **rows, int nrows, int x, int y, int s, uint32_t col) {
    for (int r = 0; r < nrows; r++)
        for (int k = 0; rows[r][k]; k++)
            if (rows[r][k] == '#') gfx_fill(c, x + k * s, y + r * s, s, s, col);
}

static void bar_text(char *app, int na, char *clk, int nc) {
    int t = top_window();
    strncpy0(app, t >= 0 ? win_name(&wins[t]) : "Desktop", na);
    struct rtc_time tm; rtc_read_local(&tm);
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const int mt[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = tm.year - (tm.mon < 3);
    int dow = (y + y / 4 - y / 100 + y / 400 + mt[(tm.mon - 1) % 12] + tm.day) % 7;
    int h12 = tm.hour % 12 ? tm.hour % 12 : 12;
    if (settings_int("time.24h")) ksnprintf(clk, nc, "%s %02d:%02d", wd[dow], tm.hour, tm.min);
    else ksnprintf(clk, nc, "%s %d:%02d %s", wd[dow], h12, tm.min, tm.hour < 12 ? "AM" : "PM");
}

/* ---------------- composition ---------------- */
static uint32_t fbcol(uint32_t c) {
    if (fb_xrgb) return c;
    struct bootinfo *b = &bootinfo;
    return ((c >> 16 & 255) >> (8 - b->r_size)) << b->r_pos | ((c >> 8 & 255) >> (8 - b->g_size)) << b->g_pos |
           ((c & 255) >> (8 - b->b_size)) << b->b_pos;
}

static const char *cursor_img[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX" };

static const char *cursor_resize[] = {   /* diagonal resize, hotspot at the centre */
    "XXXXXX       ", "X....X       ", "X...X        ", "X....X       ", "X.X..X       ", "XX X..X      ",
    "     X..X    ", "      X..X XX", "       X..X.X", "       X....X", "        X...X", "       X....X", "       XXXXXX" };
static const char *cur_text[] = {   /* hotspot 4,9 */
    "XXXXXXXXX", "X...X...X", "XXXX.XXXX", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "XXXX.XXXX", "X...X...X", "XXXXXXXXX" };
static const char *cur_hand[] = {   /* hotspot 5,1 */
    "   XXXXX", "   X...X", "   X...X", "   X...X", "   X...XXXX", "   X...X..XXXX", "   X...X..X..XXXX", "   X...X..X..X..X", "XXXX...X..X..X..X", "X......X..X..X..X", "XX..............X", " X..............X", " X..............X", " X..............X", " XXX............X", "   XX..........XX", "    X..........X", "    X..........X", "    XXXXXXXXXXXX" };
static const char *cur_wait[] = {   /* hotspot 6,9 */
    "XXXXXXXXXXXXX", "X...........X", "X...........X", "X...........X", "X...........X", "XX.........XX", " XX.......XX", "  XX.....XX", "   XX...XX", "    X...X", "   XX...XX", "  XX.....XX", " XX.......XX", "XX.........XX", "X...........X", "X...........X", "X...........X", "X...........X", "XXXXXXXXXXXXX" };
static const char *cur_crosshair[] = {   /* hotspot 8,8 */
    "       XXX", "       X.X", "       X.X", "       X.X", "       X.X", "       X.X", "       X.X", "XXXXXXXXXXXXXXXXX", "X......X X......X", "XXXXXXXXXXXXXXXXX", "       X.X", "       X.X", "       X.X", "       X.X", "       X.X", "       X.X", "       XXX" };
static const char *cur_move[] = {   /* hotspot 9,9 */
    "", "        XXX", "       XX.XX", "      XX...XX", "      X.....X", "      XXX.XXX", "   XXX  X.X  XXX", "  XX.X  X.X  X.XX", " XX..XXXX.XXXX..XX", " X...............X", " XX..XXXX.XXXX..XX", "  XX.X  X.X  X.XX", "   XXX  X.X  XXX", "      XXX.XXX", "      X.....X", "      XX...XX", "       XX.XX", "        XXX", "" };
static const char *cur_ew[] = {   /* hotspot 10,4 */
    "", "   XXX         XXX", "  XX.X         X.XX", " XX..XXXXXXXXXXX..XX", " X.................X", " XX..XXXXXXXXXXX..XX", "  XX.X         X.XX", "   XXX         XXX", "" };
static const char *cur_ns[] = {   /* hotspot 4,10 */
    "", "   XXX", "  XX.XX", " XX...XX", " X.....X", " XXX.XXX", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", "   X.X", " XXX.XXX", " X.....X", " XX...XX", "  XX.XX", "   XXX", "" };
static const char *cur_nwse[] = {   /* hotspot 9,9 */
    "XXXXXXX", "X.....X", "X....XX", "X...XX", "X..X.XX", "X.XXX.XX", "XXX XX.XX", "     XX.XX", "      XX.XX", "       XX.XX", "        XX.XX", "         XX.XX", "          XX.XX XXX", "           XX.XXX.X", "            XX.X..X", "             XX...X", "            XX....X", "            X.....X", "            XXXXXXX" };
static const char *cur_nesw[] = {   /* hotspot 9,9 */
    "            XXXXXXX", "            X.....X", "            XX....X", "             XX...X", "            XX.X..X", "           XX.XXX.X", "          XX.XX XXX", "         XX.XX", "        XX.XX", "       XX.XX", "      XX.XX", "     XX.XX", "XXX XX.XX", "X.XXX.XX", "X..X.XX", "X...XX", "X....XX", "X.....X", "XXXXXXX" };
static const char *cur_notallowed[] = {   /* hotspot 8,8 */
    "     XXXXXXX", "   XXX.....XXX", "  XX.........XX", " XX....XXX....XX", " X....XX XXX...X", "XX.....XX  XX..XX", "X...X...XX  X...X", "X..XXX...XX XX..X", "X..X XX...XX X..X", "X..XX XX...XXX..X", "X...X  XX...X...X", "XX..XX  XX.....XX", " X...XXX XX....X", " XX....XXX....XX", "  XX.........XX", "   XXX.....XXX", "     XXXXXXX" };
/* the shapes apps can ask for (WM_CURSOR_*, the same numbers as AXWIN_CURSOR_*), then the frame's own resize */
struct cursor_shape { const char **img; int rows, w, hx, hy; };
#define CUR_FRAME_RESIZE 12
static const struct cursor_shape cshape[] = {
    { cursor_img, 19, 12, 0, 0 }, { cur_text, 18, 9, 4, 9 }, { cur_hand, 19, 17, 5, 1 }, { cur_wait, 19, 13, 6, 9 },
    { cur_crosshair, 17, 17, 8, 8 }, { cur_move, 19, 18, 9, 9 }, { cur_ew, 9, 20, 10, 4 }, { cur_ns, 21, 8, 4, 10 },
    { cur_nwse, 19, 19, 9, 9 }, { cur_nesw, 19, 19, 9, 9 }, { cur_notallowed, 17, 17, 8, 8 }, { 0, 0, 0, 0, 0 },
    { cursor_resize, 13, 13, 6, 6 } };
static const char **cur_img = cursor_img;
static int cur_rows = 19, cur_w = 12, cur_hx, cur_hy, cur_shape;

/* a screen pixel of the stretched desktop (bilinear) */
static uint32_t scaled_px(int px, int py) {
    int x = sx_i[px], y = sy_i[py]; uint32_t fx = sx_f[px], fy = sy_f[py];
    int x1 = x + 1 < W ? x + 1 : x, y1 = y + 1 < H ? y + 1 : y;
    uint32_t a = bb[y * W + x], b = bb[y * W + x1], c = bb[y1 * W + x], d = bb[y1 * W + x1], out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t top = (a >> sh & 255) * (256 - fx) + (b >> sh & 255) * fx, bot = (c >> sh & 255) * (256 - fx) + (d >> sh & 255) * fx;
        out |= ((top * (256 - fy) + bot * fy) >> 16) << sh;
    }
    return out;
}

/* The cursor is drawn straight onto the screen (save-under), so moving it never recomposes. */
static void cursor_paint(int cx, int cy, bool show) {
    if (scaled) { cx = ox + cx * dw / W; cy = oy + cy * dh / H; }
    cx -= cur_hx; cy -= cur_hy;
    int SW = scaled ? PW : W, SH = scaled ? PH : H;
    for (int j = 0; j < cur_rows; j++) {
        int y = cy + j;
        if (y >= SH) break;
        if (y < 0) continue;
        for (int i = 0; cur_img[j][i]; i++) {
            int x = cx + i;
            if (x >= SW) break;
            if (x < 0) continue;
            char ch = cur_img[j][i];
            if (ch == ' ') continue;
            uint32_t c;
            if (show) c = ch == 'X' ? 0x000000 : 0xFFFFFF;
            else if (!scaled) c = bb[y * W + x];
            else c = x >= ox && x < ox + dw && y >= oy && y < oy + dh ? scaled_px(x - ox, y - oy) : 0;
            fb32[(size_t)y * fb_stride + x] = fbcol(c);
        }
    }
}

static void flush_rect(struct rect r) {
    if (scaled) {
        int x0 = r.x * dw / W - 1, x1 = ((r.x + r.w) * dw + W - 1) / W + 1, y0 = r.y * dh / H - 1, y1 = ((r.y + r.h) * dh + H - 1) / H + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dw) x1 = dw;
        if (y1 > dh) y1 = dh;
        for (int py = y0; py < y1; py++) {
            volatile uint32_t *d = fb32 + (size_t)(oy + py) * fb_stride + ox;
            for (int px = x0; px < x1; px++) d[px] = fbcol(scaled_px(px, py));
        }
        int cx = mx * dw / W - cur_hx - 2, cy = my * dh / H - cur_hy - 2;
        if (cx < x1 && cx + cur_w + 4 > x0 && cy < y1 && cy + cur_rows + 4 > y0) cursor_paint(mx, my, true);
        return;
    }
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *s = bb + j * W + r.x;
        volatile uint32_t *d = fb32 + (size_t)j * fb_stride + r.x;
        if (fb_xrgb) for (int i = 0; i < r.w; i++) d[i] = s[i];
        else for (int i = 0; i < r.w; i++) d[i] = fbcol(s[i]);
    }
    if (overlap(r, (struct rect){ mx - cur_hx, my - cur_hy, cur_w, cur_rows })) cursor_paint(mx, my, true);
}

/* draw a message straight onto the screen (used by the watchdog when the system is stuck) */
void desktop_emergency(const char *const *lines, int n) {
    if (!bb || !fb32) return;
    struct canvas c = { bb, W, H, W, 0, 0, W, H };
    int h = 16 + n * 20;
    gfx_fill(&c, 20, H - 120 - h, W - 40 > 780 ? 780 : W - 40, h, 0x7F1D1D);
    for (int i = 0; i < n; i++) gfx_mono_text(&c, 30, H - 120 - h + 8 + i * 20, lines[i], 0xFFFFFF);
    flush_rect((struct rect){ 20, H - 120 - h, W - 40 > 780 ? 780 : W - 40, h });
}

static void set_cursor(int shape) {
    if (shape < 0 || shape > CUR_FRAME_RESIZE) shape = 0;
    if (shape == cur_shape) return;
    const struct cursor_shape *c = &cshape[shape];
    cursor_paint(mx, my, false);
    cur_shape = shape; cur_img = c->img; cur_rows = c->rows; cur_w = c->w; cur_hx = c->hx; cur_hy = c->hy;
    cursor_paint(mx, my, true);
}

static void draw_window(int idx, struct rect r) {
    struct window *w = &wins[idx];
    if (w->popup) { (void)r; gfx_blit(&cv, w->x, w->y, w->buf, w->w, w->h, w->w); return; }
    bool active = idx == top_window();
    char t[80];
    bool hung = w->close_req && uptime_ms() - w->close_req > 1000;
    ksnprintf(t, sizeof t, "%s%s", w->title, hung ? " (Not responding)" : "");
    /* Highland frame, clipped to the damaged rectangle via a sub-framebuffer */
    struct framebuffer sub = { bb + r.y * W + r.x, r.w, r.h, W * 4 };
    hl_render_window(&sub, w->x - r.x, w->y - r.y, fw(w), fh(w), t, HL_WIN_BG, active);
    gfx_blit_rr(&cv, w->x + 1, w->y + TITLE, w->buf, w->w, w->h, w->w, w->x + 1, w->y + 1, fw(w) - 2, fh(w) - 2, HL_WIN_RADIUS - 1);
    /* title on the left, quiet buttons on the right (close turns red under the mouse) */
    int cy = w->y + HL_TITLE_H / 2;
    int hk = hover_ctl >= 1000 && (hover_ctl - 1000) / 4 == idx ? (hover_ctl - 1000) % 4 : -1;
    int maxw = btn_cx(w, 1) - 18 - (w->x + 20) - 12;
    char tt[80]; strncpy0(tt, t, sizeof tt);
    for (int n = strlen(tt); n > 3 && gfx_text_width(&font_ui, tt) > maxw; n--) { tt[n - 1] = 0; tt[n - 2] = tt[n - 3] = '.'; }
    gfx_text(&cv, &font_ui, w->x + 20, cy + 5, active ? HL_TEXT_DIM : HL_TEXT_FAINT, tt);
    for (int k = 0; k < 3; k++) {
        if (k == 2 && !w->resizable) continue;
        struct rect b = dot(w, k); int bx = btn_cx(w, k);
        uint32_t ic = active ? HL_TEXT_FAINT : 0x55534Bu;
        if (hk == k) {
            gfx_rrect(&cv, b.x, b.y, b.w, b.h, 7, k == 0 ? HL_RED : 0x2A2A28u, 255);
            ic = k == 0 ? 0x111111u : HL_TEXT;
        }
        if (k == 0) { gfx_line(&cv, bx - 5, cy - 5, bx + 5, cy + 5, ic, 1); gfx_line(&cv, bx - 5, cy + 5, bx + 5, cy - 5, ic, 1); }
        else if (k == 1) gfx_fill(&cv, bx - 5, cy, 11, 1, ic);
        else if (w->maximized) {                               /* restore: two overlapping squares */
            gfx_rrect_outline(&cv, bx - 3, cy - 6, 9, 9, 1, ic, 255);
            gfx_fill(&cv, bx - 6, cy - 3, 9, 9, hk == k ? 0x2A2A28u : 0x181817u);
            gfx_rrect_outline(&cv, bx - 6, cy - 3, 9, 9, 1, ic, 255);
        } else gfx_rrect_outline(&cv, bx - 5, cy - 5, 11, 11, 1, ic, 255);
    }
}

static void draw_dock_overlay(void) {
    if (ctx_app >= 0) {
        struct rect c = ctx_rect();
        bool h = hover_ctl == 600;
        gfx_rrect(&cv, c.x - 2, c.y + 6, c.w + 4, c.h + 2, 10, HL_SHADOW, 120);
        gfx_rrect(&cv, c.x, c.y, c.w, c.h, 9, h ? 0x2A2A28 : HL_DOCK_BG, 245);
        gfx_rrect_outline(&cv, c.x, c.y, c.w, c.h, 9, HL_WIN_BORDER, 255);
        const char *l = is_pinned(ctx_app) ? "Remove from dock" : "Keep in dock";
        gfx_text(&cv, &font_ui, c.x + (c.w - gfx_text_width(&font_ui, l)) / 2, c.y + 23, HL_TEXT, l);
        return;
    }
    if (hover_ctl >= 100 && hover_ctl < 100 + ndock) {
        int i = hover_ctl - 100;
        struct rect r = dock_item(i);
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 14, 0xFFFFFF, 26);
        const char *l = app_name(dock_app[i]);
        int lw = gfx_text_width(&font_ui, l) + 22;
        int bx = r.x + r.w / 2 - lw / 2, by = dock_rect().y - 38;
        gfx_rrect(&cv, bx, by, lw, 28, 8, HL_DOCK_BG, 235);
        gfx_rrect_outline(&cv, bx, by, lw, 28, 8, HL_WIN_BORDER, 255);
        gfx_text(&cv, &font_ui, bx + 11, by + 19, HL_TEXT, l);
    }
}

static void draw_menu(void) {
    struct rect m = menu_rect();
    gfx_rrect(&cv, m.x - 2, m.y + 10, m.w + 4, m.h + 4, HL_WIN_RADIUS + 2, HL_SHADOW, HL_SHADOW_ALPHA);
    gfx_rrect(&cv, m.x, m.y, m.w, m.h, HL_WIN_RADIUS, HL_WIN_BG, HL_WIN_ALPHA);
    gfx_rrect_outline(&cv, m.x, m.y, m.w, m.h, HL_WIN_RADIUS, HL_ACCENT, 90);
    ax_logo(&full, m.x + 20, m.y + 14, ax_logo_img(0), HL_CREAM, 255);
    gfx_mono_text(&cv, m.x + 20, m.y + 40, OS_NAME " " OS_VERSION " - Axiom Core", HL_TEXT_FAINT);
    gfx_blend(&cv, m.x + 1, m.y + 66, m.w - 2, 1, 0xFFFFFF, 16);
    for (int k = 0; k < nsec; k++) gfx_mono_text(&cv, m.x + 22, sec_y[k] + 4, sec_name[k], HL_ACCENT);
    for (int i = 0; i < mn; i++) {
        struct rect r = menu_item(i); int ap = mapp[i];
        if (hover_ctl == 200 + i) gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, HL_ACCENT, 36);
        const uint32_t *ic = hl_app_icon(apps_get(ap)->icon, 1);
        for (int j = 0; j < 34; j++) {                    /* the app's icon, blended */
            int yy = r.y + 3 + j;
            if (yy < cv.cy0 || yy >= cv.cy1) continue;
            for (int k = 0; k < 34; k++) {
                int xx = r.x + 6 + k; uint32_t s = ic[j * 34 + k], al = s >> 24;
                if (!al || xx < cv.cx0 || xx >= cv.cx1) continue;
                uint32_t *d = &cv.px[yy * cv.stride + xx], dv = *d;
                *d = ((s >> 16 & 255) * al + (dv >> 16 & 255) * (255 - al)) / 255 << 16 |
                     ((s >> 8 & 255) * al + (dv >> 8 & 255) * (255 - al)) / 255 << 8 | ((s & 255) * al + (dv & 255) * (255 - al)) / 255;
            }
        }
        gfx_mono_text(&cv, r.x + 50, r.y + 12, app_name(ap), hover_ctl == 200 + i ? HL_TEXT : HL_TEXT_DIM);
        if (app_running(ap)) gfx_circle(&cv, r.x + 50 + (int)strlen(app_name(ap)) * 8 + 10, r.y + r.h / 2, 3, HL_GREEN, 255);
        /* in the dock? */
        struct rect p = menu_pin(i); bool pin = is_pinned(ap), hp = hover_ctl == 500 + i;
        if (pin) { gfx_rrect(&cv, p.x + 4, p.y + 4, p.w - 8, p.h - 8, 5, HL_ACCENT, hp ? 200 : 255); gfx_fill(&cv, p.x + 9, p.y + p.h - 11, p.w - 18, 2, HL_NAVY); }
        else gfx_rrect_outline(&cv, p.x + 4, p.y + 4, p.w - 8, p.h - 8, 5, hp ? HL_TEXT : HL_TEXT_FAINT, 255);
    }
    gfx_mono_text(&cv, m.x + m.w - 228, m.y + m.h - 44, "Filled square: in the dock.", HL_TEXT_FAINT);
    gfx_mono_text(&cv, m.x + m.w - 228, m.y + m.h - 28, "Click it to add or remove.", HL_TEXT_FAINT);
    gfx_blend(&cv, m.x + 1, m.y + m.h - 64, m.w - 2, 1, 0xFFFFFF, 16);
    static const char *bl[] = { "Sleep", "Restart", "Shut down" };
    for (int k = 0; k < 3; k++) {
        struct rect r = menu_btn(k);
        bool h = hover_ctl == 300 + k, red = k == 2 && h;
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, red ? HL_RED : h ? 0x2A2A28 : 0x1C1C1B, 255);
        gfx_rrect_outline(&cv, r.x, r.y, r.w, r.h, 8, HL_WIN_BORDER, 255);
        int tw = (int)strlen(bl[k]) * 8;
        gfx_mono_text(&cv, r.x + (r.w - tw) / 2, r.y + 9, bl[k], red ? 0xFFFFFF : HL_TEXT);
    }
}

static int wrap_lines(const char *s, char out[3][80], int maxw) {
    int n = 0; char line[80] = ""; int ll = 0;
    while (*s && n < 3) {
        const char *we = s; while (*we && *we != ' ') we++;
        char word[80]; int wl = we - s < 79 ? we - s : 79;
        memcpy(word, s, wl); word[wl] = 0;
        char trial[160]; ksnprintf(trial, sizeof trial, "%s%s%s", line, ll ? " " : "", word);
        if (ll && gfx_text_width(&font_ui, trial) > maxw) { strncpy0(out[n++], line, 80); strncpy0(line, word, 80); }
        else strncpy0(line, trial, 80);
        ll = strlen(line);
        s = *we ? we + 1 : we;
    }
    if (ll && n < 3) strncpy0(out[n++], line, 80);
    return n;
}

static void draw_toasts(void) {
    for (int i = 0; i < 4; i++) {
        if (!toasts[i].used) continue;
        struct rect r = toast_rect(i);
        int tl = strlen(toasts[i].title);
        bool bad = tl >= 7 && !strcmp(toasts[i].title + tl - 7, "crashed");
        gfx_rrect(&cv, r.x - 2, r.y + 8, r.w + 4, r.h + 4, HL_WIN_RADIUS + 2, HL_SHADOW, HL_SHADOW_ALPHA);
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, HL_WIN_RADIUS, HL_WIN_BG, HL_WIN_ALPHA);
        gfx_rrect_outline(&cv, r.x, r.y, r.w, r.h, HL_WIN_RADIUS, bad ? HL_RED : HL_ACCENT, 110);
        gfx_circle(&cv, r.x + 22, r.y + 23, 5, bad ? HL_RED : HL_ACCENT, 255);
        gfx_mono_text(&cv, r.x + 36, r.y + 15, toasts[i].title, HL_TEXT);
        char lines[3][80];
        int n = wrap_lines(toasts[i].msg, lines, r.w - 52);
        for (int k = 0; k < n; k++) gfx_text(&cv, &font_ui, r.x + 36, r.y + 52 + k * 18, HL_TEXT_DIM, lines[k]);
    }
}

static struct rect content_rect(struct window *w) { return (struct rect){ cx0(w), cy0(w), w->w, w->h }; }
static struct rect intersect(struct rect a, struct rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){ x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
}

/* ---------------- lock screen and screen off ---------------- */
static bool locked, screen_off;
static uint64_t last_input;
static char lock_pw[64]; static int lock_len;
static char lock_msg[80];

static void draw_lock_screen(void) {
    gfx_blit(&cv, 0, 0, wall, W, H, W);
    gfx_blend(&cv, 0, 0, W, H, 0x000000, 70);
    struct rtc_time t; rtc_read_local(&t);
    static const char *wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mn[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    static const int mt[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y0 = t.year - (t.mon < 3), dow = (y0 + y0 / 4 - y0 / 100 + y0 / 400 + mt[(t.mon - 1) % 12] + t.day) % 7;
    char clk[32], date[64];
    if (settings_int("time.24h")) ksnprintf(clk, sizeof clk, "%02d:%02d", t.hour, t.min);
    else ksnprintf(clk, sizeof clk, "%d:%02d", t.hour % 12 ? t.hour % 12 : 12, t.min);
    ksnprintf(date, sizeof date, "%s %d %s", wd[dow], t.day, mn[(t.mon - 1) % 12]);
    int cy = H / 2 - 90;
    gfx_text_center(&cv, &font_big, 0, W, cy, HL_TEXT, clk);
    gfx_text_center(&cv, &font_ui, 0, W, cy + 30, HL_TEXT_DIM, date);
    int fw_ = 300, fx = (W - fw_) / 2, fy = cy + 70;
    gfx_rrect(&cv, fx, fy, fw_, 40, 12, HL_TERM_BG, 230);
    gfx_rrect_outline(&cv, fx, fy, fw_, 40, 12, HL_ACCENT, 200);
    char dots[64]; int n = lock_len < 40 ? lock_len : 40;
    for (int i = 0; i < n; i++) dots[i] = '*';
    dots[n] = 0;
    if (lock_len) gfx_text_center(&cv, &font_title, fx, fw_, fy + 28, HL_TEXT, dots);
    else gfx_text_center(&cv, &font_ui, fx, fw_, fy + 26, HL_TEXT_FAINT, "Password");
    gfx_text_center(&cv, &font_ui, 0, W, fy + 70, lock_msg[0] ? HL_RED : HL_TEXT_FAINT, lock_msg[0] ? lock_msg : "Type your password and press Enter");
    { const struct logo_img *l = ax_logo_img(0); ax_logo(&full, (W - l->w) / 2, H - 52, l, HL_CREAM, 150); }
}

void desktop_lock(void) {
    if (!settings_get("lock.password")[0]) { desktop_notify("No password set", "Set one in Settings > Lock screen to lock this computer."); return; }
    if (locked) return;
    locked = true; lock_len = 0; lock_msg[0] = 0; memset(lock_pw, 0, sizeof lock_pw);
    if (menu_open) menu_open = false;
    damage(0, 0, W, H);
}

static void lock_key(int k) {
    if (k == '\n') {
        lock_pw[lock_len] = 0;
        if (settings_check_password(lock_pw)) { locked = false; lock_msg[0] = 0; damage(0, 0, W, H); }
        else { strncpy0(lock_msg, "That password isn't right. Try again.", sizeof lock_msg); sleep_ms(600); }
        memset(lock_pw, 0, sizeof lock_pw); lock_len = 0;
    } else if (k == '\b') { if (lock_len) lock_pw[--lock_len] = 0; }
    else if (k == 27) { lock_len = 0; memset(lock_pw, 0, sizeof lock_pw); }
    else if (k >= 32 && k <= 255 && k != 127 && lock_len < 63) { lock_pw[lock_len++] = (char)k; lock_msg[0] = 0; }
    damage(0, 0, W, H);
}

/* ---------------- sleep, shut down, restart ---------------- */
/* Sleep: the screen goes dark (with AXIOM's own graphics driver the monitor itself goes into standby), the desktop
 * looks at the keyboard and mouse less often, and a key, the mouse or the power button wakes it - to the lock screen
 * if there's a password. (Suspend-to-RAM, where the whole PC powers down, would need every driver to restart its
 * hardware afterwards; not yet.) */
static uint64_t off_since;
static void toggle_menu(bool open);
static void screen_sleep(void) { if (!screen_off) { screen_off = true; off_since = uptime_ms(); damage(0, 0, W, H); intelgfx_display(false); } }
static bool screen_wake(void) {                           /* false if it was asleep only a moment (the click that sent it) */
    if (!screen_off) return true;
    if (uptime_ms() - off_since < 1500) return false;
    intelgfx_display(true); screen_off = false; damage(0, 0, W, H); return false;
}
static void go_to_sleep(void) { if (settings_get("lock.password")[0]) desktop_lock(); screen_sleep(); }

/* Shut down and Restart first ask every app to close, as its close button would - an app with unsaved work can
 * ask about it - then flush the disks and switch off. If an app is still open after a few seconds, a box at the
 * top says which and offers "anyway" or Cancel. */
static int power_act;                                     /* 0 no, 1 shutting down, 2 restarting */
static uint64_t power_deadline; static bool power_stuck;
static char power_msg[96];                                /* the last full-screen message */
static struct rect power_rect(void) { return (struct rect){ (W - 560) / 2, HL_BAR_H + 16, 560, 150 }; }
static struct rect power_btn(int k) { struct rect b = power_rect(); return (struct rect){ b.x + b.w - 360 + k * 180, b.y + b.h - 52, 166, 36 }; }  /* Cancel, anyway */
static int windows_open(char *first, int n) {
    int c = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && !wins[i].popup) { if (!c && first) strncpy0(first, win_name(&wins[i]), n); c++; }
    return c;
}
static void power_start(int act) {
    if (power_act) return;
    if (menu_open) toggle_menu(false);
    power_act = act; power_stuck = false; power_deadline = uptime_ms() + 6000;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].owner) {
        struct dunc_event e = { EV_CLOSE, wins[i].id, 0, 0, 0, 0 };
        event_push(wins[i].owner, &e);
    }
    damage_r(power_rect());
}
static void power_cancel(void) { damage_r(power_rect()); power_act = 0; power_stuck = false; }
static void power_screen(const char *msg) {               /* the whole screen: dark, one line in the middle */
    strncpy0(power_msg, msg, sizeof power_msg);
    struct rect all = { 0, 0, W, H };
    gfx_clip(&cv, 0, 0, W, H); gfx_fill(&cv, 0, 0, W, H, 0x0B0B0A);
    ax_logo(&full, W / 2 - 60, H / 2 - 70, ax_logo_img(0), HL_CREAM, 255);
    gfx_text_center(&cv, &font_ui, 0, W, H / 2 + 20, HL_TEXT_DIM, power_msg);
    flush_rect(all);
}
static void power_finish(void) {
    int act = power_act;
    power_screen(act == 2 ? "Restarting..." : "Shutting down...");
    if (act == 2) acpi_reboot(); else acpi_poweroff();
    power_screen("It's now safe to turn off your computer.");   /* power-off isn't available here */
    for (;;) task_sleep(1000);
}
static void power_tick(uint64_t now) {
    if (acpi_power_button()) {                            /* the PC's power button: wake if asleep, else shut down */
        if (screen_off) { off_since = 0; screen_wake(); last_input = now; }
        else power_start(1);
    }
    if (!power_act) return;
    char first[48] = "";
    if (!windows_open(first, sizeof first)) power_finish();
    else if (now > power_deadline && !power_stuck) { power_stuck = true; damage_r(power_rect()); }
}
static void draw_power(void) {
    struct rect b = power_rect();
    gfx_rrect(&cv, b.x - 2, b.y + 8, b.w + 4, b.h + 4, 18, HL_SHADOW, 120);
    gfx_rrect(&cv, b.x, b.y, b.w, b.h, 14, HL_WIN_BG, 248);
    gfx_rrect_outline(&cv, b.x, b.y, b.w, b.h, 14, HL_ACCENT, 110);
    const char *what = power_act == 2 ? "Restarting" : "Shutting down";
    gfx_text(&cv, &font_ui_bold, b.x + 22, b.y + 32, HL_TEXT, what);
    char first[48] = "", m[140]; int n = windows_open(first, sizeof first);
    if (power_stuck) ksnprintf(m, sizeof m, "%s%s hasn't closed. It may be asking you to save something.", first, n > 1 ? " (and others)" : "");
    else ksnprintf(m, sizeof m, "Closing apps... (%d open)", n);
    gfx_text(&cv, &font_ui, b.x + 22, b.y + 62, HL_TEXT_DIM, m);
    for (int k = 0; k < 2; k++) {
        struct rect r = power_btn(k);
        const char *l = k ? (power_act == 2 ? "Restart anyway" : "Shut down anyway") : "Cancel";
        bool h = hover_ctl == 320 + k;
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, k && h ? HL_RED : h ? 0x2A2A28 : 0x1C1C1B, 255);
        gfx_rrect_outline(&cv, r.x, r.y, r.w, r.h, 8, HL_WIN_BORDER, 255);
        int tw = (int)strlen(l) * 8;
        gfx_mono_text(&cv, r.x + (r.w - tw) / 2, r.y + 10, l, k && h ? 0xFFFFFF : HL_TEXT);
    }
}

/* the area a window fills when dragged to the left/right edge or the top */
static struct rect snap_rect(int z) {
    int top = HL_BAR_H, h = H - top;
    if (z == 1) return (struct rect){ 0, top, W / 2, h };
    if (z == 2) return (struct rect){ W / 2, top, W - W / 2, h };
    return (struct rect){ 0, top, W, h };
}
static struct rect switcher_rect(void) { int h = 70 + sw_n * 44; if (h > H - 120) h = H - 120; return (struct rect){ (W - 460) / 2, (H - h) / 2, 460, h }; }
static void draw_switcher(void) {
    struct rect s = switcher_rect();
    gfx_rrect(&cv, s.x - 2, s.y + 8, s.w + 4, s.h + 4, 18, HL_SHADOW, 120);
    gfx_rrect(&cv, s.x, s.y, s.w, s.h, 16, HL_WIN_BG, 245);
    gfx_rrect_outline(&cv, s.x, s.y, s.w, s.h, 16, HL_CREAM, 70);
    gfx_text(&cv, &font_ui_bold, s.x + 20, s.y + 30, HL_TEXT_DIM, "Switch to");
    for (int i = 0; i < sw_n && 50 + i * 44 + 40 < s.h; i++) {
        struct window *w = &wins[sw_list[i]];
        if (!w->used) continue;
        int y = s.y + 44 + i * 44;
        if (i == sw_sel) gfx_rrect(&cv, s.x + 10, y, s.w - 20, 40, 10, HL_CREAM, 40);
        const struct app_entry *ae = win_app(w) && win_app(w)[0] ? apps_find(win_app(w)) : 0;
        const uint32_t *ic = hl_app_icon(ae ? ae->icon : "gear", 1);
        for (int j = 0; j < 34; j++) for (int k = 0; k < 34; k++) {
            uint32_t p = ic[j * 34 + k], al = p >> 24;
            if (al) { uint32_t *d = &bb[(y + 3 + j) * W + s.x + 18 + k]; *d = al == 255 ? (p & 0xFFFFFF) : gfx_mix(*d, p & 0xFFFFFF, al); }
        }
        char t[48]; strncpy0(t, w->title, sizeof t);
        gfx_text(&cv, i == sw_sel ? &font_ui_bold : &font_ui, s.x + 64, y + 26, i == sw_sel ? HL_TEXT : HL_TEXT_DIM, t);
        if (w->minimized) gfx_text(&cv, &font_ui, s.x + s.w - 110, y + 26, HL_TEXT_FAINT, "minimised");
    }
}

/* after switching to the monitor's full size (Settings > Display): keep it? No answer in 15 s = go back */
static uint64_t keep_until;
static struct rect keep_rect(void) { return (struct rect){ (W - 520) / 2, (H - 170) / 2, 520, 170 }; }
static struct rect keep_btn(int k) { struct rect b = keep_rect(); return (struct rect){ b.x + b.w - 300 + k * 150, b.y + b.h - 54, 136, 38 }; }
static void draw_keep(void) {
    struct rect b = keep_rect();
    gfx_rrect(&cv, b.x - 2, b.y + 10, b.w + 4, b.h + 4, 18, HL_SHADOW, 140);
    gfx_rrect(&cv, b.x, b.y, b.w, b.h, 16, HL_WIN_BG, 250);
    gfx_rrect_outline(&cv, b.x, b.y, b.w, b.h, 16, HL_ACCENT, 120);
    char t[96]; ksnprintf(t, sizeof t, "Keep this screen size (%ux%u)?", bootinfo.fb_w, bootinfo.fb_h);
    gfx_text(&cv, &font_ui_bold, b.x + 24, b.y + 40, HL_TEXT, t);
    long left = (long)(keep_until - uptime_ms()) / 1000 + 1; if (left < 0) left = 0;
    ksnprintf(t, sizeof t, "AXIOM goes back to the previous size in %ld s.", left);
    gfx_text(&cv, &font_ui, b.x + 24, b.y + 68, HL_TEXT_DIM, t);
    static const char *l[2] = { "Go back", "Keep" };
    for (int k = 0; k < 2; k++) {
        struct rect r = keep_btn(k);
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 9, k ? HL_ACCENT : 0x2A2A28, 255);
        gfx_text(&cv, &font_ui_bold, r.x + (r.w - gfx_text_width(&font_ui_bold, l[k])) / 2, r.y + 25, k ? HL_NAVY : HL_TEXT, l[k]);
    }
}

static void compose(struct rect r) {
    gfx_clip(&cv, r.x, r.y, r.w, r.h);
    if (screen_off) { gfx_fill(&cv, r.x, r.y, r.w, r.h, 0x000000); flush_rect(r); return; }
    if (locked) { draw_lock_screen(); flush_rect(r); return; }
    /* Occlusion: the back buffer keeps the last frame, so when the damage lies inside a
     * window's content only its pixels need redrawing - nothing below it, and not its frame
     * (the rounded-corner pixels outside the content mask are unchanged). */
    int start = -1;
    for (int z = nz - 1; z >= 0; z--) {
        struct window *w = &wins[zorder[z]];
        if (!w->minimized && contains(content_rect(w), r)) { start = z; break; }
    }
    if (start < 0) {
        gfx_blit(&cv, 0, 0, wall, W, H, W); start = 0;
        if (overlap(r, dock_area())) {                     /* the dock sits on the desktop, under the windows */
            int running[HL_MAX_DOCK]; const char *icons[HL_MAX_DOCK];
            for (int i = 0; i < ndock; i++) { running[i] = app_running(dock_app[i]); icons[i] = apps_get(dock_app[i])->icon; }
            if (ndock) hl_render_dock(&full, icons, running, ndock);
        }
    }
    for (int z = start; z < nz; z++) {
        struct window *w = &wins[zorder[z]];
        if (w->minimized || !overlap(frame_area(w), r)) continue;
        if (!w->popup && contains(content_rect(w), intersect(r, frame_area(w))))
            gfx_blit_rr(&cv, w->x + 1, w->y + TITLE, w->buf, w->w, w->h, w->w, w->x + 1, w->y + 1, fw(w) - 2, fh(w) - 2, HL_WIN_RADIUS - 1);
        else draw_window(zorder[z], r);
    }
    if (overlap(r, bar_rect())) {
        char app[48], clk[32]; bar_text(app, sizeof app, clk, sizeof clk);
        hl_render_topbar(&full, app, clk);
        if (hover_ctl == 50 || menu_open) { struct rect c = crown_btn(); gfx_rrect(&cv, c.x, c.y, c.w, c.h, 8, HL_CREAM, 22); }
        /* Bluetooth status, left of the clock */
        int bx = W - 16 - (int)strlen(clk) * 8 - 22, cy = HL_BAR_H / 2;
        uint32_t bc = bt_present() ? HL_TEXT_DIM : 0x3C3B37;
        gfx_line(&cv, bx, cy - 7, bx, cy + 7, bc, 1);
        gfx_line(&cv, bx, cy - 7, bx + 4, cy - 3, bc, 1); gfx_line(&cv, bx + 4, cy - 3, bx - 4, cy + 4, bc, 1);
        gfx_line(&cv, bx - 4, cy - 4, bx + 4, cy + 3, bc, 1); gfx_line(&cv, bx + 4, cy + 3, bx, cy + 7, bc, 1);
        /* network: two linked boxes, bright when connected */
        int nx = bx - 30;
        uint32_t nc = net_ready() ? HL_TEXT_DIM : 0x3C3B37;
        if (net_present()) {
            gfx_rrect_outline(&cv, nx - 4, cy - 8, 8, 6, 1, nc, 255);
            gfx_rrect_outline(&cv, nx - 10, cy + 2, 8, 6, 1, nc, 255);
            gfx_rrect_outline(&cv, nx + 2, cy + 2, 8, 6, 1, nc, 255);
            gfx_fill(&cv, nx, cy - 2, 1, 2, nc); gfx_fill(&cv, nx - 6, cy, 13, 1, nc);
            gfx_fill(&cv, nx - 6, cy, 1, 2, nc); gfx_fill(&cv, nx + 6, cy, 1, 2, nc);
        }
    }
    draw_dock_overlay();
    if (menu_open) draw_menu();
    if (snap_zone) { struct rect s = snap_rect(snap_zone); gfx_rrect(&cv, s.x + 6, s.y + 6, s.w - 12, s.h - 12, 14, HL_CREAM, 34); gfx_rrect_outline(&cv, s.x + 6, s.y + 6, s.w - 12, s.h - 12, 14, HL_CREAM, 120); }
    if (sw_until) draw_switcher();
    if (keep_until) draw_keep();
    if (power_act) draw_power();
    draw_toasts();
    flush_rect(r);
}

/* ---------------- window management ---------------- */
static void focus_changed(int old_top) {
    int t = top_window();
    if (t == old_top) return;
    if (old_top >= 0 && wins[old_top].used) {
        struct dunc_event e = { EV_BLUR, wins[old_top].id, 0, 0, 0, 0 };
        event_push(wins[old_top].owner, &e);
        damage_frame(&wins[old_top]);
    }
    if (t >= 0) {
        struct dunc_event e = { EV_FOCUS, wins[t].id, 0, 0, 0, 0 };
        event_push(wins[t].owner, &e);
        damage_frame(&wins[t]);
    }
    damage_r(bar_rect());
}

static void raise(int idx) {                     /* (popups stay above the other windows; a new one on top) */
    int old = top_window(), k = 0, z[MAX_WIN];
    for (int i = 0; i < nz; i++) if (zorder[i] != idx && !wins[zorder[i]].popup) z[k++] = zorder[i];
    if (!wins[idx].popup) z[k++] = idx;
    for (int i = 0; i < nz; i++) if (zorder[i] != idx && wins[zorder[i]].popup) z[k++] = zorder[i];
    if (wins[idx].popup) z[k++] = idx;
    memcpy(zorder, z, sizeof z[0] * k); nz = k;
    wins[idx].minimized = false;
    damage_frame(&wins[idx]);
    focus_changed(old);
}

static void update_hover(void);
static void remove_window(int idx) {
    int old = top_window();
    struct window *w = &wins[idx];
    damage_frame(w);
    for (int i = 0; i < w->pages; i++) page_free((uint8_t *)w->buf + i * 4096);
    w->used = false;
    int k = 0;
    for (int i = 0; i < nz; i++) if (zorder[i] != idx) zorder[k++] = zorder[i];
    nz = k;
    if (drag_win == idx) drag_win = -1;
    if (resize_win == idx) resize_win = -1;
    if (capture_win == idx) capture_win = -1;
    if (hover_win == idx) hover_win = -1;
    hover_ctl = 0;
    focus_changed(old == idx ? -1 : old);
    update_hover();                                  /* (the pointer's shape may have been this window's) */
}

static struct window *find(struct task *t, int id) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].id == id && wins[i].owner == t) return &wins[i];
    return 0;
}

struct window *wm_create(struct task *t, int w, int h, const char *title, uint32_t **buf, int *pages, uint64_t uva, int flags) {
    return wm_create_at(t, w, h, title, buf, pages, uva, flags, WM_ANYWHERE, WM_ANYWHERE);
}
/* x, y: where its content goes on the screen (WM_ANYWHERE: the desktop chooses) */
struct window *wm_create_at(struct task *t, int w, int h, const char *title, uint32_t **buf, int *pages, uint64_t uva, int flags, int x, int y) {
    int top = HL_BAR_H + 8, bottom = dock_rect().y - 8;
    bool popup = flags & WIN_POPUP;
    if (!desktop_ok || w < 1 || h < 1) return 0;
    if (!popup && (w < 100 || h < 60 || w > W - 20 || h + TITLE + 1 > H - HL_BAR_H - 16)) return 0;
    if (popup && (w > W || h > H)) return 0;
    int idx = -1, open = 0;
    for (int i = 0; i < MAX_WIN; i++) { if (!wins[i].used && idx < 0) idx = i; if (wins[i].used) open++; }
    if (idx < 0) return 0;
    struct window *win = &wins[idx];
    int np = (w * h * 4 + 4095) / 4096;
    uint32_t *b = page_alloc(np);
    if (!b) return 0;
    memset(win, 0, sizeof *win);
    win->used = true; win->id = next_wid++; win->owner = t; win->w = w; win->h = h;
    win->buf = b; win->pages = np; win->uva = uva;
    win->resizable = !popup && (flags & WIN_RESIZABLE);
    win->popup = popup; win->move_events = flags & WIN_MOVE_EVENTS; win->sent_x = win->sent_y = -100000;
    win->minimized = flags & WIN_HIDDEN;
    win->want_w = w; win->want_h = h;
    strncpy0(win->title, title, sizeof win->title);
    for (int i = 0; i < w * h; i++) b[i] = HL_WIN_BG;
    int off = (open % 5) * 32 - 48;
    win->x = (W - fw(win)) / 2 + off;
    win->y = top + (bottom - top - fh(win)) / 2 + off;
    if (win->x < 8) win->x = 8;
    if (win->x + fw(win) > W - 8) win->x = W - 8 - fw(win);
    if (win->y + fh(win) > bottom) win->y = bottom - fh(win);
    if (win->y < top) win->y = top;
    if (x != WM_ANYWHERE) { win->x = popup ? x : x - 1; win->y = popup ? y : y - TITLE; if (!popup && win->y < HL_BAR_H) win->y = HL_BAR_H; }
    raise(idx);
    *buf = b; *pages = np;
    return win;
}

int wm_id(struct window *w) { return w->id; }

static int max_content_w(void) { return W - 2; }
static int max_content_h(void) { return H - HL_BAR_H - TITLE - 1; }

/* Ask the app to redraw at a new size (it answers with SYS_WIN_RESIZE). */
static void request_size(struct window *w, int cw, int ch) {
    if (cw < MIN_W) cw = MIN_W;
    if (ch < MIN_H) ch = MIN_H;
    if (cw > max_content_w()) cw = max_content_w();
    if (ch > H - HL_BAR_H - TITLE - 1) ch = H - HL_BAR_H - TITLE - 1;
    w->want_w = cw; w->want_h = ch;
    if (w->resize_sent || (cw == w->w && ch == w->h)) return;
    struct dunc_event e = { EV_RESIZE, w->id, cw, ch, 0, 0 };
    event_push(w->owner, &e);
    w->resize_sent = true;
}

/* Called by the app: swap in a buffer of the size we asked for. */
bool wm_resize(struct task *t, int id, int *nw, int *nh) {
    struct window *w = find(t, id);
    if (!w || !w->resizable) return false;
    int cw = w->want_w, ch = w->want_h;
    w->resize_sent = false;
    if (cw == w->w && ch == w->h) { *nw = cw; *nh = ch; return true; }
    int np = (cw * ch * 4 + 4095) / 4096;
    uint32_t *nb = page_alloc(np);
    if (!nb) return false;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            nb[y * cw + x] = (x < w->w && y < w->h) ? w->buf[y * w->w + x] : HL_WIN_BG;
    damage_frame(w);
    if (w->uva) vm_unmap(t->pml4, w->uva, w->pages);           /* (uva 0: a Linux program's window, mapped by linux.c) */
    for (int i = 0; i < w->pages; i++) page_free((uint8_t *)w->buf + i * 4096);
    if (w->uva) for (int i = 0; i < np; i++) vm_map(t->pml4, w->uva + i * 4096ULL, (uint64_t)nb + i * 4096ULL, true, false);
    w->buf = nb; w->pages = np; w->w = cw; w->h = ch;
    damage_frame(w);
    *nw = cw; *nh = ch;
    return true;
}

static void toggle_maximize(struct window *w) {
    if (!w->resizable) return;
    hover_ctl = 0;                               /* the button moves away from under the mouse */
    damage_frame(w);
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = HL_BAR_H;
        w->maximized = true;
        request_size(w, max_content_w(), max_content_h());
    } else {
        w->x = w->rx; w->y = w->ry;
        w->maximized = false;
        request_size(w, w->rw, w->rh);
    }
    damage_frame(w);
}

bool wm_flush(struct task *t, int id, int x, int y, int w, int h) {
    struct window *win = find(t, id);
    if (!win) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    if (!win->minimized && w > 0 && h > 0) damage(cx0(win) + x, cy0(win) + y, w, h);
    win->close_req = 0;
    return true;
}

bool wm_set_title(struct task *t, int id, const char *title) {
    struct window *win = find(t, id);
    if (!win) return false;
    strncpy0(win->title, title, sizeof win->title);
    damage(win->x, win->y, fw(win), TITLE);
    return true;
}

bool wm_close(struct task *t, int id) {
    struct window *win = find(t, id);
    if (!win) return false;
    if (win->uva) vm_unmap(t->pml4, win->uva, win->pages);
    remove_window(win - wins);
    return true;
}

void wm_task_exit(struct task *t) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].owner == t) remove_window(i);
}

int wm_count(struct task *t) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].owner == t) n++;
    return n;
}

/* where its content is on the screen */
bool wm_pos(struct task *t, int id, int *x, int *y) {
    struct window *w = find(t, id);
    if (!w) return false;
    *x = cx0(w); *y = cy0(w);
    return true;
}
/* move it so its content starts at x, y (a window's title bar stays below the top bar) */
bool wm_move(struct task *t, int id, int x, int y) {
    struct window *w = find(t, id);
    if (!w) return false;
    if (w->maximized || w->snapped) return true;               /* (the desktop placed it: it stays) */
    damage_frame(w);
    w->x = w->popup ? x : x - 1; w->y = w->popup ? y : y - TITLE;
    if (!w->popup && w->y < HL_BAR_H) w->y = HL_BAR_H;
    damage_frame(w);
    return true;
}
/* show or hide it (a hidden window is minimised; a popup just isn't drawn) */
bool wm_show(struct task *t, int id, bool show) {
    struct window *w = find(t, id);
    if (!w) return false;
    if (w->minimized == !show) return true;
    int idx = w - wins;
    if (show) raise(idx);
    else { int old = top_window(); w->minimized = true; damage_frame(w); focus_changed(old); if (capture_win == idx) capture_win = -1; }
    return true;
}
/* the window is shown for another program (a Wayland app shown by axwl): its app, for the top bar and the dock */
bool wm_set_app(struct task *t, int id, const char *app_id, const char *name) {
    struct window *w = find(t, id);
    if (!w) return false;
    strncpy0(w->app_id, app_id ? app_id : "", sizeof w->app_id);
    strncpy0(w->app_name, name ? name : "", sizeof w->app_name);
    damage_r(bar_rect()); damage_r(dock_area());
    return true;
}
bool wm_set_cursor(struct task *t, int id, int shape) {
    struct window *w = find(t, id);
    if (!w) return false;
    if (shape < 0 || shape >= CUR_FRAME_RESIZE) shape = 0;
    if (w->cursor == shape) return true;
    w->cursor = shape;
    update_hover();
    return true;
}

void desktop_notify(const char *title, const char *msg) {
    sound_notify();
    kprintf("[notify] %s: %s\n", title, msg);
    if (!desktop_ok) return;
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!toasts[i].used) { slot = i; break; }
    if (slot < 0) { memmove(&toasts[0], &toasts[1], sizeof toasts[0] * 3); slot = 3; damage(W - 400, 0, 400, HL_BAR_H + 4 * 102 + 30); }
    strncpy0(toasts[slot].title, title, sizeof toasts[slot].title);
    strncpy0(toasts[slot].msg, msg, sizeof toasts[slot].msg);
    toasts[slot].until = uptime_ms() + 6000;
    toasts[slot].used = true;
    struct rect r = toast_rect(slot);
    damage(r.x - 6, r.y, r.w + 12, r.h + 16);
}

/* ---------------- input ---------------- */
static void send_mouse(int idx, int type, int button) {
    struct window *w = &wins[idx];
    struct dunc_event e = { type, w->id, mx - cx0(w), my - cy0(w), 0, button };
    event_push(w->owner, &e);
}

static int window_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        if (!w->minimized && x >= w->x && x < w->x + fw(w) && y >= w->y && y < w->y + fh(w)) return zorder[i];
    }
    return -1;
}

static void set_hover(int ctl, struct rect r) {
    if (ctl == hover_ctl) return;
    if (hover_ctl) damage(hx, hy, hw, hh);
    hover_ctl = ctl; hx = r.x; hy = r.y; hw = r.w; hh = r.h;
    if (ctl) damage(hx, hy, hw, hh);
}

static int resize_zone(int *wi_out) {
    int wi = window_at(mx, my);
    for (int z = nz - 1; z >= 0 && wi < 0; z--) {   /* zones extend a few pixels outside the frame */
        struct window *w = &wins[zorder[z]];
        if (!w->minimized && mx >= w->x && mx < w->x + fw(w) + 5 && my >= w->y && my < w->y + fh(w) + 5) wi = zorder[z];
    }
    if (wi < 0 || !wins[wi].resizable) return 0;
    struct window *w = &wins[wi];
    bool right = mx >= w->x + fw(w) - 6, bottom = my >= w->y + fh(w) - 6;
    bool corner = mx >= w->x + fw(w) - 16 && my >= w->y + fh(w) - 16;
    *wi_out = wi;
    return corner ? 3 : right ? 1 : bottom ? 2 : 0;
}

static void update_hover(void) {
    int zw, shape = 0;
    if (resize_win >= 0 || resize_zone(&zw)) shape = CUR_FRAME_RESIZE;
    else if (drag_win < 0) {                       /* over an app's content: the shape it asked for */
        int t = capture_win >= 0 ? capture_win : window_at(mx, my);
        if (t >= 0 && in_rect(content_rect(&wins[t]), mx, my)) shape = wins[t].cursor;
    }
    set_cursor(shape);
    if (power_act) for (int k = 0; k < 2; k++) if (in_rect(power_btn(k), mx, my)) { set_hover(320 + k, power_btn(k)); return; }
    if (menu_open) {
        for (int i = 0; i < mn; i++) if (in_rect(menu_pin(i), mx, my)) { set_hover(500 + i, menu_item(i)); return; }
        for (int i = 0; i < mn; i++) if (in_rect(menu_item(i), mx, my)) { set_hover(200 + i, menu_item(i)); return; }
        for (int k = 0; k < 3; k++) if (in_rect(menu_btn(k), mx, my)) { set_hover(300 + k, menu_btn(k)); return; }
    }
    if (in_rect(crown_btn(), mx, my)) { set_hover(50, bar_rect()); return; }
    if (ctx_app >= 0 && in_rect(ctx_rect(), mx, my)) { set_hover(600, ctx_rect()); return; }
    if (window_at(mx, my) < 0) for (int i = 0; i < ndock; i++)
        if (in_rect(dock_item(i), mx, my)) { struct rect d = dock_item(i); set_hover(100 + i, (struct rect){ d.x - 110, d.y - 48, d.w + 220, d.h + 48 }); return; }
    if (window_at(mx, my) < 0 && in_rect(dock_rect(), mx, my)) { set_hover(0, (struct rect){0, 0, 0, 0}); return; }
    int wi = window_at(mx, my);
    if (wi >= 0 && !wins[wi].popup) {
        struct window *w = &wins[wi];
        for (int k = 0; k < 3; k++) { struct rect b = dot(w, k); if (b.w && in_rect(b, mx, my)) { set_hover(1000 + wi * 4 + k, b); return; } }
    }
    set_hover(0, (struct rect){0, 0, 0, 0});
}

static void toggle_menu(bool open) {
    if (menu_open == open) return;
    menu_open = open;
    if (open) menu_layout();
    struct rect m = menu_rect();
    damage(m.x - 6, m.y, m.w + 12, m.h + 20);
    damage_r(bar_rect());
    hover_ctl = 0;
    update_hover();
}

static void toggle_pin(int a) {
    char list[112]; int k = 0; list[0] = 0;
    bool pin = !is_pinned(a);
    for (int i = 0; i < ndock; i++) if (dock_pinned[i] && dock_app[i] != a) k += ksnprintf(list + k, sizeof list - k, "%s%s", k ? "," : "", apps_get(dock_app[i])->id);
    if (pin) k += ksnprintf(list + k, sizeof list - k, "%s%s", k ? "," : "", apps_get(a)->id);
    damage_r(dock_area());
    settings_set("dock.apps", list);
    dock_rebuild(); dock_dirty = false;
    damage_r(dock_area()); dock_area_cached = dock_area();
    if (menu_open) damage_r(menu_rect());
}

static void launch(int a) {
    if (task_spawn(apps_get(a)->id) < 0) {
        char m[96]; ksnprintf(m, sizeof m, "Couldn't start %s (out of memory or too many apps).", app_name(a));
        desktop_notify("Couldn't open app", m);
    }
    damage_r(dock_area());
}

static void activate_app(int a) {
    int top = top_window(), best = -1;
    for (int z = nz - 1; z >= 0; z--)
        if (!wins[zorder[z]].popup && app_index(win_app(&wins[zorder[z]])) == a) { best = zorder[z]; break; }
    if (best < 0) { if (!app_running(a)) launch(a); return; }
    if (best == top) { int old = top; wins[best].minimized = true; damage_frame(&wins[best]); focus_changed(old); }
    else raise(best);
}

static void close_window_clicked(struct window *w) {
    if (w->close_req && uptime_ms() - w->close_req > 1000) {
        char m[128]; ksnprintf(m, sizeof m, "%s wasn't responding, so AXIOM closed it.", w->owner->name);
        task_kill(w->owner->pid);
        desktop_notify("App closed", m);
    } else {
        if (!w->close_req) w->close_req = uptime_ms();
        struct dunc_event e = { EV_CLOSE, w->id, 0, 0, 0, 0 };
        event_push(w->owner, &e);
    }
}

static void click(int button) {
    if (power_act) {
        if (in_rect(power_btn(0), mx, my)) { power_cancel(); return; }
        if (in_rect(power_btn(1), mx, my)) { power_finish(); return; }
        if (in_rect(power_rect(), mx, my)) return;
    }
    if (keep_until) {                                      /* the keep-this-size box answers first */
        if (in_rect(keep_btn(1), mx, my)) { keep_until = 0; damage(0, 0, W, H); intelgfx_native_answer(true); desktop_notify("Screen size", "Kept. You can change it in Settings > Display."); }
        else if (in_rect(keep_btn(0), mx, my)) { keep_until = 0; intelgfx_native_answer(false); }
        return;
    }
    if (menu_open) {
        if (in_rect(menu_rect(), mx, my)) {
            for (int i = 0; i < mn; i++) if (in_rect(menu_pin(i), mx, my)) { toggle_pin(mapp[i]); return; }
            for (int i = 0; i < mn; i++) if (in_rect(menu_item(i), mx, my)) { toggle_menu(false); launch(mapp[i]); return; }
            if (in_rect(menu_btn(0), mx, my)) { toggle_menu(false); go_to_sleep(); }
            if (in_rect(menu_btn(1), mx, my)) power_start(2);
            if (in_rect(menu_btn(2), mx, my)) power_start(1);
            return;
        }
        toggle_menu(false);
        if (in_rect(crown_btn(), mx, my)) return;
    }
    if (ctx_app >= 0) {                                    /* the dock's right-click button */
        int a = ctx_app; bool hit = in_rect(ctx_rect(), mx, my);
        damage_r(ctx_rect()); ctx_app = -1;
        if (hit) { toggle_pin(a); return; }
    }
    if (in_rect(crown_btn(), mx, my)) { toggle_menu(true); return; }
    bool on_win = window_at(mx, my) >= 0;                  /* windows cover the dock */
    if (!on_win) for (int i = 0; i < ndock; i++) if (in_rect(dock_item(i), mx, my)) {
        if (button == 2) { ctx_app = dock_app[i]; ctx_slot = i; hover_ctl = 0; damage_r(ctx_rect()); damage_r(dock_area()); return; }
        activate_app(dock_app[i]); return;
    }
    if ((!on_win && in_rect(dock_rect(), mx, my)) || my < HL_BAR_H) return;
    int zw, zone = resize_zone(&zw);
    if (zone && button == 1) {
        if (zw != top_window()) raise(zw);
        struct window *w = &wins[zw];
        resize_win = zw; resize_mode = zone; resize_ox = mx; resize_oy = my; resize_w0 = w->w; resize_h0 = w->h;
        w->maximized = false;
        return;
    }
    int wi = window_at(mx, my);
    if (wi < 0) return;
    struct window *w = &wins[wi];
    if (w->popup) { capture_win = wi; send_mouse(wi, EV_MOUSE_DOWN, button); return; }   /* (no raising, no focus) */
    if (wi != top_window()) raise(wi);
    if (my < w->y + HL_TITLE_H) {
        if (button != 1) return;
        if (in_rect(dot(w, 0), mx, my)) close_window_clicked(w);
        else if (in_rect(dot(w, 1), mx, my)) { int old = top_window(); w->minimized = true; damage_frame(w); focus_changed(old); hover_ctl = 0; }
        else if (in_rect(dot(w, 2), mx, my)) toggle_maximize(w);
        else {
            uint64_t now = uptime_ms();
            if (now - w->last_title_click < 400) { toggle_maximize(w); w->last_title_click = 0; return; }
            w->last_title_click = now;
            if (w->maximized || w->snapped) {   /* dragging a maximised or snapped window restores it under the cursor */
                w->maximized = false; w->snapped = false;
                damage_frame(w);
                w->x = mx - w->rw / 2; if (w->x < 0) w->x = 0;
                request_size(w, w->rw, w->rh);
            }
            drag_win = wi; drag_ox = mx - w->x; drag_oy = my - w->y;
        }
        return;
    }
    capture_win = wi;
    send_mouse(wi, EV_MOUSE_DOWN, button);
}

static void handle_mouse(int dx, int dy, int b, int wheel) {
    random_event((uint64_t)(dx & 0xFFFF) | (uint64_t)(dy & 0xFFFF) << 16 | (uint64_t)b << 32);
    last_input = uptime_ms();
    /* Settings > Mouse: speed 1..10 (5 = normal), natural scrolling, left-handed buttons */
    static int fx, fy;
    int sp = settings_int("mouse.speed"); if (sp < 1 || sp > 10) sp = 5;
    fx += dx * sp; fy += dy * sp;
    dx = fx / 5; dy = fy / 5; fx -= dx * 5; fy -= dy * 5;      /* keep the remainder: slow speeds stay smooth */
    if (settings_int("mouse.natural")) wheel = -wheel;
    if (settings_int("mouse.left")) b = (b & ~3) | ((b & 1) << 1) | ((b >> 1) & 1);
    if (screen_off && !screen_wake()) return;
    if (locked) {                                           /* only the pointer moves; nothing is clicked */
        int ox = mx, oy = my;
        mx += dx; my += dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= W) mx = W - 1;
        if (my >= H) my = H - 1;
        if (mx != ox || my != oy) { cursor_paint(ox, oy, false); cursor_paint(mx, my, true); }
        return;
    }
    int ox = mx, oy = my;
    if (dx > 5 || dx < -5) dx *= 2;
    if (dy > 5 || dy < -5) dy *= 2;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= W) mx = W - 1;
    if (my >= H) my = H - 1;
    bool moved = mx != ox || my != oy;
    if (moved) { cursor_paint(ox, oy, false); cursor_paint(mx, my, true); }

    if (moved && resize_win >= 0) {
        struct window *w = &wins[resize_win];
        int nw = resize_w0 + ((resize_mode & 1) ? mx - resize_ox : 0);
        int nh = resize_h0 + ((resize_mode & 2) ? my - resize_oy : 0);
        request_size(w, nw, nh);
    }
    if (moved && drag_win >= 0) {
        struct window *w = &wins[drag_win];
        damage_frame(w);
        w->x = mx - drag_ox; w->y = my - drag_oy;
        if (w->y < HL_BAR_H) w->y = HL_BAR_H;
        if (w->y > H - HL_TITLE_H) w->y = H - HL_TITLE_H;
        if (w->x + fw(w) < 80) w->x = 80 - fw(w);
        if (w->x > W - 80) w->x = W - 80;
        damage_frame(w);
        int z = !w->resizable ? 0 : mx <= 1 ? 1 : mx >= W - 2 ? 2 : my <= HL_BAR_H ? 3 : 0;   /* snapping to an edge */
        if (z != snap_zone) { if (snap_zone) damage_r(snap_rect(snap_zone)); snap_zone = z; if (z) damage_r(snap_rect(z)); }
    }
    int pressed = b & ~mbtn, released = mbtn & ~b;
    mbtn = b;
    if (pressed & 1) click(1);
    if (pressed & 2) click(2);
    if (released & 3) {
        if (drag_win >= 0 && snap_zone) {
            struct window *w = &wins[drag_win];
            int z = snap_zone; damage_r(snap_rect(z)); snap_zone = 0;
            if (z == 3) { if (!w->maximized) toggle_maximize(w); }
            else {
                struct rect s = snap_rect(z);
                if (!w->snapped && !w->maximized) { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
                damage_frame(w);
                w->x = s.x; w->y = s.y; w->snapped = true; w->maximized = false;
                request_size(w, s.w - 2, s.h - TITLE - 1);
                damage_frame(w);
            }
        }
        drag_win = -1;
        if (resize_win >= 0) { resize_win = -1; update_hover(); }
        if (capture_win >= 0) { send_mouse(capture_win, EV_MOUSE_UP, (released & 1) ? 1 : 2); capture_win = -1; }
    }
    if (moved && drag_win < 0 && resize_win < 0) {
        update_hover();
        int target = capture_win >= 0 ? capture_win : window_at(mx, my);
        bool content = target >= 0 && my >= cy0(&wins[target]);
        if (content) send_mouse(target, EV_MOUSE_MOVE, 0);
        if (hover_win >= 0 && hover_win != (content ? target : -1) && wins[hover_win].used) send_mouse(hover_win, EV_MOUSE_LEAVE, 0);
        hover_win = content ? target : -1;
    }
    if (wheel) {
        int t = window_at(mx, my);
        if (t >= 0) { struct dunc_event e = { EV_WHEEL, wins[t].id, mx - cx0(&wins[t]), my - cy0(&wins[t]), wheel, 0 }; event_push(wins[t].owner, &e); }
    }
}

/* ---------------- screenshots (Print Screen) ---------------- */
#include "vfs.h"
#include "../user/lib/img/img.h"
static uint32_t *shot_px; static volatile bool shot_busy;

static void screenshot_task(void *arg) {
    size_t cap = png_encode_bound(W, H);
    uint8_t *png = kmalloc(cap);
    long n = png ? png_encode(shot_px, W, H, 1, png, cap) : -1;
    if (n > 0) {
        struct rtc_time t; rtc_read_local(&t);
        char name[80], path[128];
        ksnprintf(name, sizeof name, "Screenshot %d-%02d-%02d at %02d.%02d.%02d.png", t.year, t.mon, t.day, t.hour, t.min, t.sec);
        vfs_mkdir("/home/Pictures");
        ksnprintf(path, sizeof path, "/home/Pictures/%s", name);
        long w = vfs_write(path, png, n);
        char m[160];
        if (w < 0) ksnprintf(m, sizeof m, "Couldn't save it: %s.", fs_error((int)w));
        else ksnprintf(m, sizeof m, "Saved in Pictures as \"%s\".", name);
        desktop_notify(w < 0 ? "Screenshot failed" : "Screenshot taken", m);
    } else desktop_notify("Screenshot failed", "Not enough memory.");
    kfree(png);
    shot_busy = false;
    task_exit(0);
}

static void take_screenshot(void) {
    if (shot_busy) return;
    if (!shot_px) shot_px = page_alloc((W * H * 4 + 4095) / 4096);
    if (!shot_px) return;
    memcpy(shot_px, bb, (size_t)W * H * 4);      /* the composed screen, without the mouse pointer */
    shot_busy = true;
    task_create_kernel("Screenshot", screenshot_task, 0);
}

static void handle_key(int k) {
    random_event(k);
    last_input = uptime_ms();
    if (screen_off) { screen_wake(); return; }               /* the key just wakes the screen */
    if (locked) { lock_key(k); return; }
    if (k == (KEYMOD_SUPER | 'l')) { desktop_lock(); return; }
    if (KEY_CODE(k) == KEY_PRTSC) { take_screenshot(); return; }
    if (k == (KEYMOD_ALT | KEY_F4)) { int t = top_window(); if (t >= 0) close_window_clicked(&wins[t]); return; }
    if (k == KEY_SUPER) { toggle_menu(!menu_open); return; }
    if ((k & ~KEYMOD_SHIFT) == (KEYMOD_ALT | '\t')) {       /* Alt+Tab (Alt+Shift+Tab goes back) */
        uint64_t now = uptime_ms();
        if (!sw_until || now > sw_until) {                    /* a new round: windows from the most recently used */
            sw_n = 0;
            for (int z = nz - 1; z >= 0 && sw_n < 32; z--) if (wins[zorder[z]].used && !wins[zorder[z]].popup) sw_list[sw_n++] = zorder[z];
            sw_sel = 0;
        }
        if (sw_n < 1) return;
        damage_r(switcher_rect());
        sw_sel = (sw_sel + ((k & KEYMOD_SHIFT) ? sw_n - 1 : 1)) % sw_n;
        if (wins[sw_list[sw_sel]].used) raise(sw_list[sw_sel]);
        sw_until = now + 1400;
        damage_r(switcher_rect());
        return;
    }
    if (menu_open && k == 27) { toggle_menu(false); return; }
    int t = top_window();
    if (t < 0) return;
    struct dunc_event e = { EV_KEY, wins[t].id, 0, 0, k, 0 };
    event_push(wins[t].owner, &e);
}

/* ---------------- setup + main loop ---------------- */
bool desktop_available(void) {
    struct bootinfo *b = &bootinfo;
    return b->fb_addr && b->fb_type == 1 && b->fb_bpp == 32 && b->fb_w >= 800 && b->fb_h >= 600;
}

void boot_logo_show(const struct framebuffer *fb);
void boot_logo_progress(const struct framebuffer *fb, unsigned percent);
static struct framebuffer splash_fb;

void boot_progress(int percent) {
    if (!desktop_available()) return;
    if (!splash_shown) {
        splash_fb = (struct framebuffer){ mmio_virt(bootinfo.fb_addr), bootinfo.fb_w, bootinfo.fb_h, bootinfo.fb_pitch };
        boot_logo_show(&splash_fb);
        splash_shown = true;
    }
    boot_logo_progress(&splash_fb, percent);
}

static void draw_wallpaper(void);
void desktop_init(void) {
    struct bootinfo *b = &bootinfo;
    W = b->fb_w; H = b->fb_h; fb_stride = b->fb_pitch / 4;
    fb32 = mmio_virt(b->fb_addr);
    /* a smaller size chosen in Settings > Display that the graphics card didn't switch to: draw at that size and stretch */
    const char *m = settings_get("display.mode");
    int rw = (int)katoi(m), rh = 0;
    for (const char *p = m; *p; p++) if (*p == 'x') { rh = (int)katoi(p + 1); break; }
    if (!intelgfx_native_active() && rw >= 800 && rh >= 600 && rw <= W && rh <= H && (rw != W || rh != H)) {
        PW = W; PH = H;
        dw = PW; dh = PH;                                /* fill the whole screen (no black bars, even if the shapes differ a little) */
        ox = (PW - dw) / 2; oy = (PH - dh) / 2;
        sx_i = kmalloc(dw * 2); sy_i = kmalloc(dh * 2); sx_f = kmalloc(dw); sy_f = kmalloc(dh);
        if (sx_i && sy_i && sx_f && sy_f) {
            W = rw; H = rh; scaled = true;
            for (int i = 0; i < dw; i++) { long v = ((2L * i + 1) * W * 128) / dw - 128; if (v < 0) v = 0; sx_i[i] = (uint16_t)(v >> 8); sx_f[i] = (uint8_t)(v & 255); }
            for (int i = 0; i < dh; i++) { long v = ((2L * i + 1) * H * 128) / dh - 128; if (v < 0) v = 0; sy_i[i] = (uint16_t)(v >> 8); sy_f[i] = (uint8_t)(v & 255); }
            for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) fb32[(size_t)y * fb_stride + x] = fbcol(0);   /* black borders */
            kprintf("[desktop] %dx%d, stretched to the %dx%d screen\n", W, H, PW, PH);
        }
    }
    fb_xrgb = b->r_pos == 16 && b->g_pos == 8 && b->b_pos == 0;
    int pages = (W * H * 4 + 4095) / 4096;
    bb = page_alloc(pages); wall = page_alloc(pages);
    if (!bb || !wall) { kprintf("[desktop] not enough memory for the desktop\n"); return; }
    fb_write_combining(b->fb_addr, (uint64_t)b->fb_pitch * b->fb_h);
    gfx_init(&cv, bb, W, H, W);
    full = (struct framebuffer){ bb, W, H, W * 4 };
    draw_wallpaper();
    mx = W / 2; my = H / 2;
    desktop_ok = true;
}

/* Settings > Personalise: the AXIOM glow (default), plain black, or a picture
 * (/system/config/wallpaper.raw: "AXWP", width, height, then 0xRRGGBB pixels, made by Settings) */
static void draw_wallpaper(void) {
    const char *v = settings_get("desktop.wallpaper");
    if (!strcmp(v, "black")) { for (long i = 0; i < (long)W * H; i++) wall[i] = 0x050505; return; }
    if (!strcmp(v, "picture")) {
        uint64_t len; uint8_t *f = vfs_read_all("/system/config/wallpaper.raw", &len);
        if (f && len > 12 && !memcmp(f, "AXWP", 4)) {
            uint32_t pw = *(uint32_t *)(f + 4), ph = *(uint32_t *)(f + 8);
            if (pw && ph && pw <= 8192 && ph <= 8192 && len >= 12 + (uint64_t)pw * ph * 4) {
                const uint32_t *px = (const uint32_t *)(f + 12);
                /* cover the screen, keeping the picture's shape (the middle is kept) */
                uint64_t sx = (uint64_t)pw * 65536 / W, sy = (uint64_t)ph * 65536 / H, s = sx < sy ? sx : sy;
                uint64_t ox = ((uint64_t)pw * 65536 - s * W) / 2, oy = ((uint64_t)ph * 65536 - s * H) / 2;
                for (int y = 0; y < H; y++) {
                    uint32_t yy = (uint32_t)((oy + s * y) >> 16); if (yy >= ph) yy = ph - 1;
                    for (int x = 0; x < W; x++) { uint32_t xx = (uint32_t)((ox + s * x) >> 16); if (xx >= pw) xx = pw - 1; wall[y * W + x] = px[yy * pw + xx] & 0xFFFFFF; }
                }
                kfree(f);
                return;
            }
        }
        kfree(f);
    }
    struct framebuffer wf = { wall, W, H, W * 4 };
    ax_draw_wallpaper(&wf);
}
void desktop_restyle(void) {                              /* the wallpaper or accent colour changed */
    if (!desktop_ok) return;
    draw_wallpaper();
    damage(0, 0, W, H);
}

void desktop_size(int *w, int *h, bool *is_scaled) { *w = W; *h = H; if (is_scaled) *is_scaled = scaled; }

void desktop_thread(void *arg) {
    dock_rebuild(); dock_area_cached = dock_area();
    if (intelgfx_native_asking()) keep_until = uptime_ms() + 15000;
    damage(0, 0, W, H);
    for (;;) {
        if (keep_until) {                                  /* the keep-this-size countdown */
            static uint64_t last_s;
            uint64_t now = uptime_ms();
            if (now >= keep_until) { keep_until = 0; intelgfx_native_answer(false); }
            else if (now / 1000 != last_s) { last_s = now / 1000; damage_r(keep_rect()); }
        }
        ps2_poll();
        xhci_poll();
        int s;
        while ((s = serial_getc()) >= 0) { if (s == 0x1C) { linux_dump_threads(); continue; } input_push(s == '\r' ? '\n' : s == 127 ? '\b' : s); }
        int k;
        while ((k = input_pop()) >= 0) handle_key(k);
        int dx, dy, b, wh;
        while (mouse_pop(&dx, &dy, &b, &wh)) handle_mouse(dx, dy, b, wh);
        for (int i = 0; i < MAX_WIN; i++) {                /* windows that asked to hear where they've moved to */
            struct window *w = &wins[i];
            if (!w->used || !w->move_events || (cx0(w) == w->sent_x && cy0(w) == w->sent_y)) continue;
            w->sent_x = cx0(w); w->sent_y = cy0(w);
            struct dunc_event e = { EV_MOVE, w->id, w->sent_x, w->sent_y, 0, 0 };
            event_push(w->owner, &e);
        }

        uint64_t now = uptime_ms();
        for (int i = 0; i < 4; i++)
            if (toasts[i].used && now > toasts[i].until) { toasts[i].used = false; struct rect r = toast_rect(i); damage(r.x - 6, r.y, r.w + 12, r.h + 16); }
        if (sw_until && now > sw_until) { sw_until = 0; struct rect r = switcher_rect(); damage(r.x - 4, r.y, r.w + 8, r.h + 16); }
        static uint64_t last_check;
        if (now - last_check > 250) {
            last_check = now;
            if (!last_input) { last_input = now; if (settings_get("lock.password")[0]) desktop_lock(); }   /* locked at start-up */
            int lock_min = settings_int("lock.idle_min"), off_min = settings_int("screen.off_min");
            if (!locked && lock_min > 0 && settings_get("lock.password")[0] && now - last_input > (uint64_t)lock_min * 60000) desktop_lock();
            if (!screen_off && off_min > 0 && now - last_input > (uint64_t)off_min * 60000) screen_sleep();
            power_tick(now);
            static int last_min = -1;                       /* the lock screen's clock */
            if (locked && !screen_off) { struct rtc_time tt; rtc_read_local(&tt); if (tt.min != last_min) { last_min = tt.min; damage(0, 0, W, H); } }
            char app[48], clk[32], sig[96];
            bar_text(app, sizeof app, clk, sizeof clk);
            ksnprintf(sig, sizeof sig, "%s|%s|%d", app, clk, net_present() + 2 * net_ready());
            if (strcmp(sig, bar_sig)) { strncpy0(bar_sig, sig, sizeof bar_sig); damage_r(bar_rect()); }
            int rs = NVIS << 24;
            for (int i = 0; i < NVIS && i < 24; i++) if (app_running(i)) rs |= 1 << i;
            if (rs != run_sig || dock_dirty) {
                struct rect old = dock_area_cached;
                run_sig = rs; dock_dirty = false;
                dock_rebuild();
                if (ctx_app >= 0) { damage_r(ctx_rect()); ctx_app = -1; }
                damage_r(old); damage_r(dock_area()); dock_area_cached = dock_area();
                if (hover_ctl >= 100 && hover_ctl < 200) { hover_ctl = 0; update_hover(); }
                if (menu_open) damage(menu_rect().x - 6, menu_rect().y, menu_rect().w + 12, menu_rect().h + 20 + MENU_ROW);
            }
            for (int i = 0; i < MAX_WIN; i++)        /* show "(Not responding)" */
                if (wins[i].used && wins[i].close_req && now - wins[i].close_req > 1000 && now - wins[i].close_req < 1400)
                    damage(wins[i].x, wins[i].y, fw(&wins[i]), TITLE);
        }
        for (int i = 0; i < MAX_WIN; i++)
            if (wins[i].used && wins[i].resizable && !wins[i].resize_sent && (wins[i].want_w != wins[i].w || wins[i].want_h != wins[i].h))
                request_size(&wins[i], wins[i].want_w, wins[i].want_h);
        task_reap();
        hda_tick();

        int n = ndmg; struct rect todo[32];
        memcpy(todo, dmg, sizeof(struct rect) * n); ndmg = 0;
        for (int i = 0; i < n; i++) {
            compose(todo[i]);
            /* between pieces of redrawing (a playing video makes a lot of it), keep the mouse pointer moving */
            if (i + 1 < n) { ps2_poll(); xhci_poll(); int ddx, ddy, bb2, wh2; while (mouse_pop(&ddx, &ddy, &bb2, &wh2)) handle_mouse(ddx, ddy, bb2, wh2); }
        }
        task_sleep(screen_off ? 30 : n ? 4 : 8);             /* asleep: look at the keyboard and mouse less often */                           /* while the screen is changing, look at the mouse more often */
    }
}

/* a window's pixels and size (Linux programs map them themselves) */
uint32_t *wm_buffer(struct task *t, int id, int *pages, int *w, int *h) {
    struct window *win = find(t, id);
    if (!win) return 0;
    *pages = win->pages; *w = win->w; *h = win->h;
    return win->buf;
}
void wm_screen(int *w, int *h) { *w = W; *h = H; }
