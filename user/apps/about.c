/* About AXIOM — shown at start-up as a welcome screen. */
#include "../lib/dunc.h"
#include "../lib/logo_draw.h"


#define WW 600
#define WH 520
static struct app a;

static void row(int x, int y, const char *k, const char *v) {
    gfx_text(&a.c, &font_ui, x, y, UI_MUTED, k);
    gfx_text(&a.c, &font_ui_bold, x + 96, y, UI_TEXT, v);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_vgrad(c, 0, 0, WW, WH, 0x0A0A0A, 0x050505);
    gfx_vgrad(c, 0, 0, WW, 250, 0x000000, 0x0A0A0A);
    draw_logo(c, (WW - logo_splash.w) / 2, 96, &logo_splash, HL_CREAM);
    gfx_text_center(c, &font_ui, 0, WW, 206, UI_MUTED, "Axiom Core 0.2   -   Created by Bryan Duncan");

    struct sysinfo s; sys_sysinfo(&s);
    ui_card(c, 24, 266, WW - 48, 150);
    char b[96];
    row(44, 296, "Processor", s.cpu[0] ? s.cpu : "Unknown");
    ksnprintf(b, sizeof b, "%lu MiB  (%lu MiB in use)", s.mem_total >> 20, s.mem_used >> 20);
    row(44, 322, "Memory", b);
    ksnprintf(b, sizeof b, "%d x %d", s.fb_w, s.fb_h);
    row(44, 348, "Display", b);
    uint64_t up = s.uptime_ms / 1000;
    ksnprintf(b, sizeof b, "%lu min %lu s   |   %d processes   |   %s boot", up / 60, up % 60, s.nprocs, s.uefi ? "UEFI" : "BIOS");
    row(44, 374, "Running", b);
    ksnprintf(b, sizeof b, "%s", s.kernel);
    row(44, 400, "Kernel", b);

    static const char *feat[] = { "Preemptive multitasking", "Memory protection", "USB 3", "Bluetooth" };
    int fx = 24;
    int total = 0;
    for (int i = 0; i < 4; i++) total += gfx_text_width(&font_ui, feat[i]) + 24 + (i ? 8 : 0);
    fx = (WW - total) / 2;
    for (int i = 0; i < 4; i++) {
        int w = gfx_text_width(&font_ui, feat[i]) + 24;
        gfx_rrect(c, fx, 436, w, 28, 14, 0x22221F, 255);
        gfx_text(c, &font_ui, fx + 12, 455, 0xF4F1DE, feat[i]);
        fx += w + 8;
    }
    gfx_text_center(c, &font_ui, 0, WW, 496, UI_MUTED, "Open apps from the dock below, or click AXIOM in the top left.");
    app_flush(&a);
}

int main(void) {
    if (!app_open(&a, WW, WH, "Welcome to AXIOM")) return 1;
    draw();
    bool focused = true;
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, focused ? 1000 : -1);   /* only tick the uptime while in front */
        if (r > 0 && e.type == EV_CLOSE) break;
        if (r > 0 && e.type == EV_BLUR) focused = false;
        if (r > 0 && e.type == EV_FOCUS) focused = true;
        if (r == 0 || (r > 0 && e.type == EV_FOCUS)) draw();
    }
    return 0;
}
