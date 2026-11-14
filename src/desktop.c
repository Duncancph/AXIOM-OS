/* DuncOS desktop: window manager + compositor (runs as a kernel thread).
 * Windows are drawn by their programs into private pixel buffers; this thread
 * composes them with the wallpaper, shadows, taskbar, Start menu, notifications
 * and cursor into a back buffer, and copies changed areas to the screen. */
#include "kernel.h"
#include "../common/gfx.h"
#include "font8x16.h"

#define TITLE_H 36
#define RADIUS 10
#define SHADOW 20
#define TB_H 54
#define MAX_WIN 32

#define ACCENT   0x4FD1E8
#define GOLD     0xFFC83D

struct window {
    bool used, minimized;
    int id, x, y, w, h;
    struct task *owner;
    char title[64];
    uint32_t *buf; int pages; uint64_t uva;
    uint64_t close_req;
};

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN], nz, next_wid = 1;
static uint32_t *fb32, *bb, *wall;
static int W, H, fb_stride;
static bool fb_xrgb, splash_shown, desktop_ok;
static struct canvas cv;

static int mx, my, mbtn, drag_win = -1, drag_ox, drag_oy, capture_win = -1, hover_win = -1;
static int hover_ctl;   /* id of the control under the cursor (for highlights) */
static int hx, hy, hw, hh;   /* its damage rect */
static bool menu_open;
static int last_min = -1;

struct toast { bool used; char title[64], msg[160]; uint64_t until; };
static struct toast toasts[4];

/* ---------------- pinned apps ---------------- */
enum { IC_TERM, IC_MON, IC_BT, IC_CALC, IC_SHIELD, IC_CROWN };
struct pin { const char *app, *label; uint32_t c1, c2; int icon; };
static const struct pin pins[] = {
    {"terminal",  "Terminal",        0x3A4152, 0x161A22, IC_TERM},
    {"monitor",   "System Monitor",  0x34D399, 0x047857, IC_MON},
    {"bluetooth", "Bluetooth",       0x60A5FA, 0x1D4ED8, IC_BT},
    {"calc",      "Calculator",      0xFDBA74, 0xEA580C, IC_CALC},
    {"sandbox",   "Protection Test", 0xF87171, 0xB91C1C, IC_SHIELD},
    {"about",     "About DuncOS",    0x22303F, 0x0A1428, IC_CROWN},
};
#define NPINS ((int)(sizeof pins / sizeof pins[0]))

/* ---------------- damage tracking ---------------- */
struct rect { int x, y, w, h; };
static struct rect dmg[32]; static int ndmg;

static bool overlap(struct rect a, struct rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static struct rect runion(struct rect a, struct rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}
static void damage(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    struct rect r = { x, y, w, h };
    for (int i = 0; i < ndmg; i++)
        if (overlap(dmg[i], r)) { r = runion(r, dmg[i]); dmg[i] = dmg[--ndmg]; i = -1; }
    if (ndmg == 32) { for (int i = 1; i < ndmg; i++) dmg[0] = runion(dmg[0], dmg[i]); ndmg = 1; dmg[0] = runion(dmg[0], r); return; }
    dmg[ndmg++] = r;
}
static void damage_all(void) { ndmg = 0; damage(0, 0, W, H); }
static void damage_frame(struct window *w) { damage(w->x - SHADOW, w->y - SHADOW, w->w + 2 * SHADOW, w->h + TITLE_H + 2 * SHADOW + 8); }

/* ---------------- drawing helpers ---------------- */
static void draw_crown(struct canvas *c, int x, int y, int w, int h, uint32_t col) {
    int peak_h = h * 62 / 100;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            bool in;
            if (j >= peak_h) in = j < h;
            else {
                int q = w / 4, k = i % (2 * q), dist = k < q ? k : 2 * q - k;  /* zig-zag top edge */
                int top = dist * peak_h * 8 / 10 / (q ? q : 1);
                in = j >= top;
            }
            if (in) gfx_fill(c, x + i, y + j, 1, 1, col);
        }
    int r = w / 12 + 1;
    gfx_circle(c, x + 1, y + 1, r, col, 255);
    gfx_circle(c, x + w / 2, y + 1, r, col, 255);
    gfx_circle(c, x + w - 1, y + 1, r, col, 255);
}

static void draw_bt_rune(struct canvas *c, int cx, int cy, int s, uint32_t col, int t) {
    int k = s / 4, top = cy - s / 2, bot = cy + s / 2;
    gfx_line(c, cx, top, cx, bot, col, t);
    gfx_line(c, cx, top, cx + k, top + k, col, t);
    gfx_line(c, cx + k, top + k, cx - k, bot - k, col, t);
    gfx_line(c, cx - k, top + k, cx + k, bot - k, col, t);
    gfx_line(c, cx + k, bot - k, cx, bot, col, t);
}

static void draw_icon(struct canvas *c, int x, int y, int s, const struct pin *p) {
    gfx_rrect_vgrad(c, x, y, s, s, s / 4, p->c1, p->c2);
    gfx_rrect_outline(c, x, y, s, s, s / 4, 0xFFFFFF, 40);
    int cx = x + s / 2, cy = y + s / 2;
    switch (p->icon) {
    case IC_TERM:
        gfx_line(c, x + s * 26 / 100, y + s * 34 / 100, x + s * 44 / 100, y + s / 2, 0xFFFFFF, s > 40 ? 3 : 2);
        gfx_line(c, x + s * 44 / 100, y + s / 2, x + s * 26 / 100, y + s * 66 / 100, 0xFFFFFF, s > 40 ? 3 : 2);
        gfx_fill(c, x + s * 50 / 100, y + s * 64 / 100, s * 24 / 100, s > 40 ? 3 : 2, ACCENT);
        break;
    case IC_MON: {
        int bw = s / 8, base = y + s * 74 / 100;
        int hts[3] = { s * 22 / 100, s * 44 / 100, s * 32 / 100 };
        for (int i = 0; i < 3; i++) gfx_rrect(c, x + s * 24 / 100 + i * (bw + s / 10), base - hts[i], bw, hts[i], 2, 0xFFFFFF, 255);
        break;
    }
    case IC_BT: draw_bt_rune(c, cx, cy, s * 56 / 100, 0xFFFFFF, s > 40 ? 3 : 2); break;
    case IC_CALC: {
        int g = s * 15 / 100, o = s * 20 / 100;
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++)
            gfx_rrect(c, x + o + i * (g + s * 12 / 100), y + o + j * (g + s * 12 / 100), g + 4, g + 4, 3, (i + j) == 2 ? 0xFFFFFF : 0xFFF1E6, (i + j) == 2 ? 255 : 210);
        break;
    }
    case IC_SHIELD: {
        int sw = s * 50 / 100, sh = s * 58 / 100, sx = cx - sw / 2, sy = y + s * 20 / 100;
        for (int j = 0; j < sh; j++) {
            int half = sw / 2;
            if (j > sh * 45 / 100) half = half * (sh - j) / (sh - sh * 45 / 100);
            gfx_fill(c, cx - half, sy + j, half * 2, 1, 0xFFFFFF);
        }
        gfx_fill(c, cx - 1, sy + sh * 18 / 100, 3, sh * 36 / 100, 0xDC2626);
        gfx_fill(c, cx - 1, sy + sh * 62 / 100, 3, 3, 0xDC2626);
        (void)sx;
        break;
    }
    case IC_CROWN: draw_crown(c, x + s * 22 / 100, y + s * 30 / 100, s * 56 / 100, s * 40 / 100, GOLD); break;
    }
}

/* Soft drop shadow around a rounded rectangle. */
static void draw_shadow(int x, int y, int w, int h, int spread, int maxa) {
    int x0 = x - spread, y0 = y - spread, x1 = x + w + spread, y1 = y + h + spread;
    if (x0 < cv.cx0) x0 = cv.cx0;
    if (y0 < cv.cy0) y0 = cv.cy0;
    if (x1 > cv.cx1) x1 = cv.cx1;
    if (y1 > cv.cy1) y1 = cv.cy1;
    int r = RADIUS, hx2 = w - 2 * r, hy2 = h - 2 * r;
    for (int j = y0; j < y1; j++) {
        uint32_t *p = cv.px + j * cv.stride;
        for (int i = x0; i < x1; i++) {
            /* rounded-rect signed distance, in 1/4 pixels */
            int px = (i - x) * 4 + 2 - w * 2, py = (j - y) * 4 + 2 - h * 2;
            if (px < 0) px = -px;
            if (py < 0) py = -py;
            int qx = px - hx2 * 2, qy = py - hy2 * 2;
            if (qx < 0 && qy < 0 && j > y + r && j < y + h - r) {   /* deep inside: hidden by the window */
                if (i < x + w - r) { i = x + w - r - 1; }
                continue;
            }
            int d;
            if (qx > 0 && qy > 0) d = gfx_isqrt(qx * qx + qy * qy);
            else d = qx > qy ? qx : qy;
            d -= r * 4;
            if (d >= spread * 4) continue;
            int a = d <= 0 ? maxa : maxa * (spread * 4 - d) * (spread * 4 - d) / (spread * spread * 16);
            if (a > 0) p[i] = gfx_mix(p[i], 0x000000, a);
        }
    }
}

/* ---------------- layout ---------------- */
static int tb_y(void) { return H - TB_H; }
static int dock_n(void) { return NPINS + 1; }
static struct rect dock_item(int i) {      /* 0 = Start button, 1.. = pinned apps */
    int total = dock_n() * 46 + (dock_n() - 1) * 6;
    return (struct rect){ (W - total) / 2 + i * 52, tb_y() + 5, 46, 44 };
}
#define MENU_W 452
#define MENU_H 410
static struct rect menu_rect(void) { return (struct rect){ (W - MENU_W) / 2, tb_y() - 12 - MENU_H, MENU_W, MENU_H }; }
static struct rect menu_cell(int i) {
    struct rect m = menu_rect();
    return (struct rect){ m.x + 22 + (i % 3) * 136, m.y + 104 + (i / 3) * 112, 136, 104 };
}
static struct rect menu_btn(int k) {       /* 0 = restart, 1 = shut down */
    struct rect m = menu_rect();
    return (struct rect){ m.x + m.w - 20 - (2 - k) * 112 + 6, m.y + m.h - 50, 104, 34 };
}
static bool in_rect(struct rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static struct rect close_btn(struct window *w) { return (struct rect){ w->x + w->w - 44, w->y + 4, 38, 28 }; }
static struct rect min_btn(struct window *w) { return (struct rect){ w->x + w->w - 84, w->y + 4, 38, 28 }; }

static int top_window(void) {
    for (int i = nz - 1; i >= 0; i--) if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}

static bool pin_running(const struct pin *p, bool *focused) {
    bool run = false; int top = top_window();
    if (focused) *focused = false;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].owner->app && !strcmp(wins[i].owner->app, p->app)) {
            run = true;
            if (focused && i == top) *focused = true;
        }
    if (!run)
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].app && !strcmp(tasks[i].app, p->app)) run = true;
    return run;
}

/* ---------------- composition ---------------- */
static void draw_window(int idx) {
    struct window *w = &wins[idx];
    bool active = idx == top_window();
    int fh = w->h + TITLE_H;
    draw_shadow(w->x, w->y + 6, w->w, fh, SHADOW, active ? 120 : 70);
    uint32_t tcol = active ? 0x1F2533 : 0x181C27;
    gfx_rrect(&cv, w->x, w->y, w->w, fh, RADIUS, tcol, 255);
    gfx_blit_rr(&cv, w->x, w->y + TITLE_H, w->buf, w->w, w->h, w->w, w->x, w->y, w->w, fh, RADIUS);
    gfx_rrect_outline(&cv, w->x, w->y, w->w, fh, RADIUS, active ? 0x5A6782 : 0x343B4C, 255);

    /* title: app icon + text */
    int tx = w->x + 14;
    for (int i = 0; i < NPINS; i++)
        if (w->owner->app && !strcmp(w->owner->app, pins[i].app)) { draw_icon(&cv, tx, w->y + 9, 18, &pins[i]); tx += 26; break; }
    char t[80];
    bool hung = w->close_req && uptime_ms() - w->close_req > 1000;
    ksnprintf(t, sizeof t, "%s%s", w->title, hung ? "  (Not responding)" : "");
    gfx_text(&cv, &font_ui_bold, tx, w->y + 23, active ? 0xEEF1F7 : 0x8B93A7, t);

    /* minimize + close */
    struct rect cb = close_btn(w), mb = min_btn(w);
    if (hover_ctl == 1000 + idx * 2) gfx_rrect(&cv, cb.x, cb.y, cb.w, cb.h, 7, 0xE5484D, 255);
    if (hover_ctl == 1001 + idx * 2) gfx_rrect(&cv, mb.x, mb.y, mb.w, mb.h, 7, 0xFFFFFF, 28);
    uint32_t gcol = active || hover_ctl == 1000 + idx * 2 ? 0xE6E9F0 : 0x8B93A7;
    int ccx = cb.x + cb.w / 2, ccy = cb.y + cb.h / 2;
    gfx_line(&cv, ccx - 5, ccy - 5, ccx + 5, ccy + 5, gcol, 2);
    gfx_line(&cv, ccx - 5, ccy + 5, ccx + 5, ccy - 5, gcol, 2);
    gfx_fill(&cv, mb.x + mb.w / 2 - 6, mb.y + mb.h / 2, 12, 2, gcol);
}

static void draw_taskbar(void) {
    int y = tb_y();
    gfx_blend(&cv, 0, y, W, TB_H, 0x0C111C, 215);
    gfx_blend(&cv, 0, y, W, 1, 0xFFFFFF, 30);

    for (int i = 0; i < dock_n(); i++) {
        struct rect r = dock_item(i);
        bool hover = hover_ctl == 100 + i;
        bool focused = false, running = i > 0 && pin_running(&pins[i - 1], &focused);
        if (hover || focused || (i == 0 && menu_open)) gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, 0xFFFFFF, hover ? 34 : 20);
        if (i == 0) draw_crown(&cv, r.x + 11, r.y + 14, 24, 17, GOLD);
        else draw_icon(&cv, r.x + 8, r.y + 7, 30, &pins[i - 1]);
        if (running) {
            int pw = focused ? 16 : 6;
            gfx_rrect(&cv, r.x + (r.w - pw) / 2, r.y + r.h - 3, pw, 3, 1, focused ? ACCENT : 0x9AA3B5, 255);
        }
    }

    /* clock + date + Bluetooth status */
    struct rtc_time t; rtc_read(&t);
    static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char tm[16], dt[24];
    ksnprintf(tm, sizeof tm, "%02d:%02d", t.hour, t.min);
    ksnprintf(dt, sizeof dt, "%d %s %d", t.day, (t.mon >= 1 && t.mon <= 12) ? mon[t.mon - 1] : "?", t.year);
    int tw = gfx_text_width(&font_ui, dt), cw = gfx_text_width(&font_ui, tm);
    int rx = W - 18 - tw;
    gfx_text(&cv, &font_ui, W - 18 - cw, y + 23, 0xEEF1F7, tm);
    gfx_text(&cv, &font_ui, rx, y + 42, 0x9AA3B5, dt);
    draw_bt_rune(&cv, rx - 26, y + TB_H / 2, 18, bt_present() ? 0xEEF1F7 : 0x4B5366, 2);

    /* left: OS name */
    gfx_text(&cv, &font_ui_bold, 18, y + 23, 0xEEF1F7, OS_NAME);
    gfx_text(&cv, &font_ui, 18, y + 42, 0x9AA3B5, KERNEL_NAME " kernel v" OS_VERSION);

    /* tooltip */
    if (hover_ctl >= 101 && hover_ctl < 100 + dock_n() && !menu_open) {
        struct rect r = dock_item(hover_ctl - 100);
        const char *l = pins[hover_ctl - 101].label;
        int lw = gfx_text_width(&font_ui, l) + 22;
        int bx = r.x + r.w / 2 - lw / 2, by = y - 38;
        gfx_rrect(&cv, bx, by, lw, 28, 8, 0x1B2130, 245);
        gfx_rrect_outline(&cv, bx, by, lw, 28, 8, 0x3A4256, 255);
        gfx_text(&cv, &font_ui, bx + 11, by + 19, 0xEEF1F7, l);
    }
}

static void draw_menu(void) {
    struct rect m = menu_rect();
    draw_shadow(m.x, m.y + 8, m.w, m.h, 24, 140);
    gfx_rrect(&cv, m.x, m.y, m.w, m.h, 14, 0x161B27, 248);
    gfx_rrect_outline(&cv, m.x, m.y, m.w, m.h, 14, 0x3A4256, 255);
    draw_crown(&cv, m.x + 24, m.y + 26, 34, 24, GOLD);
    gfx_text(&cv, &font_title, m.x + 72, m.y + 42, 0xF0F0F0, OS_NAME);
    gfx_text(&cv, &font_ui, m.x + 72 + gfx_text_width(&font_title, OS_NAME) + 10, m.y + 42, 0x8B93A7, "Thane kernel v" OS_VERSION);
    gfx_text(&cv, &font_ui_bold, m.x + 26, m.y + 90, 0x9AA3B5, "Apps");
    for (int i = 0; i < NPINS; i++) {
        struct rect r = menu_cell(i);
        if (hover_ctl == 200 + i) gfx_rrect(&cv, r.x + 4, r.y, r.w - 8, r.h, 10, 0xFFFFFF, 22);
        draw_icon(&cv, r.x + (r.w - 48) / 2, r.y + 12, 48, &pins[i]);
        gfx_text_center(&cv, &font_ui, r.x, r.w, r.y + 84, 0xE6E9F0, pins[i].label);
    }
    gfx_blend(&cv, m.x + 1, m.y + m.h - 68, m.w - 2, 1, 0xFFFFFF, 22);
    gfx_text(&cv, &font_ui, m.x + 26, m.y + m.h - 28, 0x8B93A7, "Created by Bryan Duncan");
    static const char *bl[] = { "Restart", "Shut down" };
    for (int k = 0; k < 2; k++) {
        struct rect r = menu_btn(k);
        bool h = hover_ctl == 300 + k;
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 8, k ? (h ? 0xE5484D : 0x2A3142) : (h ? 0x3A4256 : 0x2A3142), 255);
        gfx_text_center(&cv, &font_ui_bold, r.x, r.w, r.y + 22, 0xF0F0F0, bl[k]);
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

static struct rect toast_rect(int i) { return (struct rect){ W - 376, 16 + i * 102, 360, 92 }; }

static void draw_toasts(void) {
    for (int i = 0; i < 4; i++) {
        if (!toasts[i].used) continue;
        struct rect r = toast_rect(i);
        bool bad = strncmp(toasts[i].title + strlen(toasts[i].title) - 7, "crashed", 7) == 0;
        draw_shadow(r.x, r.y + 4, r.w, r.h, 14, 110);
        gfx_rrect(&cv, r.x, r.y, r.w, r.h, 12, 0x1B2130, 250);
        gfx_rrect_outline(&cv, r.x, r.y, r.w, r.h, 12, 0x3A4256, 255);
        gfx_rrect(&cv, r.x + 12, r.y + 14, 4, r.h - 28, 2, bad ? 0xE5484D : ACCENT, 255);
        gfx_text(&cv, &font_ui_bold, r.x + 28, r.y + 28, 0xF0F0F0, toasts[i].title);
        char lines[3][80];
        int n = wrap_lines(toasts[i].msg, lines, r.w - 44);
        for (int k = 0; k < n; k++) gfx_text(&cv, &font_ui, r.x + 28, r.y + 50 + k * 18, 0xB4BBCB, lines[k]);
    }
}

static const char *cursor_img[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX" };
static void draw_cursor(void) {
    for (int j = 0; j < 19; j++)
        for (int i = 0; cursor_img[j][i]; i++) {
            char ch = cursor_img[j][i];
            if (ch == 'X') gfx_fill(&cv, mx + i, my + j, 1, 1, 0x000000);
            else if (ch == '.') gfx_fill(&cv, mx + i, my + j, 1, 1, 0xFFFFFF);
        }
}

static void flush_rect(struct rect r) {
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *s = bb + j * W + r.x;
        volatile uint32_t *d = fb32 + (size_t)j * fb_stride + r.x;
        if (fb_xrgb) for (int i = 0; i < r.w; i++) d[i] = s[i];
        else {
            struct bootinfo *b = &bootinfo;
            for (int i = 0; i < r.w; i++) {
                uint32_t c = s[i];
                d[i] = ((c >> 16 & 255) >> (8 - b->r_size)) << b->r_pos | ((c >> 8 & 255) >> (8 - b->g_size)) << b->g_pos |
                       ((c & 255) >> (8 - b->b_size)) << b->b_pos;
            }
        }
    }
}

static void compose(struct rect r) {
    gfx_clip(&cv, r.x, r.y, r.w, r.h);
    gfx_blit(&cv, 0, 0, wall, W, H, W);
    for (int z = 0; z < nz; z++) {
        struct window *w = &wins[zorder[z]];
        if (w->minimized) continue;
        struct rect fr = { w->x - SHADOW, w->y - SHADOW, w->w + 2 * SHADOW, w->h + TITLE_H + 2 * SHADOW + 8 };
        if (overlap(fr, r)) draw_window(zorder[z]);
    }
    if (overlap((struct rect){ 0, tb_y() - 44, W, TB_H + 44 }, r)) draw_taskbar();
    if (menu_open) { struct rect m = menu_rect(); m.x -= 30; m.y -= 30; m.w += 60; m.h += 70; if (overlap(m, r)) draw_menu(); }
    draw_toasts();
    if (overlap((struct rect){ mx, my, 13, 20 }, r)) draw_cursor();
    flush_rect(r);
}

/* ---------------- wallpaper ---------------- */
static void glow(int cx, int cy, int rad, uint32_t col, int maxa) {
    for (int y = cy - rad < 0 ? 0 : cy - rad; y < H && y < cy + rad; y++)
        for (int x = cx - rad < 0 ? 0 : cx - rad; x < W && x < cx + rad; x++) {
            int dx = x - cx, dy = y - cy;
            int d = gfx_isqrt(dx * dx + dy * dy);
            if (d >= rad) continue;
            int t = rad - d;
            int a = maxa * t / rad * t / rad;
            wall[y * W + x] = gfx_mix(wall[y * W + x], col, a);
        }
}

static void build_wallpaper(void) {
    struct canvas wc; gfx_init(&wc, wall, W, H, W);
    gfx_vgrad(&wc, 0, 0, W, H, 0x0B1630, 0x04050A);
    glow(W * 18 / 100, H * 22 / 100, W * 55 / 100, 0x1E6E8C, 150);
    glow(W * 88 / 100, H * 92 / 100, W * 55 / 100, 0x4C2A85, 150);
    glow(W * 62 / 100, H * 8 / 100, W * 30 / 100, 0x8A6A1E, 60);
    /* faint DuncOS crown watermark */
    static const char *crown[] = {
        "            .     '     .", "        .  /\\    /\\    /\\  .", "          /  \\  /  \\  /  \\",
        "         /    \\/    \\/    \\", "        |__________________|" };
    int s = W >= 1600 ? 3 : 2, cw = 28 * 8 * s;
    int cx0 = (W - cw) / 2, cy0 = (H - TB_H) / 2 - 5 * 16 * s / 2;
    for (int l = 0; l < 5; l++)
        for (int i = 0; crown[l][i]; i++) {
            char ch = crown[l][i];
            if (ch <= ' ' || ch > FONT_LAST) continue;
            const uint8_t *g = font8x16[ch - FONT_FIRST];
            for (int r = 0; r < 16; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (0x80 >> b))
                        gfx_blend(&wc, cx0 + (i * 8 + b) * s, cy0 + (l * 16 + r) * s, s, s, GOLD, 34);
        }
}

/* ---------------- window management ---------------- */
static void focus_changed(int old_top) {
    int t = top_window();
    if (t == old_top) return;
    if (old_top >= 0 && wins[old_top].used) {
        struct dunc_event e = { EV_BLUR, wins[old_top].id, 0, 0, 0, 0 };
        event_push(wins[old_top].owner, &e);
        damage(wins[old_top].x, wins[old_top].y, wins[old_top].w, TITLE_H);
    }
    if (t >= 0) {
        struct dunc_event e = { EV_FOCUS, wins[t].id, 0, 0, 0, 0 };
        event_push(wins[t].owner, &e);
        damage_frame(&wins[t]);
    }
    damage(0, tb_y(), W, TB_H);
}

static void raise(int idx) {
    int old = top_window();
    int k = 0;
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
    if (capture_win == idx) capture_win = -1;
    if (hover_win == idx) hover_win = -1;
    hover_ctl = 0;
    focus_changed(old == idx ? -1 : old);
    damage(0, tb_y(), W, TB_H);
}

static struct window *find(struct task *t, int id) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].id == id && wins[i].owner == t) return &wins[i];
    return 0;
}

struct window *wm_create(struct task *t, int w, int h, const char *title, uint32_t **buf, int *pages, uint64_t uva) {
    if (!desktop_ok || w < 100 || h < 60 || w > W - 20 || h > H - TB_H - TITLE_H - 10) return 0;
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
    strncpy0(win->title, title, sizeof win->title);
    for (int i = 0; i < w * h; i++) b[i] = 0x161A23;
    int off = (open % 5) * 32 - 48;
    win->x = (W - w) / 2 + off;
    win->y = (H - TB_H - h - TITLE_H) / 2 + off;
    if (win->x < 8) win->x = 8;
    if (win->y < 8) win->y = 8;
    if (win->x + w > W - 8) win->x = W - 8 - w;
    if (win->y + h + TITLE_H > H - TB_H - 8) win->y = H - TB_H - 8 - h - TITLE_H;
    raise(idx);
    *buf = b; *pages = np;
    return win;
}

int wm_id(struct window *w) { return w->id; }

bool wm_flush(struct task *t, int id, int x, int y, int w, int h) {
    struct window *win = find(t, id);
    if (!win) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    if (!win->minimized && w > 0 && h > 0) damage(win->x + x, win->y + TITLE_H + y, w, h);
    win->close_req = 0;    /* it's alive */
    return true;
}

bool wm_set_title(struct task *t, int id, const char *title) {
    struct window *win = find(t, id);
    if (!win) return false;
    strncpy0(win->title, title, sizeof win->title);
    damage(win->x, win->y, win->w, TITLE_H);
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
    damage(0, tb_y(), W, TB_H);
}

int wm_count(struct task *t) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].owner == t) n++;
    return n;
}

void desktop_notify(const char *title, const char *msg) {
    kprintf("[notify] %s: %s\n", title, msg);
    if (!desktop_ok) return;
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!toasts[i].used) { slot = i; break; }
    if (slot < 0) { memmove(&toasts[0], &toasts[1], sizeof toasts[0] * 3); slot = 3; damage(W - 400, 0, 400, 4 * 102 + 30); }
    strncpy0(toasts[slot].title, title, sizeof toasts[slot].title);
    strncpy0(toasts[slot].msg, msg, sizeof toasts[slot].msg);
    toasts[slot].until = uptime_ms() + 6000;
    toasts[slot].used = true;
    struct rect r = toast_rect(slot);
    damage(r.x - 20, r.y - 20, r.w + 40, r.h + 44);
}

/* ---------------- input ---------------- */
static void send_mouse(int idx, int type, int button) {
    struct window *w = &wins[idx];
    struct dunc_event e = { type, w->id, mx - w->x, my - w->y - TITLE_H, 0, button };
    event_push(w->owner, &e);
}

static int window_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        if (!w->minimized && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h + TITLE_H) return zorder[i];
    }
    return -1;
}

static void set_hover(int ctl, struct rect r) {
    if (ctl == hover_ctl) return;
    if (hover_ctl) damage(hx, hy, hw, hh);
    hover_ctl = ctl; hx = r.x; hy = r.y; hw = r.w; hh = r.h;
    if (ctl) damage(hx, hy, hw, hh);
}

static void update_hover(void) {
    if (menu_open) {
        for (int i = 0; i < NPINS; i++) { struct rect r = menu_cell(i); if (in_rect(r, mx, my)) { set_hover(200 + i, r); return; } }
        for (int k = 0; k < 2; k++) { struct rect r = menu_btn(k); if (in_rect(r, mx, my)) { set_hover(300 + k, r); return; } }
    }
    if (my >= tb_y()) {
        for (int i = 0; i < dock_n(); i++) {
            struct rect r = dock_item(i);
            if (in_rect(r, mx, my)) { struct rect d = { r.x - 120, r.y - 50, r.w + 240, r.h + 50 }; set_hover(100 + i, d); return; }
        }
        set_hover(0, (struct rect){0, 0, 0, 0});
        return;
    }
    int wi = window_at(mx, my);
    if (wi >= 0) {
        struct window *w = &wins[wi];
        struct rect cb = close_btn(w), mb = min_btn(w);
        if (in_rect(cb, mx, my)) { set_hover(1000 + wi * 2, cb); return; }
        if (in_rect(mb, mx, my)) { set_hover(1001 + wi * 2, mb); return; }
    }
    set_hover(0, (struct rect){0, 0, 0, 0});
}

static void toggle_menu(bool open) {
    if (menu_open == open) return;
    menu_open = open;
    struct rect m = menu_rect();
    damage(m.x - 40, m.y - 40, m.w + 80, m.h + 90);
    damage(0, tb_y(), W, TB_H);
    hover_ctl = 0;
    update_hover();
}

static void launch(const struct pin *p) {
    int pid = task_spawn(p->app);
    if (pid < 0) {
        char m[96]; ksnprintf(m, sizeof m, "Couldn't start %s (out of memory or too many apps).", p->label);
        desktop_notify("Couldn't open app", m);
    }
    damage(0, tb_y(), W, TB_H);
}

static void activate_pin(const struct pin *p) {
    int top = top_window(), best = -1;
    for (int z = nz - 1; z >= 0; z--) {
        struct window *w = &wins[zorder[z]];
        if (w->owner->app && !strcmp(w->owner->app, p->app)) { best = zorder[z]; break; }
    }
    if (best < 0) {
        if (!pin_running(p, 0)) launch(p);
        return;
    }
    if (best == top) { int old = top; wins[best].minimized = true; damage_frame(&wins[best]); focus_changed(old); }
    else raise(best);
}

static void click(int button) {
    if (menu_open) {
        struct rect m = menu_rect();
        if (in_rect(m, mx, my)) {
            for (int i = 0; i < NPINS; i++) if (in_rect(menu_cell(i), mx, my)) { toggle_menu(false); launch(&pins[i]); return; }
            if (in_rect(menu_btn(0), mx, my)) acpi_reboot();
            if (in_rect(menu_btn(1), mx, my)) { acpi_poweroff(); desktop_notify("Shut down", "This computer doesn't support ACPI power-off."); }
            return;
        }
        toggle_menu(false);
        if (in_rect(dock_item(0), mx, my)) return;
    }
    if (my >= tb_y()) {
        for (int i = 0; i < dock_n(); i++)
            if (in_rect(dock_item(i), mx, my)) {
                if (i == 0) toggle_menu(!menu_open);
                else activate_pin(&pins[i - 1]);
            }
        return;
    }
    int wi = window_at(mx, my);
    if (wi < 0) return;
    struct window *w = &wins[wi];
    if (wi != top_window()) raise(wi);
    if (my < w->y + TITLE_H) {
        if (button != 1) return;
        if (in_rect(close_btn(w), mx, my)) {
            if (w->close_req && uptime_ms() - w->close_req > 1000) {
                char m[128]; ksnprintf(m, sizeof m, "%s wasn't responding, so DuncOS closed it.", w->owner->name);
                struct task *t = w->owner;
                task_kill(t->pid);
                desktop_notify("App closed", m);
            } else {
                if (!w->close_req) w->close_req = uptime_ms();
                struct dunc_event e = { EV_CLOSE, w->id, 0, 0, 0, 0 };
                event_push(w->owner, &e);
            }
        } else if (in_rect(min_btn(w), mx, my)) {
            int old = top_window(); w->minimized = true; damage_frame(w); focus_changed(old);
            hover_ctl = 0;
        } else { drag_win = wi; drag_ox = mx - w->x; drag_oy = my - w->y; }
        return;
    }
    capture_win = wi;
    send_mouse(wi, EV_MOUSE_DOWN, button);
}

static void handle_mouse(int dx, int dy, int b, int wheel) {
    int ox = mx, oy = my;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > 5) dx *= 2;
    if (ay > 5) dy *= 2;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= W) mx = W - 1;
    if (my >= H) my = H - 1;
    bool moved = mx != ox || my != oy;
    if (moved) { damage(ox, oy, 13, 20); damage(mx, my, 13, 20); }

    if (moved && drag_win >= 0) {
        struct window *w = &wins[drag_win];
        damage_frame(w);
        w->x = mx - drag_ox; w->y = my - drag_oy;
        if (w->y < 0) w->y = 0;
        if (w->y > tb_y() - TITLE_H) w->y = tb_y() - TITLE_H;
        if (w->x + w->w < 60) w->x = 60 - w->w;
        if (w->x > W - 60) w->x = W - 60;
        damage_frame(w);
    }
    int pressed = b & ~mbtn, released = mbtn & ~b;
    mbtn = b;
    if (pressed & 1) click(1);
    if (pressed & 2) click(2);
    if (released & 3) {
        if (drag_win >= 0) drag_win = -1;
        if (capture_win >= 0) { send_mouse(capture_win, EV_MOUSE_UP, (released & 1) ? 1 : 2); capture_win = -1; }
    }
    if (moved && drag_win < 0) {
        update_hover();
        int target = capture_win >= 0 ? capture_win : window_at(mx, my);
        if (target >= 0 && my >= wins[target].y + TITLE_H) send_mouse(target, EV_MOUSE_MOVE, 0);
        if (hover_win >= 0 && hover_win != target && wins[hover_win].used) send_mouse(hover_win, EV_MOUSE_LEAVE, 0);
        hover_win = target >= 0 && my >= wins[target].y + TITLE_H ? target : -1;
    }
    if (wheel) {
        int target = window_at(mx, my);
        if (target >= 0) { struct dunc_event e = { EV_WHEEL, wins[target].id, mx - wins[target].x, my - wins[target].y - TITLE_H, wheel, 0 }; event_push(wins[target].owner, &e); }
    }
}

static void handle_key(int k) {
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

struct framebuffer { volatile uint32_t *addr; uint32_t width, height, pitch; };
void boot_logo_show(const struct framebuffer *fb);
void boot_logo_progress(const struct framebuffer *fb, unsigned percent);
static struct framebuffer splash_fb;

void boot_progress(int percent) {
    if (!desktop_available()) return;
    if (!splash_shown) {
        splash_fb = (struct framebuffer){ (volatile uint32_t *)bootinfo.fb_addr, bootinfo.fb_w, bootinfo.fb_h, bootinfo.fb_pitch };
        boot_logo_show(&splash_fb);
        splash_shown = true;
    }
    boot_logo_progress(&splash_fb, percent);
}

void desktop_init(void) {
    struct bootinfo *b = &bootinfo;
    W = b->fb_w; H = b->fb_h; fb_stride = b->fb_pitch / 4;
    fb32 = (uint32_t *)b->fb_addr;
    fb_xrgb = b->r_pos == 16 && b->g_pos == 8 && b->b_pos == 0;
    int pages = (W * H * 4 + 4095) / 4096;
    bb = page_alloc(pages); wall = page_alloc(pages);
    if (!bb || !wall) { kprintf("[desktop] not enough memory for the desktop\n"); return; }
    fb_write_combining(b->fb_addr, (uint64_t)b->fb_pitch * H);
    gfx_init(&cv, bb, W, H, W);
    build_wallpaper();
    mx = W / 2; my = H / 2;
    desktop_ok = true;
}

void desktop_thread(void *arg) {
    damage_all();
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
            if (toasts[i].used && now > toasts[i].until) { toasts[i].used = false; struct rect r = toast_rect(i); damage(r.x - 20, r.y - 20, r.w + 40, r.h + 44); }
        static uint64_t last_check;
        if (now - last_check > 500) {
            last_check = now;
            struct rtc_time t; rtc_read(&t);
            if (t.min != last_min) { last_min = t.min; damage(W - 200, tb_y(), 200, TB_H); }
            for (int i = 0; i < MAX_WIN; i++)        /* show "(Not responding)" */
                if (wins[i].used && wins[i].close_req && now - wins[i].close_req > 1000 && now - wins[i].close_req < 1600)
                    damage(wins[i].x, wins[i].y, wins[i].w, TITLE_H);
            damage(0, tb_y(), W, TB_H);              /* running indicators */
        }
        task_reap();

        int n = ndmg; struct rect todo[32];
        memcpy(todo, dmg, sizeof(struct rect) * n); ndmg = 0;
        for (int i = 0; i < n; i++) compose(todo[i]);
        task_sleep(8);
    }
}
