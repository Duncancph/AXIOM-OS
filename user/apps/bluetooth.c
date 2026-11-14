/* Bluetooth settings: adapter details and a scan for nearby devices. */
#include "../lib/dunc.h"

static int WW = 540, WH = 560;
static struct app a;
static struct bt_adapter ad;
static struct bt_device devs[64];
static int ndev = -1, hover;
static bool scanning;

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
    }
    app_flush(&a);
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "Bluetooth", WIN_RESIZABLE)) return 1;
    sys_bt(BT_INFO, &ad, 0);
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }

        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_MOVE && ui_in(e.x, e.y, WW - 170, 252, 150, 36);
            if (h != hover) { hover = h; draw(); }
        }
        if (e.type == EV_MOUSE_UP && e.button == 1 && ad.present && ad.ok && ui_in(e.x, e.y, WW - 170, 252, 150, 36)) {
            scanning = true; draw();
            sys_bt(BT_SCAN, 0, 10);
            ndev = sys_bt(BT_RESULTS, devs, 64);
            scanning = false; draw();
        }
    }
    return 0;
}
