/* USB HID keyboard (boot protocol). */
#include "usb.h"

static uint8_t prev[8];
static int kbd_dci;

static const char lo[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
static bool caps;

static void on_report(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) return;   /* device gone / error: stop polling */
    uint8_t *r = d->ep_buf[dci];
    if (len >= 3 && r[2] != 1) {   /* 1 = rollover error */
        bool shift = r[0] & 0x22, ctrl = r[0] & 0x11;
        for (int i = 2; i < 8 && i < (int)len; i++) {
            uint8_t k = r[i];
            if (!k) continue;
            bool seen = false;
            for (int j = 2; j < 8; j++) if (prev[j] == k) seen = true;
            if (seen) continue;
            if (k >= 0x04 && k <= 0x38) {
                char c = (shift ? hi : lo)[k - 4];
                if (caps && c >= 'a' && c <= 'z') c -= 32; else if (caps && c >= 'A' && c <= 'Z') c += 32;
                if (ctrl && (c | 32) >= 'a' && (c | 32) <= 'z') c = (c | 32) - 'a' + 1;
                input_push(c);
            } else if (k == 0x39) caps = !caps;
            else if (k == 0x4F) input_push(KEY_RIGHT);
            else if (k == 0x50) input_push(KEY_LEFT);
            else if (k == 0x51) input_push(KEY_DOWN);
            else if (k == 0x52) input_push(KEY_UP);
            else if (k == 0x4A) input_push(KEY_HOME);
            else if (k == 0x4D) input_push(KEY_END);
            else if (k == 0x4C) input_push(KEY_DEL);
            else if (k == 0x4B) input_push(KEY_PGUP);
            else if (k == 0x4E) input_push(KEY_PGDN);
            else if (k == 0x58) input_push('\n');
            else if (k == 0xE3 || k == 0xE7) input_push(KEY_SUPER);
        }
        if ((r[0] & 0x88) && !(prev[0] & 0x88)) input_push(KEY_SUPER);
        memcpy(prev, r, 8);
    }
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

/* USB HID mouse (boot protocol): byte 0 buttons, 1 dx, 2 dy, [3 wheel] */
static void on_mouse(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) return;
    int8_t *r = (int8_t *)d->ep_buf[dci];
    if (len >= 3) mouse_push(r[1], r[2], r[0] & 7, len >= 4 ? -r[3] : 0);
    usb_queue_in(d, dci, on_mouse);
}

void usbmouse_attach(struct usb_dev *d, int iface) {
    const uint8_t *ss;
    const uint8_t *ep = usb_find_endpoint(d, iface, 3, true, &ss);
    if (!ep) return;
    usb_control(d, 0x21, 0x0B, 0, iface, 0, 0);   /* SET_PROTOCOL(boot) */
    usb_control(d, 0x21, 0x0A, 0, iface, 0, 0);
    int dci = usb_configure_ep(d, ep, ss);
    if (dci < 0) return;
    d->driver = d->driver ? "keyboard + mouse" : "mouse";
    mouse_present = true;
    usb_queue_in(d, dci, on_mouse);
}
