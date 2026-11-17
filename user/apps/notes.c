/* Notes: quick text notes, saved as you type (in /home/Notes, one .txt file per note).
 * The list on the left is newest first; the first line of a note is its title. */
#include "../lib/dunc.h"

static int WW = 820, WH = 560;
#define SIDE 230
#define DIR "/home/Notes"
static struct app a;

struct note { char file[64]; char title[64]; uint64_t mtime; };
static struct note notes[200]; static int nnotes, cur = -1, list_off;
static char *text; static int len, cap, cursor, scroll_line;
static bool all_sel;                                       /* Ctrl+A: the whole note is selected (copy, cut, or replace it) */
static bool dirty; static uint64_t last_edit;
static int hover = -1;
static char status[96];

static void ensure(int n) { if (n + 1 > cap) { cap = (n + 1) * 2 + 256; text = realloc(text, cap); } }

static void title_of(const char *s, int n, char *out, int max) {
    int k = 0, i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\n')) i++;
    for (; i < n && s[i] != '\n' && k < max - 1; i++) out[k++] = s[i];
    out[k] = 0;
    if (!k) strncpy0(out, "New note", max);
}

static void load_list(void) {
    struct dunc_dirent e[200];
    fs_mkdir(DIR);
    int n = fs_list(DIR, e, 200);
    nnotes = 0;
    for (int i = 0; i < n && nnotes < 200; i++) {
        int l = strlen(e[i].name);
        if (e[i].is_dir || l < 5 || strcmp(e[i].name + l - 4, ".txt")) continue;
        struct note *t = &notes[nnotes++];
        strncpy0(t->file, e[i].name, sizeof t->file);
        char path[96], buf[80]; ksnprintf(path, sizeof path, DIR "/%s", e[i].name);
        long r = fs_read(path, buf, 0, sizeof buf - 1); if (r < 0) r = 0;
        title_of(buf, (int)r, t->title, sizeof t->title);
        t->mtime = (uint64_t)katoi(e[i].name);           /* files are named by creation time: newest first */
    }
    for (int i = 1; i < nnotes; i++) for (int j = i; j > 0 && notes[j].mtime > notes[j - 1].mtime; j--) { struct note t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t; }
}

static void save(void) {
    if (cur < 0 || !dirty) return;
    char path[96]; ksnprintf(path, sizeof path, DIR "/%s", notes[cur].file);
    long r = fs_write(path, text, len);
    dirty = false;
    title_of(text, len, notes[cur].title, sizeof notes[cur].title);
    strncpy0(status, r < 0 ? "Couldn't save!" : "Saved", sizeof status);
}

static void open_note(int i) {
    save();
    cur = i; len = 0; cursor = 0; scroll_line = 0;
    if (i < 0) return;
    char path[96]; ksnprintf(path, sizeof path, DIR "/%s", notes[i].file);
    struct dunc_stat st;
    long n = fs_stat(path, &st) < 0 ? 0 : (long)st.size;
    ensure(n);
    long r = n ? fs_read(path, text, 0, n) : 0;
    len = r > 0 ? (int)r : 0;
    text[len] = 0;
    len = utf8_to_cp1252(text, len); text[len] = 0;
    cursor = len;
    status[0] = 0;
}

static void new_note(void) {
    save();
    struct rtc_time t; sys_rtc(&t);
    char name[64];
    ksnprintf(name, sizeof name, "%04d%02d%02d%02d%02d%02d.txt", t.year, t.mon, t.day, t.hour, t.min, t.sec);
    char path[96]; ksnprintf(path, sizeof path, DIR "/%s", name);
    fs_write(path, "", 0);
    load_list();
    for (int i = 0; i < nnotes; i++) if (!strcmp(notes[i].file, name)) { open_note(i); return; }
}

static void delete_note(void) {
    if (cur < 0) return;
    char path[96]; ksnprintf(path, sizeof path, DIR "/%s", notes[cur].file);
    fs_delete(path);
    dirty = false; cur = -1; len = 0;
    load_list();
    open_note(nnotes ? 0 : -1);
    strncpy0(status, "Note deleted", sizeof status);
}

/* ---- the text, wrapped into lines ---- */
struct vline { int start, n; };
static struct vline *vl; static int nvl, vlcap;
static void wrap(int width) {
    nvl = 0;
    const struct font *f = &font_ui;
    int i = 0;
    for (;;) {
        if (nvl + 1 > vlcap) { vlcap = vlcap ? vlcap * 2 : 256; vl = realloc(vl, vlcap * sizeof *vl); }
        int s = i, w = 0, lastsp = -1;
        while (i < len && text[i] != '\n') {
            unsigned ch = (unsigned char)text[i]; if (ch < 32) ch = '?';
            int cw = f->g[ch - 32].adv;
            if (w + cw > width && i > s) { if (lastsp >= s) i = lastsp + 1; break; }
            if (text[i] == ' ') lastsp = i;
            w += cw; i++;
        }
        vl[nvl].start = s; vl[nvl].n = i - s; nvl++;
        if (i < len && text[i] == '\n') { i++; if (i == len) { if (nvl + 1 > vlcap) { vlcap *= 2; vl = realloc(vl, vlcap * sizeof *vl); } vl[nvl].start = i; vl[nvl].n = 0; nvl++; break; } continue; }
        if (i >= len) break;
    }
}
static int line_of(int pos) { for (int l = nvl - 1; l >= 0; l--) if (vl[l].start <= pos) return l; return 0; }
static int x_in_line(int l, int pos) { char t[512]; int n = pos - vl[l].start; if (n > 511) n = 511; memcpy(t, text + vl[l].start, n); t[n] = 0; return gfx_text_width(&font_ui, t); }
static int pos_at_x(int l, int x) {
    int best = vl[l].start, w = 0;
    for (int i = 0; i < vl[l].n; i++) {
        unsigned ch = (unsigned char)text[vl[l].start + i]; if (ch < 32) ch = '?';
        int cw = font_ui.g[ch - 32].adv;
        if (w + cw / 2 > x) return vl[l].start + i;
        w += cw; best = vl[l].start + i + 1;
    }
    return best;
}

#define LH 22
static int rows(void) { return (WH - 64 - 16) / LH; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_fill(c, 0, 0, SIDE, WH, 0x0C0C0B);
    ui_button(c, 16, 14, SIDE - 32, 34, "+ New note", BTN_PRIMARY, hover == 1, false);
    int y = 62;
    if (!nnotes) gfx_text(c, &font_ui, 20, y + 20, UI_MUTED, "No notes yet.");
    for (int i = list_off; i < nnotes && y + 44 < WH; i++, y += 46) {
        if (i == cur) gfx_rrect(c, 10, y, SIDE - 20, 42, 9, 0x26261F, 255);
        else if (hover == 100 + i) gfx_rrect(c, 10, y, SIDE - 20, 42, 9, UI_PANEL, 255);
        char t[40]; strncpy0(t, i == cur ? notes[cur].title : notes[i].title, sizeof t);
        if (i == cur && len) title_of(text, len, t, sizeof t);
        while (gfx_text_width(&font_ui_bold, t) > SIDE - 44 && strlen(t) > 3) { int l = strlen(t); t[l - 4] = '.'; t[l - 3] = '.'; t[l - 2] = '.'; t[l - 1] = 0; }
        gfx_text(c, &font_ui_bold, 22, y + 18, i == cur ? UI_TEXT : HL_TEXT_DIM, t);
        const char *f = notes[i].file; char d[24] = "";
        if (strlen(f) >= 14) { static const int at[] = { 6, 7, -1, 4, 5, -1, 0, 1, 2, 3, -2, 8, 9, -3, 10, 11 }; int k = 0;
            for (int j = 0; j < 16; j++) d[k++] = at[j] >= 0 ? f[at[j]] : at[j] == -1 ? '/' : at[j] == -2 ? ' ' : ':';
            d[k] = 0; }                                      /* DD/MM/YYYY HH:MM from the file name */
        gfx_text(c, &font_ui, 22, y + 36, UI_MUTED, d);
    }
    /* the note */
    int ex = SIDE + 24, ew = WW - SIDE - 48;
    if (cur < 0) {
        gfx_text_center(c, &font_title, SIDE, WW - SIDE, WH / 2 - 10, UI_TEXT, "Notes");
        gfx_text_center(c, &font_ui, SIDE, WW - SIDE, WH / 2 + 18, UI_MUTED, "Press \"+ New note\" to start writing. Notes save themselves.");
    } else {
        ui_button(c, WW - 120, 14, 100, 34, "Delete", BTN_NORMAL, hover == 2, false);
        gfx_text(c, &font_ui, ex, 36, UI_MUTED, status[0] ? status : "Notes save as you type");
        wrap(ew);
        int cl = line_of(cursor);
        if (cl < scroll_line) scroll_line = cl;
        if (cl >= scroll_line + rows()) scroll_line = cl - rows() + 1;
        gfx_clip(c, SIDE, 64, WW - SIDE, WH - 64);
        for (int l = scroll_line; l < nvl && l < scroll_line + rows(); l++) {
            char t[512]; int n = vl[l].n > 511 ? 511 : vl[l].n; memcpy(t, text + vl[l].start, n); t[n] = 0;
            int by = 64 + (l - scroll_line) * LH + 16;
            if (all_sel && n) gfx_rrect(c, ex - 2, by - 15, gfx_text_width(l == 0 ? &font_ui_bold : &font_ui, t) + 4, LH - 2, 3, UI_ACCENT, 70);
            gfx_text(c, l == 0 ? &font_ui_bold : &font_ui, ex, by, UI_TEXT, t);
        }
        int cy = 64 + (cl - scroll_line) * LH;
        gfx_fill(c, ex + x_in_line(cl, cursor), cy + 1, 2, 19, UI_ACCENT);
        gfx_clip(c, 0, 0, WW, WH);
    }
    app_flush(&a);
}

static int hit(int x, int y) {
    if (ui_in(x, y, 16, 14, SIDE - 32, 34)) return 1;
    if (cur >= 0 && ui_in(x, y, WW - 120, 14, 100, 34)) return 2;
    if (x < SIDE && y >= 62) { int i = list_off + (y - 62) / 46; if (i < nnotes) return 100 + i; }
    if (x >= SIDE && y >= 64) return 3;
    return -1;
}

static void insert(const char *s, int n) {
    ensure(len + n);
    memmove(text + cursor + n, text + cursor, len - cursor);
    memcpy(text + cursor, s, n); len += n; cursor += n; text[len] = 0;
    dirty = true; last_edit = sys_time(); status[0] = 0;
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "Notes", WIN_RESIZABLE)) return 1;
    ensure(0);
    load_list();
    if (nnotes) open_note(0);
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, dirty ? 800 : -1);
        if (r == 0) { if (dirty && sys_time() - last_edit >= 700) { save(); draw(); } continue; }
        if (e.type == EV_CLOSE) { save(); break; }
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h == 3) h = -1; if (h != hover) { hover = h; draw(); } continue; }
        if (e.type == EV_WHEEL) {
            if (e.x < SIDE) { list_off += e.key; if (list_off > nnotes - 1) list_off = nnotes - 1; if (list_off < 0) list_off = 0; }
            else if (cur >= 0) { int l = line_of(cursor) + e.key * 3; if (l < 0) l = 0; if (l >= nvl) l = nvl - 1; cursor = vl[l].start; }
            draw(); continue;
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 1) new_note();
            else if (h == 2) delete_note();
            else if (h >= 100) open_note(h - 100);
            else if (h == 3 && cur >= 0) {
                all_sel = false;
                int l = scroll_line + (e.y - 64) / LH; if (l >= nvl) l = nvl - 1; if (l < 0) l = 0;
                cursor = pos_at_x(l, e.x - (SIDE + 24));
            }
            draw(); continue;
        }
        if (e.type != EV_KEY) continue;
        int k = e.key, code = KEY_CODE(k);
        if (cur < 0) { if (key_is_char(k) || k == '\n') { new_note(); if (k == '\n') { draw(); continue; } } else continue; }
        if (k == 1) { all_sel = len > 0; draw(); continue; }                   /* Ctrl+A: select all */
        if (all_sel && (k == 3 || k == 24)) {                                    /* Ctrl+C / Ctrl+X: copy (the clipboard holds UTF-8) */
            char *u = malloc(len * 3 + 1);
            if (u) { long n = cp1252_to_utf8(text, len, u, len * 3 + 1); clip_set(u, n); free(u); }
            if (k == 3) { strncpy0(status, "Copied", sizeof status); draw(); continue; }
        }
        if (all_sel) {
            all_sel = false;
            bool replaces = k == 24 || k == '\b' || code == KEY_DEL || k == 22 || k == '\n' || k == '\t' || key_is_char(k);
            if (replaces) { len = 0; cursor = 0; text[0] = 0; dirty = true; last_edit = sys_time(); }
            if (k == 24 || k == '\b' || code == KEY_DEL) { draw(); continue; }
        }
        if (k == '\b') { if (cursor > 0) { memmove(text + cursor - 1, text + cursor, len - cursor); cursor--; len--; text[len] = 0; dirty = true; last_edit = sys_time(); } }
        else if (code == KEY_DEL) { if (cursor < len) { memmove(text + cursor, text + cursor + 1, len - cursor - 1); len--; text[len] = 0; dirty = true; last_edit = sys_time(); } }
        else if (code == KEY_LEFT) { if (cursor > 0) cursor--; }
        else if (code == KEY_RIGHT) { if (cursor < len) cursor++; }
        else if (code == KEY_UP || code == KEY_DOWN) {
            wrap(WW - SIDE - 48);
            int l = line_of(cursor), x = x_in_line(l, cursor);
            l += code == KEY_UP ? -1 : 1;
            if (l >= 0 && l < nvl) cursor = pos_at_x(l, x);
        }
        else if (code == KEY_HOME) { wrap(WW - SIDE - 48); cursor = vl[line_of(cursor)].start; }
        else if (code == KEY_END) { wrap(WW - SIDE - 48); int l = line_of(cursor); cursor = vl[l].start + vl[l].n; }
        else if (k == 22) {                                   /* paste */
            long n = clip_get(0, 0);
            if (n > 0) { char *b = malloc(n + 1); n = clip_get(b, n); n = utf8_to_cp1252(b, n); int k2 = 0; for (long i = 0; i < n; i++) if (b[i] != '\r') b[k2++] = b[i]; insert(b, k2); free(b); }
        }
        else if (k == 19) { save(); }                         /* Ctrl+S */
        else if (k == '\n') insert("\n", 1);
        else if (k == '\t') insert("    ", 4);
        else if (key_is_char(k)) { char ch = (char)k; insert(&ch, 1); }
        draw();
    }
    return 0;
}
