/* Network: connection status, addresses, live traffic and a connection test. */
#include "../lib/dunc.h"

#define WW 560
#define WH 600
#define HIST 60
static struct app a;
static struct dunc_netinfo ni;
static int hist_in[HIST], hist_out[HIST];
static uint64_t last_rx, last_tx, last_t;
static int hover = -1, dialog;
static char test[4][96]; static uint32_t test_col[4]; static int ntest;
static char input[32]; static int ilen;

static void icon(struct canvas *c, int cx, int cy, uint32_t col) {   /* three linked computers */
    gfx_rrect(c, cx - 7, cy - 16, 14, 10, 2, col, 255);
    gfx_rrect(c, cx - 21, cy + 6, 14, 10, 2, col, 255);
    gfx_rrect(c, cx + 7, cy + 6, 14, 10, 2, col, 255);
    gfx_line(c, cx, cy - 6, cx, cy, col, 2);
    gfx_line(c, cx - 14, cy, cx + 14, cy, col, 2);
    gfx_line(c, cx - 14, cy, cx - 14, cy + 6, col, 2);
    gfx_line(c, cx + 14, cy, cx + 14, cy + 6, col, 2);
}

static void kv(int x, int y, const char *k, const char *v) {
    gfx_text(&a.c, &font_ui, x, y, UI_MUTED, k);
    gfx_text(&a.c, &font_ui_bold, x + 120, y, UI_TEXT, v);
}

static void human(uint64_t b, char *o, int n) {
    if (b < 1024) ksnprintf(o, n, "%lu B", b);
    else if (b < 1048576) ksnprintf(o, n, "%lu.%lu KB", b / 1024, b % 1024 * 10 / 1024);
    else ksnprintf(o, n, "%lu.%lu MB", b >> 20, (b >> 10) % 1024 * 10 / 1024);
}

static void graph(struct canvas *c, int x, int y, int w, int h) {
    gfx_fill(c, x, y, w, h, HL_TERM_BG);
    int mx = 1024;
    for (int i = 0; i < HIST; i++) { if (hist_in[i] > mx) mx = hist_in[i]; if (hist_out[i] > mx) mx = hist_out[i]; }
    for (int s = 0; s < 2; s++) {
        int *hh = s ? hist_out : hist_in; uint32_t col = s ? HL_GOLD : HL_ACCENT;
        int px = -1, py = 0;
        for (int i = 0; i < HIST; i++) {
            int gx = x + i * (w - 1) / (HIST - 1), gy = y + h - 2 - hh[i] * (h - 4) / mx;
            if (px >= 0) gfx_line(c, px, py, gx, gy, col, 2);
            px = gx; py = gy;
        }
    }
    char m[32], hb[24]; human(mx, hb, sizeof hb); ksnprintf(m, sizeof m, "%s/s", hb);
    gfx_text(c, &font_ui, x + w - 8 - gfx_text_width(&font_ui, m), y + 16, UI_MUTED, m);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    bool ok = ni.present && ni.link && ni.configured;
    gfx_circle(c, 52, 50, 28, ok ? 0x0E7490 : 0x2A3142, 255);
    icon(c, 52, 50, 0xFFFFFF);
    gfx_text(c, &font_title, 96, 46, UI_TEXT, "Network");
    const char *st = !ni.present ? "No supported network adapter" : !ni.link ? "No cable connected" : !ni.configured ? "Getting an address..." : "Connected";
    gfx_text(c, &font_ui, 96, 68, ok ? UI_GREEN : UI_MUTED, st);

    if (!ni.present) {
        ui_card(c, 20, 104, WW - 40, 140);
        gfx_text(c, &font_ui_bold, 40, 136, UI_TEXT, "No wired network adapter detected");
        gfx_text(c, &font_ui, 40, 162, UI_MUTED, "DuncOS supports Intel (e1000, e1000e, I217-I219) and");
        gfx_text(c, &font_ui, 40, 182, UI_MUTED, "Realtek (RTL8111/8168) Ethernet. Wi-Fi isn't supported yet:");
        gfx_text(c, &font_ui, 40, 202, UI_MUTED, "plug in a network cable, or a USB Ethernet adapter later on.");
        app_flush(&a);
        return;
    }
    char b[64], ip[16], m[16], g[16], d[16];
    ip_str(ni.ip, ip); ip_str(ni.mask, m); ip_str(ni.gw, g); ip_str(ni.dns, d);
    ui_card(c, 20, 100, WW - 40, 202);
    kv(40, 128, "Adapter", ni.adapter);
    ksnprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    kv(40, 152, "MAC address", b);
    kv(40, 176, "IP address", ni.configured ? ip : "-");
    kv(40, 200, "Subnet mask", ni.configured ? m : "-");
    kv(40, 224, "Gateway", ni.configured ? g : "-");
    ksnprintf(b, sizeof b, "%s%s", ni.configured || ni.dns_manual ? d : "-", ni.dns_manual ? "  (set by you)" : "");
    kv(40, 248, "DNS server", b);
    ksnprintf(b, sizeof b, "%u min (DHCP)", ni.lease_s / 60);
    kv(40, 272, "Lease", ni.configured ? b : "-");

    ui_card(c, 20, 314, WW - 40, 132);
    char rx[24], tx[24];
    human(ni.rx_bytes, rx, sizeof rx); human(ni.tx_bytes, tx, sizeof tx);
    gfx_circle(c, 44, 337, 4, HL_ACCENT, 255);
    ksnprintf(b, sizeof b, "Received %s (%lu packets)", rx, ni.rx_packets);
    gfx_text(c, &font_ui, 54, 342, UI_TEXT, b);
    gfx_circle(c, 44, 359, 4, HL_GOLD, 255);
    ksnprintf(b, sizeof b, "Sent %s (%lu packets)", tx, ni.tx_packets);
    gfx_text(c, &font_ui, 54, 364, UI_TEXT, b);
    graph(c, 36, 374, WW - 72, 60);

    static const char *bl[] = { "Test connection", "Renew address", "DNS server..." };
    for (int i = 0; i < 3; i++) ui_button(c, 20 + i * 176, 458, 168, 34, bl[i], i == 0 ? BTN_PRIMARY : BTN_NORMAL, hover == i, !ni.link && i < 2);
    for (int i = 0; i < ntest; i++) gfx_text(c, &font_ui, 24, 518 + i * 20, test_col[i], test[i]);

    if (dialog) {
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 90);
        int dw = 380, dh = 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 12, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 12, HL_ACCENT, 120);
        gfx_mono_text(c, dx + 20, dy + 18, "DNS server", HL_TEXT);
        gfx_text(c, &font_ui, dx + 20, dy + 54, UI_MUTED, "An address like 1.1.1.1, or empty for automatic.");
        gfx_rrect(c, dx + 20, dy + 66, dw - 40, 34, 8, HL_TERM_BG, 255);
        gfx_rrect_outline(c, dx + 20, dy + 66, dw - 40, 34, 8, HL_ACCENT, 160);
        gfx_mono_text(c, dx + 32, dy + 75, input, HL_TEXT);
        gfx_fill(c, dx + 32 + ilen * 8, dy + 75, 8, 16, HL_ACCENT);
        ui_button(c, dx + dw - 116, dy + dh - 48, 96, 32, "OK", BTN_PRIMARY, hover == 10, false);
        ui_button(c, dx + dw - 222, dy + dh - 48, 96, 32, "Cancel", BTN_NORMAL, hover == 11, false);
    }
    app_flush(&a);
}

static void sample(void) {
    sys_netinfo(&ni);
    uint64_t now = sys_time();
    if (last_t) {
        uint64_t dt = now - last_t ? now - last_t : 1;
        memmove(hist_in, hist_in + 1, sizeof(int) * (HIST - 1));
        memmove(hist_out, hist_out + 1, sizeof(int) * (HIST - 1));
        hist_in[HIST - 1] = (int)((ni.rx_bytes - last_rx) * 1000 / dt);
        hist_out[HIST - 1] = (int)((ni.tx_bytes - last_tx) * 1000 / dt);
    }
    last_rx = ni.rx_bytes; last_tx = ni.tx_bytes; last_t = now;
}

static void line(uint32_t col, const char *fmt, ...) {
    if (ntest >= 4) return;
    va_list ap; va_start(ap, fmt); kvsnprintf(test[ntest], 96, fmt, ap); va_end(ap);
    test_col[ntest++] = col;
    draw();
}

static void run_test(void) {
    ntest = 0;
    if (!ni.configured) { line(UI_RED, "No address yet. Check the cable, then press Renew address."); return; }
    char g[16], d[16]; ip_str(ni.gw, g); ip_str(ni.dns, d);
    line(UI_MUTED, "Testing...");
    ntest = 0;
    long us = sys_ping(ni.gw, 2000);
    if (us >= 0) line(UI_GREEN, "Router %s answered in %ld.%ld ms", g, us / 1000, us / 100 % 10);
    else line(UI_RED, "Router %s didn't answer (it may block ping)", g);
    uint32_t ip;
    int r = sys_resolve("example.com", &ip);
    char x[16];
    if (r == 0) { ip_str(ip, x); line(UI_GREEN, "DNS %s works: example.com is %s", d, x); }
    else line(UI_RED, "DNS %s: %s", d, net_strerror(r));
    if (r == 0) {
        static char body[4096]; int status = 0;
        long n = http_get("http://example.com/", body, sizeof body, &status, 0, 0, 0, 0);
        if (n >= 0) line(UI_GREEN, "Web: http://example.com answered (HTTP %d)", status);
        else line(UI_RED, "Web: %s", http_strerror(n));
    }
}

static int hit(int x, int y) {
    if (dialog) {
        int dw = 380, dh = 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        if (ui_in(x, y, dx + dw - 116, dy + dh - 48, 96, 32)) return 10;
        if (ui_in(x, y, dx + dw - 222, dy + dh - 48, 96, 32)) return 11;
        return -1;
    }
    for (int i = 0; i < 3; i++) if (ui_in(x, y, 20 + i * 176, 458, 168, 34)) return i;
    return -1;
}

static void dns_ok(void) {
    uint32_t ip;
    if (!ilen) { sys_set_dns(0); ntest = 0; line(UI_MUTED, "DNS server: automatic (from the network)."); }
    else if (ip_parse(input, &ip)) { sys_set_dns(ip); ntest = 0; line(UI_MUTED, "DNS server set to %s.", input); }
    else { ntest = 0; line(UI_RED, "'%s' isn't an address like 1.1.1.1.", input); }
    dialog = 0;
}

int main(void) {
    if (!app_open(&a, WW, WH, "Network")) return 1;
    sample(); draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, 1000);
        if (r == 0) { sample(); draw(); continue; }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 0 && ni.link) run_test();
            else if (h == 1 && ni.link) { sys_net_renew(); ntest = 0; line(UI_MUTED, "Asking the network for a new address..."); }
            else if (h == 2) { dialog = 1; input[0] = 0; ilen = 0; if (ni.dns_manual) { ip_str(ni.dns, input); ilen = strlen(input); } }
            else if (h == 10) dns_ok();
            else if (h == 11) dialog = 0;
            draw();
        }
        if (e.type == EV_KEY && dialog) {
            int k = e.key;
            if (k == '\n') dns_ok();
            else if (k == 27) dialog = 0;
            else if (k == '\b') { if (ilen) input[--ilen] = 0; }
            else if (((k >= '0' && k <= '9') || k == '.') && ilen < 15) { input[ilen++] = k; input[ilen] = 0; }
            draw();
        }
    }
    return 0;
}
