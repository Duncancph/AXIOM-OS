/* Calendar: a month at a glance, with your own events (saved in /home/.calendar.txt as "YYYY-MM-DD|text" lines). */
#include "../lib/dunc.h"

static int WW = 860, WH = 560;
#define GRIDW 560
#define FILE "/home/.calendar.txt"
static struct app a;

static int ty, tm, td;                 /* today */
static int vy, vm;                     /* the month shown */
static int sy, sm, sd;                 /* the selected day */
struct ev { int y, m, d; char text[100]; };
static struct ev evs[500]; static int nev;
static char input[100]; static int ilen; static bool input_focus = true;   /* typing goes straight into "Add" */
static int hover = -1;

static const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char *WD[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *WDL[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int mdays(int y, int m) { static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && leap(y) ? 29 : d[m - 1]; }
static int weekday(int y, int m, int d) {             /* 0 = Monday */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */
    return (w + 6) % 7;
}

static void load(void) {
    static char buf[64 * 1024];
    long n = fs_read(FILE, buf, 0, sizeof buf - 1);
    nev = 0;
    if (n <= 0) return;
    buf[n] = 0;
    for (char *p = buf; *p && nev < 500;) {
        char *e = p; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        if (e - p > 11 && p[4] == '-' && p[7] == '-' && p[10] == '|') {
            struct ev *v = &evs[nev++];
            v->y = (int)katoi(p); v->m = (int)katoi(p + 5); v->d = (int)katoi(p + 8);
            strncpy0(v->text, p + 11, sizeof v->text);
            utf8_to_cp1252(v->text, strlen(v->text) + 1);
        }
        *e = save; p = *e ? e + 1 : e;
    }
}
static void save(void) {
    static char buf[64 * 1024]; long k = 0;
    for (int i = 0; i < nev && k < (long)sizeof buf - 400; i++) {
        char t[300]; long n = cp1252_to_utf8(evs[i].text, strlen(evs[i].text), t, sizeof t);
        t[n] = 0;
        k += ksnprintf(buf + k, sizeof buf - k, "%04d-%02d-%02d|%s\n", evs[i].y, evs[i].m, evs[i].d, t);
    }
    fs_write(FILE, buf, k);
}
static int count_on(int y, int m, int d) { int n = 0; for (int i = 0; i < nev; i++) if (evs[i].y == y && evs[i].m == m && evs[i].d == d) n++; return n; }

static void cell_rect(int i, int *x, int *y, int *w, int *h) { int cw = (GRIDW - 40) / 7, ch = (WH - 150) / 6; *x = 20 + (i % 7) * cw; *y = 110 + (i / 7) * ch; *w = cw - 4; *h = ch - 4; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    char t[64]; ksnprintf(t, sizeof t, "%s %d", MONTHS[vm - 1], vy);
    gfx_text(c, &font_title, 24, 44, UI_TEXT, t);
    ui_button(c, GRIDW - 216, 18, 40, 34, "<", BTN_NORMAL, hover == 1, false);
    ui_button(c, GRIDW - 170, 18, 84, 34, "Today", BTN_NORMAL, hover == 3, false);
    ui_button(c, GRIDW - 80, 18, 40, 34, ">", BTN_NORMAL, hover == 2, false);
    int cw = (GRIDW - 40) / 7;
    for (int i = 0; i < 7; i++) gfx_text_center(c, &font_ui_bold, 20 + i * cw, cw - 4, 96, i >= 5 ? UI_MUTED : HL_TEXT_DIM, WD[i]);
    int first = weekday(vy, vm, 1), nd = mdays(vy, vm);
    for (int i = 0; i < 42; i++) {
        int d = i - first + 1;
        int x, y, w, h; cell_rect(i, &x, &y, &w, &h);
        if (d < 1 || d > nd) continue;
        bool today = vy == ty && vm == tm && d == td, sel = vy == sy && vm == sm && d == sd;
        gfx_rrect(c, x, y, w, h, 8, sel ? 0x2E2E2B : hover == 100 + i ? 0x1E1E1C : UI_PANEL, 255);
        if (today) gfx_rrect_outline(c, x, y, w, h, 8, UI_ACCENT, 255);
        char ds[4]; ksnprintf(ds, sizeof ds, "%d", d);
        gfx_text(c, today ? &font_ui_bold : &font_ui, x + 8, y + 20, today ? UI_ACCENT : UI_TEXT, ds);
        int n = count_on(vy, vm, d);
        for (int k = 0; k < n && k < 4; k++) gfx_circle(c, x + 12 + k * 10, y + h - 10, 3, UI_ACCENT, 255);
    }
    /* the selected day */
    int px = GRIDW + 10, pw = WW - px - 20;
    ui_card(c, px, 18, pw, WH - 36);
    int wd = weekday(sy, sm, sd);
    char dh[64]; ksnprintf(dh, sizeof dh, "%s %d %s", WDL[wd], sd, MONTHS[sm - 1]);
    gfx_text(c, &font_ui_bold, px + 16, 48, UI_TEXT, dh);
    int y = 72, shown = 0;
    for (int i = 0; i < nev && y < WH - 110; i++) {
        if (!(evs[i].y == sy && evs[i].m == sm && evs[i].d == sd)) continue;
        shown++;
        gfx_rrect(c, px + 12, y, pw - 24, 34, 8, 0x1E1E1C, 255);
        char e[40]; strncpy0(e, evs[i].text, sizeof e);
        gfx_text(c, &font_ui, px + 24, y + 22, UI_TEXT, e);
        gfx_text(c, &font_ui_bold, px + pw - 36, y + 22, hover == 200 + i ? UI_RED : UI_MUTED, "x");
        y += 40;
    }
    if (!shown) gfx_text(c, &font_ui, px + 16, y + 16, UI_MUTED, "Nothing planned.");
    int iy = WH - 96;
    gfx_rrect(c, px + 12, iy, pw - 24, 34, 8, HL_TERM_BG, 255);
    gfx_rrect_outline(c, px + 12, iy, pw - 24, 34, 8, input_focus ? UI_ACCENT : UI_BORDER, 255);
    if (ilen || input_focus) { gfx_text(c, &font_ui, px + 24, iy + 22, UI_TEXT, input); if (input_focus) gfx_fill(c, px + 25 + gfx_text_width(&font_ui, input), iy + 8, 2, 18, UI_ACCENT); }
    else gfx_text(c, &font_ui, px + 24, iy + 22, UI_MUTED, "Add something to this day...");
    ui_button(c, px + 12, iy + 42, pw - 24, 32, "Add", BTN_PRIMARY, hover == 4, !ilen);
    app_flush(&a);
}

static int hit(int x, int y) {
    if (ui_in(x, y, GRIDW - 216, 18, 40, 34)) return 1;
    if (ui_in(x, y, GRIDW - 80, 18, 40, 34)) return 2;
    if (ui_in(x, y, GRIDW - 170, 18, 84, 34)) return 3;
    int px = GRIDW + 10, pw = WW - px - 20;
    if (ui_in(x, y, px + 12, WH - 54, pw - 24, 32)) return 4;
    if (ui_in(x, y, px + 12, WH - 96, pw - 24, 34)) return 5;
    int first = weekday(vy, vm, 1), nd = mdays(vy, vm);
    for (int i = 0; i < 42; i++) { int cx, cy, cw, ch; cell_rect(i, &cx, &cy, &cw, &ch); int d = i - first + 1; if (d >= 1 && d <= nd && ui_in(x, y, cx, cy, cw, ch)) return 100 + i; }
    int ey = 72;
    for (int i = 0; i < nev; i++) {
        if (!(evs[i].y == sy && evs[i].m == sm && evs[i].d == sd)) continue;
        if (ui_in(x, y, px + pw - 48, ey, 36, 34)) return 200 + i;
        ey += 40;
    }
    return -1;
}

static void add(void) {
    if (!ilen || nev >= 500) return;
    struct ev *v = &evs[nev++]; v->y = sy; v->m = sm; v->d = sd; strncpy0(v->text, input, sizeof v->text);
    ilen = 0; input[0] = 0;
    save();
}

int main(void) {
    struct rtc_time t; sys_rtc(&t);
    ty = t.year; tm = t.mon; td = t.day;
    vy = sy = ty; vm = sm = tm; sd = td;
    if (!app_open_ex(&a, WW, WH, "Calendar", WIN_RESIZABLE)) return 1;
    load();
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; if (WW < 700) WW = 700; draw(); } continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } continue; }
        if (e.type == EV_WHEEL) { vm += e.key > 0 ? 1 : -1; if (vm > 12) { vm = 1; vy++; } if (vm < 1) { vm = 12; vy--; } draw(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h != -1) input_focus = h == 5 || h == 4 || (h >= 100 && h < 142);
            if (h == 1) { if (--vm < 1) { vm = 12; vy--; } }
            else if (h == 2) { if (++vm > 12) { vm = 1; vy++; } }
            else if (h == 3) { vy = sy = ty; vm = sm = tm; sd = td; }
            else if (h == 4) add();
            else if (h >= 100 && h < 142) { sy = vy; sm = vm; sd = h - 100 - weekday(vy, vm, 1) + 1; input_focus = true; }
            else if (h >= 200) { int i = h - 200; for (int k = i; k + 1 < nev; k++) evs[k] = evs[k + 1]; nev--; save(); }
            draw(); continue;
        }
        if (e.type != EV_KEY) continue;
        int k = e.key, code = KEY_CODE(k);
        if (input_focus) {
            if (k == '\n') add();
            else if (k == '\b') { if (ilen) input[--ilen] = 0; }
            else if (k == 27) input_focus = false;
            else if (k == 22) clip_paste_line(input, &ilen, sizeof input - 1);
            else if (key_is_char(k) && ilen < (int)sizeof input - 1) { input[ilen++] = (char)k; input[ilen] = 0; }
        } else if (code == KEY_PGUP || code == KEY_LEFT) { if (--vm < 1) { vm = 12; vy--; } }
        else if (code == KEY_PGDN || code == KEY_RIGHT) { if (++vm > 12) { vm = 1; vy++; } }
        draw();
    }
    return 0;
}
