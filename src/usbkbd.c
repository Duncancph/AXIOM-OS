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

/* USB HID mouse (boot protocol): byte 0 buttons, 1 dx, 2 dy, [3 wheel] */
static void on_mouse(struct usb_dev *d, int dci, int cc, uint32_t len) {
    if (cc != CC_SUCCESS && cc != CC_SHORT) { usb_recover_in(d, dci, cc, on_mouse); return; }
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
