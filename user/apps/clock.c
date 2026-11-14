/* Clock - analog and digital clock for AXIOM (installed with: clan install clock). */
#include "../lib/dunc.h"

#define WW 340
#define WH 420
static struct app a;

/* sin(i * 6 degrees) * 1000 for i = 0..15 (quarter circle); other quadrants by symmetry */
static const int SIN[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };
static int isin(int i) { i %= 60; if (i < 0) i += 60; if (i <= 15) return SIN[i]; if (i <= 30) return SIN[30 - i]; if (i <= 45) return -SIN[i - 30]; return -SIN[60 - i]; }
static int icos(int i) { return isin(i + 15); }

static void hand(int cx, int cy, int pos60, int len, uint32_t col, int t) {
    gfx_line(&a.c, cx, cy, cx + isin(pos60) * len / 1000, cy - icos(pos60) * len / 1000, col, t);
}

static void draw(void) {
    struct canvas *c = &a.c;
    struct rtc_time t; sys_rtc(&t);
    gfx_vgrad(c, 0, 0, WW, WH, HL_NAVY, 0x050505);
    int cx = WW / 2, cy = 150, r = 112;
    gfx_circle(c, cx, cy, r + 6, HL_WIN_BORDER, 255);
    gfx_circle(c, cx, cy, r, HL_TERM_BG, 255);
    for (int i = 0; i < 60; i++) {
        int in = i % 5 ? r - 8 : r - 16;
        gfx_line(c, cx + isin(i) * in / 1000, cy - icos(i) * in / 1000, cx + isin(i) * (r - 4) / 1000, cy - icos(i) * (r - 4) / 1000,
                 i % 5 ? HL_TEXT_FAINT : HL_TEXT, i % 15 ? 1 : 3);
    }
    hand(cx, cy, (t.hour % 12) * 5 + t.min / 12, r * 50 / 100, HL_TEXT, 4);
    hand(cx, cy, t.min, r * 75 / 100, HL_TEXT, 3);
    hand(cx, cy, t.sec, r * 85 / 100, HL_GOLD, 1);
    gfx_circle(c, cx, cy, 5, HL_GOLD, 255);
    char tm[16], dt[48];
    ksnprintf(tm, sizeof tm, "%02d:%02d:%02d", t.hour, t.min, t.sec);
    static const char *mon[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
    ksnprintf(dt, sizeof dt, "%d %s %d", t.day, (t.mon >= 1 && t.mon <= 12) ? mon[t.mon - 1] : "?", t.year);
    gfx_text_center(c, &font_big, 0, WW, 330, HL_TEXT, tm);
    gfx_text_center(c, &font_ui, 0, WW, 362, HL_TEXT_DIM, dt);
    gfx_text_center(c, &font_ui, 0, WW, 396, HL_TEXT_FAINT, "Time from the computer's clock");
    app_flush(&a);
}

int main(void) {
    if (!app_open(&a, WW, WH, "Clock")) return 1;
    for (;;) {
        draw();
        struct dunc_event e;
        if (sys_event(&e, 1000) > 0 && e.type == EV_CLOSE) break;
    }
    return 0;
}
