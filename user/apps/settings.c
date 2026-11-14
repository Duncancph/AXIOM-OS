/* Settings: keyboard, date & time, display, sound, lock screen, power, updates, storage (install AXIOM), about. */
#include "../lib/dunc.h"
#include "../lib/img/img.h"

#define WW 820
#define WH 580
#define SIDE 190
static struct app a;
enum { S_KEYB, S_MOUSE, S_TIME, S_DISPLAY, S_PERSONAL, S_SOUND, S_LOCK, S_POWER, S_UPDATES, S_STORAGE, S_ABOUT, NSECT };
static const char *sect_names[NSECT] = { "Keyboard", "Mouse", "Date & time", "Display", "Personalise", "Sound", "Lock screen", "Power", "Updates", "Storage", "About" };
static int sect, hover = -1;

/* ---- clickable regions, rebuilt on every draw ---- */
struct hit { int x, y, w, h, id; };
static struct hit hits[96]; static int nhits;
static void add_hit(int x, int y, int w, int h, int id) { if (nhits < 96) hits[nhits++] = (struct hit){ x, y, w, h, id }; }
static int hit_at(int x, int y) { for (int i = nhits - 1; i >= 0; i--) if (ui_in(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return hits[i].id; return -1; }
enum { H_SECT = 0, H_LIST = 100, H_TOGGLE = 300, H_BTN = 400, H_FIELD = 500, H_DISK = 600 };

static char sv[16][112];                       /* cached setting values */
static const char *keys[16] = { "keyboard.layout", "time.zone", "time.rtc_local", "time.24h", "display.mode", "display.current",
                                "sound.volume", "lock.password", "lock.idle_min", "screen.off_min", "mouse.speed", "mouse.natural", "mouse.left", "system.cores" };
static const char *get(const char *k) { for (int i = 0; i < 14; i++) if (!strcmp(keys[i], k)) return sv[i]; return ""; }
static void reload(void) { for (int i = 0; i < 14; i++) sys_setting_get(keys[i], sv[i], sizeof sv[i]); }
static void set(const char *k, const char *v) { sys_setting_set(k, v); reload(); }

/* "id=Name;id=Name" lists from the kernel */
static char listbuf[4096]; static char *lid[64], *lname[64]; static int nlist, list_scroll;
static void load_list(const char *key) {
    nlist = 0; list_scroll = 0;
    if (sys_setting_get(key, listbuf, sizeof listbuf) <= 0) return;
    for (char *p = listbuf; *p && nlist < 64;) {
        char *e = p; while (*e && *e != ';') e++;
        char save = *e; *e = 0;
        char *eq = p; while (*eq && *eq != '=') eq++;
        if (*eq) { *eq = 0; lid[nlist] = p; lname[nlist] = eq + 1; nlist++; }
        if (!save) break;
        p = e + 1;
    }
}

/* ---- widgets ---- */
static void heading(const char *t, const char *sub) {
    gfx_text(&a.c, &font_title, SIDE + 32, 48, UI_TEXT, t);
    if (sub) gfx_text(&a.c, &font_ui, SIDE + 32, 72, UI_MUTED, sub);
}
static void label(int x, int y, uint32_t col, const char *t) { gfx_text(&a.c, &font_ui, x, y, col, t); }

static void toggle(int x, int y, const char *text, bool on, int id) {
    struct canvas *c = &a.c;
    gfx_rrect(c, x, y, 40, 22, 11, on ? UI_ACCENT : 0x2A2A28, 255);
    gfx_circle(c, on ? x + 29 : x + 11, y + 11, 8, on ? HL_NAVY : UI_TEXT, 255);
    label(x + 52, y + 16, UI_TEXT, text);
    add_hit(x, y, 52 + gfx_text_width(&font_ui, text), 22, id);
}

static void option(int x, int y, int w, const char *text, bool sel, int id) {
    struct canvas *c = &a.c;
    if (sel) gfx_rrect(c, x, y, w, 30, 8, 0x26261F, 255);
    else if (hover == id) gfx_rrect(c, x, y, w, 30, 8, UI_PANEL2, 255);
    gfx_circle(c, x + 16, y + 15, 7, sel ? UI_ACCENT : 0x3C3B37, 255);
    if (sel) gfx_circle(c, x + 16, y + 15, 3, HL_NAVY, 255);
    label(x + 32, y + 20, sel ? UI_TEXT : HL_TEXT_DIM, text);
    add_hit(x, y, w, 30, id);
}

static void button(int x, int y, int w, const char *t, int style, int id, bool disabled) {
    ui_button(&a.c, x, y, w, 34, t, style, hover == id, disabled);
    if (!disabled) add_hit(x, y, w, 34, id);
}

/* text fields */
struct field { char s[64]; int len; bool secret; };
static struct field fields[4]; static int focus = -1;
static void field(int x, int y, int w, int f, const char *placeholder) {
    struct canvas *c = &a.c;
    gfx_rrect(c, x, y, w, 34, 8, HL_TERM_BG, 255);
    gfx_rrect_outline(c, x, y, w, 34, 8, focus == f ? UI_ACCENT : UI_BORDER, 255);
    char shown[64];
    if (fields[f].secret) { for (int i = 0; i < fields[f].len; i++) shown[i] = '*'; shown[fields[f].len] = 0; }
    else strncpy0(shown, fields[f].s, sizeof shown);
    if (!fields[f].len && focus != f) label(x + 12, y + 22, UI_MUTED, placeholder);
    else label(x + 12, y + 22, UI_TEXT, shown);
    if (focus == f) gfx_fill(c, x + 12 + gfx_text_width(&font_ui, shown) + 1, y + 9, 2, 17, UI_ACCENT);
    add_hit(x, y, w, 34, H_FIELD + f);
}
static void clear_fields(void) { memset(fields, 0, sizeof fields); fields[0].secret = fields[1].secret = true; focus = -1; }

static char msg[120]; static uint32_t msg_col;
static void say(uint32_t col, const char *m) { strncpy0(msg, m, sizeof msg); msg_col = col; }

/* ---- sections ---- */
static void draw_keyboard(void) {
    heading("Keyboard", "Choose the layout printed on your keyboard. It changes straight away.");
    int y = 100;
    for (int i = 0; i < nlist; i++, y += 36) option(SIDE + 32, y, 300, lname[i], !strcmp(lid[i], get("keyboard.layout")), H_LIST + i);
    label(SIDE + 360, 116, UI_MUTED, "Try it:");
    field(SIDE + 360, 128, 240, 2, "Type here");
    label(SIDE + 360, 190, UI_MUTED, "AltGr (right Alt) types the third");
    label(SIDE + 360, 210, UI_MUTED, "symbol on a key, e.g. \x80 or @.");
    label(SIDE + 360, 238, UI_MUTED, "Accent keys (\xb4 ^ \xa8) combine with");
    label(SIDE + 360, 258, UI_MUTED, "the next letter: ^ then e = \xea");
}

static void draw_mouse(void) {
    heading("Mouse and touchpad", "Changes take effect straight away.");
    int sp = (int)katoi(get("mouse.speed")); if (sp < 1 || sp > 10) sp = 5;
    label(SIDE + 32, 104, UI_TEXT, "Pointer speed");
    int x = SIDE + 32, y = 120, w = 400;
    gfx_rrect(&a.c, x, y + 8, w, 6, 3, 0x2A2A28, 255);
    gfx_rrect(&a.c, x, y + 8, w * (sp - 1) / 9, 6, 3, UI_ACCENT, 255);
    for (int i = 0; i < 10; i++) gfx_fill(&a.c, x + w * i / 9, y + 18, 1, 5, UI_MUTED);
    gfx_circle(&a.c, x + w * (sp - 1) / 9, y + 11, 9, UI_TEXT, 255);
    add_hit(x - 10, y - 4, w + 20, 30, H_TOGGLE + 20);
    label(x, y + 42, UI_MUTED, "Slow"); label(x + w - gfx_text_width(&font_ui, "Fast"), y + 42, UI_MUTED, "Fast");
    toggle(SIDE + 32, 196, "Natural scrolling (the page follows your fingers, as on phones)", !strcmp(get("mouse.natural"), "1"), H_TOGGLE + 21);
    toggle(SIDE + 32, 230, "Left-handed (swap the left and right buttons)", !strcmp(get("mouse.left"), "1"), H_TOGGLE + 22);
}

static void draw_time(void) {
    struct rtc_time t; sys_rtc(&t);
    char b[64];
    bool h24 = !strcmp(get("time.24h"), "1");
    if (h24) ksnprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.min, t.sec);
    else ksnprintf(b, sizeof b, "%d:%02d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.min, t.sec, t.hour < 12 ? "AM" : "PM");
    heading("Date & time", 0);
    gfx_text(&a.c, &font_big, SIDE + 32, 104, UI_TEXT, b);
    static const char *mn[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    int o = t.utc_offset_min, ao = o < 0 ? -o : o;
    ksnprintf(b, sizeof b, "%d %s %d   (UTC%c%d:%02d)", t.day, mn[(t.mon - 1) % 12], t.year, o < 0 ? '-' : '+', ao / 60, ao % 60);
    label(SIDE + 32, 128, UI_MUTED, b);
    toggle(SIDE + 32, 146, "24-hour clock", h24, H_TOGGLE + 0);
    toggle(SIDE + 32, 178, "This PC's clock is set to local time (as Windows sets it)", !strcmp(get("time.rtc_local"), "1"), H_TOGGLE + 1);
    label(SIDE + 32, 232, UI_TEXT, "Time zone");
    int x = SIDE + 32, y = 244, rows = 9, w = WW - SIDE - 64;
    ui_card(&a.c, x, y, w, rows * 32 + 8);
    for (int i = 0; i < rows && list_scroll + i < nlist; i++) {
        int k = list_scroll + i;
        option(x + 4, y + 4 + i * 32, w - 8, lname[k], !strcmp(lid[k], get("time.zone")), H_LIST + k);
    }
    if (nlist > rows) {                                  /* scroll bar */
        int th = (rows * 32) * rows / nlist, ty = y + 4 + (rows * 32 - th) * list_scroll / (nlist - rows);
        gfx_rrect(&a.c, x + w - 8, ty, 4, th, 2, 0x3C3B37, 255);
    }
}

static const char *modes[] = { "", "1024x768", "1280x720", "1280x800", "1366x768", "1440x900", "1600x900", "1920x1080", "1920x1200", "2560x1440" };
/* ---- Personalise: wallpaper, accent colour, name ---- */
static const struct { const char *name; const char *hex; } accents[] = {
    { "Cream", "" }, { "Sky", "9CC9F5" }, { "Mint", "9FE0B6" }, { "Peach", "F5B88E" }, { "Rose", "F2A7C3" }, { "Lilac", "C9B6F5" } };
static char pics[8][96]; static int npics;
static bool is_pic(const char *n) {
    int l = strlen(n); if (l < 5) return false;
    const char *e = n + l - 4; char x[5]; for (int i = 0; i < 4; i++) x[i] = (e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]; x[4] = 0;
    return !strcmp(x, ".png") || !strcmp(x, ".jpg") || !strcmp(x, "jpeg") || !strcmp(x, ".bmp") || !strcmp(x, ".gif");
}
static void find_pictures(void) {
    static const char *dirs[] = { "/home/Pictures", "/home", "/home/Downloads" };
    struct dunc_dirent e[64]; npics = 0;
    for (int d = 0; d < 3; d++) {
        int n = fs_list(dirs[d], e, 64);
        for (int i = 0; i < n && npics < 8; i++) if (!e[i].is_dir && is_pic(e[i].name)) ksnprintf(pics[npics++], sizeof pics[0], "%s/%s", dirs[d], e[i].name);
    }
}
static void *wa(void *c, size_t n) { return malloc(n); }
static void wf_(void *c, void *p) { free(p); }
static void use_picture(const char *path) {
    struct dunc_stat st;
    if (fs_stat(path, &st) < 0 || st.size > (32 << 20)) { say(UI_RED, "That picture can't be read (or is bigger than 32 MB)."); return; }
    uint8_t *data = malloc(st.size); if (!data) return;
    long n = fs_read(path, data, 0, st.size);
    struct image im; struct img_alloc al = { 0, wa, wf_ };
    int r = n > 0 ? img_decode(data, n, &im, &al) : -1;
    free(data);
    if (r != IMG_OK) { say(UI_RED, "That picture can't be opened."); return; }
    int w = im.w, h = im.h, step = 1;                     /* keep it to about screen size */
    while (w / step > 2560 || h / step > 1600) step++;
    int ow = w / step, oh = h / step;
    long bytes = 12 + (long)ow * oh * 4;
    uint8_t *out = malloc(bytes);
    if (!out) { free(im.px); say(UI_RED, "Not enough memory for that picture."); return; }
    memcpy(out, "AXWP", 4); *(uint32_t *)(out + 4) = ow; *(uint32_t *)(out + 8) = oh;
    uint32_t *px = (uint32_t *)(out + 12);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        uint32_t p = im.px[(long)y * step * w + (long)x * step], al = p >> 24;
        px[(long)y * ow + x] = al == 255 ? (p & 0xFFFFFF) : gfx_mix(0x050505, p & 0xFFFFFF, al);
    }
    free(im.px);
    fs_mkdir("/system/config");
    long wr = fs_write("/system/config/wallpaper.raw", out, bytes);
    free(out);
    if (wr < 0) { say(UI_RED, "The picture couldn't be saved."); return; }
    sys_setting_set("desktop.wallpaper", "axiom"); sys_setting_set("desktop.wallpaper", "picture");
    say(UI_GREEN, "Wallpaper changed.");
}
static void draw_personal(void) {
    heading("Personalise", "Wallpaper, accent colour and your name.");
    char wp[32] = ""; sys_setting_get("desktop.wallpaper", wp, sizeof wp);
    label(SIDE + 32, 104, UI_TEXT, "Wallpaper");
    option(SIDE + 32, 116, 200, "AXIOM glow", !strcmp(wp, "axiom") || !wp[0], H_LIST + 0);
    option(SIDE + 252, 116, 200, "Plain black", !strcmp(wp, "black"), H_LIST + 1);
    int y = 158;
    if (!npics) label(SIDE + 32, y + 20, UI_MUTED, "For a picture, put a PNG or JPEG in Pictures (or Home) in Files.");
    for (int i = 0; i < npics; i++) {
        const char *nm = pics[i]; for (const char *q = pics[i]; *q; q++) if (*q == '/') nm = q + 1;
        option(SIDE + 32 + (i % 2) * 220, y + (i / 2) * 36, 200, nm, false, H_LIST + 10 + i);
    }
    y += ((npics + 1) / 2) * 36 + (npics ? 14 : 44);
    label(SIDE + 32, y, UI_TEXT, "Accent colour"); y += 12;
    char ac[16] = ""; sys_setting_get("ui.accent", ac, sizeof ac);
    for (int i = 0; i < 6; i++) {
        int x = SIDE + 32 + i * 92;
        uint32_t col = accent_from_setting(accents[i].hex);
        bool sel = !strcmp(ac, accents[i].hex);
        gfx_rrect(&a.c, x, y, 84, 52, 10, sel ? 0x2E2E2B : UI_PANEL, 255);
        if (sel) gfx_rrect_outline(&a.c, x, y, 84, 52, 10, col, 255);
        gfx_circle(&a.c, x + 42, y + 18, 10, col, 255);
        gfx_text_center(&a.c, &font_ui, x, 84, y + 46, sel ? UI_TEXT : UI_MUTED, accents[i].name);
        add_hit(x, y, 84, 52, H_LIST + 30 + i);
    }
    y += 76;
    label(SIDE + 32, y, UI_TEXT, "Your name (shown in the terminal)");
    field(SIDE + 32, y + 12, 260, 3, "bryan");
    button(SIDE + 304, y + 12, 90, "Save", BTN_PRIMARY, H_BTN + 50, false);
}

static void draw_display(void) {
    char b[96];
    heading("Display", 0);
    ksnprintf(b, sizeof b, "The screen is %s now.", get("display.current"));
    label(SIDE + 32, 74, UI_MUTED, b);
    label(SIDE + 32, 104, UI_TEXT, "Screen size from the next start:");
    for (int i = 0; i < 10; i++) {
        int col = i / 5, row = i % 5;
        option(SIDE + 32 + col * 260, 116 + row * 36, 240, i ? modes[i] : "Automatic (best available)", !strcmp(get("display.mode"), modes[i]), H_LIST + i);
    }
    label(SIDE + 32, 316, UI_MUTED, "It applies when AXIOM next starts. If the graphics card can't switch to");
    label(SIDE + 32, 336, UI_MUTED, "that size (common on older PCs), AXIOM draws the desktop at that size and");
    label(SIDE + 32, 356, UI_MUTED, "stretches it to fill the screen, so everything looks bigger. A size larger");
    label(SIDE + 32, 376, UI_MUTED, "than the screen can show is ignored.");
}

static void draw_sound(void) {
    heading("Sound", 0);
    struct sysinfo si; sys_sysinfo(&si);
    int vol = (int)katoi(get("sound.volume"));
    label(SIDE + 32, 100, UI_TEXT, "Volume");
    int x = SIDE + 32, y = 116, w = 400;
    gfx_rrect(&a.c, x, y + 8, w, 6, 3, 0x2A2A28, 255);
    gfx_rrect(&a.c, x, y + 8, w * vol / 100, 6, 3, UI_ACCENT, 255);
    gfx_circle(&a.c, x + w * vol / 100, y + 11, 9, UI_TEXT, 255);
    add_hit(x - 10, y - 4, w + 20, 30, H_TOGGLE + 10);
    char b[16]; ksnprintf(b, sizeof b, "%d%%", vol); label(x + w + 20, y + 16, UI_TEXT, b);
    button(SIDE + 32, 160, 160, "Play test sound", BTN_PRIMARY, H_BTN + 10, false);
}

static void draw_lock(void) {
    bool has = get("lock.password")[0];
    heading("Lock screen", has ? "A password protects this computer. Lock it with Windows key + L." : "No password is set: anyone at this computer can use it.");
    label(SIDE + 32, 104, UI_TEXT, has ? "Change password" : "Set a password");
    field(SIDE + 32, 116, 260, 0, "New password");
    field(SIDE + 32, 158, 260, 1, "Type it again");
    button(SIDE + 32, 202, 150, has ? "Change" : "Set password", BTN_PRIMARY, H_BTN + 0, false);
    if (has) button(SIDE + 192, 202, 150, "Remove password", BTN_DANGER, H_BTN + 1, false);
    if (has) button(SIDE + 352, 202, 120, "Lock now", BTN_NORMAL, H_BTN + 2, false);
    label(SIDE + 32, 276, UI_TEXT, "Lock automatically when not used for");
    static const char *v[] = { "0", "1", "5", "10", "30" }, *n[] = { "Never", "1 minute", "5 minutes", "10 minutes", "30 minutes" };
    for (int i = 0; i < 5; i++) option(SIDE + 32 + (i % 3) * 190, 288 + (i / 3) * 36, 180, n[i], !strcmp(get("lock.idle_min"), v[i]), H_LIST + i);
    if (!has) label(SIDE + 32, 380, UI_MUTED, "(Automatic locking needs a password.)");
}

static void draw_power(void) {
    heading("Power", 0);
    label(SIDE + 32, 100, UI_TEXT, "Turn the screen off when not used for");
    static const char *v[] = { "0", "2", "5", "10", "30" }, *n[] = { "Never", "2 minutes", "5 minutes", "10 minutes", "30 minutes" };
    for (int i = 0; i < 5; i++) option(SIDE + 32 + (i % 3) * 190, 112 + (i / 3) * 36, 180, n[i], !strcmp(get("screen.off_min"), v[i]), H_LIST + i);
    label(SIDE + 32, 212, UI_MUTED, "Move the mouse or press a key to turn it back on.");
    button(SIDE + 32, 236, 140, "Restart", BTN_NORMAL, H_BTN + 3, false);
    button(SIDE + 182, 236, 140, "Shut down", BTN_DANGER, H_BTN + 4, false);
    ui_card(&a.c, SIDE + 32, 296, WW - SIDE - 64, 86);
    label(SIDE + 48, 322, UI_TEXT, "Battery and sleep");
    label(SIDE + 48, 344, UI_MUTED, "Not available yet: laptops report their battery and sleep through");
    label(SIDE + 48, 364, UI_MUTED, "ACPI programs that AXIOM can't run yet. Plug laptops in.");
}


static const char *strchr_(const char *s, char c) { for (; *s; s++) if (*s == c) return s; return 0; }
void draw(void);
/* ---- Updates: fetch a signed update over the web; the kernel checks the signature and every file ---- */
enum { U_IDLE, U_CHECKING, U_CURRENT, U_AVAILABLE, U_DOWNLOADING, U_INSTALLING, U_DONE, U_FAILED };
static int ustate; static char umsg[160];
static char *mtext; static long mlen;                     /* the manifest */
static char uver[32], unotes[204]; static long ubuild, ubuild_now;
static long dl_got, dl_total; static int dl_file;
static struct dunc_install inst; static bool installing;

struct buf { char *p; long n, cap, max; };
static bool buf_sink(const char *d, long n, void *ctx) {
    struct buf *b = ctx;
    if (b->n + n > b->max) return false;
    if (b->n + n > b->cap) { long c = b->cap ? b->cap * 2 : 65536; while (c < b->n + n) c *= 2; char *q = realloc(b->p, c); if (!q) return false; b->p = q; b->cap = c; }
    memcpy(b->p + b->n, d, n); b->n += n;
    if (ustate == U_DOWNLOADING) {
        dl_got = b->n;
        static uint64_t last; uint64_t now = sys_time();
        if (now - last > 150) { last = now; draw(); }
    }
    return true;
}
static const char *upd_error(long r) {
    switch (r) {
    case UPD_ESIG: return "The update isn't signed with the AXIOM key, so it was refused.";
    case UPD_EFORMAT: return "The update file is damaged or in a format this AXIOM doesn't know.";
    case UPD_EOLD: return "That update isn't newer than this system.";
    case UPD_EHASH: return "A downloaded file doesn't match the signed update (damaged download?).";
    case UPD_ENODISK: return "Updates install to AXIOM on a disk. Install it first (Settings > Storage).";
    case UPD_ENOMEM: return "Not enough memory to install the update.";
    default: return "The installer is busy. Try again in a moment.";
    }
}
static void manifest_field(const char *name, char *out, int max) {
    out[0] = 0;
    for (const char *p = mtext; p && p < mtext + mlen;) {
        const char *e = p; while (e < mtext + mlen && *e != '\n') e++;
        int n = (int)strlen(name);
        if (e - p > n && !strncmp(p, name, n) && p[n] == ' ') { int k = (int)(e - p - n - 1); if (k >= max) k = max - 1; memcpy(out, p + n + 1, k); out[k] = 0; return; }
        p = e + 1;
    }
}
/* resolve a path in the manifest against the manifest's URL */
static void url_of(const char *base, const char *rel, char *out, int max) {
    strncpy0(out, base, max);
    char *slash = 0; for (char *p = out; *p; p++) if (*p == '/') slash = p;
    if (slash) slash[1] = 0;
    int n = (int)strlen(out); strncpy0(out + n, rel, max - n);
}
static void check_updates(void) {
    char url[120]; sys_setting_get("update.url", url, sizeof url);
    if (!url[0]) { ustate = U_FAILED; strncpy0(umsg, "Updates aren't set up for this copy of AXIOM yet.", sizeof umsg); return; }
    ustate = U_CHECKING; draw();
    struct buf b = { 0, 0, 0, 16384 };
    int status = 0;
    struct http_req rq = { "GET", 0, 0, 0, 0, "Cache-Control: no-cache\r\n", 20000 };
    char fresh[160]; ksnprintf(fresh, sizeof fresh, "%s%cnocache=%lu", url, strchr_(url, '?') ? '&' : '?', (unsigned long)sys_time());   /* skip caches */
    long r = http_request(fresh, &rq, buf_sink, &b, &status, 0, 0, 0, 0, 0);
    free(mtext); mtext = b.p; mlen = b.n;
    if (r < 0) { ustate = U_FAILED; ksnprintf(umsg, sizeof umsg, "Couldn't check for updates: %s.", http_strerror(r)); return; }
    if (status == 404) { ustate = U_CURRENT; strncpy0(umsg, "No updates have been published yet.", sizeof umsg); return; }
    if (status != 200) { ustate = U_FAILED; ksnprintf(umsg, sizeof umsg, "Couldn't check for updates: the server answered %d.", status); return; }
    long v = sys_call(SYS_UPDATE, UPDATE_VERIFY, (long)mtext, mlen, 0, 0);
    if (v < 0) { ustate = U_FAILED; strncpy0(umsg, upd_error(v), sizeof umsg); return; }
    ubuild = v;
    manifest_field("version", uver, sizeof uver); manifest_field("notes", unotes, sizeof unotes);
    if (ubuild <= ubuild_now) { ustate = U_CURRENT; strncpy0(umsg, "AXIOM is up to date.", sizeof umsg); return; }
    ustate = U_AVAILABLE; umsg[0] = 0;
}
static void *ia(void *c, size_t n) { return malloc(n); }
static void if_(void *c, void *p) { free(p); }
static void install_update(void) {
    static const char *roles[UPD_FILES] = { "kernel", "initrd", "bootx64", "biosboot" };
    struct dunc_update u = { mtext, mlen, { 0 }, { 0 } };
    char url[120]; sys_setting_get("update.url", url, sizeof url);
    ustate = U_DOWNLOADING;
    for (int i = 0; i < UPD_FILES; i++) {
        char name[24], line[200], path[100] = ""; long size = 0;
        ksnprintf(name, sizeof name, "file %s", roles[i]);
        manifest_field(name, line, sizeof line);             /* "<path> <size> <sha>" */
        if (!line[0] && i >= 3) continue;                   /* optional file not in this update */
        int k = 0; while (line[k] && line[k] != ' ' && k < 99) { path[k] = line[k]; k++; } path[k] = 0;
        size = katoi(line + k + 1);
        char full[240]; url_of(url, path, full, sizeof full);
        dl_file = i; dl_got = 0; dl_total = size;
        struct buf b = { 0, 0, 0, 256L << 20 };
        int status = 0; long total = 0;
        struct http_req rq = { "GET", 0, 0, 0, 0, 0, 30000 };
        long r = http_request(full, &rq, buf_sink, &b, &status, &total, 0, 0, 0, 0);
        if (r < 0 || status != 200) {
            ustate = U_FAILED;
            if (r < 0) ksnprintf(umsg, sizeof umsg, "The download failed: %s.", http_strerror(r));
            else ksnprintf(umsg, sizeof umsg, "The download failed: the server answered %d.", status);
            free(b.p); goto out;
        }
        k = (int)strlen(path);
        if (k > 3 && !strcmp(path + k - 3, ".gz")) {
            uint8_t *out = malloc(size + 1);
            struct img_alloc al = { 0, ia, if_ };
            long n = out ? gunzip((const uint8_t *)b.p, b.n, out, size + 1, &al) : -1;
            free(b.p);
            if (n != size) { free(out); ustate = U_FAILED; strncpy0(umsg, "A downloaded file couldn't be unpacked (damaged download?).", sizeof umsg); goto out; }
            u.data[i] = out; u.len[i] = n;
        } else { u.data[i] = (const unsigned char *)b.p; u.len[i] = b.n; }
    }
    ustate = U_INSTALLING; draw();
    long r = sys_call(SYS_UPDATE, UPDATE_APPLY, (long)&u, 0, 0, 0);
    if (r < 0) { ustate = U_FAILED; strncpy0(umsg, upd_error(r), sizeof umsg); }
    else { installing = true; memset(&inst, 0, sizeof inst); inst.state = 1; }
out:
    for (int i = 0; i < UPD_FILES; i++) free((void *)u.data[i]);
}
static void draw_updates(void) {
    char b[200];
    struct sysinfo si; sys_sysinfo(&si);
    ksnprintf(b, sizeof b, "This is AXIOM %s, build %ld. Updates are signed; AXIOM checks them before installing.", strncmp(si.kernel, "Thane ", 6) ? si.kernel : si.kernel + 6, ubuild_now);
    heading("Updates", b);
    int y = 100, x = SIDE + 32, w = WW - SIDE - 64;
    ui_card(&a.c, x, y, w, 150);
    if (ustate == U_AVAILABLE || ustate == U_DOWNLOADING || ustate == U_INSTALLING) {
        ksnprintf(b, sizeof b, "AXIOM %s (build %ld) is available", uver, ubuild);
        gfx_text(&a.c, &font_ui_bold, x + 16, y + 30, UI_TEXT, b);
        if (unotes[0]) { utf8_to_cp1252(unotes, strlen(unotes) + 1); label(x + 16, y + 54, UI_MUTED, unotes); }
        label(x + 16, y + 78, UI_MUTED, "Your files, apps and settings are kept. Restart afterwards to use it.");
        if (ustate == U_AVAILABLE) button(x + 16, y + 98, 200, "Download and install", BTN_PRIMARY, H_BTN + 30, false);
        if (ustate == U_DOWNLOADING) {
            static const char *what[] = { "the kernel", "the system files", "the boot loader", "the BIOS boot loader" };
            ksnprintf(b, sizeof b, "Downloading %s: %ld KB", what[dl_file], dl_got >> 10);
            label(x + 16, y + 112, UI_TEXT, b);
        }
        if (ustate == U_INSTALLING && !installing) label(x + 16, y + 112, UI_TEXT, "Checking the signature and files...");
    } else {
        gfx_text(&a.c, &font_ui_bold, x + 16, y + 30, UI_TEXT, ustate == U_CHECKING ? "Checking for updates..." : ustate == U_DONE ? "Update installed" : "AXIOM updates");
        label(x + 16, y + 54, ustate == U_FAILED ? UI_RED : ustate == U_CURRENT || ustate == U_DONE ? UI_GREEN : UI_MUTED,
              umsg[0] ? umsg : "Check for a newer version of AXIOM over the internet.");
        if (ustate == U_DONE) button(x + 16, y + 98, 150, "Restart now", BTN_PRIMARY, H_BTN + 32, false);
        else button(x + 16, y + 98, 180, "Check for updates", BTN_PRIMARY, H_BTN + 31, ustate == U_CHECKING);
    }
    if (installing || ustate == U_INSTALLING) {
        int py = y + 176;
        gfx_rrect(&a.c, x, py, w, 10, 5, 0x2A2A28, 255);
        gfx_rrect(&a.c, x, py, w * inst.percent / 100, 10, 5, inst.state == 3 ? UI_RED : UI_ACCENT, 255);
        label(x, py + 30, inst.state == 3 ? UI_RED : UI_TEXT, inst.msg);
    }
    char url[120]; sys_setting_get("update.url", url, sizeof url);
    if (url[0]) { ksnprintf(b, sizeof b, "Source: %s", url); label(x, WH - 52, HL_TEXT_FAINT, b); }
}

static struct dunc_disk disks[8]; static int ndisks, confirm_disk = -1, confirm_mode;
static void fmt_size(uint64_t b, char *o, int n) {
    if (b >= (10ull << 30)) ksnprintf(o, n, "%lu GB", b >> 30);
    else ksnprintf(o, n, "%lu MB", b >> 20);
}
static void draw_storage(void) {
    struct dunc_volume v[8]; int nv = sys_call(SYS_FS_VOLUMES, (long)v, 8, 0, 0, 0);
    bool persistent = nv > 0 && strcmp(v[0].kind, "Memory");
    heading("Storage", persistent ? "Your files are saved on a disk." : "AXIOM is running from memory: files are lost when you shut down.");
    int y = 92;
    for (int i = 0; i < ndisks && y < 420; i++) {
        struct dunc_disk *d = &disks[i];
        int h = 54 + 18 * d->nparts;
        ui_card(&a.c, SIDE + 32, y, WW - SIDE - 64, h);
        char b[160], sz[24]; fmt_size(d->bytes, sz, sizeof sz);
        ksnprintf(b, sizeof b, "%s  -  %s, %s", d->model, d->kind == DISK_SATA ? "SATA" : d->kind == DISK_NVME ? "NVMe" : "USB", sz);
        gfx_text(&a.c, &font_ui_bold, SIDE + 48, y + 24, UI_TEXT, b);
        if (d->system) label(SIDE + 48, y + 44, UI_GREEN, "Your files are saved here.");
        else if (!d->nparts) label(SIDE + 48, y + 44, UI_MUTED, "Empty (no partitions).");
        for (int k = 0; k < d->nparts; k++) {
            char p[96], ps[24]; fmt_size(d->part[k].bytes, ps, sizeof ps);
            ksnprintf(p, sizeof p, "Partition %d: %s %s %s", k + 1, ps, d->part[k].fs, d->part[k].label);
            label(SIDE + 48, y + (d->system ? 62 : 44) + 18 * k - (d->system ? 0 : 0), UI_MUTED, p);
        }
        if (!installing) {
            if (d->duncos) button(WW - 32 - 150, y + 10, 134, "Update AXIOM", BTN_NORMAL, H_DISK + i * 2 + 1, false);
            if (!d->system) button(WW - 32 - (d->duncos ? 300 : 150), y + 10, 134, "Install here...", BTN_PRIMARY, H_DISK + i * 2, false);
        }
        y += h + 10;
    }
    if (!ndisks) label(SIDE + 32, 110, UI_MUTED, "No disks found. AXIOM supports SATA (AHCI), NVMe and USB drives.");
    if (installing || inst.state >= 2) {
        int py = WH - 96;
        gfx_rrect(&a.c, SIDE + 32, py, WW - SIDE - 64, 10, 5, 0x2A2A28, 255);
        gfx_rrect(&a.c, SIDE + 32, py, (WW - SIDE - 64) * inst.percent / 100, 10, 5, inst.state == 3 ? UI_RED : UI_ACCENT, 255);
        label(SIDE + 32, py + 30, inst.state == 3 ? UI_RED : inst.state == 2 ? UI_GREEN : UI_TEXT, inst.msg);
    }
}

static void draw_about(void) {
    struct sysinfo s; sys_sysinfo(&s);
    heading("About AXIOM", "Made by Bryan Duncan. Running on the Thane kernel.");
    char b[128];
    int y = 110;
    ksnprintf(b, sizeof b, "Kernel: %s", s.kernel); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Processor: %s", s.cpu); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Memory: %lu MB, %lu MB in use", s.mem_total >> 20, s.mem_used >> 20); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Display: %s", get("display.current")); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Up for %lu minutes", s.uptime_ms / 60000); label(SIDE + 32, y, UI_TEXT, b); y += 34;
    /* processor cores */
    bool all = !strcmp(get("system.cores"), "all");
    ui_card(&a.c, SIDE + 32, y, WW - SIDE - 64, 92);
    gfx_text(&a.c, &font_ui_bold, SIDE + 48, y + 26, UI_TEXT, "Use all processor cores");
    ksnprintf(b, sizeof b, "This computer has %d core%s; AXIOM is using %d.%s", s.ncpus_found, s.ncpus_found == 1 ? "" : "s", s.ncpus_on,
              (all && s.ncpus_on < s.ncpus_found) || (!all && s.ncpus_on > 1) ? " Restart to apply." : "");
    label(SIDE + 48, y + 46, UI_MUTED, b);
    label(SIDE + 48, y + 64, UI_MUTED, "Apps run side by side on all of them. New: turn off if anything misbehaves.");
    toggle(WW - 32 - 64, y + 30, "", all, H_TOGGLE + 40);
    y += 106;
    label(SIDE + 32, y, UI_MUTED, "Fonts: GNU Unifont (GPLv2+ with font exception), Inter (SIL OFL 1.1)."); y += 22;
    label(SIDE + 32, y, UI_MUTED, "Trusted certificate authorities: Mozilla CA list (MPL 2.0)."); y += 34;
    ui_card(&a.c, SIDE + 32, y, WW - SIDE - 64, 92);
    gfx_text(&a.c, &font_ui_bold, SIDE + 48, y + 26, UI_TEXT, "App protection test");
    label(SIDE + 48, y + 46, UI_MUTED, "Opens an app that misbehaves on purpose, to show");
    label(SIDE + 48, y + 64, UI_MUTED, "AXIOM stopping it without affecting anything else.");
    button(WW - 32 - 150, y + 14, 134, "Run the test", BTN_NORMAL, H_BTN + 40, false);
}

void draw(void) {
    struct canvas *c = &a.c;
    nhits = 0;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_fill(c, 0, 0, SIDE, WH, 0x0A0A0A);
    for (int i = 0; i < NSECT; i++) {
        int y = 20 + i * 42;
        if (i == sect) { gfx_rrect(c, 12, y, SIDE - 24, 36, 10, 0x26261F, 255); gfx_fill(c, 12, y + 9, 3, 18, UI_ACCENT); }
        else if (hover == H_SECT + i) gfx_rrect(c, 12, y, SIDE - 24, 36, 10, UI_PANEL, 255);
        gfx_text(c, i == sect ? &font_ui_bold : &font_ui, 30, y + 23, i == sect ? UI_TEXT : HL_TEXT_DIM, sect_names[i]);
        add_hit(12, y, SIDE - 24, 36, H_SECT + i);
    }
    switch (sect) {
    case S_KEYB: draw_keyboard(); break; case S_MOUSE: draw_mouse(); break; case S_TIME: draw_time(); break; case S_DISPLAY: draw_display(); break; case S_PERSONAL: draw_personal(); break;
    case S_SOUND: draw_sound(); break; case S_LOCK: draw_lock(); break; case S_POWER: draw_power(); break;
    case S_UPDATES: draw_updates(); break; case S_STORAGE: draw_storage(); break; case S_ABOUT: draw_about(); break;
    }
    if (msg[0]) label(SIDE + 32, WH - 24, msg_col, msg);
    if (confirm_disk >= 0) {                                 /* "erase this disk?" */
        struct dunc_disk *d = &disks[confirm_disk];
        nhits = 0;
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 110);
        int dw = 520, dh = 230, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 14, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 14, confirm_mode == INSTALL_ERASE ? UI_RED : UI_ACCENT, 160);
        char b[128], sz[24]; fmt_size(d->bytes, sz, sizeof sz);
        if (confirm_mode == INSTALL_ERASE) {
            gfx_text(c, &font_title, dx + 24, dy + 44, UI_TEXT, "Erase this disk and install AXIOM?");
            ksnprintf(b, sizeof b, "%s (%s)", d->model, sz); gfx_text(c, &font_ui_bold, dx + 24, dy + 76, UI_TEXT, b);
            label(dx + 24, dy + 102, UI_RED, "Everything on it will be deleted, including Windows or Linux");
            label(dx + 24, dy + 122, UI_RED, "if they are on this disk. This can't be undone.");
            label(dx + 24, dy + 148, UI_MUTED, "Your AXIOM files are copied onto it. Starting from it needs UEFI.");
        } else {
            gfx_text(c, &font_title, dx + 24, dy + 44, UI_TEXT, "Update AXIOM on this disk?");
            ksnprintf(b, sizeof b, "%s (%s)", d->model, sz); gfx_text(c, &font_ui_bold, dx + 24, dy + 76, UI_TEXT, b);
            label(dx + 24, dy + 104, UI_MUTED, "The system files are replaced with the version running now.");
            label(dx + 24, dy + 124, UI_MUTED, "Your files, apps and settings on it are kept.");
        }
        ui_button(c, dx + dw - 200, dy + dh - 54, 176, 36, confirm_mode == INSTALL_ERASE ? "Erase and install" : "Update", confirm_mode == INSTALL_ERASE ? BTN_DANGER : BTN_PRIMARY, hover == H_BTN + 20, false);
        ui_button(c, dx + dw - 312, dy + dh - 54, 100, 36, "Cancel", BTN_NORMAL, hover == H_BTN + 21, false);
        add_hit(dx + dw - 200, dy + dh - 54, 176, 36, H_BTN + 20);
        add_hit(dx + dw - 312, dy + dh - 54, 100, 36, H_BTN + 21);
    }
    app_flush(&a);
}

static void open_section(int s) {
    sect = s; msg[0] = 0; clear_fields(); confirm_disk = -1;
    if (s == S_KEYB) { load_list("keyboard.layouts"); focus = 2; }
    if (s == S_TIME) {
        load_list("time.zones");
        for (int i = 0; i < nlist; i++) if (!strcmp(lid[i], get("time.zone"))) list_scroll = i > 4 ? i - 4 : 0;
        if (list_scroll > nlist - 9) list_scroll = nlist > 9 ? nlist - 9 : 0;
    }
    if (s == S_STORAGE) ndisks = sys_disks(disks, 8);
    if (s == S_PERSONAL) { find_pictures(); char un[64] = ""; sys_setting_get("user.name", un, sizeof un); strncpy0(fields[3].s, un, sizeof fields[3].s); fields[3].len = strlen(fields[3].s); }
}

static void click(int id, int mx) {
    if (id >= H_SECT && id < H_SECT + NSECT) { open_section(id - H_SECT); return; }
    if (id >= H_FIELD && id < H_FIELD + 4) { focus = id - H_FIELD; return; }
    focus = sect == S_KEYB ? 2 : -1;
    if (id >= H_LIST && id < H_LIST + 100) {
        int i = id - H_LIST;
        if (sect == S_KEYB && i < nlist) set("keyboard.layout", lid[i]);
        if (sect == S_TIME && i < nlist) set("time.zone", lid[i]);
        if (sect == S_PERSONAL) {
            if (i == 0) { sys_setting_set("desktop.wallpaper", "axiom"); say(UI_MUTED, "Wallpaper: AXIOM glow."); }
            else if (i == 1) { sys_setting_set("desktop.wallpaper", "black"); say(UI_MUTED, "Wallpaper: plain black."); }
            else if (i >= 10 && i < 10 + npics) { say(UI_MUTED, "Setting the wallpaper..."); draw(); use_picture(pics[i - 10]); }
            else if (i >= 30 && i < 36) { sys_setting_set("ui.accent", accents[i - 30].hex); hl_accent = accent_from_setting(accents[i - 30].hex); say(UI_GREEN, "Accent colour changed. Apps opened from now on use it too."); }
            return;
        }
        if (sect == S_DISPLAY) { set("display.mode", modes[i]); say(UI_MUTED, i ? "Saved. It takes effect the next time AXIOM starts." : "Saved: automatic."); }
        if (sect == S_LOCK) { static const char *v[] = { "0", "1", "5", "10", "30" }; set("lock.idle_min", v[i]); }
        if (sect == S_POWER) { static const char *v[] = { "0", "2", "5", "10", "30" }; set("screen.off_min", v[i]); }
        return;
    }
    if (id == H_TOGGLE + 0) set("time.24h", strcmp(get("time.24h"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 1) set("time.rtc_local", strcmp(get("time.rtc_local"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 20) {
        int v = 1 + ((mx - (SIDE + 32)) * 9 + 200) / 400; if (v < 1) v = 1; if (v > 10) v = 10;
        char b[8]; ksnprintf(b, sizeof b, "%d", v); set("mouse.speed", b);
    }
    if (id == H_TOGGLE + 21) set("mouse.natural", strcmp(get("mouse.natural"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 22) set("mouse.left", strcmp(get("mouse.left"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 10) {
        int v = (mx - (SIDE + 32)) * 100 / 400; if (v < 0) v = 0; if (v > 100) v = 100;
        char b[8]; ksnprintf(b, sizeof b, "%d", v); set("sound.volume", b);
    }
    if (id == H_BTN + 10) { if (sys_call(SYS_SOUND, SOUND_TEST, 0, 0, 0, 0) < 0) say(UI_RED, "No sound card found (AXIOM supports Intel HD Audio)."); }
    if (id == H_BTN + 0) {
        if (!fields[0].len) say(UI_RED, "Type a password first.");
        else if (strcmp(fields[0].s, fields[1].s)) say(UI_RED, "The two passwords don't match.");
        else { set("lock.password", fields[0].s); say(UI_GREEN, "Password saved. Windows key + L locks the screen."); clear_fields(); }
    }
    if (id == H_BTN + 1) { set("lock.password", ""); set("lock.idle_min", "0"); say(UI_MUTED, "Password removed."); }
    if (id == H_BTN + 2) sys_call(SYS_LOCK, 0, 0, 0, 0, 0);
    if (id == H_BTN + 3) sys_power(1);
    if (id == H_BTN + 4) sys_power(0);
    if (id >= H_DISK && id < H_DISK + 16) { confirm_disk = (id - H_DISK) / 2; confirm_mode = (id - H_DISK) % 2 ? INSTALL_UPDATE : INSTALL_ERASE; }
    if (id == H_BTN + 40) sys_spawn("sandbox");
    if (id == H_TOGGLE + 40) set("system.cores", !strcmp(get("system.cores"), "all") ? "one" : "all");
    if (id == H_BTN + 50) {
        int k = 0; char nm[41];                                   /* letters, digits, - and _ only, lower case */
        for (int i = 0; fields[3].s[i] && k < 32; i++) { char c = fields[3].s[i]; if (c >= 'A' && c <= 'Z') c += 32; if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') nm[k++] = c; }
        nm[k] = 0;
        if (!k) say(UI_RED, "Type a name first (letters and numbers).");
        else { sys_setting_set("user.name", nm); say(UI_GREEN, "Saved. New terminals will show it."); }
    }
    if (id == H_BTN + 31) check_updates();
    if (id == H_BTN + 30 && ustate == U_AVAILABLE) install_update();
    if (id == H_BTN + 32) sys_power(1);
    if (id == H_BTN + 21) confirm_disk = -1;
    if (id == H_BTN + 20 && confirm_disk >= 0) {
        int r = sys_install(disks[confirm_disk].id, confirm_mode);
        if (r) say(UI_RED, "The installer couldn't start.");
        else { installing = true; memset(&inst, 0, sizeof inst); inst.state = 1; }
        confirm_disk = -1;
    }
}

static void key(int k) {
    if (confirm_disk >= 0) { if (k == 27) confirm_disk = -1; return; }
    if (sect == S_TIME && (k == KEY_DOWN || k == KEY_UP || k == KEY_PGDN || k == KEY_PGUP)) {
        list_scroll += k == KEY_DOWN ? 1 : k == KEY_UP ? -1 : k == KEY_PGDN ? 9 : -9;
        if (list_scroll > nlist - 9) list_scroll = nlist - 9;
        if (list_scroll < 0) list_scroll = 0;
        return;
    }
    if (focus < 0) return;
    struct field *f = &fields[focus];
    if (k == '\b') { if (f->len) f->s[--f->len] = 0; }
    else if (k == '\t' && sect == S_LOCK) focus = focus == 0 ? 1 : 0;
    else if (k == '\n' && sect == S_LOCK) click(H_BTN + 0, 0);
    else if (k == '\n' && sect == S_PERSONAL) click(H_BTN + 50, 0);
    else if (k == 22 && !f->secret) clip_paste_line(f->s, &f->len, 41);
    else if (key_is_char(k) && f->len < 40) { f->s[f->len++] = (char)k; f->s[f->len] = 0; }
}

int main(void) {
    char arg[64] = ""; sys_getarg(arg, sizeof arg);
    if (!app_open(&a, WW, WH, "Settings")) return 1;
    reload();
    ubuild_now = sys_call(SYS_UPDATE, UPDATE_BUILD, 0, 0, 0, 0);
    int start = 0;
    for (int i = 0; i < NSECT; i++) {                        /* "open settings storage" etc. */
        char lower[24]; int k = 0; for (const char *p = sect_names[i]; *p && k < 23; p++) lower[k++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
        lower[k] = 0;
        if (arg[0] && !strncmp(lower, arg, strlen(arg))) start = i;
    }
    open_section(start);
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, installing ? 250 : 1000);
        if (r == 0) {
            if (installing) {
                sys_install_status(&inst);
                if (inst.state >= 2) {
                    installing = false; ndisks = sys_disks(disks, 8);
                    if (ustate == U_INSTALLING) { ustate = inst.state == 2 ? U_DONE : U_FAILED; strncpy0(umsg, inst.msg, sizeof umsg); }
                }
            }
            if (sect == S_TIME || installing || sect == S_STORAGE || sect == S_UPDATES) draw();
            continue;
        }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_MOVE ? hit_at(e.x, e.y) : -1;
            if (h != hover) { hover = h; draw(); }
            continue;
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) { int h = hit_at(e.x, e.y); if (h >= 0) click(h, e.x); else if (confirm_disk < 0) focus = sect == S_KEYB ? 2 : -1; }
        if (e.type == EV_WHEEL && sect == S_TIME) { list_scroll += e.key * 2; if (list_scroll > nlist - 9) list_scroll = nlist - 9; if (list_scroll < 0) list_scroll = 0; }
        if (e.type == EV_KEY) key(e.key);
        draw();
    }
    return 0;
}
