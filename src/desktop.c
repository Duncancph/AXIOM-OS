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
};
static inline int fw(struct window *w) { return w->w + 2; }
static inline int fh(struct window *w) { return w->h + TITLE + 1; }

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
static int napps_dock(void) { int n = apps_count(); return n > HL_MAX_DOCK ? HL_MAX_DOCK : n; }
#define HL_NAPPS napps_dock()
static const char *app_name(int i) { return apps_get(i)->name; }

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
static struct rect dock_rect(void) { int x, y, w, h; hl_dock_rect(W, H, HL_NAPPS, &x, &y, &w, &h); return (struct rect){ x, y, w, h }; }
static struct rect dock_area(void) { struct rect d = dock_rect(); return (struct rect){ d.x - 1, d.y - 1, d.w + 2, d.h + 10 }; }  /* incl. shadow */
static struct rect dock_item(int i) { struct rect d = dock_rect(); return (struct rect){ d.x + HL_DOCK_PAD + i * (HL_DOCK_ICON + HL_DOCK_GAP), d.y + HL_DOCK_PAD, HL_DOCK_ICON, HL_DOCK_ICON }; }
static struct rect frame_area(struct window *w) { return (struct rect){ w->x - 8, w->y - 2, fw(w) + 16, fh(w) + 30 }; }
static struct rect crown_btn(void) { return (struct rect){ 6, 3, 132, HL_BAR_H - 6 }; }
#define MENU_W 300
#define MENU_ROW 46
static struct rect menu_rect(void) { return (struct rect){ 8, HL_BAR_H + 8, MENU_W, 92 + HL_NAPPS * MENU_ROW + 64 }; }
static struct rect menu_item(int i) { struct rect m = menu_rect(); return (struct rect){ m.x + 10, m.y + 84 + i * MENU_ROW, m.w - 20, MENU_ROW - 4 }; }
static struct rect menu_btn(int k) { struct rect m = menu_rect(); return (struct rect){ m.x + 14 + k * 140, m.y + m.h - 50, 132, 34 }; }
static struct rect toast_rect(int i) { return (struct rect){ W - 376, HL_BAR_H + 12 + i * 102, 360, 92 }; }
static struct rect dot(struct window *w, int k) { return (struct rect){ w->x + 20 + k * 20 - 8, w->y + HL_TITLE_H / 2 - 8, 16, 16 }; }

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
static int top_window(void) {
    for (int i = nz - 1; i >= 0; i--) if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}
static int app_index(const char *id) {
    for (int i = 0; id && i < HL_NAPPS; i++) if (!strcmp(apps_get(i)->id, id)) return i;
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
    strncpy0(app, t >= 0 ? wins[t].owner->name : "Desktop", na);
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
static const char **cur_img = cursor_img;
static int cur_rows = 19, cur_hx, cur_hy;

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
        if (cx < x1 && cx + 17 > x0 && cy < y1 && cy + 23 > y0) cursor_paint(mx, my, true);
        return;
    }
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *s = bb + j * W + r.x;
        volatile uint32_t *d = fb32 + (size_t)j * fb_stride + r.x;
        if (fb_xrgb) for (int i = 0; i < r.w; i++) d[i] = s[i];
        else for (int i = 0; i < r.w; i++) d[i] = fbcol(s[i]);
    }
    if (overlap(r, (struct rect){ mx - cur_hx, my - cur_hy, 13, 19 })) cursor_paint(mx, my, true);
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

static void set_cursor(bool resize) {
    const char **img = resize ? cursor_resize : cursor_img;
    if (img == cur_img) return;
    cursor_paint(mx, my, false);
    cur_img = img; cur_rows = resize ? 13 : 19; cur_hx = resize ? 6 : 0; cur_hy = resize ? 6 : 0;
    cursor_paint(mx, my, true);
}

static void draw_window(int idx, struct rect r) {
    struct window *w = &wins[idx];
    bool active = idx == top_window();
    char t[80];
    bool hung = w->close_req && uptime_ms() - w->close_req > 1000;
    ksnprintf(t, sizeof t, "%s%s", w->title, hung ? " (Not responding)" : "");
    /* Highland frame, clipped to the damaged rectangle via a sub-framebuffer */
    struct framebuffer sub = { bb + r.y * W + r.x, r.w, r.h, W * 4 };
    hl_render_window(&sub, w->x - r.x, w->y - r.y, fw(w), fh(w), t, HL_WIN_BG, active);
    gfx_blit_rr(&cv, w->x + 1, w->y + TITLE, w->buf, w->w, w->h, w->w, w->x + 1, w->y + 1, fw(w) - 2, fh(w) - 2, HL_WIN_RADIUS - 1);
    if (hover_ctl >= 1000 && (hover_ctl - 1000) / 4 == idx) {   /* show symbols on the window dots */
        int cy = w->y + HL_TITLE_H / 2;
        gfx_line(&cv, w->x + 17, cy - 3, w->x + 23, cy + 3, 0x5A1010, 2);
        gfx_line(&cv, w->x + 17, cy + 3, w->x + 23, cy - 3, 0x5A1010, 2);
        gfx_fill(&cv, w->x + 36, cy - 1, 9, 2, 0x5A3C00);
        if (w->resizable) { gfx_fill(&cv, w->x + 56, cy - 1, 9, 2, 0x0E4A22); gfx_fill(&cv, w->x + 59, cy - 4, 2, 9, 0x0E4A22); }
    }
    if (!w->resizable) gfx_circle(&cv, w->x + 60, w->y + HL_TITLE_H / 2, HL_DOT_R + 1, HL_WIN_BG, 170);   /* can't maximise */
}

static void draw_dock_overlay(void) {
    if (hover_ctl >= 100 && hover_ctl < 100 + HL_NAPPS) {
        int i = hover_ctl - 100;
        struct rect r = dock_item(i);
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 14, 0xFFFFFF, 26);
        const char *l = app_name(i);
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
    gfx_mono_text(&cv, m.x + 20, m.y + 40, OS_NAME " " OS_VERSION " - Thane kernel", HL_TEXT_FAINT);
    gfx_blend(&cv, m.x + 1, m.y + 66, m.w - 2, 1, 0xFFFFFF, 16);
    for (int i = 0; i < HL_NAPPS; i++) {
        struct rect r = menu_item(i);
        if (hover_ctl == 200 + i) gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, HL_ACCENT, 36);
        const uint32_t *ic = hl_app_icon(apps_get(i)->icon, 1);
        for (int j = 0; j < 34; j++) {                    /* the app's icon, blended */
            int yy = r.y + 4 + j;
            if (yy < cv.cy0 || yy >= cv.cy1) continue;
            for (int k = 0; k < 34; k++) {
                int xx = r.x + 6 + k; uint32_t s = ic[j * 34 + k], al = s >> 24;
                if (!al || xx < cv.cx0 || xx >= cv.cx1) continue;
                uint32_t *d = &cv.px[yy * cv.stride + xx], dv = *d;
                *d = ((s >> 16 & 255) * al + (dv >> 16 & 255) * (255 - al)) / 255 << 16 |
                     ((s >> 8 & 255) * al + (dv >> 8 & 255) * (255 - al)) / 255 << 8 | ((s & 255) * al + (dv & 255) * (255 - al)) / 255;
            }
        }
        gfx_mono_text(&cv, r.x + 52, r.y + 13, app_name(i), hover_ctl == 200 + i ? HL_TEXT : HL_TEXT_DIM);
        if (apps_get(i)->installed) gfx_mono_text(&cv, r.x + 52 + (int)strlen(app_name(i)) * 8 + 8, r.y + 13, "clan", HL_GOLD);
        if (app_running(i)) gfx_circle(&cv, r.x + r.w - 14, r.y + r.h / 2, 3, HL_GREEN, 255);
    }
    gfx_blend(&cv, m.x + 1, m.y + m.h - 64, m.w - 2, 1, 0xFFFFFF, 16);
    static const char *bl[] = { "Restart", "Shut down" };
    for (int k = 0; k < 2; k++) {
        struct rect r = menu_btn(k);
        bool h = hover_ctl == 300 + k;
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, k && h ? HL_RED : h ? 0x2A2A28 : 0x1C1C1B, 255);
        gfx_rrect_outline(&cv, r.x, r.y, r.w, r.h, 8, HL_WIN_BORDER, 255);
        int tw = (int)strlen(bl[k]) * 8;
        gfx_mono_text(&cv, r.x + (r.w - tw) / 2, r.y + 9, bl[k], k && h ? 0xFFFFFF : HL_TEXT);
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

static struct rect content_rect(struct window *w) { return (struct rect){ w->x + 1, w->y + TITLE, w->w, w->h }; }
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
    if (start < 0) { gfx_blit(&cv, 0, 0, wall, W, H, W); start = 0; }
    for (int z = start; z < nz; z++) {
        struct window *w = &wins[zorder[z]];
        if (w->minimized || !overlap(frame_area(w), r)) continue;
        if (contains(content_rect(w), intersect(r, frame_area(w))))
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
    if (overlap(r, dock_area())) {
        int running[HL_MAX_DOCK]; const char *icons[HL_MAX_DOCK];
        for (int i = 0; i < HL_NAPPS; i++) { running[i] = app_running(i); icons[i] = apps_get(i)->icon; }
        hl_render_dock(&full, icons, running, HL_NAPPS);
    }
    draw_dock_overlay();
    if (menu_open) draw_menu();
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

static void raise(int idx) {
    int old = top_window(), k = 0;
    for (int i = 0; i < nz; i++) if (zorder[i] != idx) zorder[k++] = zorder[i];
    zorder[k++] = idx; nz = k;
    wins[idx].minimized = false;
    damage_frame(&wins[idx]);
    focus_changed(old);
}

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
}

static struct window *find(struct task *t, int id) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].id == id && wins[i].owner == t) return &wins[i];
    return 0;
}

struct window *wm_create(struct task *t, int w, int h, const char *title, uint32_t **buf, int *pages, uint64_t uva, int flags) {
    int top = HL_BAR_H + 8, bottom = dock_rect().y - 8;
    if (!desktop_ok || w < 100 || h < 60 || w > W - 20 || h + TITLE + 1 > H - HL_BAR_H - 16) return 0;
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
    win->resizable = flags & WIN_RESIZABLE;
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
    raise(idx);
    *buf = b; *pages = np;
    return win;
}

int wm_id(struct window *w) { return w->id; }

static int max_content_w(void) { return W - 2; }
static int max_content_h(void) { return dock_rect().y - 8 - HL_BAR_H - TITLE - 1; }

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
    vm_unmap(t->pml4, w->uva, w->pages);
    for (int i = 0; i < w->pages; i++) page_free((uint8_t *)w->buf + i * 4096);
    for (int i = 0; i < np; i++) vm_map(t->pml4, w->uva + i * 4096ULL, (uint64_t)nb + i * 4096ULL, true, false);
    w->buf = nb; w->pages = np; w->w = cw; w->h = ch;
    damage_frame(w);
    *nw = cw; *nh = ch;
    return true;
}

static void toggle_maximize(struct window *w) {
    if (!w->resizable) return;
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
    if (!win->minimized && w > 0 && h > 0) damage(win->x + 1 + x, win->y + TITLE + y, w, h);
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
    vm_unmap(t->pml4, win->uva, win->pages);
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
    struct dunc_event e = { type, w->id, mx - w->x - 1, my - w->y - TITLE, 0, button };
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
    int zw; set_cursor(resize_win >= 0 || resize_zone(&zw));
    if (menu_open) {
        for (int i = 0; i < HL_NAPPS; i++) if (in_rect(menu_item(i), mx, my)) { set_hover(200 + i, menu_item(i)); return; }
        for (int k = 0; k < 2; k++) if (in_rect(menu_btn(k), mx, my)) { set_hover(300 + k, menu_btn(k)); return; }
    }
    if (in_rect(crown_btn(), mx, my)) { set_hover(50, bar_rect()); return; }
    for (int i = 0; i < HL_NAPPS; i++)
        if (in_rect(dock_item(i), mx, my)) { struct rect d = dock_item(i); set_hover(100 + i, (struct rect){ d.x - 110, d.y - 48, d.w + 220, d.h + 48 }); return; }
    if (in_rect(dock_rect(), mx, my)) { set_hover(0, (struct rect){0, 0, 0, 0}); return; }
    int wi = window_at(mx, my);
    if (wi >= 0) {
        struct window *w = &wins[wi];
        struct rect dots = { w->x + 10, w->y + 10, 60, 20 };
        if (in_rect(dots, mx, my)) { set_hover(1000 + wi * 4, dots); return; }
    }
    set_hover(0, (struct rect){0, 0, 0, 0});
}

static void toggle_menu(bool open) {
    if (menu_open == open) return;
    menu_open = open;
    struct rect m = menu_rect();
    damage(m.x - 6, m.y, m.w + 12, m.h + 20);
    damage_r(bar_rect());
    hover_ctl = 0;
    update_hover();
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
        if (app_index(wins[zorder[z]].owner->app) == a) { best = zorder[z]; break; }
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
    if (menu_open) {
        if (in_rect(menu_rect(), mx, my)) {
            for (int i = 0; i < HL_NAPPS; i++) if (in_rect(menu_item(i), mx, my)) { toggle_menu(false); launch(i); return; }
            if (in_rect(menu_btn(0), mx, my)) acpi_reboot();
            if (in_rect(menu_btn(1), mx, my)) { acpi_poweroff(); desktop_notify("Shut down", "This computer doesn't support ACPI power-off."); }
            return;
        }
        toggle_menu(false);
        if (in_rect(crown_btn(), mx, my)) return;
    }
    if (in_rect(crown_btn(), mx, my)) { toggle_menu(true); return; }
    for (int i = 0; i < HL_NAPPS; i++) if (in_rect(dock_item(i), mx, my)) { activate_app(i); return; }
    if (in_rect(dock_rect(), mx, my) || my < HL_BAR_H) return;
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
            if (w->maximized) {   /* dragging a maximised window restores it under the cursor */
                w->maximized = false;
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
    if (screen_off) { screen_off = false; damage(0, 0, W, H); }
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
    }
    int pressed = b & ~mbtn, released = mbtn & ~b;
    mbtn = b;
    if (pressed & 1) click(1);
    if (pressed & 2) click(2);
    if (released & 3) {
        drag_win = -1;
        if (resize_win >= 0) { resize_win = -1; update_hover(); }
        if (capture_win >= 0) { send_mouse(capture_win, EV_MOUSE_UP, (released & 1) ? 1 : 2); capture_win = -1; }
    }
    if (moved && drag_win < 0 && resize_win < 0) {
        update_hover();
        int target = capture_win >= 0 ? capture_win : window_at(mx, my);
        bool content = target >= 0 && my >= wins[target].y + TITLE;
        if (content) send_mouse(target, EV_MOUSE_MOVE, 0);
        if (hover_win >= 0 && hover_win != (content ? target : -1) && wins[hover_win].used) send_mouse(hover_win, EV_MOUSE_LEAVE, 0);
        hover_win = content ? target : -1;
    }
    if (wheel) {
        int t = window_at(mx, my);
        if (t >= 0) { struct dunc_event e = { EV_WHEEL, wins[t].id, mx - wins[t].x - 1, my - wins[t].y - TITLE, wheel, 0 }; event_push(wins[t].owner, &e); }
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
    if (screen_off) { screen_off = false; damage(0, 0, W, H); return; }      /* the key just wakes the screen */
    if (locked) { lock_key(k); return; }
    if (k == (KEYMOD_SUPER | 'l')) { desktop_lock(); return; }
    if (KEY_CODE(k) == KEY_PRTSC) { take_screenshot(); return; }
    if (k == (KEYMOD_ALT | KEY_F4)) { int t = top_window(); if (t >= 0) close_window_clicked(&wins[t]); return; }
    if (k == KEY_SUPER) { toggle_menu(!menu_open); return; }
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

void desktop_init(void) {
    struct bootinfo *b = &bootinfo;
    W = b->fb_w; H = b->fb_h; fb_stride = b->fb_pitch / 4;
    fb32 = mmio_virt(b->fb_addr);
    /* a smaller size chosen in Settings > Display that the graphics card didn't switch to: draw at that size and stretch */
    const char *m = settings_get("display.mode");
    int rw = (int)katoi(m), rh = 0;
    for (const char *p = m; *p; p++) if (*p == 'x') { rh = (int)katoi(p + 1); break; }
    if (rw >= 800 && rh >= 600 && rw <= W && rh <= H && (rw != W || rh != H)) {
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
    struct framebuffer wf = { wall, W, H, W * 4 };
    ax_draw_wallpaper(&wf);
    mx = W / 2; my = H / 2;
    desktop_ok = true;
}

void desktop_size(int *w, int *h, bool *is_scaled) { *w = W; *h = H; if (is_scaled) *is_scaled = scaled; }

void desktop_thread(void *arg) {
    damage(0, 0, W, H);
    for (;;) {
        ps2_poll();
        xhci_poll();
        int s;
        while ((s = serial_getc()) >= 0) input_push(s == '\r' ? '\n' : s == 127 ? '\b' : s);
        int k;
        while ((k = input_pop()) >= 0) handle_key(k);
        int dx, dy, b, wh;
        while (mouse_pop(&dx, &dy, &b, &wh)) handle_mouse(dx, dy, b, wh);

        uint64_t now = uptime_ms();
        for (int i = 0; i < 4; i++)
            if (toasts[i].used && now > toasts[i].until) { toasts[i].used = false; struct rect r = toast_rect(i); damage(r.x - 6, r.y, r.w + 12, r.h + 16); }
        static uint64_t last_check;
        if (now - last_check > 250) {
            last_check = now;
            if (!last_input) { last_input = now; if (settings_get("lock.password")[0]) desktop_lock(); }   /* locked at start-up */
            int lock_min = settings_int("lock.idle_min"), off_min = settings_int("screen.off_min");
            if (!locked && lock_min > 0 && settings_get("lock.password")[0] && now - last_input > (uint64_t)lock_min * 60000) desktop_lock();
            if (!screen_off && off_min > 0 && now - last_input > (uint64_t)off_min * 60000) { screen_off = true; damage(0, 0, W, H); }
            static int last_min = -1;                       /* the lock screen's clock */
            if (locked && !screen_off) { struct rtc_time tt; rtc_read_local(&tt); if (tt.min != last_min) { last_min = tt.min; damage(0, 0, W, H); } }
            char app[48], clk[32], sig[96];
            bar_text(app, sizeof app, clk, sizeof clk);
            ksnprintf(sig, sizeof sig, "%s|%s|%d", app, clk, net_present() + 2 * net_ready());
            if (strcmp(sig, bar_sig)) { strncpy0(bar_sig, sig, sizeof bar_sig); damage_r(bar_rect()); }
            int rs = HL_NAPPS << 20;
            for (int i = 0; i < HL_NAPPS; i++) if (app_running(i)) rs |= 1 << i;
            if (rs != run_sig) {
                struct rect old = dock_area_cached;
                run_sig = rs;
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
        for (int i = 0; i < n; i++) compose(todo[i]);
        task_sleep(8);
    }
}
