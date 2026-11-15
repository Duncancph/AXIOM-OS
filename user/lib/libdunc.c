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
uint32_t hl_accent = HL_CREAM;
uint32_t accent_from_setting(const char *v) {             /* "rrggbb" -> colour (cream if unset) */
    uint32_t c = 0; int n = 0;
    for (; *v && n < 6; v++, n++) { int d = *v >= '0' && *v <= '9' ? *v - '0' : *v >= 'a' && *v <= 'f' ? *v - 'a' + 10 : *v >= 'A' && *v <= 'F' ? *v - 'A' + 10 : -1; if (d < 0) break; c = c << 4 | d; }
    return n == 6 ? c : HL_CREAM;
}
bool app_open_ex(struct app *a, int w, int h, const char *title, int flags) {
    { char v[16] = ""; sys_setting_get("ui.accent", v, sizeof v); hl_accent = accent_from_setting(v); }
    if (sys_call(SYS_WIN_CREATE, w, h, (long)title, (long)&a->wi, flags) < 0) return false;
    gfx_init(&a->c, a->wi.buf, a->wi.w, a->wi.h, a->wi.w);
    return true;
}
void app_flush(struct app *a) { sys_call(SYS_WIN_FLUSH, a->wi.id, 0, 0, a->wi.w, a->wi.h); }
void app_flush_rect(struct app *a, int x, int y, int w, int h) { sys_call(SYS_WIN_FLUSH, a->wi.id, x, y, w, h); }

void ui_card(struct canvas *c, int x, int y, int w, int h) {
    gfx_rrect(c, x, y, w, h, 10, UI_PANEL, 255);
    gfx_rrect_outline(c, x, y, w, h, 10, UI_BORDER, 255);
}

void ui_button(struct canvas *c, int x, int y, int w, int h, const char *label, int style, bool hover, bool disabled) {
    uint32_t bg, fg = UI_TEXT;
    if (style == BTN_PRIMARY) { bg = hover ? 0xFFFDF0 : UI_ACCENT; fg = HL_ON_ACCENT; }
    else if (style == BTN_DANGER) { bg = hover ? 0xF48A7A : UI_RED; fg = HL_ON_ACCENT; }
    else bg = hover ? 0x2E2E2B : UI_PANEL2;
    if (disabled) { bg = 0x1C1C1B; fg = 0x5A5850; }
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

int clip_paste_line(char *buf, int *len, int max) {
    static char b[2048];
    long n = clip_get(b, sizeof b);
    if (n <= 0) return 0;
    n = utf8_to_cp1252(b, n);
    int added = 0;
    for (long i = 0; i < n && b[i] != '\n' && b[i] != '\r' && *len < max - 1; i++) {
        int k = (unsigned char)b[i];
        if (k == '\t') k = ' ';
        if (key_is_char(k)) { buf[(*len)++] = (char)k; added++; }
    }
    buf[*len] = 0;
    return added;
}

static bool ext_is(const char *path, const char *list) {
    const char *dot = 0;
    for (const char *p = path; *p; p++) { if (*p == '.') dot = p; if (*p == '/') dot = 0; }
    if (!dot) return false;
    char e[12]; int k = 0;
    for (const char *p = dot + 1; *p && k < 11; p++) e[k++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    e[k] = 0;
    for (const char *l = list; *l;) {
        int n = 0; while (l[n] && l[n] != ' ') n++;
        if (n == k && !strncmp(l, e, n)) return true;
        l += n; while (*l == ' ') l++;
    }
    return false;
}

const char *app_for_file(const char *path) {
    if (ext_is(path, "png jpg jpeg gif bmp")) return "viewer";
    if (ext_is(path, "html htm xhtml")) return "web";
    if (ext_is(path, "mp3")) return "music";
    if (ext_is(path, "mp4") || ext_is(path, "m4v") || ext_is(path, "mov")) return "video";
    if (ext_is(path, "elf bin img iso zip gz tgz tar xz 7z rar exe dll so o a wav ogg flac mp4 mkv avi mov pdf doc docx xls xlsx ppt pptx")) return 0;
    return "editor";
}

const char *open_file(const char *path) {
    const char *app = app_for_file(path);
    if (!app) return "No app on AXIOM can open this kind of file yet.";
    struct dunc_app ap[24]; int n = sys_apps(ap, 24); bool have = false;
    for (int k = 0; k < n; k++) if (!strcmp(ap[k].id, app)) have = true;
    if (!have) {                                         /* a clan package: install it */
        char cmd[48], out[256]; ksnprintf(cmd, sizeof cmd, "clan install %s", app);
        sys_kcmd(cmd, out, sizeof out);
    }
    char arg[300];
    if (!strcmp(app, "web")) ksnprintf(arg, sizeof arg, "file://%s", path); else strncpy0(arg, path, sizeof arg);
    if (sys_spawn_arg(app, arg) < 0) return "The app couldn't start.";
    return 0;
}
