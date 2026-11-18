/* USB HID keyboard (boot protocol). */
#include "usb.h"

static uint8_t prev[8];
static int kbd_dci;

/* a boot-protocol keyboard report (8 bytes: modifiers, reserved, 6 keys): new keys are pressed
 * (shared with Bluetooth keyboards, which send the same reports) */
void hid_kbd_report(uint8_t *last, const uint8_t *r, int len) {
    if (len < 3 || r[2] == 1) return;                       /* 1 = rollover error */
    if ((r[0] & 0x88) && !(last[0] & 0x88)) kbd_super(true);
    if (!(r[0] & 0x88) && (last[0] & 0x88)) kbd_super(false);
    for (int i = 2; i < 8 && i < len; i++) {
        uint8_t k = r[i];
        if (!k) continue;
        bool seen = false;
        for (int j = 2; j < 8; j++) if (last[j] == k) seen = true;
        if (!seen) kbd_hid_key(k, r[0]);
    }
    memset(last, 0, 8); memcpy(last, r, len < 8 ? len : 8);
}

static void on_report(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) { usb_recover_in(d, dci, cc, on_report); return; }
    hid_kbd_report(prev, d->ep_buf[dci], (int)len);
    usb_queue_in(d, dci, on_report);
}

void usbkbd_attach(struct usb_dev *d, int iface) {
    const uint8_t *ss;
    const uint8_t *ep = usb_find_endpoint(d, iface, 3, true, &ss);
    if (!ep) return;
    usb_control(d, 0x21, 0x0B, 0, iface, 0, 0);   /* SET_PROTOCOL(boot) */
    usb_control(d, 0x21, 0x0A, 0, iface, 0, 0);   /* SET_IDLE(0) */
    int dci = usb_configure_ep(d, ep, ss);
    if (dci < 0) return;
    kbd_dci = dci;
    d->driver = "keyboard";
    usb_queue_in(d, dci, on_report);
}

/* USB HID mouse. Most mice send their wheel only in report protocol, so the mouse's report descriptor is read and
 * the buttons, X, Y and wheel are taken from where it says they are (with a report ID if it has them). A mouse
 * whose descriptor isn't understood, or that reports absolute positions (a tablet), uses the boot protocol:
 * byte 0 buttons, 1 dx, 2 dy, [3 wheel]. */
struct hidmouse { bool ok; int id, bo, bn, xo, xs, yo, ys, wo, ws; };
static struct hidmouse hm[8];                                   /* by the device's slot */

/* the input fields of a report descriptor: the first report with relative X and Y, and its buttons and wheel */
static bool hid_parse_mouse(const uint8_t *d, int n, struct hidmouse *m) {
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0, usage[16], umin = 0, umax = 0; int nu = 0; bool range = false;
    static int pos[256];                                        /* the next bit of each report's input */
    memset(pos, 0, sizeof pos); memset(m, 0, sizeof *m); m->xs = m->ys = 0; m->id = -1;
    bool rel_xy = false;
    for (int i = 0; i < n;) {
        uint8_t pre = d[i];
        if (pre == 0xFE) { if (i + 1 >= n) break; i += 3 + d[i + 1]; continue; }   /* (a long item) */
        int sz = pre & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        uint32_t v = 0; for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int type = (pre >> 2) & 3, tag = pre >> 4;
        i += 1 + sz;
        if (type == 1) {                                        /* global */
            if (tag == 0) page = v; else if (tag == 7) rsize = v; else if (tag == 9) rcount = v; else if (tag == 8) rid = v & 0xFF;
        } else if (type == 2) {                                 /* local */
            if (tag == 0 && nu < 16) usage[nu++] = sz == 4 ? v : (page << 16) | v;
            else if (tag == 1) { umin = sz == 4 ? v : (page << 16) | v; range = true; }
            else if (tag == 2) umax = sz == 4 ? v : (page << 16) | v;
        } else if (type == 0) {                                 /* main */
            if (tag == 8) {                                     /* input */
                for (uint32_t k = 0; k < rcount && k < 64; k++) {
                    uint32_t u = (int)k < nu ? usage[k] : range && umin + k <= umax ? umin + k : nu ? usage[nu - 1] : 0;
                    int at = pos[rid];
                    if (!(v & 1)) {                             /* (constant: padding) */
                        uint32_t up = u >> 16, uu = u & 0xFFFF;
                        if (up == 9 && uu == 1 && !m->bn) { m->bo = at; m->bn = rcount - k < 5 ? (int)(rcount - k) : 5; m->id = m->id < 0 ? (int)rid : m->id; }
                        else if (up == 1 && uu == 0x30 && !m->xs) { m->xo = at; m->xs = rsize; rel_xy = v & 4; m->id = rid; }
                        else if (up == 1 && uu == 0x31 && !m->ys && (int)rid == m->id) { m->yo = at; m->ys = rsize; }
                        else if (up == 1 && uu == 0x38 && !m->ws && (int)rid == m->id) { m->wo = at; m->ws = rsize; }
                    }
                    pos[rid] += rsize;
                }
            }
            nu = 0; range = false; umin = umax = 0;
        }
    }
    m->ok = m->xs && m->ys && m->xs <= 32 && m->ys <= 32 && m->ws <= 32 && rel_xy;
    return m->ok;
}
static int hid_field(const uint8_t *r, int len, int off, int size, bool sign) {   /* little-endian bits */
    if (!size || (off + size + 7) / 8 > len) return 0;
    uint32_t v = 0;
    for (int b = 0; b < size; b++) if (r[(off + b) / 8] >> ((off + b) % 8) & 1) v |= 1u << b;
    if (sign && size < 32 && (v >> (size - 1) & 1)) v |= ~0u << size;
    return (int)v;
}
static void on_mouse(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) { usb_recover_in(d, dci, cc, on_mouse); return; }
    const uint8_t *r = d->ep_buf[dci];
    struct hidmouse *m = &hm[d->slot & 7];
    if (m->ok) {
        int n = (int)len;
        if (m->id > 0) { if (n < 1 || r[0] != m->id) { usb_queue_in(d, dci, on_mouse); return; } r++; n--; }   /* (another report) */
        int bt = hid_field(r, n, m->bo, m->bn, false);
        mouse_push(hid_field(r, n, m->xo, m->xs, true), hid_field(r, n, m->yo, m->ys, true), bt & 7, -hid_field(r, n, m->wo, m->ws, true));
    } else if (len >= 3) {
        const int8_t *b = (const int8_t *)r;
        mouse_push(b[1], b[2], b[0] & 7, len >= 4 ? -b[3] : 0);
    }
    usb_queue_in(d, dci, on_mouse);
}

void usbmouse_attach(struct usb_dev *d, int iface) {
    const uint8_t *ss;
    const uint8_t *ep = usb_find_endpoint(d, iface, 3, true, &ss);
    if (!ep) return;
    struct hidmouse *m = &hm[d->slot & 7]; m->ok = false;
    int rlen = 0, cur = -1;                                     /* the report descriptor's length, from the HID descriptor */
    for (int i = 0; i + 1 < d->config_len && d->config[i]; i += d->config[i]) {
        const uint8_t *p = &d->config[i];
        if (p[1] == 4) cur = p[2];
        if (p[1] == 0x21 && cur == iface && p[0] >= 9 && p[6] == 0x22) { rlen = p[7] | p[8] << 8; break; }
    }
    static uint8_t rd[1024];
    if (rlen > 0 && rlen <= (int)sizeof rd && usb_control(d, 0x81, 6, 0x2200, iface, (uint16_t)rlen, rd) >= 0 && hid_parse_mouse(rd, rlen, m))
        usb_control(d, 0x21, 0x0B, 1, iface, 0, 0);             /* SET_PROTOCOL(report) */
    else usb_control(d, 0x21, 0x0B, 0, iface, 0, 0);            /* SET_PROTOCOL(boot) */
    LOG_INFO("USB mouse: %s\n", m->ok ? (m->ws ? "report protocol, with a wheel" : "report protocol, no wheel") : "boot protocol");
    usb_control(d, 0x21, 0x0A, 0, iface, 0, 0);
    int dci = usb_configure_ep(d, ep, ss);
    if (dci < 0) return;
    d->driver = d->driver ? "keyboard + mouse" : "mouse";
    mouse_present = true;
    usb_queue_in(d, dci, on_mouse);
}
