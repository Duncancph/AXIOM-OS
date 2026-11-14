/* Settings: keyboard, date & time, display, sound, lock screen, power, storage (install DuncOS), about. */
#include "../lib/dunc.h"

#define WW 820
#define WH 580
#define SIDE 190
static struct app a;
enum { S_KEYB, S_TIME, S_DISPLAY, S_SOUND, S_LOCK, S_POWER, S_STORAGE, S_ABOUT, NSECT };
static const char *sect_names[NSECT] = { "Keyboard", "Date & time", "Display", "Sound", "Lock screen", "Power", "Storage", "About" };
static int sect, hover = -1;

/* ---- clickable regions, rebuilt on every draw ---- */
struct hit { int x, y, w, h, id; };
static struct hit hits[96]; static int nhits;
static void add_hit(int x, int y, int w, int h, int id) { if (nhits < 96) hits[nhits++] = (struct hit){ x, y, w, h, id }; }
static int hit_at(int x, int y) { for (int i = nhits - 1; i >= 0; i--) if (ui_in(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return hits[i].id; return -1; }
enum { H_SECT = 0, H_LIST = 100, H_TOGGLE = 300, H_BTN = 400, H_FIELD = 500, H_DISK = 600 };

static char sv[16][112];                       /* cached setting values */
static const char *keys[16] = { "keyboard.layout", "time.zone", "time.rtc_local", "time.24h", "display.mode", "display.current",
                                "sound.volume", "lock.password", "lock.idle_min", "screen.off_min" };
static const char *get(const char *k) { for (int i = 0; i < 10; i++) if (!strcmp(keys[i], k)) return sv[i]; return ""; }
static void reload(void) { for (int i = 0; i < 10; i++) sys_setting_get(keys[i], sv[i], sizeof sv[i]); }
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
    gfx_rrect(c, x, y, 40, 22, 11, on ? UI_ACCENT : 0x2A3654, 255);
    gfx_circle(c, on ? x + 29 : x + 11, y + 11, 8, on ? HL_NAVY : UI_TEXT, 255);
    label(x + 52, y + 16, UI_TEXT, text);
    add_hit(x, y, 52 + gfx_text_width(&font_ui, text), 22, id);
}

static void option(int x, int y, int w, const char *text, bool sel, int id) {
    struct canvas *c = &a.c;
    if (sel) gfx_rrect(c, x, y, w, 30, 8, 0x16304A, 255);
    else if (hover == id) gfx_rrect(c, x, y, w, 30, 8, UI_PANEL2, 255);
    gfx_circle(c, x + 16, y + 15, 7, sel ? UI_ACCENT : 0x3A4766, 255);
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
        gfx_rrect(&a.c, x + w - 8, ty, 4, th, 2, 0x3A4766, 255);
    }
}

static const char *modes[] = { "", "1024x768", "1280x720", "1280x800", "1366x768", "1440x900", "1600x900", "1920x1080", "1920x1200", "2560x1440" };
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
    label(SIDE + 32, 316, UI_MUTED, "DuncOS picks this size if your screen and graphics card offer it,");
    label(SIDE + 32, 336, UI_MUTED, "otherwise the next best. It applies when DuncOS starts from a disk it is");
    label(SIDE + 32, 356, UI_MUTED, "installed on (Settings > Storage). Started from the USB stick image,");
    label(SIDE + 32, 376, UI_MUTED, "choose \"safe graphics\" in the start menu if the screen looks wrong.");
}

static void draw_sound(void) {
    heading("Sound", 0);
    struct sysinfo si; sys_sysinfo(&si);
    int vol = (int)katoi(get("sound.volume"));
    label(SIDE + 32, 100, UI_TEXT, "Volume");
    int x = SIDE + 32, y = 116, w = 400;
    gfx_rrect(&a.c, x, y + 8, w, 6, 3, 0x2A3654, 255);
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
    label(SIDE + 48, 364, UI_MUTED, "ACPI programs that DuncOS can't run yet. Plug laptops in.");
}

static struct dunc_disk disks[8]; static int ndisks, confirm_disk = -1, confirm_mode;
static struct dunc_install inst; static bool installing;
static void fmt_size(uint64_t b, char *o, int n) {
    if (b >= (10ull << 30)) ksnprintf(o, n, "%lu GB", b >> 30);
    else ksnprintf(o, n, "%lu MB", b >> 20);
}
static void draw_storage(void) {
    struct dunc_volume v[8]; int nv = sys_call(SYS_FS_VOLUMES, (long)v, 8, 0, 0, 0);
    bool persistent = nv > 0 && strcmp(v[0].kind, "Memory");
    heading("Storage", persistent ? "Your files are saved on a disk." : "DuncOS is running from memory: files are lost when you shut down.");
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
            if (d->duncos) button(WW - 32 - 150, y + 10, 134, "Update DuncOS", BTN_NORMAL, H_DISK + i * 2 + 1, false);
            if (!d->system) button(WW - 32 - (d->duncos ? 300 : 150), y + 10, 134, "Install here...", BTN_PRIMARY, H_DISK + i * 2, false);
        }
        y += h + 10;
    }
    if (!ndisks) label(SIDE + 32, 110, UI_MUTED, "No disks found. DuncOS supports SATA (AHCI), NVMe and USB drives.");
    if (installing || inst.state >= 2) {
        int py = WH - 96;
        gfx_rrect(&a.c, SIDE + 32, py, WW - SIDE - 64, 10, 5, 0x2A3654, 255);
        gfx_rrect(&a.c, SIDE + 32, py, (WW - SIDE - 64) * inst.percent / 100, 10, 5, inst.state == 3 ? UI_RED : UI_ACCENT, 255);
        label(SIDE + 32, py + 30, inst.state == 3 ? UI_RED : inst.state == 2 ? UI_GREEN : UI_TEXT, inst.msg);
    }
}

static void draw_about(void) {
    struct sysinfo s; sys_sysinfo(&s);
    heading("About DuncOS", "Made by Bryan Duncan. Running on the Thane kernel.");
    char b[128];
    int y = 110;
    ksnprintf(b, sizeof b, "Kernel: %s", s.kernel); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Processor: %s", s.cpu); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Memory: %lu MB, %lu MB in use", s.mem_total >> 20, s.mem_used >> 20); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Display: %s", get("display.current")); label(SIDE + 32, y, UI_TEXT, b); y += 26;
    ksnprintf(b, sizeof b, "Up for %lu minutes", s.uptime_ms / 60000); label(SIDE + 32, y, UI_TEXT, b); y += 40;
    label(SIDE + 32, y, UI_MUTED, "Fonts: GNU Unifont (GPLv2+ with font exception), Inter (SIL OFL 1.1)."); y += 22;
    label(SIDE + 32, y, UI_MUTED, "Trusted certificate authorities: Mozilla CA list (MPL 2.0).");
}

static void draw(void) {
    struct canvas *c = &a.c;
    nhits = 0;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_fill(c, 0, 0, SIDE, WH, 0x0C1426);
    for (int i = 0; i < NSECT; i++) {
        int y = 20 + i * 42;
        if (i == sect) { gfx_rrect(c, 12, y, SIDE - 24, 36, 10, 0x16304A, 255); gfx_fill(c, 12, y + 9, 3, 18, UI_ACCENT); }
        else if (hover == H_SECT + i) gfx_rrect(c, 12, y, SIDE - 24, 36, 10, UI_PANEL, 255);
        gfx_text(c, i == sect ? &font_ui_bold : &font_ui, 30, y + 23, i == sect ? UI_TEXT : HL_TEXT_DIM, sect_names[i]);
        add_hit(12, y, SIDE - 24, 36, H_SECT + i);
    }
    switch (sect) {
    case S_KEYB: draw_keyboard(); break; case S_TIME: draw_time(); break; case S_DISPLAY: draw_display(); break;
    case S_SOUND: draw_sound(); break; case S_LOCK: draw_lock(); break; case S_POWER: draw_power(); break;
    case S_STORAGE: draw_storage(); break; case S_ABOUT: draw_about(); break;
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
            gfx_text(c, &font_title, dx + 24, dy + 44, UI_TEXT, "Erase this disk and install DuncOS?");
            ksnprintf(b, sizeof b, "%s (%s)", d->model, sz); gfx_text(c, &font_ui_bold, dx + 24, dy + 76, UI_TEXT, b);
            label(dx + 24, dy + 102, UI_RED, "Everything on it will be deleted, including Windows or Linux");
            label(dx + 24, dy + 122, UI_RED, "if they are on this disk. This can't be undone.");
            label(dx + 24, dy + 148, UI_MUTED, "Your DuncOS files are copied onto it. Starting from it needs UEFI.");
        } else {
            gfx_text(c, &font_title, dx + 24, dy + 44, UI_TEXT, "Update DuncOS on this disk?");
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
}

static void click(int id, int mx) {
    if (id >= H_SECT && id < H_SECT + NSECT) { open_section(id - H_SECT); return; }
    if (id >= H_FIELD && id < H_FIELD + 4) { focus = id - H_FIELD; return; }
    focus = sect == S_KEYB ? 2 : -1;
    if (id >= H_LIST && id < H_LIST + 100) {
        int i = id - H_LIST;
        if (sect == S_KEYB && i < nlist) set("keyboard.layout", lid[i]);
        if (sect == S_TIME && i < nlist) set("time.zone", lid[i]);
        if (sect == S_DISPLAY) { set("display.mode", modes[i]); say(UI_MUTED, i ? "Saved. It takes effect the next time DuncOS starts." : "Saved: automatic."); }
        if (sect == S_LOCK) { static const char *v[] = { "0", "1", "5", "10", "30" }; set("lock.idle_min", v[i]); }
        if (sect == S_POWER) { static const char *v[] = { "0", "2", "5", "10", "30" }; set("screen.off_min", v[i]); }
        return;
    }
    if (id == H_TOGGLE + 0) set("time.24h", strcmp(get("time.24h"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 1) set("time.rtc_local", strcmp(get("time.rtc_local"), "1") ? "1" : "0");
    if (id == H_TOGGLE + 10) {
        int v = (mx - (SIDE + 32)) * 100 / 400; if (v < 0) v = 0; if (v > 100) v = 100;
        char b[8]; ksnprintf(b, sizeof b, "%d", v); set("sound.volume", b);
    }
    if (id == H_BTN + 10) { if (sys_call(SYS_SOUND, SOUND_TEST, 0, 0, 0, 0) < 0) say(UI_RED, "No sound card found (DuncOS supports Intel HD Audio)."); }
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
    else if (k == 22 && !f->secret) clip_paste_line(f->s, &f->len, 41);
    else if (key_is_char(k) && f->len < 40) { f->s[f->len++] = (char)k; f->s[f->len] = 0; }
}

int main(void) {
    char arg[64] = ""; sys_getarg(arg, sizeof arg);
    if (!app_open(&a, WW, WH, "Settings")) return 1;
    reload();
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
                if (inst.state >= 2) { installing = false; ndisks = sys_disks(disks, 8); }
            }
            if (sect == S_TIME || installing || sect == S_STORAGE) draw();
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
