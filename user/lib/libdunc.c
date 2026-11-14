#include "dunc.h"

long sys_call(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8) : "memory");
    return r;
}

bool app_open(struct app *a, int w, int h, const char *title) {
    if (sys_call(SYS_WIN_CREATE, w, h, (long)title, (long)&a->wi, 0) < 0) return false;
    gfx_init(&a->c, a->wi.buf, a->wi.w, a->wi.h, a->wi.w);
    return true;
}
void app_flush(struct app *a) { sys_call(SYS_WIN_FLUSH, a->wi.id, 0, 0, a->wi.w, a->wi.h); }
void app_flush_rect(struct app *a, int x, int y, int w, int h) { sys_call(SYS_WIN_FLUSH, a->wi.id, x, y, w, h); }

static uint8_t heap[512 << 10];
static size_t heap_pos;
void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (heap_pos + n > sizeof heap) return 0;
    void *p = heap + heap_pos;
    heap_pos += n;
    return p;
}

void ui_card(struct canvas *c, int x, int y, int w, int h) {
    gfx_rrect(c, x, y, w, h, 10, UI_PANEL, 255);
    gfx_rrect_outline(c, x, y, w, h, 10, UI_BORDER, 255);
}

void ui_button(struct canvas *c, int x, int y, int w, int h, const char *label, int style, bool hover, bool disabled) {
    uint32_t bg, fg = UI_TEXT;
    if (style == BTN_PRIMARY) { bg = hover ? 0x7ADFF0 : UI_ACCENT; fg = HL_NAVY; }
    else if (style == BTN_DANGER) { bg = hover ? 0xFF8A8A : UI_RED; fg = HL_NAVY; }
    else bg = hover ? 0x24365A : UI_PANEL2;
    if (disabled) { bg = 0x17213A; fg = 0x4A5874; }
    gfx_rrect(c, x, y, w, h, 8, bg, 255);
    if (style == BTN_NORMAL && !disabled) gfx_rrect_outline(c, x, y, w, h, 8, UI_BORDER, 255);
    const struct font *f = &font_ui_bold;
    gfx_text_center(c, f, x, w, y + (h + f->ascent - f->descent) / 2 - 1, fg, label);
}
