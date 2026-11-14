/* Bluetooth host stack, first stage: talks HCI to a USB Bluetooth controller.
 * Commands go out over the control endpoint, events come back on the interrupt
 * endpoint. Supports adapter info and discovery of Classic + Low Energy devices. */
#include "usb.h"
#include "vfs.h"

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
static bool scanning;
static volatile int hci_busy;              /* a command is waiting for its answer: the background thread keeps off the events */
static int hci_cmd_(uint16_t op, const void *params, uint8_t plen, uint8_t *ret, int retmax);
static int hci_cmd(uint16_t op, const void *params, uint8_t plen, uint8_t *ret, int retmax) {
    hci_busy++; int r = hci_cmd_(op, params, plen, ret, retmax); hci_busy--; return r;
}
static int hci_cmd_(uint16_t op, const void *params, uint8_t plen, uint8_t *ret, int retmax) {
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
            int n = l - 1 < 63 ? l - 1 : 63;
            char raw[64]; memcpy(raw, p + i + 2, n);
            long m = utf8_to_cp1252(raw, n);                /* names are UTF-8 */
            int k = 0; for (long j = 0; j < m && k < 63; j++) { uint8_t c = raw[j]; f->name[k++] = c >= 32 && c != 127 ? c : '?'; }
            f->name[k] = 0;
        }
        i += l + 1;
    }
}

static void set_name(struct found *f, const uint8_t *s, int max) {
    char raw[249]; int n = 0;
    while (n < max && n < 248 && s[n]) { raw[n] = s[n]; n++; }
    long m = utf8_to_cp1252(raw, n);                         /* names are UTF-8 */
    int k = 0;
    for (long j = 0; j < m && k < 63; j++) { uint8_t c = raw[j]; f->name[k++] = c >= 32 && c != 127 ? c : '?'; }
    f->name[k] = 0;
}

static bool hid_event(uint8_t *e);
static void handle_async(uint8_t *e) {
    if (hid_event(e)) return;
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


/* ---------------- pairing (Classic Secure Simple Pairing, with legacy PIN fallback) ---------------- */
#define MAXKEYS 16
struct bkey { uint8_t addr[6], key[16]; char name[64]; };
static struct bkey keys[MAXKEYS]; static int nkeys; static bool keys_loaded;
static struct bt_pair pst;
static volatile int pair_answer = -1;
static uint8_t pair_addr[6];

static const char *KEYFILE = "/system/config/bluetooth";
static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void keys_load(void) {
    keys_loaded = true; nkeys = 0;
    static char b[MAXKEYS * 140];
    long n = vfs_read(KEYFILE, b, 0, sizeof b - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l && nkeys < MAXKEYS;) {             /* "aabbccddeeff <32 hex key> name" (address in HCI byte order) */
        char *e = l; while (*e && *e != '\n') e++;
        struct bkey *k = &keys[nkeys]; bool ok = e - l >= 6 * 2 + 1 + 32;
        for (int i = 0; ok && i < 6; i++) { int h = hexd(l[2 * i]), lo = hexd(l[2 * i + 1]); if (h < 0 || lo < 0) ok = false; else k->addr[i] = h << 4 | lo; }
        for (int i = 0; ok && i < 16; i++) { int h = hexd(l[13 + 2 * i]), lo = hexd(l[14 + 2 * i]); if (h < 0 || lo < 0) ok = false; else k->key[i] = h << 4 | lo; }
        if (ok) { int m = 0; for (char *p = l + 46; p < e && m < 63; p++) k->name[m++] = *p; k->name[m] = 0; nkeys++; }
        l = *e ? e + 1 : e;
    }
}

static void keys_save(void) {
    static char b[MAXKEYS * 140]; int n = 0;
    for (int i = 0; i < nkeys; i++) {
        for (int j = 0; j < 6; j++) n += ksnprintf(b + n, sizeof b - n, "%02x", keys[i].addr[j]);
        b[n++] = ' ';
        for (int j = 0; j < 16; j++) n += ksnprintf(b + n, sizeof b - n, "%02x", keys[i].key[j]);
        n += ksnprintf(b + n, sizeof b - n, " %s\n", keys[i].name);
    }
    vfs_write(KEYFILE, b, n);
}

static struct bkey *key_find(const uint8_t *a) {
    if (!keys_loaded) keys_load();
    for (int i = 0; i < nkeys; i++) if (!memcmp(keys[i].addr, a, 6)) return &keys[i];
    return 0;
}

/* ---------------- Bluetooth keyboards and mice (Classic HID over L2CAP) ----------------
 * Paired keyboards and mice connect to us (or we connect to them right after pairing); they open two
 * L2CAP channels, control (PSM 0x11) and interrupt (PSM 0x13), and send boot-protocol reports on the
 * interrupt channel: A1 01 <8-byte keyboard report> or A1 02 <buttons dx dy [wheel]>. */
static int acl_in_dci = -1, acl_out_dci = -1;
void bt_set_acl(int in_dci, int out_dci) { acl_in_dci = in_dci; acl_out_dci = out_dci; }
#define ACLQ 32
static uint8_t aclq[ACLQ][1100]; static int aclq_n[ACLQ]; static volatile int aclq_h, aclq_t;
static uint8_t acl_raw[2200]; static int acl_raw_len;
static volatile bool pairing_now;
struct hconn {
    bool used; uint16_t handle; uint8_t addr[6];
    uint16_t ctrl_l, ctrl_r, intr_l, intr_r;           /* our and their channel ids (0: not open) */
    uint8_t cfg;                                        /* bits: 1 ctrl we're configured, 2 ctrl they are, 4/8 the same for intr */
    bool hid_on, outgoing; uint8_t ident; uint8_t kprev[8]; int lastb;
    uint8_t rx[700]; int rx_len, rx_want;               /* an L2CAP packet being put together */
};
static struct hconn hc[4];
static uint16_t next_cid = 0x40;

#ifndef BT_TEST
static void on_acl(struct usb_dev *d, int dci, int cc, uint32_t len) {   /* raw data from the adapter: split into ACL packets */
    if (cc != CC_SUCCESS && cc != CC_SHORT) { usb_recover_in(d, dci, cc, on_acl); return; }
    uint8_t *b = d->ep_buf[dci];
    if (acl_raw_len + len > sizeof acl_raw) acl_raw_len = 0;
    memcpy(acl_raw + acl_raw_len, b, len); acl_raw_len += len;
    while (acl_raw_len >= 4) {
        int n = 4 + (acl_raw[2] | acl_raw[3] << 8);
        if (n > (int)sizeof aclq[0]) { acl_raw_len = 0; break; }
        if (acl_raw_len < n) break;
        if ((aclq_h + 1) % ACLQ != aclq_t) { memcpy(aclq[aclq_h], acl_raw, n); aclq_n[aclq_h] = n; aclq_h = (aclq_h + 1) % ACLQ; }
        memmove(acl_raw, acl_raw + n, acl_raw_len - n); acl_raw_len -= n;
    }
    usb_queue_in(d, dci, on_acl);
}
static void acl_send(struct hconn *c, uint16_t cid, const uint8_t *data, int len) {
    if (!bdev || acl_out_dci < 0 || len > 600) return;
    static uint8_t pkt[620];
    pkt[0] = c->handle & 0xFF; pkt[1] = ((c->handle >> 8) & 0x0F) | 0x20;      /* first packet, flushable */
    pkt[2] = (len + 4) & 0xFF; pkt[3] = (len + 4) >> 8;
    pkt[4] = len & 0xFF; pkt[5] = len >> 8; pkt[6] = cid & 0xFF; pkt[7] = cid >> 8;
    memcpy(pkt + 8, data, len);
    usb_bulk(bdev, acl_out_dci, pkt, 8 + len, 1000);
}
#else
static void acl_send(struct hconn *c, uint16_t cid, const uint8_t *data, int len) { bt_test_acl_sent(c->handle, cid, data, len); }
#endif
static void sig_send(struct hconn *c, uint8_t code, uint8_t id, const uint8_t *d, int n) {
    uint8_t b[64]; b[0] = code; b[1] = id; b[2] = n; b[3] = 0; memcpy(b + 4, d, n);
    acl_send(c, 1, b, 4 + n);
}
static void l2_connect(struct hconn *c, uint16_t psm) {                   /* we open a channel (after pairing) */
    uint16_t l = next_cid++;
    if (psm == 0x11) c->ctrl_l = l; else c->intr_l = l;
    uint8_t d[4] = { psm & 0xFF, psm >> 8, l & 0xFF, l >> 8 };
    sig_send(c, 0x02, ++c->ident, d, 4);
}
static void l2_config(struct hconn *c, uint16_t remote) {                  /* our side of the configuration: defaults */
    uint8_t d[4] = { remote & 0xFF, remote >> 8, 0, 0 };
    sig_send(c, 0x04, ++c->ident, d, 4);
}
static void check_ready(struct hconn *c) {
    if (!c->hid_on && (c->cfg & 3) == 3 && (c->cfg & 12) == 12) {
        c->hid_on = true;
        uint8_t boot = 0x70;                                              /* HIDP SET_PROTOCOL: boot protocol */
        acl_send(c, c->ctrl_r, &boot, 1);
        const char *nm = "A Bluetooth device";
        for (int i = 0; i < nkeys; i++) if (!memcmp(keys[i].addr, c->addr, 6) && keys[i].name[0]) nm = keys[i].name;
        char m[96]; ksnprintf(m, sizeof m, "%s is connected.", nm);
#ifndef BT_TEST
        desktop_notify("Bluetooth", m);
#endif
        if (c->outgoing && c->ctrl_l) { /* nothing more */ }
    }
}
static void l2_signal(struct hconn *c, const uint8_t *p, int len) {
    while (len >= 4) {
        uint8_t code = p[0], id = p[1]; int n = p[2] | p[3] << 8;
        const uint8_t *d = p + 4;
        if (n > len - 4) return;
        if (code == 0x02 && n >= 4) {                                     /* Connection Request (from the device) */
            uint16_t psm = d[0] | d[1] << 8, scid = d[2] | d[3] << 8, l = next_cid++;
            uint16_t result = (psm == 0x11 || psm == 0x13) ? 0 : 2;
            if (!result) { if (psm == 0x11) { c->ctrl_l = l; c->ctrl_r = scid; } else { c->intr_l = l; c->intr_r = scid; } }
            uint8_t r[8] = { l & 0xFF, l >> 8, scid & 0xFF, scid >> 8, result & 0xFF, result >> 8, 0, 0 };
            if (result) { r[0] = r[1] = 0; }
            sig_send(c, 0x03, id, r, 8);
            if (!result) l2_config(c, scid);
        } else if (code == 0x03 && n >= 8) {                              /* Connection Response (to ours) */
            uint16_t dcid = d[0] | d[1] << 8, scid = d[2] | d[3] << 8, result = d[4] | d[5] << 8;
            if (result == 0) {
                if (scid == c->ctrl_l) c->ctrl_r = dcid; else if (scid == c->intr_l) c->intr_r = dcid;
                l2_config(c, dcid);
            }
        } else if (code == 0x04 && n >= 4) {                              /* Configuration Request: we accept theirs */
            uint16_t dcid = d[0] | d[1] << 8;
            uint16_t remote = dcid == c->ctrl_l ? c->ctrl_r : dcid == c->intr_l ? c->intr_r : 0;
            uint8_t r[6] = { remote & 0xFF, remote >> 8, 0, 0, 0, 0 };
            sig_send(c, 0x05, id, r, 6);
            if (dcid == c->ctrl_l) c->cfg |= 2; else if (dcid == c->intr_l) c->cfg |= 8;
            check_ready(c);
        } else if (code == 0x05 && n >= 6) {                              /* Configuration Response (to ours) */
            uint16_t scid = d[0] | d[1] << 8, result = d[4] | d[5] << 8;
            if (result == 0) { if (scid == c->ctrl_l) c->cfg |= 1; else if (scid == c->intr_l) c->cfg |= 4; }
            check_ready(c);
            /* after pairing we open the control channel first, then the interrupt one */
            if (c->outgoing && scid == c->ctrl_l && !c->intr_l) l2_connect(c, 0x13);
        } else if (code == 0x06 && n >= 4) {                              /* Disconnection Request */
            uint16_t dcid = d[0] | d[1] << 8, scid = d[2] | d[3] << 8;
            uint8_t r[4] = { dcid & 0xFF, dcid >> 8, scid & 0xFF, scid >> 8 };
            sig_send(c, 0x07, id, r, 4);
            if (dcid == c->ctrl_l) { c->ctrl_l = c->ctrl_r = 0; c->cfg &= ~3; }
            if (dcid == c->intr_l) { c->intr_l = c->intr_r = 0; c->cfg &= ~12; }
            c->hid_on = false;
        } else if (code == 0x08) {                                        /* Echo Request */
            sig_send(c, 0x09, id, d, n > 40 ? 40 : n);
        } else if (code == 0x0A && n >= 2) {                              /* Information Request: not supported */
            uint8_t r[4] = { d[0], d[1], 1, 0 };
            sig_send(c, 0x0B, id, r, 4);
        }
        p += 4 + n; len -= 4 + n;
    }
}
static void hid_input(struct hconn *c, const uint8_t *p, int len) {
    if (len < 2 || p[0] != 0xA1) return;                                  /* DATA | INPUT */
    if (p[1] == 0x01 && len >= 4) hid_kbd_report(c->kprev, p + 2, len - 2 > 8 ? 8 : len - 2);
    else if (p[1] == 0x02 && len >= 5) {
#ifndef BT_TEST
        mouse_push((int8_t)p[3], (int8_t)p[4], p[2] & 7, len >= 6 ? -(int8_t)p[5] : 0);
#else
        bt_test_mouse((int8_t)p[3], (int8_t)p[4], p[2] & 7);
#endif
    }
}
static struct hconn *conn_by_handle(uint16_t h) { for (int i = 0; i < 4; i++) if (hc[i].used && hc[i].handle == h) return &hc[i]; return 0; }
static void acl_packet(const uint8_t *b, int n) {                         /* one ACL packet from the adapter */
    uint16_t h = (b[0] | b[1] << 8) & 0x0FFF; int pb = (b[1] >> 4) & 3, dl = b[2] | b[3] << 8;
    struct hconn *c = conn_by_handle(h);
    if (!c || dl + 4 > n) return;
    const uint8_t *d = b + 4;
    if (pb == 2 || pb == 0) {                                             /* the start of an L2CAP packet */
        if (dl < 4) return;
        c->rx_want = 4 + (d[0] | d[1] << 8); c->rx_len = 0;
        if (c->rx_want > (int)sizeof c->rx) { c->rx_want = 0; return; }
    }
    if (!c->rx_want || c->rx_len + dl > c->rx_want) { c->rx_want = 0; return; }
    memcpy(c->rx + c->rx_len, d, dl); c->rx_len += dl;
    if (c->rx_len < c->rx_want) return;
    uint16_t cid = c->rx[2] | c->rx[3] << 8; int l = c->rx_want - 4;
    c->rx_want = 0;
    if (cid == 1) l2_signal(c, c->rx + 4, l);
    else if (cid && cid == c->intr_l) hid_input(c, c->rx + 4, l);
}
static void hid_new_conn(uint16_t handle, const uint8_t *addr, bool outgoing) {
    struct hconn *c = 0;
    for (int i = 0; i < 4; i++) if (!hc[i].used) { c = &hc[i]; break; }
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->used = true; c->handle = handle; memcpy(c->addr, addr, 6); c->outgoing = outgoing;
}
/* events about connections to paired devices; returns true if it was one */
static bool hid_event(uint8_t *e) {
    uint8_t code = e[0], *p = e + 2;
    if (code == 0x04) {                                                   /* Connection_Request: a paired device is back */
        if (!keys_loaded) keys_load();
        uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x01;                        /* accept, it stays in charge */
        if (key_find(p)) hci_cmd(0x0409, r, 7, 0, 0);
        else { uint8_t rj[7]; memcpy(rj, p, 6); rj[6] = 0x0F; hci_cmd(0x040A, rj, 7, 0, 0); }   /* reject unknown devices */
        return true;
    }
    if (code == 0x03 && !pairing_now) {                                   /* Connection_Complete (they connected to us) */
        if (p[0] == 0) hid_new_conn(p[1] | (p[2] & 0x0F) << 8, p + 3, false);
        return true;
    }
    if (code == 0x17 && !pairing_now) {                                   /* Link_Key_Request */
        struct bkey *k = key_find(p);
        if (k) { uint8_t r[22]; memcpy(r, p, 6); memcpy(r + 6, k->key, 16); hci_cmd(0x040B, r, 22, 0, 0); }
        else hci_cmd(0x040C, p, 6, 0, 0);
        return true;
    }
    if (code == 0x05) {                                                   /* Disconnection_Complete */
        struct hconn *c = conn_by_handle(p[1] | (p[2] & 0x0F) << 8);
        if (c) {
            if (c->hid_on) { uint8_t z[8] = { 0 }; hid_kbd_report(c->kprev, z, 8); }   /* release any held keys */
            c->used = false;
        }
        return !pairing_now;
    }
    if (code == 0x13 || code == 0x08) return true;                        /* completed packets, encryption change */
    return false;
}
static void acl_poll(void) {
    while (aclq_t != aclq_h) { int t = aclq_t; acl_packet(aclq[t], aclq_n[t]); aclq_t = (t + 1) % ACLQ; }
}
#ifdef BT_TEST
static void handle_async(uint8_t *e);
void bt_test_event(uint8_t *e) { handle_async(e); }
void bt_test_acl_in(const uint8_t *b, int n) { acl_packet(b, n); }
#endif


static void say(int state, const char *m) { pst.state = state; strncpy0(pst.msg, m, sizeof pst.msg); }

/* Pair with a Classic device found by a scan. Blocks until done (run it in its own task). */
void bt_pair_run(const uint8_t *addr) {
    pairing_now = true;
    memset(&pst, 0, sizeof pst);
    memcpy(pst.addr, addr, 6);
    struct found *f = 0;
    for (int i = 0; i < nfound; i++) if (!memcmp(found[i].addr, addr, 6) && !found[i].le) f = &found[i];
    strncpy0(pst.name, f && f->name[0] ? f->name : "the device", sizeof pst.name);
    pair_answer = -1;
    if (!bdev || !info_ok) { say(BTP_FAILED, "No working Bluetooth adapter."); pairing_now = false; return; }
    say(BTP_CONNECTING, "Connecting...");
    uint8_t all[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F};
    hci_cmd(0x0C01, all, 8, 0, 0);                              /* Set_Event_Mask: everything */
    uint8_t on = 1; hci_cmd(0x0C56, &on, 1, 0, 0);              /* Write_Simple_Pairing_Mode */
    uint8_t cc[13]; memcpy(cc, addr, 6);
    cc[6] = 0x18; cc[7] = 0xCC;                                  /* packet types DM1..DH5 */
    cc[8] = f ? f->pscan : 1; cc[9] = 0;
    uint16_t clk = f ? (f->clk | 0x8000) : 0; cc[10] = clk & 0xFF; cc[11] = clk >> 8; cc[12] = 1;
    if (hci_cmd(0x0405, cc, 13, 0, 0) != 0) { say(BTP_FAILED, "The adapter refused to connect."); pairing_now = false; return; }   /* Create_Connection */
    int handle = -1; bool authed = false; uint8_t newkey[16]; bool havekey = false; int remote_io = -1;
    uint64_t end = uptime_ms() + 60000;
    while (uptime_ms() < end && pst.state != BTP_DONE && pst.state != BTP_FAILED) {
        if (pst.state == BTP_CONFIRM && pair_answer >= 0) {     /* the user answered the code question */
            hci_cmd(pair_answer ? 0x042C : 0x042D, addr, 6, 0, 0);
            if (!pair_answer) { say(BTP_FAILED, "Pairing cancelled."); break; }
            say(BTP_WORKING, "Finishing...");
        }
        uint8_t *e = next_event();
        if (!e) { kyield(); continue; }
        uint8_t code = e[0], *p = e + 2;
        switch (code) {
        case 0x03:                                              /* Connection_Complete */
            if (memcmp(p + 3, addr, 6)) break;
            if (p[0]) { say(BTP_FAILED, p[0] == 0x04 ? "The device didn't answer. Is it switched on and visible?" : "The connection failed."); break; }
            handle = p[1] | (p[2] & 0x0F) << 8;
            say(BTP_WORKING, "Connected. Pairing...");
            { uint8_t h[2] = { handle & 0xFF, handle >> 8 }; hci_cmd(0x0411, h, 2, 0, 0); }   /* Authentication_Requested */
            break;
        case 0x17: {                                            /* Link_Key_Request */
            struct bkey *k = key_find(p);
            if (k) { uint8_t r[22]; memcpy(r, p, 6); memcpy(r + 6, k->key, 16); hci_cmd(0x040B, r, 22, 0, 0); }
            else hci_cmd(0x040C, p, 6, 0, 0);
            break;
        }
        case 0x31: {                                            /* IO_Capability_Request: we can show a number and ask yes/no */
            uint8_t r[9]; memcpy(r, p, 6); r[6] = 1; r[7] = 0; r[8] = 0x03;
            hci_cmd(0x042B, r, 9, 0, 0);
            break;
        }
        case 0x32:                                              /* IO_Capability_Response: what the device can do */
            if (!memcmp(p, addr, 6)) remote_io = p[6];
            break;
        case 0x33:                                              /* User_Confirmation_Request */
            if (remote_io == 3) {                               /* no screen or keys (headphones, speakers): "just works" */
                hci_cmd(0x042C, addr, 6, 0, 0);                 /* the user already chose Pair */
                say(BTP_WORKING, "Pairing...");
                break;
            }
            pst.code = p[6] | p[7] << 8 | p[8] << 16 | (uint32_t)p[9] << 24;
            say(BTP_CONFIRM, "Check that the same number is shown on the device.");
            break;
        case 0x34: hci_cmd(0x042F, p, 6, 0, 0); break;           /* User_Passkey_Request: we have no keyboard role here */
        case 0x3B:                                              /* User_Passkey_Notification: type this on the device */
            pst.code = p[6] | p[7] << 8 | p[8] << 16 | (uint32_t)p[9] << 24;
            say(BTP_TYPE_CODE, "Type this number on the device's keyboard, then press Enter on it.");
            break;
        case 0x16: {                                            /* PIN_Code_Request (older devices): the usual 0000 */
            uint8_t r[23] = {0}; memcpy(r, p, 6); r[6] = 4; memcpy(r + 7, "0000", 4);
            hci_cmd(0x040D, r, 23, 0, 0);
            break;
        }
        case 0x18:                                              /* Link_Key_Notification: remember it */
            if (!memcmp(p, addr, 6)) { memcpy(newkey, p + 6, 16); havekey = true; }
            break;
        case 0x36:                                              /* Simple_Pairing_Complete */
            if (p[0]) say(BTP_FAILED, p[0] == 0x05 ? "The number wasn't confirmed on the device." : "Pairing failed.");
            break;
        case 0x06:                                              /* Authentication_Complete */
            if ((p[1] | (p[2] & 0x0F) << 8) != handle) break;
            if (p[0]) { say(BTP_FAILED, p[0] == 0x05 ? "Pairing was refused." : "Pairing failed."); break; }
            authed = true;
            break;
        case 0x05:                                              /* Disconnection_Complete */
            if ((p[1] | (p[2] & 0x0F) << 8) == handle && !authed) say(BTP_FAILED, "The device disconnected.");
            break;
        default: handle_async(e);
        }
        if (authed && pst.state != BTP_FAILED) {
            if (havekey) {
                struct bkey *k = key_find(addr);
                if (!k && nkeys < MAXKEYS) k = &keys[nkeys++];
                if (k) { memcpy(k->addr, addr, 6); memcpy(k->key, newkey, 16); strncpy0(k->name, pst.name, sizeof k->name); keys_save(); }
            }
            char m[96]; ksnprintf(m, sizeof m, "Paired with %s.", pst.name);
            say(BTP_DONE, m);
        }
    }
    if (pst.state != BTP_DONE && pst.state != BTP_FAILED) say(BTP_FAILED, "Pairing timed out.");
    bool hid = f && ((f->cod >> 8) & 0x1F) == 5;                 /* a keyboard, mouse or other input device */
    if (handle >= 0 && pst.state == BTP_DONE && hid) {
        /* keep the connection: encrypt it and open the HID channels (control, then interrupt) */
        uint8_t en[3] = { handle & 0xFF, handle >> 8, 1 }; hci_cmd(0x0413, en, 3, 0, 0);
        hid_new_conn((uint16_t)handle, addr, true);
        struct hconn *c = conn_by_handle((uint16_t)handle);
        if (c) l2_connect(c, 0x11);
    } else if (handle >= 0) { uint8_t d[3] = { handle & 0xFF, handle >> 8, 0x13 }; hci_cmd(0x0406, d, 3, 0, 0); }   /* Disconnect */
    pairing_now = false;
}

void bt_pair_confirm(int yes) { pair_answer = yes ? 1 : 0; }
void bt_pair_status(struct bt_pair *out) { *out = pst; }
int bt_paired(struct bt_paired *out, int max) {
    if (!keys_loaded) keys_load();
    int n = 0;
    for (int i = 0; i < nkeys && n < max; i++, n++) { memcpy(out[n].addr, keys[i].addr, 6); strncpy0(out[n].name, keys[i].name, sizeof out[n].name); }
    return n;
}
void bt_forget(const uint8_t *addr) {
    if (!keys_loaded) keys_load();
    for (int i = 0; i < nkeys; i++) if (!memcmp(keys[i].addr, addr, 6)) { keys[i] = keys[--nkeys]; keys_save(); return; }
}

#ifndef BT_TEST
static void pair_task(void *arg) { bt_pair_run(pair_addr); task_exit(0); }
int bt_pair_start(const uint8_t *addr) {
    if (pst.state > BTP_IDLE && pst.state < BTP_DONE) return -1;
    memcpy(pair_addr, addr, 6);
    memset(&pst, 0, sizeof pst); pst.state = BTP_CONNECTING;
    task_create_kernel("Bluetooth pairing", pair_task, 0);
    return 0;
}
#endif

/* ---------------- public API ---------------- */

static void bt_detach(struct usb_dev *d) {
    if (bdev == d) { bdev = 0; info_ok = false; desktop_notify("Bluetooth", "The Bluetooth adapter was removed."); }
}

#ifndef BT_TEST
/* follows connections from paired devices while nothing else is talking to the adapter */
static void bt_thread(void *arg) {
    for (;;) {
        if (bdev) {
            xhci_poll();
            /* take an event only while no command or pairing is waiting for one (checked with interrupts off,
             * so another task can't start a command between the check and the take) */
            uint8_t *e = 0; uint64_t fl;
            __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
            if (!pairing_now && !hci_busy && !scanning && evq_t != evq_h) { e = evq[evq_t]; evq_t = (evq_t + 1) % EVQ; }
            __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
            if (e) { handle_async(e); continue; }
        }
        acl_poll();
        task_sleep(8);
    }
}
#endif

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
#ifndef BT_TEST
    /* be reachable for paired keyboards and mice: accept connections (page scan) and read their data */
    uint8_t all[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F};
    hci_cmd(0x0C01, all, 8, 0, 0);                                            /* Set_Event_Mask */
    uint8_t ssp = 1; hci_cmd(0x0C56, &ssp, 1, 0, 0);                          /* Write_Simple_Pairing_Mode */
    uint8_t cod[3] = { 0x04, 0x01, 0x00 }; hci_cmd(0x0C24, cod, 3, 0, 0);     /* Write_Class_of_Device: a desktop computer */
    uint8_t scan = 0x02; hci_cmd(0x0C1A, &scan, 1, 0, 0);                     /* Write_Scan_Enable: page scan */
    if (acl_in_dci > 0) usb_queue_in(d, acl_in_dci, on_acl);
    static bool thread_started;
    if (!thread_started) { thread_started = true; task_create_kernel("Bluetooth", bt_thread, 0); }
#endif
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
