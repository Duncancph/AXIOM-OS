#include "dunc.h"

long sys_call(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8) : "memory");
    return r;
}

bool app_open(struct app *a, int w, int h, const char *title) { return app_open_ex(a, w, h, title, 0); }
bool app_resize(struct app *a, const struct dunc_event *e) {
    if (e->type != EV_RESIZE) return false;
    if (sys_call(SYS_WIN_RESIZE, a->wi.id, (long)&a->wi, 0, 0, 0) < 0) return false;
    gfx_init(&a->c, a->wi.buf, a->wi.w, a->wi.h, a->wi.w);
    return true;
}
bool app_open_ex(struct app *a, int w, int h, const char *title, int flags) {
    if (sys_call(SYS_WIN_CREATE, w, h, (long)title, (long)&a->wi, flags) < 0) return false;
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

const char *fs_strerror(int e) {
    switch (e) {
    case FS_ENOENT: return "No such file or folder";
    case FS_EEXIST: return "Already exists";
    case FS_ENOTDIR: return "Not a folder";
    case FS_EISDIR: return "Is a folder";
    case FS_ENOSPC: return "Not enough space";
    case FS_EIO: return "Drive read/write error";
    case FS_ENOTEMPTY: return "Folder is not empty";
    case FS_EROFS: return "Can't change this";
    case FS_EINVAL: return "Invalid name or path";
    }
    return "Error";
}

void path_resolve(const char *cwd, const char *in, char *out, int n) {
    char tmp[512];
    if (in[0] == '~') ksnprintf(tmp, sizeof tmp, "/home%s", in + 1);
    else if (in[0] == '/') strncpy0(tmp, in, sizeof tmp);
    else ksnprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    /* normalise . and .. */
    char comp[32][64]; int nc = 0;
    for (char *p = tmp; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        char c[64]; int k = 0;
        while (*p && *p != '/') { if (k < 63) c[k++] = *p; p++; }
        c[k] = 0;
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (nc) nc--; continue; }
        if (nc < 32) strncpy0(comp[nc++], c, 64);
    }
    int k = 0;
    if (!nc) { strncpy0(out, "/", n); return; }
    for (int i = 0; i < nc; i++) {
        if (k + 1 < n) out[k++] = '/';
        for (const char *s = comp[i]; *s && k + 1 < n; s++) out[k++] = *s;
    }
    out[k] = 0;
}
