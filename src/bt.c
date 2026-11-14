/* Bluetooth host stack, first stage: talks HCI to a USB Bluetooth controller.
 * Commands go out over the control endpoint, events come back on the interrupt
 * endpoint. Supports adapter info and discovery of Classic + Low Energy devices. */
#include "usb.h"

static struct usb_dev *bdev;
static int bt_iface, bt_dci;
static uint8_t asm_buf[300]; static int asm_len;
#define EVQ 32
static uint8_t evq[EVQ][260]; static volatile int evq_h, evq_t;

static uint8_t bdaddr[6]; static char local_name[249];
static uint8_t hci_ver, lmp_ver; static uint16_t manufacturer, hci_rev, lmp_sub;
static bool info_ok, le_ok;

struct found { uint8_t addr[6]; bool le; int8_t rssi; uint32_t cod; char name[64]; uint8_t pscan; uint16_t clk; };
static struct found found[64]; static int nfound;

bool bt_present(void) { return bdev != 0; }

static void on_event(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc == CC_SUCCESS || cc == CC_SHORT) {
        uint8_t *b = d->ep_buf[dci];
        if (asm_len + len > sizeof asm_buf) asm_len = 0;
        memcpy(asm_buf + asm_len, b, len);
        asm_len += len;
        while (asm_len >= 2 && asm_len >= 2 + asm_buf[1]) {
            int n = 2 + asm_buf[1];
            if ((evq_h + 1) % EVQ != evq_t) { memcpy(evq[evq_h], asm_buf, n); evq_h = (evq_h + 1) % EVQ; }
            memmove(asm_buf, asm_buf + n, asm_len - n);
            asm_len -= n;
        }
    } else if (cc != CC_SUCCESS) {
        return;   /* endpoint error: stop */
    }
    usb_queue_in(d, dci, on_event);
}

static uint8_t *next_event(void) {
    xhci_poll();
    if (evq_t == evq_h) return 0;
    uint8_t *e = evq[evq_t];
    evq_t = (evq_t + 1) % EVQ;
    return e;
}

static void handle_async(uint8_t *e);

/* Send a command and wait for its Command Complete / Command Status.
 * Returns the status byte (0 = success), or -1 on timeout. Return params copied to ret. */
static int hci_cmd(uint16_t op, const void *params, uint8_t plen, uint8_t *ret, int retmax) {
    uint8_t pkt[260];
    pkt[0] = op & 0xFF; pkt[1] = op >> 8; pkt[2] = plen;
    if (plen) memcpy(pkt + 3, params, plen);
    if (usb_control(bdev, 0x20, 0, 0, bt_iface, 3 + plen, pkt) < 0) return -1;
    uint64_t end = uptime_ms() + 3000;
    while (uptime_ms() < end) {
        uint8_t *e = next_event();
        if (!e) { kyield(); continue; }
        if (e[0] == 0x0E && e[1] >= 3 && (e[3] | e[4] << 8) == op) {
            int n = e[1] - 3;
            if (ret) memcpy(ret, e + 5, n < retmax ? n : retmax);
            return n > 0 ? e[5] : 0;
        }
        if (e[0] == 0x0F && e[1] >= 4 && (e[4] | e[5] << 8) == op) return e[2];
        handle_async(e);
    }
    return -1;
}

static const char *mfr_name(uint16_t m) {
    switch (m) {
    case 2: return "Intel"; case 10: return "Qualcomm (CSR)"; case 13: return "Texas Instruments";
    case 15: return "Broadcom"; case 29: return "Qualcomm"; case 48: return "ST Microelectronics";
    case 70: return "MediaTek"; case 93: return "Realtek"; case 305: return "Cypress"; case 1521: return "Espressif";
    }
    return "Unknown";
}

static const char *ver_name(uint8_t v) {
    static const char *n[] = {"1.0b","1.1","1.2","2.0","2.1","3.0","4.0","4.1","4.2","5.0","5.1","5.2","5.3","5.4","6.0","6.1"};
    return v < 16 ? n[v] : "?";
}

static void fmt_addr(char *s, const uint8_t *a) {
    ksnprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static const char *cod_name(uint32_t cod) {
    switch ((cod >> 8) & 0x1F) {
    case 1: return "Computer"; case 2: return "Phone"; case 3: return "Network"; case 4: return "Audio";
    case 5: return "Input"; case 6: return "Imaging"; case 7: return "Wearable"; case 8: return "Toy"; case 9: return "Health";
    }
    return "";
}

/* ---------------- discovery results ---------------- */

static struct found *lookup(const uint8_t *a, bool le) {
    for (int i = 0; i < nfound; i++) if (!memcmp(found[i].addr, a, 6) && found[i].le == le) return &found[i];
    if (nfound >= 64) return 0;
    struct found *f = &found[nfound++];
    memset(f, 0, sizeof *f);
    memcpy(f->addr, a, 6); f->le = le; f->rssi = -127;
    return f;
}

static void announce(struct found *f) {
    char a[18]; fmt_addr(a, f->addr);
    console_set_color(f->le ? C_LCYAN : C_LMAGENTA);
    kprintf("  %s %s", f->le ? "[LE]     " : "[Classic]", a);
    console_set_color(C_LGRAY);
    if (f->rssi > -127) kprintf("  %4d dBm", f->rssi);
    kprintf("  %s%s%s\n", f->name[0] ? f->name : "(no name yet)", (!f->le && cod_name(f->cod)[0]) ? "  - " : "", f->le ? "" : cod_name(f->cod));
}

static void parse_ad(struct found *f, const uint8_t *p, int len) {
    for (int i = 0; i + 1 < len && p[i];) {
        int l = p[i], t = p[i + 1];
        if (i + 1 + l > len) break;
        if ((t == 0x08 || t == 0x09) && l > 1 && (t == 0x09 || !f->name[0])) {
            int n = l - 1 < 63 ? l - 1 : 63, k = 0;
            for (int j = 0; j < n; j++) { uint8_t c = p[i + 2 + j]; f->name[k++] = (c >= 32 && c < 127) ? c : '?'; }
            f->name[k] = 0;
        }
        i += l + 1;
    }
}

static void set_name(struct found *f, const uint8_t *s, int max) {
    int k = 0;
    for (int j = 0; j < max && s[j] && k < 63; j++) f->name[k++] = (s[j] >= 32 && s[j] < 127) ? s[j] : '?';
    f->name[k] = 0;
}

static void handle_async(uint8_t *e) {
    uint8_t code = e[0], *p = e + 2;
    if (code == 0x02 || code == 0x22) {          /* Inquiry Result (with RSSI) */
        int n = p[0];
        for (int i = 0; i < n; i++) {
            const uint8_t *r = p + 1 + i * 14;    /* both formats use 14 bytes per response */
            struct found *f = lookup(r, false);
            if (!f) continue;
            bool isnew = f->cod == 0 && !f->name[0];
            f->pscan = r[6];
            if (code == 0x02) { f->cod = r[9] | r[10] << 8 | r[11] << 16; f->clk = r[12] | r[13] << 8; }
            else { f->cod = r[8] | r[9] << 8 | r[10] << 16; f->clk = r[11] | r[12] << 8; f->rssi = (int8_t)r[13]; }
            if (isnew) announce(f);
        }
    } else if (code == 0x2F) {                   /* Extended Inquiry Result */
        struct found *f = lookup(p + 1, false);
        if (!f) return;
        bool isnew = f->cod == 0 && !f->name[0];
        f->pscan = p[7];
        f->cod = p[9] | p[10] << 8 | p[11] << 16;
        f->clk = p[12] | p[13] << 8;
        f->rssi = (int8_t)p[14];
        parse_ad(f, p + 15, e[1] - 15);
        if (isnew) announce(f);
    } else if (code == 0x07) {                   /* Remote Name Request Complete */
        if (p[0] == 0) { struct found *f = lookup(p + 1, false); if (f) set_name(f, p + 7, 248); }
    } else if (code == 0x3E && p[0] == 0x02) {   /* LE Advertising Report */
        int n = p[1];
        const uint8_t *q = p + 2;
        for (int i = 0; i < n; i++) {
            /* Reports are sent one at a time by essentially all controllers; parse sequentially. */
            const uint8_t *addr = q + 2; int dlen = q[8];
            struct found *f = lookup(addr, true);
            if (f) {
                bool had_name = f->name[0], isnew = f->rssi == -127;
                parse_ad(f, q + 9, dlen);
                f->rssi = (int8_t)q[9 + dlen];
                if (isnew || (!had_name && f->name[0])) announce(f);
            }
            q += 10 + dlen;
        }
    }
}

/* ---------------- public API ---------------- */

static void bt_detach(struct usb_dev *d) {
    if (bdev == d) { bdev = 0; info_ok = false; desktop_notify("Bluetooth", "The Bluetooth adapter was removed."); }
}

void bt_attach(struct usb_dev *d, int iface, int dci) {
    bdev = d; bt_iface = iface; bt_dci = dci;
    d->detach = bt_detach;
    usb_queue_in(d, dci, on_event);
    int st = hci_cmd(0x0C03, 0, 0, 0, 0);       /* HCI_Reset */
    if (st != 0) {
        LOG_WARN("Bluetooth: controller did not accept HCI reset (%d). It may need vendor firmware.\n", st);
    }
    uint8_t r[256];
    if (hci_cmd(0x1001, 0, 0, r, sizeof r) == 0) {          /* Read_Local_Version_Information */
        hci_ver = r[1]; hci_rev = r[2] | r[3] << 8; lmp_ver = r[4];
        manufacturer = r[5] | r[6] << 8; lmp_sub = r[7] | r[8] << 8;
        info_ok = true;
    }
    if (hci_cmd(0x1009, 0, 0, r, sizeof r) == 0) memcpy(bdaddr, r + 1, 6);   /* Read_BD_ADDR */
    if (hci_cmd(0x0C14, 0, 0, r, sizeof r) == 0) {                            /* Read_Local_Name */
        int k = 0;
        for (int j = 1; j < 249 && r[j] && k < 248; j++) local_name[k++] = (r[j] >= 32 && r[j] < 127) ? r[j] : '?';
        local_name[k] = 0;
    }
    if (hci_cmd(0x1003, 0, 0, r, sizeof r) == 0) le_ok = (r[1 + 4] >> 6) & 1;   /* LMP features byte 4 bit 6: LE supported */
    char a[18]; fmt_addr(a, bdaddr);
    if (info_ok)
        LOG_OK("Bluetooth %s by %s, address %s\n", ver_name(hci_ver), mfr_name(manufacturer), a);
    if (manufacturer == 2 && !bdaddr[0] && !bdaddr[1] && !bdaddr[2])
        LOG_WARN("Intel Bluetooth needs a firmware upload before it works (not supported yet)\n");
}

void bt_info(void) {
    if (!bdev) { kprintf("No Bluetooth adapter found.\n"); return; }
    char a[18]; fmt_addr(a, bdaddr);
    kprintf("Bluetooth adapter\n");
    kprintf("  USB device    %04x:%04x %s %s\n", bdev->vid, bdev->pid, bdev->manufacturer, bdev->product);
    if (!info_ok) { kprintf("  Status        not responding to HCI commands\n"); return; }
    kprintf("  Chip maker    %s (id %u)\n", mfr_name(manufacturer), manufacturer);
    kprintf("  Version       Bluetooth %s (HCI rev %u, LMP subversion %u)\n", ver_name(hci_ver), hci_rev, lmp_sub);
    kprintf("  Address       %s\n", a);
    kprintf("  Name          %s\n", local_name[0] ? local_name : "(none)");
    kprintf("  Low Energy    %s\n", le_ok ? "yes" : "no");
}

void bt_list(void) {
    if (!nfound) { kprintf("No devices found yet. Run 'bt scan'.\n"); return; }
    for (int i = 0; i < nfound; i++) announce(&found[i]);
}

static bool scanning;

void bt_scan(int seconds) {
    if (!bdev) { kprintf("No Bluetooth adapter found.\n"); return; }
    if (scanning) { kprintf("A Bluetooth scan is already running.\n"); return; }
    scanning = true;
    void bt_scan_inner(int seconds);
    bt_scan_inner(seconds);
    scanning = false;
}

void bt_scan_inner(int seconds) {
    if (!info_ok) { kprintf("Bluetooth adapter is not responding.\n"); return; }
    if (seconds < 3) seconds = 3;
    if (seconds > 60) seconds = 60;
    nfound = 0;
    uint8_t mask[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x3F};   /* incl. LE Meta (bit 61) */
    hci_cmd(0x0C01, mask, 8, 0, 0);                              /* Set_Event_Mask */
    uint8_t mode = 2; hci_cmd(0x0C45, &mode, 1, 0, 0);           /* Write_Inquiry_Mode: extended */
    bool le = false;
    if (le_ok) {
        uint8_t lemask[8] = {0x1F,0,0,0,0,0,0,0};
        hci_cmd(0x2001, lemask, 8, 0, 0);                        /* LE_Set_Event_Mask */
        uint8_t sp[7] = {0x01, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00}; /* active, 60ms interval, 30ms window */
        hci_cmd(0x200B, sp, 7, 0, 0);                            /* LE_Set_Scan_Parameters */
        uint8_t en[2] = {1, 0};
        le = hci_cmd(0x200C, en, 2, 0, 0) == 0;                  /* LE_Set_Scan_Enable */
    }
    uint8_t len = (seconds * 100 + 127) / 128; if (len > 0x30) len = 0x30;
    uint8_t inq[5] = {0x33, 0x8B, 0x9E, len, 0};                 /* GIAC, length x1.28s, unlimited */
    bool classic = hci_cmd(0x0401, inq, 5, 0, 0) == 0;
    kprintf("Scanning for %d seconds (%s%s%s)%s\n", seconds,
            classic ? "Classic" : "", classic && le ? " + " : "", le ? "Low Energy" : "", sched_running ? "..." : ". Press any key to stop.");
    uint64_t end = uptime_ms() + seconds * 1000ULL;
    bool inquiry_done = !classic;
    while (uptime_ms() < end) {
        uint8_t *e = next_event();
        if (e) { if (e[0] == 0x01) inquiry_done = true; else handle_async(e); continue; }
        if (!sched_running && input_poll() >= 0) break;
        kyield();
    }
    if (le) { uint8_t dis[2] = {0, 0}; hci_cmd(0x200C, dis, 2, 0, 0); }
    if (!inquiry_done) hci_cmd(0x0402, 0, 0, 0, 0);              /* Inquiry_Cancel */

    /* Ask nameless Classic devices for their names. */
    for (int i = 0; i < nfound; i++) {
        struct found *f = &found[i];
        if (f->le || f->name[0]) continue;
        uint8_t rn[10];
        memcpy(rn, f->addr, 6); rn[6] = f->pscan; rn[7] = 0; rn[8] = f->clk & 0xFF; rn[9] = (f->clk >> 8) | 0x80;
        if (hci_cmd(0x0419, rn, 10, 0, 0) != 0) continue;
        uint64_t t = uptime_ms() + 5000;
        while (uptime_ms() < t && !f->name[0]) {
            uint8_t *e = next_event();
            if (!e) { kyield(); continue; }
            handle_async(e);
            if (e[0] == 0x07) break;
        }
    }
    kprintf("\nFound %d device%s:\n", nfound, nfound == 1 ? "" : "s");
    bt_list();
}

void bt_get_adapter(struct bt_adapter *a) {
    memset(a, 0, sizeof *a);
    if (!bdev) return;
    a->present = 1; a->ok = info_ok; a->le = le_ok;
    strncpy0(a->maker, mfr_name(manufacturer), sizeof a->maker);
    strncpy0(a->version, ver_name(hci_ver), sizeof a->version);
    fmt_addr(a->addr, bdaddr);
    strncpy0(a->name, local_name, sizeof a->name);
    ksnprintf(a->usb, sizeof a->usb, "%s %s (%04x:%04x)", bdev->manufacturer, bdev->product, bdev->vid, bdev->pid);
}

int bt_get_results(struct bt_device *out, int max) {
    int n = 0;
    for (int i = 0; i < nfound && n < max; i++, n++) {
        struct found *f = &found[i];
        memcpy(out[n].addr, f->addr, 6);
        out[n].le = f->le; out[n].rssi = f->rssi; out[n].cod = f->cod;
        strncpy0(out[n].name, f->name, sizeof out[n].name);
        strncpy0(out[n].kind, f->le ? "" : cod_name(f->cod), sizeof out[n].kind);
    }
    return n;
}
