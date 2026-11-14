/* Protection Test: deliberately misbehaves to show that DuncOS stops a bad
 * program without affecting the rest of the system. */
#include "../lib/dunc.h"

#define WW 560
#define WH 470
static struct app a;
static int hover = -1;

struct test { const char *name, *desc; int style; };
static const struct test tests[] = {
    { "Read kernel memory", "Peek at the Thane kernel's code at 0x100000.", BTN_DANGER },
    { "Use a null pointer", "Write to address 0, a classic crash.", BTN_DANGER },
    { "Privileged instruction", "Try to switch off interrupts with 'cli'.", BTN_DANGER },
    { "Freeze forever", "Spin in an endless loop. Everything else keeps running.", BTN_NORMAL },
};
#define NT 4
static int by(int i) { return 150 + i * 72; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_text(c, &font_title, 24, 42, UI_TEXT, "Protection Test");
    gfx_text(c, &font_ui, 24, 68, UI_MUTED, "Each DuncOS app runs in its own protected memory space. Pick a");
    gfx_text(c, &font_ui, 24, 88, UI_MUTED, "bad action below: the kernel stops just this app and tells you why.");
    char b[96];
    ksnprintf(b, sizeof b, "This app lives at 0x%lx; the kernel is off-limits.", (uint64_t)USER_BASE);
    gfx_text(c, &font_ui, 24, 118, UI_ACCENT, b);
    for (int i = 0; i < NT; i++) {
        ui_card(c, 20, by(i), WW - 40, 62);
        gfx_text(c, &font_ui_bold, 38, by(i) + 26, UI_TEXT, tests[i].name);
        gfx_text(c, &font_ui, 38, by(i) + 46, UI_MUTED, tests[i].desc);
        ui_button(c, WW - 40 - 96, by(i) + 14, 84, 34, "Run", tests[i].style, hover == i, false);
    }
    app_flush(&a);
}

static void run(int i) {
    sys_log(tests[i].name);
    if (i == 0) { volatile uint64_t *k = (volatile uint64_t *)0x100000; (void)*k; }
    if (i == 1) { volatile int *p = 0; *p = 42; }
    if (i == 2) __asm__ volatile("cli");
    if (i == 3) {
        app_title(&a, "Protection Test - frozen");
        for (;;) __asm__ volatile("" ::: "memory");
    }
}

int main(void) {
    if (!app_open(&a, WW, WH, "Protection Test")) return 1;
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = -1;
            for (int i = 0; i < NT; i++) if (e.type == EV_MOUSE_MOVE && ui_in(e.x, e.y, WW - 136, by(i) + 14, 84, 34)) h = i;
            if (h != hover) { hover = h; draw(); }
        }
        if (e.type == EV_MOUSE_UP && e.button == 1)
            for (int i = 0; i < NT; i++) if (ui_in(e.x, e.y, WW - 136, by(i) + 14, 84, 34)) run(i);
    }
    return 0;
}
