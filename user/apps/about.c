/* About DuncOS — shown at start-up as a welcome screen. */
#include "../lib/dunc.h"

static const char *crown[] = {
    "            .     '     .",
    "        .  /\\    /\\    /\\  .",
    "          /  \\  /  \\  /  \\",
    "         /    \\/    \\/    \\",
    "        |__________________|" };
static const char *title[] = {
    "    ____                   ____  _____",
    "   / __ \\__  ______  _____/ __ \\/ ___/",
    "  / / / / / / / __ \\/ ___/ / / /\\__ \\",
    " / /_/ / /_/ / / / / /__/ /_/ /___/ /",
    "/_____/\\__,_/_/ /_/\\___/\\____//____/" };

#define WW 600
#define WH 520
static struct app a;

static void block(const char **lines, int n, int y, uint32_t col) {
    int widest = 0;
    for (int i = 0; i < n; i++) { int l = strlen(lines[i]); if (l > widest) widest = l; }
    int x = (WW - widest * 8) / 2;
    for (int i = 0; i < n; i++) gfx_mono_text(&a.c, x, y + i * 16, lines[i], col);
}

static void row(int x, int y, const char *k, const char *v) {
    gfx_text(&a.c, &font_ui, x, y, UI_MUTED, k);
    gfx_text(&a.c, &font_ui_bold, x + 96, y, UI_TEXT, v);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_vgrad(c, 0, 0, WW, WH, 0x0A1428, 0x06080F);
    block(crown, 5, 22, UI_GOLD);
    block(title, 5, 118, UI_ACCENT);
    gfx_text_center(c, &font_ui_bold, 0, WW, 222, 0xF0F0F0, "~ Thane Kernel v0.2 ~");
    gfx_text_center(c, &font_ui, 0, WW, 242, 0x6B7280, "Created by Bryan Duncan");

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
        gfx_rrect(c, fx, 436, w, 28, 14, 0x16324A, 255);
        gfx_text(c, &font_ui, fx + 12, 455, 0x9FE8F4, feat[i]);
        fx += w + 8;
    }
    gfx_text_center(c, &font_ui, 0, WW, 496, UI_MUTED, "Open apps from the dock below, or press the crown for the Start menu.");
    app_flush(&a);
}

int main(void) {
    if (!app_open(&a, WW, WH, "Welcome to DuncOS")) return 1;
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
