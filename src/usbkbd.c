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
/* for the 'mouse' command: each mouse's report descriptor, and the last reports with what was read from them */
static struct { int slot, rlen; uint8_t rd[256]; } mdesc[4]; static int nmdesc;
struct mrep { int slot, len, dx, dy, bt, w; uint8_t r[16]; };
static struct mrep mlast[8], mother[8]; static int mlast_n, mother_n;   /* the last reports; the last that weren't only a move */
#define HPP_SWID 0x0A
static int mouse_bt;                                   /* the buttons held, from the last normal report */
static struct hidpp {
    struct usb_dev *d; int iface, dev;                  /* dev: the device index on the receiver (1-6) */
    volatile int got; uint8_t ans[20];                 /* the answer to the request in flight */
    int fs, hrw, hrw_old, hrw_new;                     /* feature indexes (0: none), wheel mode before / after */
    int nfeat; uint16_t feat[40];                      /* the mouse's features, by index */
    int wheel_acc, mult;                               /* (high-resolution notifications: eighths, or so) */
    int devs[6], ndev, wheel_dev;                      /* the devices that answered; the one sending 2.0 notifications */
    const char *state;
} hpp;
static void mouse_note(int slot, const uint8_t *r, int len, int dx, int dy, int bt, int w) {
    struct mrep e = { slot, len, dx, dy, bt, w, {0} }; memcpy(e.r, r, len < 16 ? len : 16);
    mlast[mlast_n++ % 8] = e;
    bool other = bt || w;                                       /* (buttons, wheel, or bytes past X and Y that aren't zero) */
    for (int b = len > 5 ? 5 : len; b < len && b < 16; b++) if (r[b]) other = true;
    if (other) mother[mother_n++ % 8] = e;
}
static void mouse_show(const struct mrep *e) {
    kprintf("  slot %d, %2d bytes:", e->slot, e->len);
    for (int b = 0; b < e->len && b < 16; b++) kprintf(" %02x", e->r[b]);
    kprintf("   -> move %d,%d buttons %d wheel %d\n", e->dx, e->dy, e->bt, e->w);
}
void usbmouse_info(void) {
    for (int k = 0; k < nmdesc; k++) {
        struct hidmouse *m = &hm[mdesc[k].slot & 7];
        kprintf("USB mouse (slot %d): %s; report %d, buttons %d@%d, X %d@%d, Y %d@%d, wheel %d@%d (bits@offset)\n  descriptor (%d bytes):",
                mdesc[k].slot, m->ok ? "report protocol" : "boot protocol", m->id, m->bn, m->bo, m->xs, m->xo, m->ys, m->yo, m->ws, m->wo, mdesc[k].rlen);
        for (int i = 0; i < mdesc[k].rlen && i < 256; i++) kprintf("%s%02x", i % 32 ? " " : "\n  ", mdesc[k].rd[i]);
        kprintf("\n");
    }
    if (!nmdesc) kprintf("No USB mouse.\n");
    if (hpp.d) {
        kprintf("Logitech HID++: devices answering:");
        for (int i = 0; i < hpp.ndev; i++) kprintf(" %d", hpp.devs[i]);
        kprintf("; device %d: %s", hpp.dev, hpp.state ? hpp.state : "");
        if (hpp.hrw) kprintf(" (wheel feature at index %d, mode %d -> %d, multiplier %d)", hpp.hrw, hpp.hrw_old, hpp.hrw_new, hpp.mult);
        kprintf("\n");
        if (hpp.nfeat > 1) {
            kprintf("  features:");
            for (int i = 1; i < hpp.nfeat; i++) kprintf(" %d:%04x", i, hpp.feat[i]);
            kprintf("\n");
        }
    }
    int n = mother_n < 8 ? mother_n : 8;
    kprintf("The last %d reports with a wheel, a button, other bytes, or another report ID (newest last):\n", n);
    for (int j = 0; j < n; j++) mouse_show(&mother[(mother_n - n + j) % 8]);
    n = mlast_n < 4 ? mlast_n : 4;
    kprintf("The last %d reports:\n", n);
    for (int j = 0; j < n; j++) mouse_show(&mlast[(mlast_n - n + j) % 8]);
}

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
/* Logitech receivers (HID++ 2.0). Logitech software (Options, Solaar) can divert a mouse's wheel to HID++
 * notifications, and the mouse keeps that: the normal report then never has a wheel. A thread asks the mouse for
 * its high-resolution wheel feature (0x2121) and sets it back to sending the wheel in the normal report, low
 * resolution, as Linux's driver does (it sets high resolution, which needs scaling). Wheel notifications that
 * still come (feature 0x2121, event 0) are used too. Requests are output report 0x11 (20 bytes):
 * [0x11, device, feature index, function << 4 | software id, parameters...]; the answer comes on the interrupt
 * endpoint with the same software id. */
static int hpp_ask(int fi, int fn, const uint8_t *p, int np) {   /* 0 answered (hpp.ans), -1 error / no answer */
    uint8_t q[20] = { 0x11, (uint8_t)hpp.dev, (uint8_t)fi, (uint8_t)(fn << 4 | HPP_SWID) };
    if (np > 16) np = 16;
    if (np) memcpy(q + 4, p, np);
    hpp.got = 0;
    if (usb_control(hpp.d, 0x21, 0x09, 0x0211, (uint16_t)hpp.iface, 20, q) < 0) return -1;   /* SET_REPORT(output 0x11) */
    for (int t = 0; t < 60 && !hpp.got; t++) task_sleep(10);
    if (!hpp.got) return -1;
    if (hpp.ans[2] == 0xFF || hpp.ans[2] == 0x8F) return -1;     /* (an error answer: 2.0 / 1.0) */
    return 0;
}
static int hpp_feature(uint16_t id) {                  /* the feature's index, 0 if the mouse hasn't got it */
    uint8_t p[2] = { (uint8_t)(id >> 8), (uint8_t)id };
    return hpp_ask(0, 0, p, 2) == 0 ? hpp.ans[4] : 0;
}
static void hpp_features(void) {                       /* hpp.dev's features, by index (for 'mouse') */
    hpp.nfeat = 0;
    hpp.fs = hpp_feature(0x0001);
    if (!hpp.fs || hpp_ask(hpp.fs, 0, 0, 0) != 0) return;
    int n = hpp.ans[4]; if (n > 39) n = 39;
    for (int i = 1; i <= n; i++) {
        uint8_t ix = (uint8_t)i;
        hpp.feat[i] = hpp_ask(hpp.fs, 1, &ix, 1) == 0 ? (uint16_t)(hpp.ans[4] << 8 | hpp.ans[5]) : 0;
    }
    hpp.nfeat = n + 1;
}
static void hpp_thread(void *arg) {
    (void)arg;
    task_sleep(800);
    /* every device on the receiver (a keyboard and a mouse can share one): the first with the wheel feature */
    int first = 0; hpp.ndev = 0;
    for (int i = 1; i <= 6; i++) {
        hpp.dev = i;
        if (hpp_ask(0, 0, (const uint8_t[]){ 0x00, 0x01 }, 2) != 0) continue;   /* (no answer: nothing paired there) */
        hpp.devs[hpp.ndev++] = i;
        if (!first) first = i;
        hpp.hrw = hpp_feature(0x2121);
        if (hpp.hrw) break;
    }
    if (!first) { hpp.dev = -1; hpp.state = "no answer to HID++ (not a Logitech HID++ device?)"; LOG_INFO("USB mouse: %s\n", hpp.state); return; }
    if (!hpp.hrw) {
        hpp.dev = hpp.wheel_dev > 0 ? hpp.wheel_dev : first;   /* (for 'mouse': the one sending notifications, if any) */
        hpp_features();
        hpp.state = "no device here has a high-resolution wheel feature";
        LOG_INFO("USB mouse: HID++: %s\n", hpp.state); return;
    }
    hpp_features();
    if (hpp_ask(hpp.hrw, 0, 0, 0) == 0) hpp.mult = hpp.ans[4] ? hpp.ans[4] : 8;   /* getWheelCapability: multiplier */
    hpp.hrw_old = hpp_ask(hpp.hrw, 1, 0, 0) == 0 ? hpp.ans[4] : -1;               /* getWheelMode */
    uint8_t mode = 0;                                  /* to the normal report (not diverted), low resolution, not inverted */
    hpp_ask(hpp.hrw, 2, &mode, 1);                     /* setWheelMode */
    hpp.hrw_new = hpp_ask(hpp.hrw, 1, 0, 0) == 0 ? hpp.ans[4] : -1;
    hpp.state = hpp.hrw_new == 0 ? "wheel set to the normal mouse report" : "couldn't change the wheel mode";
    LOG_INFO("USB mouse: HID++ device %d, wheel feature at index %d, mode %d -> %d: %s\n", hpp.dev, hpp.hrw, hpp.hrw_old, hpp.hrw_new, hpp.state);
}
/* a HID++ report from the mouse's interface: an answer to hpp_thread, or a wheel notification */
static void hpp_report(const uint8_t *r, int n) {
    if (n < 7 || (r[0] != 0x10 && r[0] != 0x11)) return;
    if (r[0] == 0x11 && (r[3] & 0x0F) == 0 && r[1] >= 1 && r[1] <= 6) hpp.wheel_dev = r[1];   /* (a 2.0 notification: who from) */
    if ((r[3] & 0x0F) == HPP_SWID || ((r[2] == 0xFF || r[2] == 0x8F) && (r[4] & 0x0F) == HPP_SWID)) {
        memcpy(hpp.ans, r, n < 20 ? n : 20); hpp.got = 1; return;
    }
    if (hpp.hrw && r[1] == hpp.dev && r[2] == hpp.hrw && r[3] == 0x00) {   /* wheelMovement: [flags: bit 4 high resolution][delta_v, big-endian] */
        int dv = (int16_t)(r[5] << 8 | r[6]);
        if (r[4] & 0x10) { hpp.wheel_acc += dv; int m = hpp.mult > 0 ? hpp.mult : 8; dv = hpp.wheel_acc / m; hpp.wheel_acc -= dv * m; }
        if (dv) mouse_push(0, 0, mouse_bt, -dv);       /* (the buttons as they are) */
    }
}
static void on_mouse(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) { usb_recover_in(d, dci, cc, on_mouse); return; }
    const uint8_t *r = d->ep_buf[dci];
    struct hidmouse *m = &hm[d->slot & 7];
    if (m->ok) {
        int n = (int)len;
        if (m->id > 0) {
            if (n < 1 || r[0] != m->id) {                       /* another report (HID++, media keys...): kept for 'mouse' */
                struct mrep e = { d->slot, n, 0, 0, 0, 0, {0} }; memcpy(e.r, r, n < 16 ? n : 16);
                mother[mother_n++ % 8] = e;
                if (d == hpp.d) hpp_report(r, n);
                usb_queue_in(d, dci, on_mouse); return;
            }
            r++; n--;
        }
        int bt = hid_field(r, n, m->bo, m->bn, false);
        int dx = hid_field(r, n, m->xo, m->xs, true), dy = hid_field(r, n, m->yo, m->ys, true), w = -hid_field(r, n, m->wo, m->ws, true);
        mouse_note(d->slot, d->ep_buf[dci], (int)len, dx, dy, bt, w);
        mouse_bt = bt & 7; mouse_push(dx, dy, bt & 7, w);
    } else if (len >= 3) {
        const int8_t *b = (const int8_t *)r;
        mouse_note(d->slot, r, (int)len, b[1], b[2], b[0] & 7, len >= 4 ? -b[3] : 0);
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
    bool got = rlen > 0 && rlen <= (int)sizeof rd && usb_control(d, 0x81, 6, 0x2200, iface, (uint16_t)rlen, rd) >= 0;
    if (nmdesc < 4) { mdesc[nmdesc].slot = d->slot; mdesc[nmdesc].rlen = got ? rlen : 0; if (got) memcpy(mdesc[nmdesc].rd, rd, rlen < 256 ? rlen : 256); nmdesc++; }
    if (got && hid_parse_mouse(rd, rlen, m))
        usb_control(d, 0x21, 0x0B, 1, iface, 0, 0);             /* SET_PROTOCOL(report) */
    else usb_control(d, 0x21, 0x0B, 0, iface, 0, 0);            /* SET_PROTOCOL(boot) */
    LOG_INFO("USB mouse: %s\n", m->ok ? (m->ws ? "report protocol, with a wheel" : "report protocol, no wheel") : "boot protocol");
    usb_control(d, 0x21, 0x0A, 0, iface, 0, 0);
    int dci = usb_configure_ep(d, ep, ss);
    if (dci < 0) return;
    d->driver = d->driver ? "keyboard + mouse" : "mouse";
    mouse_present = true;
    usb_queue_in(d, dci, on_mouse);
    if (d->vid == 0x046D && m->ok && !hpp.d) {          /* Logitech: see that its wheel isn't diverted (above) */
        memset(&hpp, 0, sizeof hpp); hpp.d = d; hpp.iface = iface; hpp.state = "asking...";
        task_create_kernel("hid++", hpp_thread, 0);
    }
}
