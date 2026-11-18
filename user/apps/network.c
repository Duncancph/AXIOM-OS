/* Network: connection status, addresses, live traffic and a connection test (Wired), and Wi-Fi: the card, nearby
 * networks (signal, security) and joining one (Wi-Fi tab). */
#include "../lib/dunc.h"

#define WW 560
#define WH 650
#define HIST 60
static struct app a;
static struct dunc_netinfo ni;
static int hist_in[HIST], hist_out[HIST];
static uint64_t last_rx, last_tx, last_t;
static int hover = -1, dialog;
static char test[4][96]; static uint32_t test_col[4]; static int ntest;
static char input[64]; static int ilen;

/* Wi-Fi */
static int tab;                                   /* 0 wired, 1 Wi-Fi */
static struct wifi_status ws;
static struct wifi_net nets[32]; static int nnets, wscroll, join_i = -1;
static bool scanned, show_pw;
static char wmsg[160]; static uint32_t wmsg_col;
#define LIST_Y 208
#define ROW_H 44
#define LIST_H 308
static void wifi_draw(struct canvas *c);
static int wifi_hit(int x, int y);

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
    bool ok = ni.present && ni.link && (ni.configured || ni.v6_configured);
    gfx_circle(c, 52, 50, 28, ok ? 0x3C3B37 : 0x2A2A28, 255);
    icon(c, 52, 50, 0xFFFFFF);
    gfx_text(c, &font_title, 96, 46, UI_TEXT, "Network");
    ui_button(c, WW - 228, 30, 100, 30, "Wired", tab == 0 ? BTN_PRIMARY : BTN_NORMAL, hover == 40, false);
    ui_button(c, WW - 120, 30, 100, 30, "Wi-Fi", tab == 1 ? BTN_PRIMARY : BTN_NORMAL, hover == 41, false);
    if (tab == 1) { wifi_draw(c); app_flush(&a); return; }
    const char *st = !ni.present ? "No supported network adapter" : !ni.link ? "No cable connected" : !ok ? "Getting an address..." :
                     ni.configured && ni.v6_configured ? "Connected (IPv4 and IPv6)" : ni.configured ? "Connected (IPv4)" : "Connected (IPv6 only)";
    gfx_text(c, &font_ui, 96, 68, ok ? UI_GREEN : UI_MUTED, st);

    if (!ni.present) {
        ui_card(c, 20, 104, WW - 40, 140);
        gfx_text(c, &font_ui_bold, 40, 136, UI_TEXT, "No network adapter detected");
        gfx_text(c, &font_ui, 40, 162, UI_MUTED, "Plug in a network cable (Intel or Realtek Ethernet), a USB");
        gfx_text(c, &font_ui, 40, 182, UI_MUTED, "Ethernet adapter, or an Android phone with USB tethering on.");
        gfx_text(c, &font_ui, 40, 202, UI_MUTED, "For Wi-Fi, open the Wi-Fi tab above.");
        app_flush(&a);
        return;
    }
    char b[96], ip[16], m[16], g[16], d[IP_STRLEN];
    ip_str(ni.ip, ip); ip_str(ni.mask, m); ip_str(ni.gw, g);
    ui_card(c, 20, 100, WW - 40, 250);
    kv(40, 128, "Adapter", ni.adapter);
    ksnprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    kv(40, 152, "MAC address", b);
    ksnprintf(b, sizeof b, "%s / %s", ip, m);
    kv(40, 176, "IPv4 address", ni.configured ? b : "-");
    if (ni.lease_s == 0xFFFFFFFFu) ksnprintf(b, sizeof b, "%s  (permanent lease)", g);
    else ksnprintf(b, sizeof b, "%s  (lease %u min)", g, ni.lease_s / 60);
    kv(40, 200, "IPv4 gateway", ni.configured ? b : "-");
    int g6 = -1;                                    /* the global IPv6 address to show (a ready one first) */
    for (int i = 1; i < ni.v6_count; i++) if (g6 < 0 || (ni.v6_state[i] == V6_PREFERRED && ni.v6_state[g6] != V6_PREFERRED)) g6 = i;
    if (g6 > 0) { ip_format(&ni.v6[g6], d); ksnprintf(b, sizeof b, "%s/%u%s", d, ni.v6[g6].prefix, ni.v6_state[g6] == V6_TENTATIVE ? "  (checking)" : ""); }
    kv(40, 224, "IPv6 address", g6 > 0 ? b : ni.v6_count ? "- (no IPv6 router on this network)" : "-");
    if (ni.v6_count) { ip_format(&ni.v6[0], d); ksnprintf(b, sizeof b, "%s", d); }
    kv(40, 248, "Link-local", ni.v6_count ? b : "-");
    if (ni.router6.v6) { ip_format(&ni.router6, d); ksnprintf(b, sizeof b, "%s", d); }
    kv(40, 272, "IPv6 router", ni.router6.v6 ? b : "-");
    bool has_dns = ni.dns_now.v6 || ni.dns_now.b[0];
    if (has_dns) { ip_format(&ni.dns_now, d); ksnprintf(b, sizeof b, "%s%s", d, ni.dns_manual ? "  (set by you)" : ""); }
    kv(40, 296, "DNS server", has_dns ? b : "-");
    ksnprintf(b, sizeof b, "%u bytes", ni.mtu6);
    kv(40, 320, "IPv6 MTU", ni.router6.v6 ? b : "-");

    ui_card(c, 20, 362, WW - 40, 132);
    char rx[24], tx[24];
    human(ni.rx_bytes, rx, sizeof rx); human(ni.tx_bytes, tx, sizeof tx);
    gfx_circle(c, 44, 385, 4, HL_ACCENT, 255);
    ksnprintf(b, sizeof b, "Received %s (%lu packets)", rx, ni.rx_packets);
    gfx_text(c, &font_ui, 54, 390, UI_TEXT, b);
    gfx_circle(c, 44, 407, 4, HL_GOLD, 255);
    ksnprintf(b, sizeof b, "Sent %s (%lu packets)", tx, ni.tx_packets);
    gfx_text(c, &font_ui, 54, 412, UI_TEXT, b);
    graph(c, 36, 422, WW - 72, 60);

    static const char *bl[] = { "Test connection", "Renew address", "DNS server..." };
    for (int i = 0; i < 3; i++) ui_button(c, 20 + i * 176, 506, 168, 34, bl[i], i == 0 ? BTN_PRIMARY : BTN_NORMAL, hover == i, !ni.link && i < 2);
    for (int i = 0; i < ntest; i++) gfx_text(c, &font_ui, 24, 566 + i * 20, test_col[i], test[i]);

    if (dialog) {
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 90);
        int dw = 440, dh = 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 12, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 12, HL_ACCENT, 120);
        gfx_mono_text(c, dx + 20, dy + 18, "DNS server (empty: automatic)", HL_TEXT);
        gfx_text(c, &font_ui, dx + 20, dy + 54, UI_MUTED, "IPv4 or IPv6, e.g. 1.1.1.1 or 2606:4700:4700::1111.");
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

static bool strchr_c(const char *s, char c) { for (; *s; s++) if (*s == c) return true; return false; }

static void run_test(void) {
    ntest = 0;
    if (!ni.configured && !ni.v6_configured) { line(UI_RED, "No address yet. Check the cable, then press Renew address."); return; }
    line(UI_MUTED, "Testing...");
    ntest = 0;
    char g[IP_STRLEN], d[IP_STRLEN], x[IP_STRLEN];
    struct ipaddr rt = ni.configured ? ip_v4(ni.gw) : ni.router6;   /* the IPv4 gateway, or the IPv6 router */
    ip_format(&rt, g);
    long us = sys_ping(&rt, 2000);
    if (us >= 0) line(UI_GREEN, "Router %s answered in %ld.%ld ms", g, us / 1000, us / 100 % 10);
    else line(UI_RED, "Router %s didn't answer (it may block ping)", g);
    ip_format(&ni.dns_now, d);
    struct ipaddr r6[8];
    int r = sys_resolve("example.com", r6, 8);
    if (r > 0) {
        int k4 = 0, k6 = 0; for (int i = 0; i < r; i++) { if (r6[i].v6) k6++; else k4++; }
        ip_format(&r6[0], x);
        line(UI_GREEN, "DNS works: example.com is %s%s", x, k4 && k6 ? " (+ IPv4)" : "");
    } else line(UI_RED, "DNS %s: %s", d, net_strerror(r ? r : NET_ENOTFOUND));
    if (r > 0) {
        static char body[4096]; int status = 0;
        long n = http_get("http://example.com/", body, sizeof body, &status, 0, 0, 0, 0);
        if (n >= 0) line(UI_GREEN, "Web: example.com answered over %s (HTTP %d)", strchr_c(http_sec.addr, ':') ? "IPv6" : "IPv4", status);
        else line(UI_RED, "Web: %s", http_strerror(n));
    }
}

static int hit(int x, int y) {
    if (!dialog && ui_in(x, y, WW - 228, 30, 100, 30)) return 40;
    if (!dialog && ui_in(x, y, WW - 120, 30, 100, 30)) return 41;
    if (tab == 1) return wifi_hit(x, y);
    if (dialog) {
        int dw = 440, dh = 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        if (ui_in(x, y, dx + dw - 116, dy + dh - 48, 96, 32)) return 10;
        if (ui_in(x, y, dx + dw - 222, dy + dh - 48, 96, 32)) return 11;
        return -1;
    }
    for (int i = 0; i < 3; i++) if (ui_in(x, y, 20 + i * 176, 506, 168, 34)) return i;
    return -1;
}

static void dns_ok(void) {
    struct ipaddr ip;
    if (!ilen) { sys_set_dns(0); ntest = 0; line(UI_MUTED, "DNS server: automatic (from the network)."); }
    else if (ip_parse(input, &ip)) { sys_set_dns(&ip); ntest = 0; line(UI_MUTED, "DNS server set to %s.", input); }
    else { ntest = 0; line(UI_RED, "'%s' isn't an IPv4 or IPv6 address.", input); }
    dialog = 0;
}

/* ---------------- Wi-Fi ---------------- */
static const char *sec_name[] = { "Open", "WEP", "WPA", "WPA2", "WPA3" };
static int bars(int dbm) { return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : dbm >= -85 ? 1 : 0; }
static void draw_bars(struct canvas *c, int x, int y, int n, uint32_t on) {      /* y = bottom */
    for (int i = 0; i < 4; i++) { int h = 5 + i * 4; gfx_rrect(c, x + i * 6, y - h, 4, h, 1, i < n ? on : 0x3A3A38, 255); }
}
static void draw_lock(struct canvas *c, int x, int y, uint32_t col) {           /* y = top */
    gfx_rrect_outline(c, x + 2, y, 8, 9, 3, col, 255);
    gfx_rrect(c, x, y + 5, 12, 9, 2, col, 255);
}
static void wmsg_set(uint32_t col, const char *t) { strncpy0(wmsg, t, sizeof wmsg); wmsg_col = col; }
static int last_state = -1;
static void wifi_refresh(void) {
    sys_wifi(WIFI_STATUS, &ws, 0);
    /* while joining, and when it ends, the card's own words show (authenticating..., connected, why not) */
    if ((ws.state == WIFI_CONNECTING || ws.state == WIFI_CONNECTED || last_state == WIFI_CONNECTING) && ws.msg[0])
        wmsg_set(ws.state == WIFI_CONNECTED ? UI_GREEN : ws.state == WIFI_CONNECTING ? HL_GOLD : UI_RED, ws.msg);
    last_state = ws.state;
}
static void wifi_results(void) { long n = sys_wifi(WIFI_RESULTS, nets, 32); nnets = n > 0 ? (int)n : 0; scanned = true; }
static int rows_vis(void) { return LIST_H / ROW_H; }

static void wrap_text(struct canvas *c, int x, int y, int w, uint32_t col, const char *t) {   /* up to 3 lines */
    char line[160] = "", word[160]; int lines = 0;
    while (*t && lines < 3) {
        int k = 0; while (t[k] && t[k] != ' ' && k < 159) { word[k] = t[k]; k++; } word[k] = 0;
        char trial[330]; ksnprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && gfx_text_width(&font_ui, trial) > w) { gfx_text(c, &font_ui, x, y + lines++ * 20, col, line); strncpy0(line, word, sizeof line); }
        else strncpy0(line, trial, sizeof line);
        t += k; while (*t == ' ') t++;
    }
    if (line[0] && lines < 3) gfx_text(c, &font_ui, x, y + lines * 20, col, line);
}
static void wifi_draw(struct canvas *c) {
    const char *st = ws.state == WIFI_NONE ? "No Wi-Fi card found" : ws.state == WIFI_OFF ? "Wi-Fi card found, not started" :
                     ws.state == WIFI_FAILED ? "The Wi-Fi card didn't start" : ws.state == WIFI_CONNECTED ? "Connected" :
                     ws.state == WIFI_CONNECTING ? "Connecting..." : "Not connected";
    gfx_text(c, &font_ui, 96, 68, ws.state == WIFI_CONNECTED ? UI_GREEN : ws.state == WIFI_FAILED ? UI_RED : UI_MUTED, st);
    if (ws.state == WIFI_NONE) {
        ui_card(c, 20, 104, WW - 40, 150);
        gfx_text(c, &font_ui_bold, 40, 136, UI_TEXT, "No supported Wi-Fi card");
        gfx_text(c, &font_ui, 40, 162, UI_MUTED, "AXIOM is learning its first Wi-Fi card, the Realtek RTL8821AE.");
        gfx_text(c, &font_ui, 40, 182, UI_MUTED, "Until yours is supported, use a network cable, a USB Ethernet");
        gfx_text(c, &font_ui, 40, 202, UI_MUTED, "adapter, or an Android phone with USB tethering.");
        return;
    }
    char b[160];
    ui_card(c, 20, 100, WW - 40, 96);
    ksnprintf(b, sizeof b, "%s", ws.adapter);
    kv(40, 128, "Adapter", b);
    if (ws.state == WIFI_OFF) kv(40, 152, "MAC address", "- (read when the card starts)");
    else { ksnprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", ws.mac[0], ws.mac[1], ws.mac[2], ws.mac[3], ws.mac[4], ws.mac[5]); kv(40, 152, "MAC address", b); }
    kv(40, 176, "Saved network", ws.saved[0] ? ws.saved : "-");
    if (ws.state == WIFI_OFF) ui_button(c, WW - 196, 116, 156, 32, "Start the card", BTN_PRIMARY, hover == 50, false);
    else ui_button(c, WW - 136, 116, 96, 32, "Scan", BTN_PRIMARY, hover == 51, ws.state == WIFI_FAILED);
    if (ws.saved[0]) ui_button(c, WW - 136, 156, 96, 28, "Forget", BTN_NORMAL, hover == 52, false);

    /* the networks */
    gfx_fill(c, 20, LIST_Y, WW - 40, LIST_H, HL_TERM_BG);
    if (!scanned) gfx_text(c, &font_ui, 40, LIST_Y + 34, UI_MUTED, ws.state == WIFI_OFF ? "Start the card, then press Scan." : "Press Scan to look for networks.");
    else if (!nnets) gfx_text(c, &font_ui, 40, LIST_Y + 34, UI_MUTED, "No networks found.");
    gfx_clip(c, 20, LIST_Y, WW - 40, LIST_H);
    for (int i = 0; i < nnets; i++) {
        int y = LIST_Y + i * ROW_H - wscroll;
        if (y + ROW_H < LIST_Y || y > LIST_Y + LIST_H) continue;
        struct wifi_net *n = &nets[i];
        if (hover == 100 + i) gfx_rrect(c, 24, y + 2, WW - 48, ROW_H - 4, 8, 0x2A2A28, 255);
        draw_bars(c, 36, y + 32, bars(n->dbm), n->dbm >= -67 ? UI_GREEN : n->dbm >= -80 ? HL_GOLD : UI_RED);
        gfx_text(c, n->ssid[0] ? &font_ui_bold : &font_ui, 76, y + 22, n->ssid[0] ? UI_TEXT : UI_MUTED, n->ssid[0] ? n->ssid : "(hidden network)");
        ksnprintf(b, sizeof b, "%s  -  channel %d  -  %d dBm", sec_name[n->sec], n->ch, n->dbm);
        gfx_text(c, &font_ui, 76, y + 38, UI_MUTED, b);
        if (n->sec != WSEC_OPEN) draw_lock(c, WW - 62, y + 14, n->sec == WSEC_WEP ? UI_RED : UI_MUTED);
        if (n->saved) { gfx_rrect(c, WW - 130, y + 13, 56, 20, 6, HL_ACCENT, 60); gfx_text(c, &font_ui, WW - 120, y + 28, HL_ACCENT, "Saved"); }
    }
    gfx_noclip(c);
    if (nnets > rows_vis()) {                                     /* scroll bar */
        int th = LIST_H * rows_vis() / nnets, ty = LIST_Y + (LIST_H - th) * wscroll / ((nnets - rows_vis()) * ROW_H);
        gfx_rrect(c, WW - 28, ty, 4, th, 2, 0x55554F, 255);
    }
    if (wmsg[0]) wrap_text(c, 24, LIST_Y + LIST_H + 26, WW - 48, wmsg_col, wmsg);
    else if (ws.msg[0]) wrap_text(c, 24, LIST_Y + LIST_H + 26, WW - 48, ws.state == WIFI_FAILED ? UI_RED : UI_MUTED, ws.msg);
    gfx_text(c, &font_ui, 24, WH - 22, UI_MUTED, ws.simulated ? "Simulated card ('wifi simulate' in CMD): for trying this screen." : "Wi-Fi is being added: scanning is in testing, joining comes next.");

    if (dialog == 2 && join_i >= 0) {                              /* join a network */
        struct wifi_net *n = &nets[join_i];
        bool need_pw = n->sec == WSEC_WPA2 || n->sec == WSEC_WPA || n->sec == WSEC_WPA3, bad = n->sec == WSEC_WEP;
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 90);
        int dw = 460, dh = need_pw ? 210 : 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 12, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 12, HL_ACCENT, 120);
        ksnprintf(b, sizeof b, "Connect to %s", n->ssid);
        gfx_text(c, &font_ui_bold, dx + 20, dy + 32, UI_TEXT, b);
        if (bad) gfx_text(c, &font_ui, dx + 20, dy + 62, UI_RED, "WEP isn't supported: it can be broken in minutes.");
        else if (!need_pw) {
            gfx_text(c, &font_ui, dx + 20, dy + 62, UI_MUTED, "This network is open: people nearby can see");
            gfx_text(c, &font_ui, dx + 20, dy + 82, UI_MUTED, "what you send that isn't encrypted (https is).");
        } else {
            ksnprintf(b, sizeof b, "Password (%s)", sec_name[n->sec]);
            gfx_text(c, &font_ui, dx + 20, dy + 62, UI_MUTED, b);
            gfx_rrect(c, dx + 20, dy + 74, dw - 40, 34, 8, HL_TERM_BG, 255);
            gfx_rrect_outline(c, dx + 20, dy + 74, dw - 40, 34, 8, HL_ACCENT, 160);
            char shown[64]; int k = 0;
            int first = ilen > 46 ? ilen - 46 : 0;
            for (int i = first; i < ilen; i++) shown[k++] = show_pw ? input[i] : '*';
            shown[k] = 0;
            gfx_mono_text(c, dx + 32, dy + 83, shown, HL_TEXT);
            gfx_fill(c, dx + 32 + k * 8, dy + 83, 8, 16, HL_ACCENT);
            ui_button(c, dx + 20, dy + 120, 140, 28, show_pw ? "Hide password" : "Show password", BTN_NORMAL, hover == 21, false);
            if (n->sec == WSEC_WPA3) gfx_text(c, &font_ui, dx + 176, dy + 140, HL_GOLD, "WPA3-only isn't supported yet");
        }
        ui_button(c, dx + dw - 116, dy + dh - 48, 96, 32, "Connect", BTN_PRIMARY, hover == 10, bad || (need_pw && ilen < 8));
        ui_button(c, dx + dw - 222, dy + dh - 48, 96, 32, "Cancel", BTN_NORMAL, hover == 11, false);
    }
}
static int wifi_hit(int x, int y) {
    if (dialog == 2 && join_i >= 0) {
        struct wifi_net *n = &nets[join_i];
        bool need_pw = n->sec == WSEC_WPA2 || n->sec == WSEC_WPA || n->sec == WSEC_WPA3;
        int dw = 460, dh = need_pw ? 210 : 170, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        if (ui_in(x, y, dx + dw - 116, dy + dh - 48, 96, 32)) return 10;
        if (ui_in(x, y, dx + dw - 222, dy + dh - 48, 96, 32)) return 11;
        if (need_pw && ui_in(x, y, dx + 20, dy + 120, 140, 28)) return 21;
        return -1;
    }
    if (ws.state == WIFI_OFF && ui_in(x, y, WW - 196, 116, 156, 32)) return 50;
    if (ws.state != WIFI_OFF && ws.state != WIFI_NONE && ui_in(x, y, WW - 136, 116, 96, 32)) return 51;
    if (ws.saved[0] && ui_in(x, y, WW - 136, 156, 96, 28)) return 52;
    if (x >= 20 && x < WW - 20 && y >= LIST_Y && y < LIST_Y + LIST_H) {
        int i = (y - LIST_Y + wscroll) / ROW_H;
        if (i >= 0 && i < nnets) return 100 + i;
    }
    return -1;
}
static void wifi_scan_now(void) {
    wmsg_set(UI_MUTED, "Scanning channels 1-13..."); draw();
    long r = sys_wifi(WIFI_SCAN, 0, 0);
    wifi_refresh();
    if (r < 0) { wmsg_set(UI_RED, "The card isn't ready to scan."); return; }
    wifi_results(); wscroll = 0;
    char m[80]; ksnprintf(m, sizeof m, "%d network%s found.", nnets, nnets == 1 ? "" : "s");
    wmsg_set(UI_MUTED, m);
}
static void wifi_join(void) {
    struct wifi_net *n = &nets[join_i];
    bool need_pw = (n->sec == WSEC_WPA2 || n->sec == WSEC_WPA || n->sec == WSEC_WPA3) && !(n->saved && !ilen);
    if (n->sec == WSEC_WEP || (need_pw && ilen < 8)) return;
    struct wifi_join j; memset(&j, 0, sizeof j);
    strncpy0(j.ssid, n->ssid, sizeof j.ssid); memcpy(j.bssid, n->bssid, 6);
    if (need_pw) strncpy0(j.pass, input, sizeof j.pass);
    memset(input, 0, sizeof input); ilen = 0;
    dialog = 0;
    wmsg_set(UI_MUTED, "Saving..."); draw();
    long r = sys_wifi(WIFI_CONNECT, &j, 0);
    memset(&j, 0, sizeof j);
    wifi_refresh(); wifi_results();
    wmsg_set(r < 0 ? UI_RED : r == 0 ? UI_GREEN : HL_GOLD, ws.msg[0] ? ws.msg : r < 0 ? "Couldn't connect." : "Joining...");
}
static void wifi_click(int h) {
    if (h == 50) {
        wmsg_set(UI_MUTED, "Starting the card (up to ~10 seconds)..."); draw();
        long r = sys_wifi(WIFI_START, 0, 0);
        wifi_refresh();
        wmsg_set(r < 0 ? UI_RED : UI_GREEN, r < 0 ? "The card didn't start. Details: 'wifi' in CMD." : "The card is ready. Press Scan.");
    } else if (h == 51) wifi_scan_now();
    else if (h == 52) { sys_wifi(WIFI_FORGET, 0, 0); wifi_refresh(); wifi_results(); wmsg_set(UI_MUTED, "Forgotten."); }
    else if (h >= 100 && h < 100 + nnets) {
        if (!nets[h - 100].ssid[0]) { wmsg_set(UI_MUTED, "Hidden networks need their name typed in (coming later)."); return; }
        join_i = h - 100; memset(input, 0, sizeof input); ilen = 0; show_pw = false;
        if (nets[join_i].saved) { wifi_join(); return; }                 /* saved: its key is kept, no password needed */
        dialog = 2;
    } else if (h == 21) show_pw = !show_pw;
    else if (h == 10 && dialog == 2) wifi_join();
    else if (h == 11) { dialog = 0; memset(input, 0, sizeof input); ilen = 0; }
}

int main(void) {
    if (!app_open(&a, WW, WH, "Network")) return 1;
    sample(); wifi_refresh();
    if (!ni.present && ws.state != WIFI_NONE) tab = 1;            /* no cable adapter: start on Wi-Fi */
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, 1000);
        if (r == 0) { sample(); if (tab == 1 && !dialog) wifi_refresh(); draw(); continue; }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 40 || h == 41) { tab = h - 40; hover = -1; if (tab == 1) wifi_refresh(); draw(); continue; }
            if (tab == 1) { wifi_click(h); draw(); continue; }
            if (h == 0 && ni.link) run_test();
            else if (h == 1 && ni.link) { sys_net_renew(); ntest = 0; line(UI_MUTED, "Asking the network for a new address..."); }
            else if (h == 2) { dialog = 1; input[0] = 0; ilen = 0; if (ni.dns_manual) { ip_format(&ni.dns_user, input); ilen = strlen(input); } }
            else if (h == 10) dns_ok();
            else if (h == 11) dialog = 0;
            draw();
        }
        if (e.type == EV_WHEEL && tab == 1 && !dialog) {
            int maxs = nnets * ROW_H - LIST_H; if (maxs < 0) maxs = 0;
            wscroll += e.key * 40; if (wscroll > maxs) wscroll = maxs; if (wscroll < 0) wscroll = 0;
            draw(); continue;
        }
        if (e.type == EV_KEY && dialog == 2) {
            int k = e.key;
            if (k == '\n') wifi_join();
            else if (k == 27) { dialog = 0; memset(input, 0, sizeof input); ilen = 0; }
            else if (k == '\b') { if (ilen) input[--ilen] = 0; }
            else if (k >= 32 && k < 127 && ilen < 63) { input[ilen++] = (char)k; input[ilen] = 0; }
            draw(); continue;
        }
        if (e.type == EV_KEY && dialog) {
            int k = e.key;
            if (k == '\n') dns_ok();
            else if (k == 27) dialog = 0;
            else if (k == '\b') { if (ilen) input[--ilen] = 0; }
            else if (((k >= '0' && k <= '9') || k == '.' || k == ':' || (k >= 'a' && k <= 'f') || (k >= 'A' && k <= 'F')) && ilen < 45) { input[ilen++] = k; input[ilen] = 0; }
            draw();
        }
    }
    return 0;
}
