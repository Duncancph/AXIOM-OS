/* Bluetooth settings: adapter details and a scan for nearby devices. */
#include "../lib/dunc.h"

static int WW = 560, WH = 600;
static struct app a;
static struct bt_adapter ad;
static struct bt_device devs[64];
static int ndev = -1, hover;
static bool scanning;
static struct bt_paired paired[16]; static int npaired;
static bool pairing; static struct bt_pair ps;
static int hover_id = -1;                  /* 100+i: Pair button of device i, 200+i: Forget, 300/301: dialog buttons */
static void addr_str(const uint8_t *a, char *o) { ksnprintf(o, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]); }
static bool is_paired(const uint8_t *a) { for (int i = 0; i < npaired; i++) if (!memcmp(paired[i].addr, a, 6)) return true; return false; }

static void rune(struct canvas *c, int cx, int cy, int s, uint32_t col, int t) {
    int k = s / 4, top = cy - s / 2, bot = cy + s / 2;
    gfx_line(c, cx, top, cx, bot, col, t);
    gfx_line(c, cx, top, cx + k, top + k, col, t);
    gfx_line(c, cx + k, top + k, cx - k, bot - k, col, t);
    gfx_line(c, cx - k, top + k, cx + k, bot - k, col, t);
    gfx_line(c, cx + k, bot - k, cx, bot, col, t);
}

static void bars(struct canvas *c, int x, int y, int rssi) {
    int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -80 ? 2 : rssi > -127 ? 1 : 0;
    for (int i = 0; i < 4; i++) gfx_rrect(c, x + i * 6, y + 12 - (i + 1) * 3, 4, (i + 1) * 3, 1, i < n ? UI_GREEN : 0x3A4256, 255);
}

static void kv(int x, int y, const char *k, const char *v) {
    gfx_text(&a.c, &font_ui, x, y, UI_MUTED, k);
    gfx_text(&a.c, &font_ui_bold, x + 90, y, UI_TEXT, v);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_circle(c, 52, 50, 28, ad.present ? 0x2563EB : 0x2A3142, 255);
    rune(c, 52, 50, 30, 0xFFFFFF, 3);
    gfx_text(c, &font_title, 96, 46, UI_TEXT, "Bluetooth");
    const char *status = !ad.present ? "No adapter found" : !ad.ok ? "Adapter not responding" : scanning ? "Scanning..." : "On";
    gfx_text(c, &font_ui, 96, 68, ad.present && ad.ok ? UI_GREEN : UI_MUTED, status);

    if (!ad.present) {
        ui_card(c, 20, 104, WW - 40, 150);
        gfx_text(c, &font_ui_bold, 40, 136, UI_TEXT, "No Bluetooth adapter detected");
        gfx_text(c, &font_ui, 40, 162, UI_MUTED, "DuncOS supports USB Bluetooth adapters. Plug in a USB");
        gfx_text(c, &font_ui, 40, 182, UI_MUTED, "Bluetooth dongle (Realtek, Broadcom or CSR chips work best)");
        gfx_text(c, &font_ui, 40, 202, UI_MUTED, "and restart. Built-in Intel Bluetooth needs firmware loading,");
        gfx_text(c, &font_ui, 40, 222, UI_MUTED, "which is coming in a later version.");
        app_flush(&a);
        return;
    }
    ui_card(c, 20, 104, WW - 40, 132);
    kv(40, 134, "Name", ad.name[0] ? ad.name : "(none)");
    kv(40, 158, "Chip", ad.maker);
    char b[64]; ksnprintf(b, sizeof b, "Bluetooth %s%s", ad.version, ad.le ? "  -  Low Energy" : "");
    kv(40, 182, "Version", b);
    kv(40, 206, "Address", ad.addr);
    ui_button(c, WW - 20 - 150, 252, 150, 36, scanning ? "Scanning..." : "Scan for devices", BTN_PRIMARY, hover, scanning || !ad.ok);
    gfx_text(c, &font_ui_bold, 22, 276, UI_TEXT, "Nearby devices");

    int y = 302;
    if (npaired) {                             /* remembered devices */
        gfx_text(c, &font_ui_bold, 22, y, UI_TEXT, "Paired devices"); y += 12;
        for (int i = 0; i < npaired && y + 40 < WH; i++, y += 42) {
            ui_card(c, 20, y, WW - 40, 36);
            char d[24]; addr_str(paired[i].addr, d);
            gfx_text(c, &font_ui_bold, 36, y + 23, UI_TEXT, paired[i].name[0] ? paired[i].name : d);
            gfx_text(c, &font_ui, 220, y + 23, UI_MUTED, d);
            ui_button(c, WW - 20 - 90, y + 4, 80, 28, "Forget", BTN_NORMAL, hover_id == 200 + i, false);
        }
        y += 18;
        gfx_text(c, &font_ui_bold, 22, y, UI_TEXT, "Nearby devices"); y += 26;
    }
    if (scanning) gfx_text(c, &font_ui, 22, y + 20, UI_MUTED, "Looking for Classic and Low Energy devices (10 seconds)...");
    else if (ndev < 0) gfx_text(c, &font_ui, 22, y + 20, UI_MUTED, "Press Scan to search for phones, headphones, watches and more.");
    else if (ndev == 0) gfx_text(c, &font_ui, 22, y + 20, UI_MUTED, "No devices found. Make sure they are discoverable.");
    for (int i = 0; i < ndev && y + 46 < WH; i++, y += 50) {
        ui_card(c, 20, y, WW - 40, 44);
        gfx_circle(c, 44, y + 22, 13, devs[i].le ? 0x0E7490 : 0x6D28D9, 255);
        gfx_text_center(c, &font_ui_bold, 31, 26, y + 27, 0xFFFFFF, devs[i].le ? "LE" : "BR");
        gfx_text(c, &font_ui_bold, 68, y + 20, UI_TEXT, devs[i].name[0] ? devs[i].name : "Unknown device");
        char d[64];
        ksnprintf(d, sizeof d, "%02X:%02X:%02X:%02X:%02X:%02X%s%s", devs[i].addr[5], devs[i].addr[4], devs[i].addr[3], devs[i].addr[2],
                  devs[i].addr[1], devs[i].addr[0], devs[i].kind[0] ? "  -  " : "", devs[i].kind);
        gfx_text(c, &font_ui, 68, y + 37, UI_MUTED, d);
        if (devs[i].rssi > -127) {
            char r[16]; ksnprintf(r, sizeof r, "%d dBm", devs[i].rssi);
            gfx_text(c, &font_ui, WW - 120, y + 28, UI_MUTED, r);
        }
        bars(c, WW - 56, y + 16, devs[i].rssi);
        if (!devs[i].le && !scanning)
            ui_button(c, WW - 200, y + 8, 64, 28, is_paired(devs[i].addr) ? "Again" : "Pair", BTN_PRIMARY, hover_id == 100 + i, pairing);
    }
    if (pairing) {                             /* the pairing dialog */
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 110);
        int dw = 440, dh = 220, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 14, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 14, ps.state == BTP_FAILED ? UI_RED : UI_ACCENT, 150);
        char t[96]; ksnprintf(t, sizeof t, "Pairing with %s", ps.name[0] ? ps.name : "device");
        gfx_text(c, &font_title, dx + 22, dy + 42, UI_TEXT, t);
        if (ps.state == BTP_CONFIRM || ps.state == BTP_TYPE_CODE) {
            char code[16]; ksnprintf(code, sizeof code, "%03u %03u", ps.code / 1000, ps.code % 1000);
            gfx_text_center(c, &font_big, dx, dw, dy + 112, UI_TEXT, code);
        }
        gfx_text(c, &font_ui, dx + 22, dy + 146, ps.state == BTP_FAILED ? UI_RED : ps.state == BTP_DONE ? UI_GREEN : UI_MUTED, ps.msg);
        if (ps.state == BTP_CONFIRM) {
            ui_button(c, dx + dw - 192, dy + dh - 50, 170, 34, "Yes, it matches", BTN_PRIMARY, hover_id == 300, false);
            ui_button(c, dx + dw - 300, dy + dh - 50, 96, 34, "No", BTN_NORMAL, hover_id == 301, false);
        } else if (ps.state == BTP_DONE || ps.state == BTP_FAILED)
            ui_button(c, dx + dw - 118, dy + dh - 50, 96, 34, "Close", BTN_PRIMARY, hover_id == 300, false);
    }
    app_flush(&a);
}

/* which button is at x, y (same layout as draw) */
static int hit(int x, int y0) {
    if (pairing) {
        int dw = 440, dh = 220, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        if (ps.state == BTP_CONFIRM) {
            if (ui_in(x, y0, dx + dw - 192, dy + dh - 50, 170, 34)) return 300;
            if (ui_in(x, y0, dx + dw - 300, dy + dh - 50, 96, 34)) return 301;
        } else if ((ps.state == BTP_DONE || ps.state == BTP_FAILED) && ui_in(x, y0, dx + dw - 118, dy + dh - 50, 96, 34)) return 300;
        return -1;
    }
    int y = 302;
    if (npaired) {
        y += 12;
        for (int i = 0; i < npaired && y + 40 < WH; i++, y += 42) if (ui_in(x, y0, WW - 110, y + 4, 80, 28)) return 200 + i;
        y += 18 + 26;
    }
    for (int i = 0; i < ndev && y + 46 < WH; i++, y += 50) if (!devs[i].le && ui_in(x, y0, WW - 200, y + 8, 64, 28)) return 100 + i;
    return -1;
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "Bluetooth", WIN_RESIZABLE)) return 1;
    sys_bt(BT_INFO, &ad, 0);
    npaired = sys_bt(BT_PAIRED, paired, 16); if (npaired < 0) npaired = 0;
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, pairing ? 250 : -1);
        if (r == 0) {                          /* pairing in progress: follow it */
            int was = ps.state;
            sys_bt(BT_PAIR_STATUS, &ps, 0);
            if (ps.state == BTP_DONE && was != BTP_DONE) { npaired = sys_bt(BT_PAIRED, paired, 16); if (npaired < 0) npaired = 0; }
            draw(); continue;
        }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE) { int h = hit(e.x, e.y); if (h != hover_id) { hover_id = h; draw(); } }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h >= 100 && h < 164 && h - 100 < ndev) {
                if (sys_bt(BT_PAIR, devs[h - 100].addr, 0) == 0) { pairing = true; memset(&ps, 0, sizeof ps); ps.state = BTP_CONNECTING; strncpy0(ps.name, devs[h - 100].name, sizeof ps.name); strncpy0(ps.msg, "Connecting...", sizeof ps.msg); }
            } else if (h >= 200 && h < 216) { sys_bt(BT_FORGET, paired[h - 200].addr, 0); npaired = sys_bt(BT_PAIRED, paired, 16); if (npaired < 0) npaired = 0; }
            else if (h == 300 && ps.state == BTP_CONFIRM) sys_bt(BT_PAIR_CONFIRM, 0, 1);
            else if (h == 301) sys_bt(BT_PAIR_CONFIRM, 0, 0);
            else if (h == 300) pairing = false;
            draw(); continue;
        }
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }

        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_MOVE && !pairing && ui_in(e.x, e.y, WW - 170, 252, 150, 36);
            if (h != hover) { hover = h; draw(); }
        }
        if (e.type == EV_MOUSE_UP && e.button == 1 && !pairing && ad.present && ad.ok && ui_in(e.x, e.y, WW - 170, 252, 150, 36)) {
            scanning = true; draw();
            sys_bt(BT_SCAN, 0, 10);
            ndev = sys_bt(BT_RESULTS, devs, 64);
            scanning = false; draw();
        }
    }
    return 0;
}
