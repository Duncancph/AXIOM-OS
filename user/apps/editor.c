/* Editor 0.3 - a text editor for DuncOS (installed with: clan install editor). */
#include "../lib/dunc.h"

static int WW = 720, WH = 500;
#define GUTTER 52
#define TOP 0
#define STATUS_H 26
#define MAXTEXT (480 * 1024)

static struct app a;
static char text[MAXTEXT]; static int len, cur, scroll_line, want_col = -1;
static char path[256]; static bool modified;
static int dialog;   /* 1 save-as, 2 unsaved changes on close */
static char input[200]; static int ilen;
static char msg[96]; static uint64_t msg_until;
static int hover = -1;
static bool closing;

static int rows(void) { return (WH - TOP - STATUS_H - 8) / 16; }
static int cols(void) { return (WW - GUTTER - 16) / 8; }

static int line_start(int pos) { while (pos > 0 && text[pos - 1] != '\n') pos--; return pos; }
static int line_end(int pos) { while (pos < len && text[pos] != '\n') pos++; return pos; }
static int line_of(int pos) { int n = 0; for (int i = 0; i < pos; i++) if (text[i] == '\n') n++; return n; }
static int pos_of_line(int line) { int p = 0; while (line > 0 && p < len) { if (text[p++] == '\n') line--; } return p; }
static int nlines(void) { return line_of(len) + 1; }

static void set_title(void) {
    char t[300];
    const char *b = path[0] ? path : "Untitled";
    for (const char *s = path; *s; s++) if (*s == '/') b = s + 1;
    ksnprintf(t, sizeof t, "Editor - %s%s", b, modified ? " *" : "");
    app_title(&a, t);
}
static void flash(const char *m) { strncpy0(msg, m, sizeof msg); msg_until = sys_time() + 3000; }

static void load_file(void) {
    if (!path[0]) return;
    long n = fs_read(path, text, 0, MAXTEXT - 1);
    if (n == FS_ENOENT) { len = 0; flash("New file - it will be created when you save."); return; }
    if (n < 0) { flash(fs_strerror((int)n)); len = 0; return; }
    len = (int)n;
    for (int i = 0; i < len; i++) if (text[i] == '\r') { memmove(text + i, text + i + 1, len - i - 1); len--; i--; }
}

static bool save(void) {
    if (!path[0]) { dialog = 1; strncpy0(input, "~/Documents/untitled.txt", sizeof input); ilen = strlen(input); return false; }
    long r = fs_write(path, text, len);
    if (r < 0) { flash(fs_strerror((int)r)); return false; }
    modified = false;
    flash("Saved.");
    set_title();
    return true;
}

static void keep_visible(void) {
    int l = line_of(cur);
    if (l < scroll_line) scroll_line = l;
    if (l >= scroll_line + rows()) scroll_line = l - rows() + 1;
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, HL_TERM_BG);
    gfx_fill(c, 0, 0, GUTTER, WH - STATUS_H, gfx_mix(HL_TERM_BG, 0x000000, 60));
    int cl = line_of(cur), p = pos_of_line(scroll_line);
    for (int r = 0; r < rows(); r++) {
        int ln = scroll_line + r;
        if (p > len || (ln > 0 && p == len && (len == 0 || text[len - 1] != '\n') && ln > cl)) break;
        int y = TOP + 6 + r * 16;
        if (ln == cl) gfx_fill(c, GUTTER, y, WW - GUTTER, 16, gfx_mix(HL_TERM_BG, 0xFFFFFF, 10));
        char num[12]; ksnprintf(num, sizeof num, "%4d", ln + 1);
        gfx_mono_text(c, 6, y, num, ln == cl ? HL_GOLD : HL_TEXT_FAINT);
        int e = line_end(p), col = 0;
        for (int i = p; i < e && col < cols(); i++, col++) {
            char ch = text[i] == '\t' ? ' ' : text[i];
            gfx_mono(c, GUTTER + 8 + col * 8, y, ch, HL_TEXT);
        }
        if (ln == cl) {
            int cc = cur - p; if (cc > cols()) cc = cols();
            gfx_fill(c, GUTTER + 8 + cc * 8, y, 2, 16, HL_ACCENT);
        }
        if (e >= len) break;
        p = e + 1;
    }
    /* status bar */
    gfx_fill(c, 0, WH - STATUS_H, WW, STATUS_H, gfx_mix(HL_WIN_BG, 0x000000, 30));
    char st[200];
    if (sys_time() < msg_until) strncpy0(st, msg, sizeof st);
    else ksnprintf(st, sizeof st, "Line %d, Col %d  -  %s  -  Ctrl+S save", cl + 1, cur - line_start(cur) + 1, modified ? "modified" : "saved");
    gfx_mono_text(c, 10, WH - STATUS_H + 5, st, sys_time() < msg_until ? HL_GOLD : HL_TEXT_FAINT);
    if (dialog) {
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 90);
        int dw = 440, dh = 150, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 12, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 12, HL_ACCENT, 120);
        if (dialog == 1) {
            gfx_mono_text(c, dx + 20, dy + 18, "Save as", HL_TEXT);
            gfx_rrect(c, dx + 20, dy + 50, dw - 40, 34, 8, HL_TERM_BG, 255);
            gfx_rrect_outline(c, dx + 20, dy + 50, dw - 40, 34, 8, HL_ACCENT, 160);
            gfx_mono_text(c, dx + 32, dy + 59, input, HL_TEXT);
            gfx_fill(c, dx + 32 + ilen * 8, dy + 59, 8, 16, HL_ACCENT);
            ui_button(c, dx + dw - 116, dy + dh - 48, 96, 32, "Save", BTN_PRIMARY, hover == 0, false);
            ui_button(c, dx + dw - 222, dy + dh - 48, 96, 32, "Cancel", BTN_NORMAL, hover == 1, false);
        } else {
            gfx_mono_text(c, dx + 20, dy + 18, "Save your changes?", HL_TEXT);
            gfx_text(c, &font_ui, dx + 20, dy + 70, UI_MUTED, "If you don't save, your changes will be lost.");
            ui_button(c, dx + dw - 116, dy + dh - 48, 96, 32, "Save", BTN_PRIMARY, hover == 0, false);
            ui_button(c, dx + dw - 222, dy + dh - 48, 96, 32, "Don't save", BTN_DANGER, hover == 1, false);
            ui_button(c, dx + dw - 328, dy + dh - 48, 96, 32, "Cancel", BTN_NORMAL, hover == 2, false);
        }
    }
    app_flush(&a);
}

static void insert(const char *s, int n) {
    if (len + n >= MAXTEXT) { flash("File is too big for Editor."); return; }
    memmove(text + cur + n, text + cur, len - cur);
    memcpy(text + cur, s, n);
    len += n; cur += n; modified = true;
}

static void move_vert(int d) {
    int col = want_col >= 0 ? want_col : cur - line_start(cur);
    want_col = col;
    int l = line_of(cur) + d;
    if (l < 0) l = 0;
    if (l >= nlines()) l = nlines() - 1;
    int s = pos_of_line(l), e = line_end(s);
    cur = s + col < e ? s + col : e;
}

static int dialog_hit(int x, int y) {
    int dw = dialog == 1 ? 440 : 440, dh = 150, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
    for (int i = 0; i < (dialog == 1 ? 2 : 3); i++) if (ui_in(x, y, dx + dw - 116 - i * 106, dy + dh - 48, 96, 32)) return i;
    return -1;
}

static void dialog_choose(int b) {
    if (dialog == 1) {
        if (b == 0 && ilen) {
            char p[256]; path_resolve("/home", input, p, sizeof p);
            strncpy0(path, p, sizeof path);
            dialog = 0;
            if (save() && closing) sys_exit(0);
            return;
        }
        dialog = 0; closing = false;
    } else {
        if (b == 0) { dialog = 0; closing = true; if (save()) sys_exit(0); return; }
        if (b == 1) sys_exit(0);
        dialog = 0;
    }
}

int main(void) {
    char arg[256];
    if (sys_getarg(arg, sizeof arg) > 0) strncpy0(path, arg, sizeof path);
    if (!app_open_ex(&a, WW, WH, "Editor", WIN_RESIZABLE)) return 1;
    load_file();
    set_title();
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, msg_until ? 1000 : -1);
        if (r == 0) { if (sys_time() > msg_until) msg_until = 0; draw(); continue; }
        if (e.type == EV_CLOSE) { if (!modified) break; dialog = 2; draw(); continue; }
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; keep_visible(); draw(); } continue; }
        if (dialog) {
            if (e.type == EV_MOUSE_MOVE) { int h = dialog_hit(e.x, e.y); if (h != hover) { hover = h; draw(); } }
            if (e.type == EV_MOUSE_DOWN) { int h = dialog_hit(e.x, e.y); if (h >= 0) dialog_choose(h); draw(); }
            if (e.type == EV_KEY) {
                int k = e.key;
                if (k == 27) { dialog = 0; closing = false; }
                else if (k == '\n') dialog_choose(0);
                else if (dialog == 1 && k == '\b') { if (ilen) input[--ilen] = 0; }
                else if (dialog == 1 && k >= 32 && k < 127 && ilen < 50) { input[ilen++] = k; input[ilen] = 0; }
                draw();
            }
            continue;
        }
        if (e.type == EV_WHEEL) { scroll_line += e.key * 3; if (scroll_line > nlines() - 1) scroll_line = nlines() - 1; if (scroll_line < 0) scroll_line = 0; draw(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1 && e.y < WH - STATUS_H) {
            int l = scroll_line + (e.y - TOP - 6) / 16;
            if (l >= nlines()) l = nlines() - 1;
            int s = pos_of_line(l), en = line_end(s), col = (e.x - GUTTER - 8 + 4) / 8;
            if (col < 0) col = 0;
            cur = s + col < en ? s + col : en;
            want_col = -1;
            draw();
            continue;
        }
        if (e.type != EV_KEY) continue;
        int k = e.key;
        bool vert = false;
        if (k == 19) save();                                   /* Ctrl+S */
        else if (k == KEY_LEFT) { if (cur > 0) cur--; }
        else if (k == KEY_RIGHT) { if (cur < len) cur++; }
        else if (k == KEY_UP) { move_vert(-1); vert = true; }
        else if (k == KEY_DOWN) { move_vert(1); vert = true; }
        else if (k == KEY_PGUP) { move_vert(-rows()); vert = true; }
        else if (k == KEY_PGDN) { move_vert(rows()); vert = true; }
        else if (k == KEY_HOME) cur = line_start(cur);
        else if (k == KEY_END) cur = line_end(cur);
        else if (k == '\b') { if (cur > 0) { memmove(text + cur - 1, text + cur, len - cur); len--; cur--; modified = true; } }
        else if (k == KEY_DEL) { if (cur < len) { memmove(text + cur, text + cur + 1, len - cur - 1); len--; modified = true; } }
        else if (k == '\n') {   /* keep the indentation of the current line */
            char ind[64]; int n = 0, s = line_start(cur);
            ind[n++] = '\n';
            while (s + n - 1 < cur && n < 63 && (text[s + n - 1] == ' ' || text[s + n - 1] == '\t')) { ind[n] = text[s + n - 1]; n++; }
            insert(ind, n);
        }
        else if (k == '\t') insert("    ", 4);
        else if (k >= 32 && k < 127) { char ch = (char)k; insert(&ch, 1); }
        if (!vert) want_col = -1;
        keep_visible();
        set_title();
        draw();
    }
    return 0;
}
