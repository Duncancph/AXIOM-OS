/* Files: browse the system disk and USB drives (Highland "Files" mockup by Bryan Duncan). */
#include "../lib/dunc.h"

static int WW = 760, WH = 480;
#define SIDE 170
#define TOOL_H 44
#define STATUS_H 28
#define CELL_W 96
#define CELL_H 92
#define MAXE 512

static struct app a;
static char cwd[256] = "/home";
static char back_stack[16][256]; static int nback;
static struct dunc_dirent ents[MAXE]; static int nents, sel = -1, scroll;
static struct dunc_volume vols[8]; static int nvols;
static char clip[256];             /* path copied with Copy (or Cut: clip_cut) */
static bool clip_cut;
/* dragging an item: onto a folder or a place in the sidebar (moves on the same drive, copies to another) */
static int drag_item = -1; static bool dragging, mouse_held; static int drag_x0, drag_y0, mouse_x, mouse_y;
/* search: results under the folder it started in */
#define MAXR 300
static bool searching; static char sq[64]; static char res[MAXR][200]; static bool res_dir[MAXR]; static int nres, rsel = -1, rscroll; static char search_root[256];
static int hover_btn = -1, hover_place = -1;
static uint64_t last_click; static int last_click_item = -1;
static char status_msg[128]; static uint64_t status_until;

/* dialog: 0 none, 1 new folder, 2 rename, 3 confirm delete, 4 install editor, 5 empty trash, 6 search */
static int dialog; static char input[64]; static int ilen;

static const char *ICON_FOLDER[16] = {   /* from desktop_highland.c */
    "................", "................", ".#####..........", "#.....#.........",
    "#......#######..", "#.............#.", "#.............#.", "#.............#.",
    "#.............#.", "#.............#.", "#.............#.", "#.............#.",
    "#.............#.", ".#############..", "................", "................",
};
static const char *ICON_FILE[16] = {
    "................", "..#########.....", "..#.......##....", "..#.......#.#...",
    "..#.......####..", "..#..........#..", "..#.######...#..", "..#..........#..",
    "..#.#######..#..", "..#..........#..", "..#.#####....#..", "..#..........#..",
    "..#..........#..", "..############..", "................", "................",
};
static const char *ICON_DRIVE[16] = {
    "................", "................", "................", "................",
    "..############..", ".#............#.", ".#............#.", ".#............#.",
    ".##############.", ".#..........#.#.", ".#..........#.#.", ".##############.",
    "................", "................", "................", "................",
};

static void icon(const char **rows, int x, int y, int s, uint32_t col) {
    for (int r = 0; r < 16; r++)
        for (int k = 0; rows[r][k]; k++)
            if (rows[r][k] == '#') gfx_fill(&a.c, x + k * s, y + r * s, s, s, col);
}

static void flash(const char *m) { strncpy0(status_msg, m, sizeof status_msg); status_until = sys_time() + 4000; }

/* ---------------- places ---------------- */
struct place { char label[40], path[64]; int drive; };
static struct place places[12]; static int nplaces;

static void load_places(void) {
    static const char *p[][2] = { {"Home", "/home"}, {"Documents", "/home/Documents"}, {"Projects", "/home/Projects"},
                                  {"Downloads", "/home/Downloads"}, {"Trash", "/home/Trash"} };
    nplaces = 0;
    for (int i = 0; i < 5; i++) { strncpy0(places[nplaces].label, p[i][0], 40); strncpy0(places[nplaces].path, p[i][1], 64); places[nplaces++].drive = 0; }
    nvols = fs_volumes(vols, 8);
    for (int i = 1; i < nvols && nplaces < 12; i++) {
        strncpy0(places[nplaces].label, vols[i].label[0] ? vols[i].label : "USB drive", 40);
        strncpy0(places[nplaces].path, vols[i].path, 64);
        places[nplaces++].drive = 1;
    }
}

static bool under(const char *path, const char *root) {
    int l = strlen(root);
    return !strncmp(path, root, l) && (path[l] == 0 || path[l] == '/');
}
static int current_place(void) {
    int best = -1, bl = 0;
    for (int i = 0; i < nplaces; i++) { int l = strlen(places[i].path); if (under(cwd, places[i].path) && l > bl) { best = i; bl = l; } }
    return best;
}
static bool in_trash(void) { return under(cwd, "/home/Trash"); }

static void title_update(void) {
    char t[300], s[256];
    if (under(cwd, "/home")) ksnprintf(s, sizeof s, "~%s", cwd + 5); else strncpy0(s, cwd, sizeof s);
    ksnprintf(t, sizeof t, "Files - %s", s);
    app_title(&a, t);
}

static void load(void) {
    nents = fs_list(cwd, ents, MAXE);
    if (nents < 0) { flash(fs_strerror(nents)); nents = 0; }
    if (sel >= nents) sel = -1;
    load_places();
    title_update();
}

static void go(const char *path, bool remember) {
    struct dunc_stat st;
    if (fs_stat(path, &st) < 0 || !st.is_dir) { flash("That folder isn't available."); return; }
    if (remember && strcmp(path, cwd)) {
        if (nback == 16) { memmove(back_stack[0], back_stack[1], sizeof back_stack[0] * 15); nback--; }
        strncpy0(back_stack[nback++], cwd, 256);
    }
    strncpy0(cwd, path, sizeof cwd);
    sel = -1; scroll = 0;
    load();
}

static void child_path(const char *name, char *out, int n) {
    if (!strcmp(cwd, "/")) ksnprintf(out, n, "/%s", name); else ksnprintf(out, n, "%s/%s", cwd, name);
}
static const char *base_name(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

/* ---------------- layout ---------------- */
static const char *tools[] = { "<", "Search", "New folder", "Rename", "Cut", "Copy", "Paste", "Delete" };
#define NTOOLS 8
enum { T_BACK, T_SEARCH, T_NEW, T_RENAME, T_CUT, T_COPY, T_PASTE, T_DELETE };
static void tool_rect(int i, int *x, int *y, int *w, int *h) {
    int px = WW - 12;
    for (int k = NTOOLS - 1; k >= 1; k--) {
        int tw = (int)strlen(tools[k]) * 8 + 20;
        px -= tw;
        if (k == i) { *x = px; *y = 8; *w = tw; *h = 28; return; }
        px -= 6;
    }
    *x = SIDE + 12; *y = 8; *w = 28; *h = 28;   /* back */
}
static int cols(void) { int c = (WW - SIDE - 24) / CELL_W; return c < 1 ? 1 : c; }
static void cell_rect(int i, int *x, int *y) {
    *x = SIDE + 14 + (i % cols()) * CELL_W;
    *y = TOOL_H + 12 + (i / cols()) * CELL_H - scroll;
}
static int place_y(int i) { return 16 + i * 30 + (places[i].drive ? 34 : 0); }

/* ---------------- drawing ---------------- */
static void draw_dialog(void) {
    struct canvas *c = &a.c;
    gfx_blend(c, 0, 0, WW, WH, 0x000000, 90);
    int dw = 380, dh = 150, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
    gfx_rrect(c, dx, dy, dw, dh, 12, UI_PANEL, 255);
    gfx_rrect_outline(c, dx, dy, dw, dh, 12, HL_ACCENT, 120);
    const char *title = dialog == 1 ? "New folder" : dialog == 2 ? "Rename" : dialog == 3 ? "Delete permanently?" :
                        dialog == 4 ? "Editor isn't installed" : dialog == 6 ? "Search this folder and the ones in it" : "Empty the Trash?";
    gfx_mono_text(c, dx + 20, dy + 18, title, HL_TEXT);
    if (dialog == 1 || dialog == 2 || dialog == 6) {
        gfx_rrect(c, dx + 20, dy + 50, dw - 40, 34, 8, HL_TERM_BG, 255);
        gfx_rrect_outline(c, dx + 20, dy + 50, dw - 40, 34, 8, HL_ACCENT, 160);
        gfx_mono_text(c, dx + 32, dy + 59, input, HL_TEXT);
        gfx_fill(c, dx + 32 + ilen * 8, dy + 59, 8, 16, HL_ACCENT);
    } else {
        char m[96];
        if (dialog == 3 && sel >= 0) ksnprintf(m, sizeof m, "'%s' will be gone for good.", ents[sel].name);
        else if (dialog == 4) strncpy0(m, "Install it now with clan?", sizeof m);
        else strncpy0(m, "Everything in the Trash will be deleted.", sizeof m);
        gfx_text(c, &font_ui, dx + 20, dy + 70, UI_MUTED, m);
    }
    const char *ok = dialog == 3 || dialog == 5 ? "Delete" : dialog == 4 ? "Install" : dialog == 6 ? "Search" : "OK";
    ui_button(c, dx + dw - 20 - 96, dy + dh - 48, 96, 32, ok, dialog == 3 || dialog == 5 ? BTN_DANGER : BTN_PRIMARY, hover_btn == 100, false);
    ui_button(c, dx + dw - 20 - 96 - 10 - 96, dy + dh - 48, 96, 32, "Cancel", BTN_NORMAL, hover_btn == 101, false);
}

static void human(uint64_t b, char *out, int n) {
    if (b < 1024) ksnprintf(out, n, "%lu B", b);
    else if (b < 1024 * 1024) ksnprintf(out, n, "%lu KB", b / 1024);
    else if (b < (1ULL << 30)) ksnprintf(out, n, "%lu MB", b >> 20);
    else ksnprintf(out, n, "%lu.%lu GB", b >> 30, ((b >> 20) & 1023) * 10 / 1024);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    /* sidebar (mockup: black at alpha 40 over the window body) */
    gfx_fill(c, 0, 0, SIDE, WH, gfx_mix(UI_BG, 0x000000, 40));
    int cp = current_place();
    for (int i = 0; i < nplaces; i++) {
        int py = place_y(i);
        if (places[i].drive && (i == 0 || !places[i - 1].drive)) gfx_mono_text(c, 22, py - 30, "Drives", HL_TEXT_FAINT);
        if (i == cp) gfx_rrect(c, 10, py - 7, 150, 30, 8, HL_ACCENT, 36);
        else if (i == hover_place) gfx_rrect(c, 10, py - 7, 150, 30, 8, 0xFFFFFF, 14);
        char l[18]; strncpy0(l, places[i].label, 17);
        gfx_mono_text(c, 22, py, l, i == cp ? HL_TEXT : HL_TEXT_DIM);
    }
    /* toolbar */
    for (int i = 0; i < NTOOLS; i++) {
        int x, y, w, h; tool_rect(i, &x, &y, &w, &h);
        bool disabled = (i == T_BACK && !nback && !searching) || ((i == T_RENAME || i == T_CUT || i == T_COPY || i == T_DELETE) && (sel < 0 || searching)) ||
                        (i == T_PASTE && (!clip[0] || searching)) || (in_trash() && (i == T_NEW || i == T_PASTE)) || (searching && i == T_NEW);
        uint32_t bg = disabled ? UI_BG : hover_btn == i ? 0x2E2E2B : UI_PANEL2;
        gfx_rrect(c, x, y, w, h, 8, bg, 255);
        if (!disabled) gfx_rrect_outline(c, x, y, w, h, 8, UI_BORDER, 255);
        gfx_mono_text(c, x + (w - (int)strlen(tools[i]) * 8) / 2, y + 6, tools[i], disabled ? 0x5A5850 : (i == 5 ? HL_RED : HL_TEXT));
    }
    if (in_trash() && nents) {
        int x, y, w, h; tool_rect(T_NEW, &x, &y, &w, &h);
        ui_button(c, x, y, w, h, "Empty", BTN_DANGER, hover_btn == 50, false);
    }
    gfx_fill(c, SIDE, TOOL_H, WW - SIDE, 1, gfx_mix(UI_BG, 0xFFFFFF, 16));
    /* grid */
    struct canvas *gc = c;
    gfx_clip(gc, SIDE, TOOL_H + 1, WW - SIDE, WH - TOOL_H - STATUS_H - 1);
    if (searching) {                                           /* search results: a list of paths */
        char h[120]; ksnprintf(h, sizeof h, "%d found for \"%s\"%s", nres, sq, nres >= MAXR ? " (the first 300)" : "");
        gfx_mono_text(gc, SIDE + 18, TOOL_H + 12, h, HL_TEXT_DIM);
        for (int i = 0; i < nres; i++) {
            int y = TOOL_H + 38 + i * 26 - rscroll;
            if (y + 26 < TOOL_H || y > WH) continue;
            if (i == rsel) gfx_rrect(gc, SIDE + 10, y - 4, WW - SIDE - 20, 24, 6, HL_ACCENT, 40);
            icon(res_dir[i] ? ICON_FOLDER : ICON_FILE, SIDE + 16, y - 2, 1, res_dir[i] ? HL_ACCENT : HL_TEXT_DIM);
            const char *shown = res[i]; int rl = strlen(search_root);
            if (!strncmp(shown, search_root, rl) && shown[rl] == '/') shown += rl + 1;
            gfx_mono_text(gc, SIDE + 40, y, shown, HL_TEXT);
        }
        if (!nres) gfx_mono_text(gc, SIDE + 18, TOOL_H + 40, "Nothing found.", HL_TEXT_FAINT);
    }
    if (!searching && !nents) gfx_mono_text(gc, SIDE + 24, TOOL_H + 24, in_trash() ? "The Trash is empty." : "This folder is empty.", HL_TEXT_FAINT);
    for (int i = 0; i < nents && !searching; i++) {
        int x, y; cell_rect(i, &x, &y);
        if (y + CELL_H < TOOL_H || y > WH) continue;
        if (i == sel) gfx_rrect(gc, x + 2, y - 4, CELL_W - 4, CELL_H, 10, HL_ACCENT, 40);
        bool drive = !strcmp(cwd, "/") && !strncmp(ents[i].name, "usb", 3);
        if (ents[i].is_dir) icon(drive ? ICON_DRIVE : ICON_FOLDER, x + 24, y + 2, 3, HL_ACCENT);
        else icon(ICON_FILE, x + 24, y + 2, 3, HL_TEXT_DIM);
        char l[16]; int n = strlen(ents[i].name);
        if (n > 11) { memcpy(l, ents[i].name, 9); l[9] = '.'; l[10] = '.'; l[11] = 0; } else strncpy0(l, ents[i].name, sizeof l);
        gfx_mono_text(gc, x + 48 - (int)strlen(l) * 4, y + 56, l, HL_TEXT);
    }
    gfx_noclip(gc);
    if (dragging && drag_item >= 0 && drag_item < nents) {     /* what's being dragged follows the pointer */
        const char *nm = ents[drag_item].name; int tw = (int)strlen(nm) * 8 + 16; if (tw > 240) tw = 240;
        gfx_rrect(c, mouse_x + 12, mouse_y + 10, tw, 24, 6, HL_ACCENT, 200);
        char l[30]; strncpy0(l, nm, sizeof l);
        gfx_mono_text(c, mouse_x + 20, mouse_y + 14, l, 0x141414);
    }
    /* status bar */
    gfx_fill(c, SIDE, WH - STATUS_H, WW - SIDE, STATUS_H, gfx_mix(UI_BG, 0x000000, 30));
    char st[160];
    if (sys_time() < status_until) strncpy0(st, status_msg, sizeof st);
    else if (sel >= 0) {
        char sz[24]; human(ents[sel].size, sz, sizeof sz);
        ksnprintf(st, sizeof st, "%s  -  %s", ents[sel].name, ents[sel].is_dir ? "folder" : sz);
    } else {
        int vi = 0;
        for (int i = 1; i < nvols; i++) if (under(cwd, vols[i].path)) vi = i;
        char fr[24]; human(vols[vi].free, fr, sizeof fr);
        ksnprintf(st, sizeof st, "%d item%s  -  %s free on %s%s", nents, nents == 1 ? "" : "s", fr, vols[vi].label,
                  clip[0] ? (clip_cut ? "  -  1 item cut (Paste moves it here)" : "  -  1 item copied") : "");
    }
    gfx_mono_text(c, SIDE + 14, WH - STATUS_H + 6, st, sys_time() < status_until ? HL_GOLD : HL_TEXT_FAINT);
    if (dialog) draw_dialog();
    app_flush(&a);
}

/* ---------------- actions ---------------- */
static void open_item(int i) {
    char p[256]; child_path(ents[i].name, p, sizeof p);
    if (ents[i].is_dir) { go(p, true); return; }
    const char *why = open_file(p);
    if (why) flash(why);
}

static void do_delete(bool permanent) {
    if (sel < 0) return;
    char p[256]; child_path(ents[sel].name, p, sizeof p);
    int r;
    if (!permanent && under(cwd, "/home") && !in_trash()) {
        char t[256]; ksnprintf(t, sizeof t, "/home/Trash/%s", ents[sel].name);
        struct dunc_stat st;
        for (int k = 2; fs_stat(t, &st) == 0 && k < 100; k++) ksnprintf(t, sizeof t, "/home/Trash/%s (%d)", ents[sel].name, k);
        r = fs_rename(p, t);
        if (r == 0) flash("Moved to the Trash.");
    } else {
        r = fs_delete(p);
        if (r == 0) flash("Deleted.");
    }
    if (r < 0) flash(fs_strerror(r));
    sel = -1;
    load();
}

/* the volume a path is on (/usbN, or the system disk) */
static void vol_of(const char *p, char *out, int n) {
    if (!strncmp(p, "/usb", 4)) { int k = 0; while (p[k] && (k == 0 || p[k] != '/') && k < n - 1) { out[k] = p[k]; k++; } out[k] = 0; }
    else strncpy0(out, "/", n);
}
/* move or copy src into the folder dir (a free name if it's taken) */
static int put_into(const char *src, const char *dir, bool move) {
    char dst[256]; ksnprintf(dst, sizeof dst, "%s/%s", strcmp(dir, "/") ? dir : "", base_name(src));
    if (under(dir, src)) return -100;                           /* a folder into itself */
    struct dunc_stat st;
    for (int k = 2; fs_stat(dst, &st) == 0 && k < 100; k++) ksnprintf(dst, sizeof dst, "%s/%s (%d)", strcmp(dir, "/") ? dir : "", base_name(src), k);
    flash(move ? "Moving..." : "Copying..."); draw();
    return move ? fs_rename(src, dst) : fs_copy(src, dst);
}
static void drop_on(const char *dir) {                       /* the end of a drag */
    if (drag_item < 0 || drag_item >= nents) return;
    char src[256]; child_path(ents[drag_item].name, src, sizeof src);
    if (!strcmp(dir, cwd)) return;
    char v1[16], v2[16]; vol_of(src, v1, sizeof v1); vol_of(dir, v2, sizeof v2);
    bool trash = !strcmp(dir, "/home/Trash"), move = trash || !strcmp(v1, v2);
    int r = put_into(src, dir, move);
    char m[160];
    if (r == -100) strncpy0(m, "A folder can't go inside itself.", sizeof m);
    else if (r < 0) strncpy0(m, fs_strerror(r), sizeof m);
    else ksnprintf(m, sizeof m, "%s %s %s.", trash ? "Moved" : move ? "Moved" : "Copied", trash ? "to the Trash" : "to", trash ? "" : base_name(dir));
    flash(m); sel = -1; load();
}
/* search: names containing the words (any case), from the folder down */
static bool has_ci(const char *h, const char *n) {
    int ln = strlen(n);
    for (; *h; h++) { int k = 0; while (k < ln && h[k] && ((h[k] | 32) == (n[k] | 32) || h[k] == n[k])) k++; if (k == ln) return true; }
    return false;
}
static void search_in(const char *dir, int depth) {
    if (depth > 8 || nres >= MAXR) return;
    static struct dunc_dirent buf[9][256];
    int n = fs_list(dir, buf[depth], 256);
    for (int i = 0; i < n && nres < MAXR; i++) {
        char p[256]; ksnprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", buf[depth][i].name);
        if (has_ci(buf[depth][i].name, sq)) { strncpy0(res[nres], p, sizeof res[0]); res_dir[nres] = buf[depth][i].is_dir; nres++; }
        if (buf[depth][i].is_dir && strcmp(p, "/system") && strcmp(p, "/home/Trash")) search_in(p, depth + 1);
    }
}
static void do_search(void) {
    strncpy0(sq, input, sizeof sq);
    strncpy0(search_root, cwd, sizeof search_root);
    nres = 0; rsel = -1; rscroll = 0;
    flash("Searching..."); draw();
    search_in(cwd, 0);
    searching = true; status_until = 0;
}
static void open_result(int i) {
    if (i < 0 || i >= nres) return;
    if (res_dir[i]) { searching = false; go(res[i], true); return; }
    const char *why = open_file(res[i]);
    if (why) flash(why);
}

static void do_paste(void) {
    if (!clip[0]) return;
    if (clip_cut) {                                            /* Cut + Paste: move it here */
        int r = put_into(clip, cwd, true);
        flash(r == -100 ? "A folder can't go inside itself." : r < 0 ? fs_strerror(r) : "Moved here.");
        if (r == 0) { clip[0] = 0; clip_cut = false; }
        load(); return;
    }
    char dst[256]; child_path(base_name(clip), dst, sizeof dst);
    struct dunc_stat st;
    for (int k = 2; fs_stat(dst, &st) == 0 && k < 100; k++) {
        char nm[96]; ksnprintf(nm, sizeof nm, "%s (copy %d)", base_name(clip), k);
        child_path(nm, dst, sizeof dst);
    }
    flash("Copying...");
    draw();
    int r = fs_copy(clip, dst);
    flash(r < 0 ? fs_strerror(r) : "Pasted.");
    load();
}

static void dialog_ok(void) {
    if (dialog == 1 && ilen) {
        char p[256]; child_path(input, p, sizeof p);
        int r = fs_mkdir(p); if (r < 0) flash(fs_strerror(r));
    } else if (dialog == 2 && ilen && sel >= 0) {
        char from[256], to[256]; child_path(ents[sel].name, from, sizeof from); child_path(input, to, sizeof to);
        int r = fs_rename(from, to); if (r < 0) flash(fs_strerror(r));
    } else if (dialog == 3) { dialog = 0; do_delete(true); return; }
    else if (dialog == 4) {
        static char out[512];
        dialog = 0; flash("Installing Editor..."); draw();
        sys_kcmd("clan install editor", out, sizeof out);
        flash("Editor installed. Double-click the file again to open it.");
    } else if (dialog == 6 && ilen) { dialog = 0; do_search(); return; }
    else if (dialog == 5) {
        for (int i = 0; i < nents; i++) { char p[256]; child_path(ents[i].name, p, sizeof p); fs_delete(p); }
        flash("Trash emptied.");
    }
    dialog = 0;
    load();
}

static void tool(int i) {
    if (i == T_BACK && searching) { searching = false; load(); }
    else if (i == T_BACK && nback) { char p[256]; strncpy0(p, back_stack[--nback], 256); go(p, false); }
    else if (i == T_SEARCH) { dialog = 6; input[0] = 0; ilen = 0; }
    else if (searching) return;
    else if (i == T_NEW && !in_trash()) { dialog = 1; strncpy0(input, "New folder", sizeof input); ilen = strlen(input); }
    else if (i == T_RENAME && sel >= 0) { dialog = 2; strncpy0(input, ents[sel].name, sizeof input); ilen = strlen(input); }
    else if (i == T_COPY && sel >= 0) { child_path(ents[sel].name, clip, sizeof clip); clip_cut = false; flash("Copied. Open another folder and press Paste."); }
    else if (i == T_CUT && sel >= 0) { child_path(ents[sel].name, clip, sizeof clip); clip_cut = true; flash("Cut. Open another folder and press Paste to move it there."); }
    else if (i == T_PASTE && !in_trash()) do_paste();
    else if (i == T_DELETE && sel >= 0) { if (in_trash() || !under(cwd, "/home")) dialog = 3; else do_delete(false); }
}

static int hit_tool(int x, int y) {
    if (in_trash() && nents) { int tx, ty, tw, th; tool_rect(T_NEW, &tx, &ty, &tw, &th); if (ui_in(x, y, tx, ty, tw, th)) return 50; }
    for (int i = 0; i < NTOOLS; i++) { int tx, ty, tw, th; tool_rect(i, &tx, &ty, &tw, &th); if (ui_in(x, y, tx, ty, tw, th)) return i; }
    return -1;
}
static int hit_item(int x, int y) {
    if (x < SIDE || y < TOOL_H || y > WH - STATUS_H) return -1;
    for (int i = 0; i < nents; i++) { int cx, cy; cell_rect(i, &cx, &cy); if (ui_in(x, y, cx + 2, cy - 4, CELL_W - 4, CELL_H)) return i; }
    return -1;
}
static int hit_result(int x, int y) {
    if (!searching || x < SIDE || y < TOOL_H + 30 || y > WH - STATUS_H) return -1;
    int i = (y - (TOOL_H + 34) + rscroll) / 26;
    return i >= 0 && i < nres ? i : -1;
}
static int hit_place(int x, int y) {
    if (x >= SIDE) return -1;
    for (int i = 0; i < nplaces; i++) if (ui_in(x, y, 10, place_y(i) - 7, 150, 30)) return i;
    return -1;
}
static int hit_dialog(int x, int y) {
    int dw = 380, dh = 150, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
    if (ui_in(x, y, dx + dw - 116, dy + dh - 48, 96, 32)) return 100;
    if (ui_in(x, y, dx + dw - 222, dy + dh - 48, 96, 32)) return 101;
    return -1;
}

int main(void) {
    char arg[256];
    if (sys_getarg(arg, sizeof arg) > 0) strncpy0(cwd, arg, sizeof cwd);
    if (!app_open_ex(&a, WW, WH, "Files", WIN_RESIZABLE)) return 1;
    load();
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, status_until ? 1000 : -1);
        if (r == 0) { if (sys_time() >= status_until) status_until = 0; draw(); continue; }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }
        if (e.type == EV_FILES_CHANGED) { load(); draw(); continue; }
        if (e.type == EV_MOUSE_MOVE) { mouse_x = e.x; mouse_y = e.y; }
        if (e.type == EV_MOUSE_MOVE && mouse_held && drag_item >= 0 && !dialog && !searching) {   /* a drag starts after a few pixels */
            int dx = e.x - drag_x0, dy = e.y - drag_y0;
            if (!dragging && dx * dx + dy * dy > 64) dragging = true;
            if (dragging) { hover_place = hit_place(e.x, e.y); draw(); continue; }
        }
        if (e.type == EV_MOUSE_UP && e.button == 1) {
            mouse_held = false;
            if (dragging) {
                dragging = false;
                int p = hit_place(e.x, e.y), it = hit_item(e.x, e.y);
                if (p >= 0) drop_on(places[p].path);
                else if (it >= 0 && it != drag_item && ents[it].is_dir) { char d[256]; child_path(ents[it].name, d, sizeof d); drop_on(d); }
                drag_item = -1; draw();
            }
            continue;
        }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int hb = -1, hp = -1;
            if (e.type == EV_MOUSE_MOVE) { hb = dialog ? hit_dialog(e.x, e.y) : hit_tool(e.x, e.y); hp = dialog ? -1 : hit_place(e.x, e.y); }
            if (hb != hover_btn || hp != hover_place) { hover_btn = hb; hover_place = hp; draw(); }
            continue;
        }
        if (e.type == EV_WHEEL && searching) {
            rscroll += e.key * 40; int maxs = nres * 26 - (WH - TOOL_H - STATUS_H - 60);
            if (rscroll > maxs) rscroll = maxs;
            if (rscroll < 0) rscroll = 0;
            draw(); continue;
        }
        if (e.type == EV_WHEEL) {
            int rows = (nents + cols() - 1) / cols(), maxs = rows * CELL_H - (WH - TOOL_H - STATUS_H - 24);
            scroll += e.key * 40;
            if (scroll > maxs) scroll = maxs;
            if (scroll < 0) scroll = 0;
            draw();
            continue;
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            if (dialog) {
                int h = hit_dialog(e.x, e.y);
                if (h == 100) dialog_ok(); else if (h == 101) dialog = 0;
                draw(); continue;
            }
            int t = hit_tool(e.x, e.y);
            if (t == 50) { dialog = 5; draw(); continue; }
            if (t >= 0) { tool(t); draw(); continue; }
            int p = hit_place(e.x, e.y);
            if (p >= 0) { searching = false; go(places[p].path, true); draw(); continue; }
            if (searching) {
                int ri = hit_result(e.x, e.y); uint64_t now = sys_time();
                if (ri >= 0 && ri == rsel && now - last_click < 450) open_result(ri);
                else { rsel = ri; last_click = now; }
                draw(); continue;
            }
            int it = hit_item(e.x, e.y);
            mouse_held = true; drag_item = it; drag_x0 = e.x; drag_y0 = e.y; dragging = false;
            uint64_t now = sys_time();
            if (it >= 0 && it == last_click_item && now - last_click < 450) { last_click_item = -1; open_item(it); draw(); continue; }
            last_click = now; last_click_item = it;
            sel = it;
            draw();
        }
        if (e.type == EV_KEY) {
            int k = e.key;
            if (dialog == 1 || dialog == 2 || dialog == 6) {
                if (k == '\n') dialog_ok();
                else if (k == 27) dialog = 0;
                else if (k == '\b') { if (ilen) input[--ilen] = 0; }
                else if (k == 22) clip_paste_line(input, &ilen, 41);
                else if (key_is_char(k) && ilen < 40) { input[ilen++] = k; input[ilen] = 0; }
            } else if (dialog) {
                if (k == '\n') dialog_ok(); else if (k == 27) dialog = 0;
            } else if (k == 6) tool(T_SEARCH);   /* Ctrl+F */
            else if (searching && k == 27) { searching = false; load(); }
            else if (searching && k == '\n') open_result(rsel);
            else if (searching && k == KEY_DOWN) { if (rsel < nres - 1) rsel++; }
            else if (searching && k == KEY_UP) { if (rsel > 0) rsel--; }
            else if (searching) { }
            else if (k == '\n' && sel >= 0) open_item(sel);
            else if (k == '\b') { char p[256]; strncpy0(p, cwd, sizeof p); char *s = p + strlen(p); while (s > p && *s != '/') s--; if (s == p) s[1] = 0; else *s = 0; go(p, true); }
            else if (k == KEY_DEL) tool(T_DELETE);
            else if (k == 3) tool(T_COPY);     /* Ctrl+C */
            else if (k == 24) tool(T_CUT);     /* Ctrl+X */
            else if (k == 22) tool(T_PASTE);   /* Ctrl+V */
            else if (k == KEY_RIGHT && nents) sel = sel < nents - 1 ? sel + 1 : sel;
            else if (k == KEY_LEFT && nents) sel = sel > 0 ? sel - 1 : 0;
            else if (k == KEY_DOWN && nents) sel = sel + cols() < nents ? sel + cols() : sel;
            else if (k == KEY_UP && nents) sel = sel - cols() >= 0 ? sel - cols() : sel;
            draw();
        }
    }
    return 0;
}
