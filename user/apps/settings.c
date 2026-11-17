/* Settings: keyboard, date & time, display, sound, lock screen, power, updates, storage (install AXIOM), about. */
#include "../lib/dunc.h"
#include "../lib/img/img.h"
#include "../lib/crypto/crypto.h"

#define WW 820
#define WH 580
#define SIDE 190
static struct app a;
enum { S_KEYB, S_MOUSE, S_TIME, S_DISPLAY, S_PERSONAL, S_SOUND, S_LOCK, S_POWER, S_UPDATES, S_APPS, S_STORAGE, S_DEVICES, S_ABOUT, NSECT };
static const char *sect_names[NSECT] = { "Keyboard", "Mouse", "Date & time", "Display", "Personalise", "Sound", "Lock screen", "Power", "Updates", "Apps", "Storage", "Devices", "About" };
static int sect, hover = -1;

/* ---- clickable regions, rebuilt on every draw ---- */
struct hit { int x, y, w, h, id; };
static struct hit hits[96]; static int nhits;
static void add_hit(int x, int y, int w, int h, int id) { if (nhits < 96) hits[nhits++] = (struct hit){ x, y, w, h, id }; }
static int hit_at(int x, int y) { for (int i = nhits - 1; i >= 0; i--) if (ui_in(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return hits[i].id; return -1; }
enum { H_SECT = 0, H_LIST = 100, H_TOGGLE = 300, H_BTN = 400, H_FIELD = 500, H_DISK = 600, H_PKG = 700 };

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

static const char *modes[] = { "", "1024x768", "1280x800", "1366x768", "1440x900", "1600x900", "1920x800", "1920x1080", "1920x1200", "2560x1440" };
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

static bool show_gfx, show_snd;
static void draw_report(const char *title, const char *sub, const char *key, int back_id, const char *none_text, const char *more);
static void draw_gfx_details(void) {
    draw_report("Graphics details", "How the firmware set up the screen, and what the monitor says about itself.", "display.report", H_BTN + 61,
                "No Intel graphics found.", "All of it: type  gfx  in the Terminal.");
}
static void draw_report(const char *title, const char *sub, const char *key, int back_id, const char *none_text, const char *more) {
    heading(title, sub);
    static char rep[3000]; sys_setting_get(key, rep, sizeof rep);
    int y = 106;
    for (char *p = rep; *p && y < WH - 70;) {
        char *e = p; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        int maxc = (WW - SIDE - 48) / 8, len = strlen(p);
        for (int o = 0; o < len && y < WH - 70; o += maxc) {   /* long lines wrap */
            char line[200]; int n = len - o < maxc ? len - o : maxc; if (n > 199) n = 199;
            memcpy(line, p + o, n); line[n] = 0;
            gfx_mono_text(&a.c, SIDE + 24 + (o ? 16 : 0), y - 12, line, o ? HL_TEXT_DIM : UI_TEXT); y += 16;
        }
        *e = save; p = *e ? e + 1 : e;
    }
    if (!rep[0]) label(SIDE + 32, y, UI_MUTED, none_text);
    button(SIDE + 24, WH - 56, 120, "Back", BTN_NORMAL, back_id, false);
    if (more) label(SIDE + 160, WH - 34, UI_MUTED, more);
}
static void draw_display(void) {
    char b[96];
    if (show_gfx) { draw_gfx_details(); return; }
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
    char gpu[160], mon[160]; sys_setting_get("display.gpu", gpu, sizeof gpu); sys_setting_get("display.monitor", mon, sizeof mon);
    if (gpu[0]) {
        ui_card(&a.c, SIDE + 32, 404, WW - SIDE - 64, 104);
        gfx_text(&a.c, &font_ui_bold, SIDE + 48, 432, UI_TEXT, gpu);
        button(WW - 32 - 130, 412, 114, "Details", BTN_NORMAL, H_BTN + 60, false);
        char m[180]; ksnprintf(m, sizeof m, "Monitor: %s", mon[0] ? mon : "it didn't say what it is");
        label(SIDE + 48, 470, UI_MUTED, m);
        char offer[48], nat[16]; sys_setting_get("display.offer", offer, sizeof offer); sys_setting_get("display.native", nat, sizeof nat);
        bool on = !strcmp(nat, "on") || !strcmp(nat, "asking");
        if (on) {
            ksnprintf(m, sizeof m, "AXIOM set the screen to the monitor's full size, %s.", offer);
            label(SIDE + 48, 490, UI_MUTED, m);
            button(WW - 32 - 280, 412, 140, "Turn off", BTN_NORMAL, H_BTN + 62, false);
        } else if (offer[0]) {
            ksnprintf(m, sizeof m, "The monitor can show %s. AXIOM can switch to it (new; it restarts).", offer);
            label(SIDE + 48, 490, UI_MUTED, m);
            char bl[40]; ksnprintf(bl, sizeof bl, "Use %s", offer);
            button(WW - 32 - 280, 412, 140, bl, BTN_PRIMARY, H_BTN + 63, false);
        } else label(SIDE + 48, 490, UI_MUTED, "This is the largest size AXIOM can show on this monitor.");
    }
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
    label(SIDE + 32, 236, UI_TEXT, "Play sound through");
    char cur[16] = "", want[16] = ""; sys_setting_get("sound.current", cur, sizeof cur); sys_setting_get("sound.output", want, sizeof want);
    if (!want[0]) strncpy0(want, "auto", sizeof want);
    option(SIDE + 32, 248, 420, "Automatic (the monitor when it's connected)", !strcmp(want, "auto"), H_LIST + 0);
    for (int i = 0; i < nlist && i < 3; i++) option(SIDE + 32, 284 + i * 36, 420, lname[i], !strcmp(want, lid[i]), H_LIST + 1 + i);
    int y2 = 284 + (nlist < 3 ? nlist : 3) * 36 + 20;
    label(SIDE + 32, y2, UI_MUTED, !cur[0] ? "No sound chip AXIOM can use was found." :
          !strcmp(cur, "monitor") ? "Now: the monitor's speakers, over HDMI (its own volume applies too)." : "Now: the headphone jack / the computer's speaker.");
    button(SIDE + 32, y2 + 24, 120, "Details", BTN_NORMAL, H_BTN + 11, false);
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
/* ---- More apps: packages too big for the system update (the Chromium-based Vertex browser) ----
 * Next to the update: packages/index.txt (one id per line) and packages/<id>.txt, a signed description:
 *   AXIOM package 1 / id / name / version / icon / description / min-build / size / sha256 / url /
 *   installed-size / run <program and arguments, relative to the install folder> / root yes (optional: unpack into
 *   the Linux folder itself, for a package that brings its own libraries) / signature <hex>
 * The archive (a tar) downloads straight to /system/linux/.downloads, resuming where it stopped (HTTP Range)
 * and hashing as it goes; once its SHA-256 matches, it unpacks into /system/linux/opt/<id> and the app gets a
 * line in /apps (it then opens from the dock and menu as a Linux program). */
#define MAXPKG 32
struct pkg { char id[24], name[32], version[16], icon[16], desc[100], url[240], run[160], sha[65]; long size, isize, minb;
             char have[16]; bool root; char req[24]; bool gz; };   /* have: the installed version, "" if none; root: unpacks into the Linux
                                               folder itself (lib/, usr/, opt/... the package brings what it needs) */
static struct pkg pkgs[MAXPKG]; static int npkg, pk_busy = -1, pk_state, pk_scroll; static bool pk_looked;
static char pk_msg[160]; static uint32_t pk_col;
static long pk_got, pk_total, pk_t0, pk_got0;
enum { PK_IDLE, PK_DOWNLOAD, PK_VERIFY, PK_UNPACK, PK_DONE, PK_FAILED };
static void pk_say(uint32_t col, const char *m) {
    strncpy0(pk_msg, m, sizeof pk_msg); pk_col = col;
    if (col == UI_RED && m[0]) { char l[300]; ksnprintf(l, sizeof l, "[apps] %s\n", m); sys_log(l); }   /* (the whole message, for the log) */
}
static void field_of(const char *t, long n, const char *name, char *out, int max) {
    out[0] = 0; int nl = (int)strlen(name);
    for (const char *p = t; p < t + n;) {
        const char *e = p; while (e < t + n && *e != '\n') e++;
        if (e - p > nl && !strncmp(p, name, nl) && p[nl] == ' ') { int k = (int)(e - p - nl - 1); if (k >= max) k = max - 1; memcpy(out, p + nl + 1, k); out[k] = 0; return; }
        p = e + 1;
    }
}
static void installed_version(const char *id, char *out, int max) {
    char path[64], line[512]; ksnprintf(path, sizeof path, "/apps/%s.info", id);
    long r = fs_read(path, line, 0, sizeof line - 1); out[0] = 0;
    if (r <= 0) return;
    line[r] = 0;
    int bars = 0; char *v = 0;
    for (char *q = line; *q; q++) if (*q == '|') { bars++; if (bars == 2) v = q + 1; else if (bars == 3) { *q = 0; break; } }
    if (v) strncpy0(out, v, max);
}
static void look_for_packages(void) {
    char url[120], base[240], u2[240]; sys_setting_get("update.url", url, sizeof url);
    pk_looked = true; npkg = 0;
    if (!url[0]) return;
    url_of(url, "packages/index.txt", base, sizeof base);
    struct buf b = { 0, 0, 0, 4096 }; int status = 0;
    struct http_req rq = { "GET", 0, 0, 0, 0, "Cache-Control: no-cache\r\n", 20000 };
    long r = http_request(base, &rq, buf_sink, &b, &status, 0, 0, 0, 0, 0);
    if (r < 0 || status != 200) { free(b.p); if (r < 0) pk_say(UI_RED, "Couldn't look for apps (no connection?)."); return; }
    for (char *p = b.p; p < b.p + b.n && npkg < MAXPKG;) {
        char *e = p; while (e < b.p + b.n && *e != '\n') e++;
        char id[24]; int k = 0; while (p + k < e && p[k] != '\r' && k < 23) { id[k] = p[k]; k++; } id[k] = 0;
        p = e + 1;
        if (!id[0] || id[0] == '#') continue;
        char rel[64]; ksnprintf(rel, sizeof rel, "packages/%s.txt", id); url_of(url, rel, u2, sizeof u2);
        struct buf m = { 0, 0, 0, 16384 }; status = 0;
        r = http_request(u2, &rq, buf_sink, &m, &status, 0, 0, 0, 0, 0);
        if (r >= 0 && status == 200 && sys_call(SYS_UPDATE, UPDATE_PACKAGE_VERIFY, (long)m.p, m.n, 0, 0) == 0) {
            struct pkg *q = &pkgs[npkg]; memset(q, 0, sizeof *q); char t[40];
            field_of(m.p, m.n, "id", q->id, sizeof q->id); field_of(m.p, m.n, "name", q->name, sizeof q->name);
            field_of(m.p, m.n, "version", q->version, sizeof q->version); field_of(m.p, m.n, "icon", q->icon, sizeof q->icon);
            field_of(m.p, m.n, "description", q->desc, sizeof q->desc); field_of(m.p, m.n, "url", q->url, sizeof q->url);
            field_of(m.p, m.n, "run", q->run, sizeof q->run); field_of(m.p, m.n, "sha256", q->sha, sizeof q->sha);
            field_of(m.p, m.n, "size", t, sizeof t); q->size = katoi(t);
            field_of(m.p, m.n, "installed-size", t, sizeof t); q->isize = katoi(t);
            field_of(m.p, m.n, "min-build", t, sizeof t); q->minb = katoi(t);
            field_of(m.p, m.n, "root", t, sizeof t); q->root = !strcmp(t, "yes");
            field_of(m.p, m.n, "requires", q->req, sizeof q->req);       /* another package to install first */
            field_of(m.p, m.n, "compression", t, sizeof t); q->gz = !strcmp(t, "gzip");
            bool ok = q->id[0] && q->url[0] && q->run[0] && strlen(q->sha) == 64 && q->size > 0;
            for (char *c = q->id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) ok = false;   /* (it becomes a folder name) */
            if (ok) { installed_version(q->id, q->have, sizeof q->have); npkg++; }
        } else if (r >= 0 && status == 200) pk_say(UI_RED, "A package description wasn't signed with the AXIOM key: it was skipped.");
        free(m.p);
    }
    free(b.p);
}

/* the download: straight to a file, hashing as it goes */
struct dl { const char *path; sha256_ctx h; char *buf; long n; long got; bool err; };
static bool dl_flush(struct dl *d) {
    if (!d->n) return true;
    if (fs_append(d->path, d->buf, d->n) != d->n) { d->err = true; return false; }
    d->n = 0; return true;
}
static bool dl_sink(const char *data, long n, void *ctx) {
    struct dl *d = ctx;
    sha256_update(&d->h, data, n);
    while (n) {
        long k = (1 << 20) - d->n; if (k > n) k = n;
        memcpy(d->buf + d->n, data, k); d->n += k; data += k; n -= k; d->got += k;
        if (d->n == (1 << 20) && !dl_flush(d)) return false;
    }
    pk_got = d->got;
    static uint64_t last; uint64_t now = sys_time();
    if (now - last > 250) { last = now; draw(); }
    return true;
}
static void mkdirs(const char *path) {                       /* every folder on the way to path (not path itself) */
    char p[300]; strncpy0(p, path, sizeof p);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; fs_mkdir(p); *q = '/'; }
}
static long oct(const char *s, int n) { long v = 0; for (int i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (s[i] - '0'); return v; }
/* unpack a tar (ustar/GNU; long names) into dir; returns files written or -1 */
static char untar_err[160];
static long untar(const char *tar, long tsize, const char *dir) {
    static char h[512], lname[256]; char *chunk = malloc(1 << 20);
    if (!chunk) return -1;
    long off = 0, files = 0; lname[0] = 0;
    while (off + 512 <= tsize) {
        long rr = fs_read(tar, h, off, 512);
        if (rr != 512) { ksnprintf(untar_err, sizeof untar_err, "reading the archive at %ld: %ld", off, rr); free(chunk); return -1; }
        off += 512;
        if (!h[0]) break;                                    /* the end */
        long size = oct(h + 124, 12); char type = h[156];
        char name[256];
        if (lname[0]) { strncpy0(name, lname, sizeof name); lname[0] = 0; }
        else {                                               /* ustar: an optional prefix (155) and the name (100) */
            int k = 0;
            if (!memcmp(h + 257, "ustar", 5) && h[345]) { while (k < 155 && h[345 + k]) { name[k] = h[345 + k]; k++; } name[k++] = '/'; }
            for (int j = 0; j < 100 && h[j]; j++) name[k++] = h[j];
            name[k] = 0;
        }
        if (type == 'L') {                                   /* GNU long name: the next entry's name */
            long k = size < 255 ? size : 255;
            if (fs_read(tar, lname, off, k) != k) { free(chunk); return -1; }
            lname[k] = 0;
            off += (size + 511) / 512 * 512; continue;
        }
        char *nm = name; while (nm[0] == '.' && nm[1] == '/') nm += 2;
        bool bad = !nm[0] || nm[0] == '/';
        for (char *q = nm; *q; q++) if (q[0] == '.' && q[1] == '.' && (q == nm || q[-1] == '/')) bad = true;   /* (no escaping the folder) */
        char out[300]; ksnprintf(out, sizeof out, "%s/%s", dir, nm);
        int ol = (int)strlen(out); if (ol && out[ol - 1] == '/') out[--ol] = 0;
        if (!bad && type == '5') { mkdirs(out); fs_mkdir(out); }
        else if (!bad && (type == '0' || type == 0)) {
            mkdirs(out);
            long w0 = fs_write(out, "", 0);
            if (w0 < 0) { ksnprintf(untar_err, sizeof untar_err, "creating %s: %ld", out, w0); free(chunk); return -1; }
            for (long done = 0; done < size;) {
                long k = size - done; if (k > (1 << 20)) k = 1 << 20;
                long r1 = fs_read(tar, chunk, off + done, k), r2 = r1 == k ? fs_append(out, chunk, k) : 0;
                if (r1 != k || r2 != k) { ksnprintf(untar_err, sizeof untar_err, "%s %s: %ld", r1 != k ? "reading for" : "writing", out, r1 != k ? r1 : r2); free(chunk); return -1; }
                done += k;
            }
            files++;
            if (files % 20 == 0) { char m[120]; ksnprintf(m, sizeof m, "Unpacking: %ld files...", files); pk_say(UI_MUTED, m); draw(); }
        }                                                    /* (links and devices are skipped) */
        off += (size + 511) / 512 * 512;
    }
    free(chunk);
    return files;
}
/* A gzip package archive is a series of gzip members (each at most GZ_MEMBER bytes once expanded, as
 * tools/make-package.py --gzip writes them: still a normal .tar.gz to gzip and tar). It's expanded a member at a
 * time into a plain tar next to it, which untar then reads. (The download's SHA-256 has been checked already.) */
#define GZ_MEMBER (1 << 20)
static void *gz_al(void *c, size_t n) { (void)c; return malloc(n); }
static void gz_fr(void *c, void *p) { (void)c; free(p); }
static long gz_expand(const char *gz, long gsize, const char *out) {
    long inmax = GZ_MEMBER + (GZ_MEMBER >> 3) + 4096;            /* a member that didn't compress, and its header */
    uint8_t *in = malloc(inmax), *ob = malloc(GZ_MEMBER);
    if (!in || !ob) { free(in); free(ob); ksnprintf(untar_err, sizeof untar_err, "not enough memory"); return -1; }
    struct img_alloc al = { 0, gz_al, gz_fr };
    long off = 0, total = 0;
    fs_write(out, "", 0);
    while (off < gsize) {
        long n = gsize - off; if (n > inmax) n = inmax;
        if (fs_read(gz, in, off, n) != n) { ksnprintf(untar_err, sizeof untar_err, "reading the archive at %ld", off); total = -1; break; }
        if (n < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { ksnprintf(untar_err, sizeof untar_err, "not gzip at %ld", off); total = -1; break; }
        long h = 10; uint8_t fl = in[3];
        if (fl & 4) h += 2 + (in[10] | in[11] << 8);               /* FEXTRA */
        if (fl & 8) { while (h < n && in[h]) h++; h++; }           /* FNAME */
        if (fl & 16) { while (h < n && in[h]) h++; h++; }          /* FCOMMENT */
        if (fl & 2) h += 2;                                        /* FHCRC */
        size_t used = 0;
        long k = h < n ? inflate_raw(in + h, (size_t)(n - h), ob, GZ_MEMBER, &used, &al) : -1;
        if (k < 0) { ksnprintf(untar_err, sizeof untar_err, "a damaged or too large gzip member at %ld", off); total = -1; break; }
        if (fs_append(out, ob, k) != k) { ksnprintf(untar_err, sizeof untar_err, "writing the archive (disk full?)"); total = -1; break; }
        total += k; off += h + (long)used + 8;                     /* (the trailer: CRC-32 and size) */
    }
    free(in); free(ob);
    return total;
}
static void pkg_install(int i) {
    struct pkg *q = &pkgs[i];
    if (q->minb > ubuild_now) { char m[120]; ksnprintf(m, sizeof m, "%s needs AXIOM build %ld or later: update AXIOM first.", q->name, q->minb); pk_say(UI_RED, m); return; }
    if (q->req[0]) {                                           /* what it needs first (e.g. GTK for a GTK app) */
        int j = -1; for (int k = 0; k < npkg; k++) if (!strcmp(pkgs[k].id, q->req)) j = k;
        if (j < 0) { char m[120]; ksnprintf(m, sizeof m, "%s needs %s, which isn't offered here.", q->name, q->req); pk_say(UI_RED, m); return; }
        if (!pkgs[j].have[0]) {
            char m[120]; ksnprintf(m, sizeof m, "Installing %s first (%s needs it)...", pkgs[j].name, q->name); pk_say(UI_MUTED, m); draw();
            pkg_install(j);
            if (!pkgs[j].have[0]) return;                       /* (it said why) */
        }
    }
    pk_busy = i; pk_state = PK_DOWNLOAD;
    char tmp[160], dir[120];
    fs_mkdir("/system/linux/.downloads");
    ksnprintf(tmp, sizeof tmp, "/system/linux/.downloads/%s-%s.tar%s", q->id, q->version, q->gz ? ".gz" : "");
    if (q->root) strncpy0(dir, "/system/linux", sizeof dir); else ksnprintf(dir, sizeof dir, "/system/linux/opt/%s", q->id);
    struct dl d; memset(&d, 0, sizeof d); d.path = tmp; d.buf = malloc(1 << 20);
    if (!d.buf) { pk_say(UI_RED, "Not enough memory to download."); pk_state = PK_FAILED; return; }
    sha256_init(&d.h);
    /* resume: hash what's already here */
    struct dunc_stat st; long have = fs_stat(tmp, &st) == 0 ? (long)st.size : -1;
    if (have > q->size) { fs_delete(tmp); have = -1; }
    if (have > 0) {
        pk_say(UI_MUTED, "Resuming the download..."); draw();
        for (long off = 0; off < have;) {
            long k = have - off; if (k > (1 << 20)) k = 1 << 20;
            if (fs_read(tmp, d.buf, off, k) != k) { have = -1; break; }
            sha256_update(&d.h, d.buf, k); off += k;
        }
        if (have < 0) { fs_delete(tmp); sha256_init(&d.h); }
    }
    if (have <= 0) { fs_write(tmp, "", 0); have = 0; }
    d.got = have; pk_got = have; pk_total = q->size; pk_t0 = (long)sys_time(); pk_got0 = have;
    if (have < q->size) {
        pk_say(UI_MUTED, "");
        char hdr[64]; ksnprintf(hdr, sizeof hdr, "Range: bytes=%ld-\r\n", have);
        struct http_req rq = { "GET", 0, 0, 0, 0, have ? hdr : 0, 30000 };
        int status = 0; long total = 0;
        long r = http_request(q->url, &rq, dl_sink, &d, &status, &total, 0, 0, 0, 0);
        if (have && status == 200) {                          /* the server sent it all again: start over */
            fs_write(tmp, "", 0); sha256_init(&d.h); d.n = 0; d.got = 0;
            r = http_request(q->url, &rq, dl_sink, &d, &status, &total, 0, 0, 0, 0);
        }
        dl_flush(&d);
        if (d.err) { free(d.buf); pk_state = PK_FAILED; pk_say(UI_RED, "Couldn't write the download (is the disk full?)."); pk_busy = -1; return; }
        if (r < 0 || (status != 200 && status != 206)) {
            free(d.buf); pk_state = PK_FAILED; pk_busy = -1;
            char m[160];
            if (r < 0) ksnprintf(m, sizeof m, "The download stopped: %s. Press Install again to carry on from %ld MB.", http_strerror(r), d.got >> 20);
            else ksnprintf(m, sizeof m, "The download failed: the server answered %d.", status);
            pk_say(UI_RED, m); return;
        }
    }
    free(d.buf);
    if (d.got < q->size) {                                     /* the connection dropped: keep what's here */
        pk_state = PK_FAILED; pk_busy = -1;
        char m[160]; ksnprintf(m, sizeof m, "The download stopped at %ld of %ld MB. Press Install to carry on from there.", d.got >> 20, q->size >> 20);
        pk_say(UI_RED, m); return;
    }
    pk_state = PK_VERIFY; pk_say(UI_MUTED, "Checking the download..."); draw();
    uint8_t h[32]; sha256_final(&d.h, h);
    char hx[65]; for (int k = 0; k < 32; k++) ksnprintf(hx + 2 * k, 3, "%02x", h[k]);
    if (d.got != q->size || strcmp(hx, q->sha)) {
        fs_delete(tmp); pk_state = PK_FAILED; pk_busy = -1;
        pk_say(UI_RED, "The download doesn't match the signed package (damaged?). It was deleted: press Install to try again.");
        return;
    }
    pk_state = PK_UNPACK; pk_say(UI_MUTED, "Unpacking..."); draw();
    fs_mkdir("/system/linux/opt"); fs_mkdir(dir);
    long tsize = q->size;
    if (q->gz) {
        char plain[160]; ksnprintf(plain, sizeof plain, "/system/linux/.downloads/%s-%s.tar", q->id, q->version);
        tsize = gz_expand(tmp, q->size, plain);
        if (tsize < 0) { fs_delete(plain); char m[200]; ksnprintf(m, sizeof m, "Couldn't unpack it: %s", untar_err); pk_state = PK_FAILED; pk_busy = -1; pk_say(UI_RED, m); return; }
        fs_delete(tmp); strncpy0(tmp, plain, sizeof tmp);
    }
    long files = untar(tmp, tsize, dir);
    if (files < 0) { char m[200]; ksnprintf(m, sizeof m, "Couldn't unpack it: %s", untar_err); pk_state = PK_FAILED; pk_busy = -1; pk_say(UI_RED, m); return; }
    fs_delete(tmp);
    /* the app's line in /apps: it runs as a Linux program from its folder */
    char info[512], ipath[64];
    ksnprintf(info, sizeof info, "%s|%s|%s|%s|%s|linux:%s/%s\n", q->id, q->name, q->version, q->icon[0] ? q->icon : "gear", q->desc, dir, q->run);
    ksnprintf(ipath, sizeof ipath, "/apps/%s.info", q->id);
    fs_write(ipath, info, strlen(info));
    sys_call(SYS_APPS, 0, -2, 0, 0, 0);                      /* the kernel reads /apps again */
    strncpy0(q->have, q->version, sizeof q->have);
    pk_state = PK_DONE; pk_busy = -1;
    char m[160]; ksnprintf(m, sizeof m, "%s %s is installed (%ld files). Open it from the dock or the AXIOM menu.", q->name, q->version, files);
    pk_say(UI_GREEN, m);
}
/* Settings > Apps: the signed packages, a row each (scrolls) */
#define PK_ROW 70
#define PK_TOP 100
static void pk_size(long b, char *o, int n) {             /* 1.2 MB / 840 KB */
    if (b < (1 << 20)) ksnprintf(o, n, "%ld KB", (b + 1023) >> 10);
    else ksnprintf(o, n, "%ld MB", (b + (1 << 19)) >> 20);
}
static int pk_rows(void) { return (WH - PK_TOP - 56) / PK_ROW; }
static void pk_fit(const struct font *f, const char *s, int maxw, char *out, int n) {   /* cut with "..." to fit */
    strncpy0(out, s, n);
    if (gfx_text_width(f, out) <= maxw) return;
    for (int k = (int)strlen(out); k > 0; k--) { out[k] = 0; strncpy0(out + k, "...", n - k); if (gfx_text_width(f, out) <= maxw) return; out[k] = 0; }
}
static void draw_apps(void) {
    heading("Apps", "More apps for AXIOM, signed like its updates. Linux desktop apps run in AXIOM windows.");
    int x = SIDE + 32, w = WW - SIDE - 64;
    char b[200], t[200];
    if (!pk_looked || !npkg) {
        label(x, PK_TOP + 20, pk_msg[0] ? pk_col : UI_MUTED, pk_msg[0] ? pk_msg : pk_looked ? "No apps have been published yet." : "Looking for apps...");
        if (pk_looked) button(x, PK_TOP + 40, 160, "Look again", BTN_NORMAL, H_BTN + 35, false);
        return;
    }
    int rows = pk_rows();
    if (pk_scroll > npkg - rows) pk_scroll = npkg - rows;
    if (pk_scroll < 0) pk_scroll = 0;
    for (int r = 0; r < rows && pk_scroll + r < npkg; r++) {
        int i = pk_scroll + r; struct pkg *q = &pkgs[i]; int cy = PK_TOP + r * PK_ROW;
        ui_card(&a.c, x, cy, w, PK_ROW - 8);
        ksnprintf(b, sizeof b, "%s %s", q->name, q->version);
        gfx_text(&a.c, &font_ui_bold, x + 16, cy + 24, UI_TEXT, b);
        char s1[24], s2[24]; pk_size(q->size, s1, sizeof s1); pk_size(q->isize ? q->isize : q->size, s2, sizeof s2);
        ksnprintf(t, sizeof t, "%s download, %s installed", s1, s2);
        label(x + 24 + gfx_text_width(&font_ui_bold, b), cy + 24, HL_TEXT_FAINT, t);
        bool busy = pk_busy == i;
        if (busy && pk_state == PK_DOWNLOAD) {
            long secs = ((long)sys_time() - pk_t0) / 1000; long rate = secs > 0 ? (pk_got - pk_got0) / secs : 0;
            int pw = w - 200, fill = pk_total ? (int)((int64_t)pw * pk_got / pk_total) : 0;
            gfx_rrect(&a.c, x + 16, cy + 38, pw, 8, 4, 0x2A2A28, 255); gfx_rrect(&a.c, x + 16, cy + 38, fill, 8, 4, UI_ACCENT, 255);
            ksnprintf(b, sizeof b, "%ld of %ld MB  (%ld KB/s)", pk_got >> 20, pk_total >> 20, rate >> 10);
            label(x + 24 + pw, cy + 47, UI_TEXT, b);
        } else {
            pk_fit(&font_ui, busy ? (pk_state == PK_VERIFY ? "Checking the download..." : "Unpacking...") : q->desc, w - 180, t, sizeof t);
            label(x + 16, cy + 46, busy ? UI_TEXT : UI_MUTED, t);
        }
        if (!busy) {
            bool same = q->have[0] && !strcmp(q->have, q->version);
            if (same) label(x + w - 112, cy + 36, UI_GREEN, "Installed");
            else button(x + w - 132, cy + 15, 116, q->have[0] ? "Update" : "Install", BTN_PRIMARY, H_PKG + i, pk_busy >= 0);
        }
    }
    if (npkg > rows) {                                         /* scroll bar */
        int th = rows * PK_ROW * rows / npkg, ty = PK_TOP + (rows * PK_ROW - th) * pk_scroll / (npkg - rows);
        gfx_rrect(&a.c, WW - 22, ty, 4, th, 2, 0x3A3A36, 255);
    }
    if (pk_msg[0]) { pk_fit(&font_ui, pk_msg, w, t, sizeof t); label(x, PK_TOP + rows * PK_ROW + 18, pk_col, t); }
}
static void draw_updates(void) {
    char b[200];
    struct sysinfo si; sys_sysinfo(&si);
    ksnprintf(b, sizeof b, "This is AXIOM %s, build %ld. Updates are signed; AXIOM checks them before installing.", strncmp(si.kernel, "Axiom Core ", 11) ? si.kernel : si.kernel + 11, ubuild_now);
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
    if (!installing && ustate != U_INSTALLING) {
        gfx_text(&a.c, &font_ui_bold, x, y + 200, UI_TEXT, "More apps");
        label(x, y + 224, UI_MUTED, "Apps that aren't part of AXIOM itself, like Vertex and Linux desktop apps.");
        button(x, y + 240, 160, "Open Apps", BTN_NORMAL, H_BTN + 37, false);
    }
    char url[120]; sys_setting_get("update.url", url, sizeof url);
    if (url[0]) { ksnprintf(b, sizeof b, "Source: %s", url); label(x, WH - 22, HL_TEXT_FAINT, b); }
}

static struct dunc_disk disks[8]; static int ndisks, confirm_disk = -1, confirm_mode;
static int inst_gb;                     /* erasing: GB for AXIOM (0: the whole disk; the rest is left empty for another system) */
static const int inst_sizes[] = { 0, 16, 32, 64, 128, 256 };
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

/* ---- Devices: everything AXIOM found and which driver looks after it ---- */
enum { H_DEV = 700 };                              /* + row index */
struct devrow { char *f[7]; };
static char devbuf[8192]; static struct devrow devs[96]; static int ndevs, dev_scroll, dev_sel = -1;
static void load_devices(void) {
    ndevs = 0; dev_scroll = 0; dev_sel = -1;
    if (sys_setting_get("devices.list", devbuf, sizeof devbuf) <= 0) return;
    for (char *l = devbuf; *l && ndevs < 96; ) {
        char *e = l; while (*e && *e != '\n') e++; if (*e) *e = 0; else e = 0;
        struct devrow *d = &devs[ndevs]; int nf = 0; d->f[nf++] = l;
        for (char *q = l; *q && nf < 7; q++) if (*q == '\t') { *q = 0; d->f[nf++] = q + 1; }
        if (nf == 7) ndevs++;
        if (!e) break;
        l = e + 1;
    }
}
static uint32_t dev_col(const char *st) {
    return !strcmp(st, "ok") ? UI_GREEN : !strcmp(st, "failed") ? UI_RED : !strcmp(st, "nodriver") ? UI_GOLD : UI_MUTED;
}
static const char *dev_status_text(const char *st) {
    return !strcmp(st, "ok") ? "Working" : !strcmp(st, "failed") ? "Didn't start" : !strcmp(st, "nodriver") ? "No driver" :
           !strcmp(st, "system") ? "Built in" : !strcmp(st, "off") ? "Turned off" : st;
}
/* one line of the list = a group heading or a device; returns how many lines there are */
static int dev_lines(int *idx, bool *head, int max) {
    int n = 0;
    for (int i = 0; i < ndevs && n < max; i++) {
        if (!i || strcmp(devs[i].f[0], devs[i - 1].f[0])) { idx[n] = i; head[n] = true; n++; if (n >= max) break; }
        idx[n] = i; head[n] = false; n++;
    }
    return n;
}
static void fit_text(char *out, int cap, const char *in, int maxw, const struct font *f) {
    strncpy0(out, in, cap);
    int len = strlen(out);
    while (len > 3 && gfx_text_width(f, out) > maxw) { len--; out[len] = 0; out[len - 1] = out[len - 2] = '.'; }
}
static void draw_dev_details(void) {
    struct devrow *d = &devs[dev_sel];
    heading(d->f[1], d->f[0]);
    int x = SIDE + 32, y = 100, w = WW - SIDE - 64;
    ui_card(&a.c, x, y, w, 250);
    gfx_circle(&a.c, x + 26, y + 30, 7, dev_col(d->f[3]), 255);
    gfx_text(&a.c, &font_ui_bold, x + 42, y + 35, UI_TEXT, dev_status_text(d->f[3]));
    const char *lab[4] = { "Driver", "Connected to", "Hardware ID", "" };
    const char *val[4] = { d->f[2][0] ? d->f[2] : (!strcmp(d->f[3], "system") ? "none needed" : "none"), d->f[4], d->f[5], 0 };
    int yy = y + 70;
    for (int i = 0; i < 3; i++) {
        if (!val[i] || !val[i][0]) continue;
        label(x + 20, yy, UI_MUTED, lab[i]); label(x + 160, yy, UI_TEXT, val[i]); yy += 26;
    }
    if (!strcmp(d->f[2], "built in") || !d->f[2][0]) {}
    else { label(x + 20, yy, UI_MUTED, "Where"); label(x + 160, yy, UI_TEXT, "Built into the Axiom Core kernel"); yy += 26; }
    /* the details, wrapped */
    yy += 8;
    const char *p = d->f[6]; int maxw = w - 40;
    while (*p && yy < y + 240) {
        char line[160]; int n = 0, cut = 0;
        while (p[n] && n < 159) {
            line[n] = p[n]; line[n + 1] = 0;
            if (gfx_text_width(&font_ui, line) > maxw) break;
            if (p[n] == ' ') cut = n;
            n++;
        }
        if (p[n] && cut) n = cut;
        memcpy(line, p, n); line[n] = 0;
        label(x + 20, yy, UI_TEXT, line); yy += 22;
        p += n; while (*p == ' ') p++;
    }
    button(SIDE + 32, WH - 64, 120, "Back", BTN_NORMAL, H_BTN + 70, false);
}
#define DEV_ROWS 15
static void draw_devices(void) {
    if (dev_sel >= 0 && dev_sel < ndevs) { draw_dev_details(); return; }
    int nok = 0, nbad = 0, nnone = 0;
    for (int i = 0; i < ndevs; i++) { const char *st = devs[i].f[3]; if (!strcmp(st, "ok")) nok++; else if (!strcmp(st, "failed")) nbad++; else if (!strcmp(st, "nodriver")) nnone++; }
    char sub[128];
    if (nbad) ksnprintf(sub, sizeof sub, "%d working, %d didn't start, %d without a driver. Click one for details.", nok, nbad, nnone);
    else ksnprintf(sub, sizeof sub, "%d working, %d without a driver. All drivers are built into the kernel.", nok, nnone);
    heading("Devices", sub);
    static int idx[200]; static bool head[200];
    int n = dev_lines(idx, head, 200);
    if (dev_scroll > n - DEV_ROWS) dev_scroll = n > DEV_ROWS ? n - DEV_ROWS : 0;
    if (dev_scroll < 0) dev_scroll = 0;
    int x = SIDE + 24, w = WW - SIDE - 48, y = 92;
    for (int k = dev_scroll; k < n && k < dev_scroll + DEV_ROWS; k++, y += 28) {
        struct devrow *d = &devs[idx[k]];
        if (head[k]) { gfx_text(&a.c, &font_ui_bold, x + 8, y + 20, UI_GOLD, d->f[0]); continue; }
        int id = H_DEV + idx[k];
        if (hover == id) gfx_rrect(&a.c, x, y + 1, w, 26, 7, UI_PANEL2, 255);
        gfx_circle(&a.c, x + 20, y + 14, 5, dev_col(d->f[3]), 255);
        char t[96]; fit_text(t, sizeof t, d->f[1], 330, &font_ui);
        label(x + 34, y + 19, UI_TEXT, t);
        fit_text(t, sizeof t, d->f[2][0] ? d->f[2] : "-", 110, &font_ui);
        label(x + 380, y + 19, UI_MUTED, t);
        label(x + 500, y + 19, dev_col(d->f[3]), dev_status_text(d->f[3]));
        add_hit(x, y + 1, w, 26, id);
    }
    if (!ndevs) label(SIDE + 32, 110, UI_MUTED, "Couldn't read the device list.");
    if (n > DEV_ROWS) {
        char m[64];
        ksnprintf(m, sizeof m, "Lines %d-%d of %d. Scroll for more.", dev_scroll + 1, dev_scroll + DEV_ROWS < n ? dev_scroll + DEV_ROWS : n, n);
        label(SIDE + 32, WH - 40, UI_MUTED, m);
    }
    label(SIDE + 360, WH - 40, UI_MUTED, "Also in CMD: devices");
}

static void draw_about(void) {
    struct sysinfo s; sys_sysinfo(&s);
    heading("About AXIOM", "Made by Bryan Duncan. Running on the Axiom Core kernel.");
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
    case S_SOUND: if (show_snd) draw_report("Sound details", "The sound chips AXIOM found and how each was set up.", "sound.report", H_BTN + 12, "Nothing found.", 0); else draw_sound(); break; case S_LOCK: draw_lock(); break; case S_POWER: draw_power(); break;
    case S_UPDATES: draw_updates(); break; case S_APPS: draw_apps(); break; case S_STORAGE: draw_storage(); break; case S_DEVICES: draw_devices(); break; case S_ABOUT: draw_about(); break;
    }
    if (msg[0]) label(SIDE + 32, WH - 24, msg_col, msg);
    if (confirm_disk >= 0) {                                 /* "erase this disk?" */
        struct dunc_disk *d = &disks[confirm_disk];
        nhits = 0;
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 110);
        int dw = 520, dh = confirm_mode == INSTALL_ERASE ? 290 : 230, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 14, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 14, confirm_mode == INSTALL_ERASE ? UI_RED : UI_ACCENT, 160);
        char b[128], sz[24]; fmt_size(d->bytes, sz, sizeof sz);
        if (confirm_mode == INSTALL_ERASE) {
            gfx_text(c, &font_title, dx + 24, dy + 44, UI_TEXT, "Erase this disk and install AXIOM?");
            ksnprintf(b, sizeof b, "%s (%s)", d->model, sz); gfx_text(c, &font_ui_bold, dx + 24, dy + 76, UI_TEXT, b);
            label(dx + 24, dy + 102, UI_RED, "Everything on it will be deleted, including Windows or Linux");
            label(dx + 24, dy + 122, UI_RED, "if they are on this disk. This can't be undone.");
            label(dx + 24, dy + 148, UI_MUTED, "Your AXIOM files are copied onto it. Starting from it needs UEFI.");
            /* how much of it AXIOM takes: the rest stays empty, for Linux or Windows next to it */
            char sb[96];
            if (!inst_gb) ksnprintf(sb, sizeof sb, "Space for AXIOM:  the whole disk");
            else ksnprintf(sb, sizeof sb, "Space for AXIOM:  %d GB  (the rest stays empty for another system)", inst_gb);
            label(dx + 24, dy + 184, UI_TEXT, sb);
            ui_button(c, dx + 24, dy + 196, 120, 30, "Change", BTN_NORMAL, hover == H_BTN + 22, false);
            add_hit(dx + 24, dy + 196, 120, 30, H_BTN + 22);
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
    if (s == S_SOUND) { load_list("sound.outputs"); show_snd = false; }
    if (s == S_TIME) {
        load_list("time.zones");
        for (int i = 0; i < nlist; i++) if (!strcmp(lid[i], get("time.zone"))) list_scroll = i > 4 ? i - 4 : 0;
        if (list_scroll > nlist - 9) list_scroll = nlist > 9 ? nlist - 9 : 0;
    }
    if (s == S_STORAGE) ndisks = sys_disks(disks, 8);
    if (s == S_DEVICES) load_devices();
    if (s == S_APPS && !pk_looked) { draw(); look_for_packages(); }   /* (shows "Looking..." meanwhile) */
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
        if (sect == S_SOUND) {
            if (i == 0) sys_setting_set("sound.output", "auto");
            else if (i - 1 < nlist) sys_setting_set("sound.output", lid[i - 1]);
            sys_call(SYS_SOUND, SOUND_TEST, 0, 0, 0, 0);
        }
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
    if (id >= H_DISK && id < H_DISK + 16) { confirm_disk = (id - H_DISK) / 2; confirm_mode = (id - H_DISK) % 2 ? INSTALL_UPDATE : INSTALL_ERASE; inst_gb = 0; }
    if (id == H_BTN + 40) sys_spawn("sandbox");
    if (id == H_BTN + 60) { show_gfx = true; }
    if (id == H_BTN + 62) { set("display.native", "off"); sys_power(1); }
    if (id == H_BTN + 63) { set("display.native", "try"); set("display.native_check", "0"); sys_power(1); }
    if (id == H_BTN + 61) { show_gfx = false; }
    if (id >= H_DEV && id < H_DEV + 96) { dev_sel = id - H_DEV; }
    if (id == H_BTN + 70) { dev_sel = -1; }
    if (id == H_BTN + 11) { show_snd = true; }
    if (id == H_BTN + 12) { show_snd = false; }
    if (id == H_TOGGLE + 40) set("system.cores", !strcmp(get("system.cores"), "all") ? "one" : "all");
    if (id == H_BTN + 50) {
        int k = 0; char nm[41];                                   /* letters, digits, - and _ only, lower case */
        for (int i = 0; fields[3].s[i] && k < 32; i++) { char c = fields[3].s[i]; if (c >= 'A' && c <= 'Z') c += 32; if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') nm[k++] = c; }
        nm[k] = 0;
        if (!k) say(UI_RED, "Type a name first (letters and numbers).");
        else { sys_setting_set("user.name", nm); say(UI_GREEN, "Saved. New terminals will show it."); }
    }
    if (id == H_BTN + 31) check_updates();
    if (id == H_BTN + 35) { pk_msg[0] = 0; look_for_packages(); }
    if (id >= H_PKG && id < H_PKG + npkg && pk_busy < 0) pkg_install(id - H_PKG);
    if (id == H_BTN + 37) { open_section(S_APPS); return; }
    if (id == H_BTN + 30 && ustate == U_AVAILABLE) install_update();
    if (id == H_BTN + 32) sys_power(1);
    if (id == H_BTN + 21) confirm_disk = -1;
    if (id == H_BTN + 22 && confirm_disk >= 0) {             /* next size that leaves room on this disk */
        uint64_t gbs = disks[confirm_disk].bytes >> 30;
        int k = 0; while (k < 6 && inst_sizes[k] != inst_gb) k++;
        for (int t = 1; t <= 6; t++) { int s = inst_sizes[(k + t) % 6]; if (!s || (uint64_t)s + 8 <= gbs) { inst_gb = s; break; } }
    }
    if (id == H_BTN + 20 && confirm_disk >= 0) {
        int r = sys_install_size(disks[confirm_disk].id, confirm_mode, confirm_mode == INSTALL_ERASE ? (long)inst_gb * 1024 : 0);
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
    if (sect == S_DEVICES) {
        if (k == 27 || k == '\b') dev_sel = -1;
        else if (k == KEY_DOWN || k == KEY_UP || k == KEY_PGDN || k == KEY_PGUP) dev_scroll += k == KEY_DOWN ? 1 : k == KEY_UP ? -1 : k == KEY_PGDN ? 12 : -12;
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
        if (e.type == EV_WHEEL && sect == S_DEVICES && dev_sel < 0) dev_scroll += e.key * 3;
        if (e.type == EV_WHEEL && sect == S_APPS) pk_scroll += e.key;
        if (e.type == EV_WHEEL && sect == S_TIME) { list_scroll += e.key * 2; if (list_scroll > nlist - 9) list_scroll = nlist - 9; if (list_scroll < 0) list_scroll = 0; }
        if (e.type == EV_KEY) key(e.key);
        draw();
    }
    return 0;
}
