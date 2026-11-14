/* Host-side test for the Bluetooth driver: a fake USB Bluetooth controller that
 * answers HCI commands and streams events back in 16-byte USB packets. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../src/usb.h"

static usb_xfer_cb evt_cb; static struct usb_dev dev; static uint8_t epbuf[64];
static uint8_t pending[64][260]; static int np, rp; static int sent_cmds[64], ncmds;

static void q(const uint8_t *e, int n) { memcpy(pending[np++], e, n); }
static void cc(uint16_t op, const uint8_t *ret, int n) {
    uint8_t e[260] = {0x0E, (uint8_t)(3 + n), 1, op & 0xFF, op >> 8};
    memcpy(e + 5, ret, n); q(e, 5 + n);
}
static void cs(uint16_t op) { uint8_t e[] = {0x0F, 4, 0, 1, op & 0xFF, op >> 8}; q(e, 6); }

int usb_control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data) {
    uint8_t *p = data; uint16_t op = p[0] | p[1] << 8;
    sent_cmds[ncmds++] = op;
    if (rt != 0x20 || req != 0 || p[2] != len - 3) { printf("FAIL: bad command framing\n"); return -1; }
    switch (op) {
    case 0x1001: { uint8_t r[] = {0, 9, 0x34, 0x12, 9, 93, 0, 0x61, 0x87}; cc(op, r, sizeof r); break; }
    case 0x1009: { uint8_t r[] = {0, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}; cc(op, r, sizeof r); break; }
    case 0x0C14: { uint8_t r[249] = {0}; strcpy((char *)r + 1, "TestAdapter"); cc(op, r, sizeof r); break; }
    case 0x1003: { uint8_t r[9] = {0, 0xFF, 0xFF, 0x8F, 0xFE, 0xDB, 0xFF, 0x5B, 0x87}; cc(op, r, sizeof r); break; }
    case 0x0401: {
        cs(op);
        /* Extended Inquiry Result: name "Test Phone", Phone class, RSSI -48 */
        uint8_t e[257] = {0x2F, 255, 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 1, 0, 0x0C, 0x02, 0x5A, 0x34, 0x12, (uint8_t)-48};
        const char *nm = "Test Phone"; e[17] = strlen(nm) + 1; e[18] = 0x09; memcpy(e + 19, nm, strlen(nm));
        q(e, 257);
        /* Inquiry Result with RSSI, no name (needs remote name request), Audio class */
        uint8_t r[] = {0x22, 15, 1, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 1, 0, 0x04, 0x04, 0x24, 0x00, 0x10, (uint8_t)-70};
        q(r, sizeof r);
        /* LE advertising report: name "Fitness Band", RSSI -60 */
        uint8_t le[64] = {0x3E, 0, 0x02, 1, 0x00, 0x01, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
        const char *ln = "Fitness Band"; int dl = 3 + 2 + strlen(ln);
        le[12] = dl; uint8_t *ad = le + 13;
        ad[0] = 2; ad[1] = 0x01; ad[2] = 0x06; ad[3] = strlen(ln) + 1; ad[4] = 0x09; memcpy(ad + 5, ln, strlen(ln));
        le[13 + dl] = (uint8_t)-60; le[1] = 13 + dl + 1 - 2;
        q(le, 13 + dl + 1);
        uint8_t done[] = {0x01, 1, 0}; q(done, 3);
        break;
    }
    case 0x0419: {
        cs(op);
        uint8_t e[257] = {0x07, 255, 0}; memcpy(e + 3, p + 3, 6); strcpy((char *)e + 9, "Car Stereo"); q(e, 257);
        break;
    }
    default: cc(op, (uint8_t[]){0}, 1);
    }
    return 0;
}

int usb_queue_in(struct usb_dev *d, int dci, usb_xfer_cb cb) { evt_cb = cb; return 0; }
static int off;
void xhci_poll(void) {
    if (!evt_cb || rp == np) return;
    uint8_t *e = pending[rp]; int total = 2 + e[1];
    int n = total - off > 16 ? 16 : total - off;   /* 16-byte max packet, like real dongles */
    memcpy(epbuf, e + off, n); off += n;
    if (off >= total) { rp++; off = 0; }
    usb_xfer_cb cb = evt_cb; evt_cb = 0;
    cb(&dev, 3, n < 16 ? CC_SHORT : CC_SUCCESS, n);
}
uint64_t uptime_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int input_poll(void) { return -1; }
void kyield(void) {}
bool sched_running;
struct task *current;
void console_set_color(int c) {}
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }

int main(void) {
    dev.ep_buf[3] = epbuf; dev.vid = 0x0BDA; dev.pid = 0xB85B;
    strcpy(dev.manufacturer, "Realtek"); strcpy(dev.product, "Bluetooth Radio");
    bt_attach(&dev, 0, 3);
    bt_info();
    bt_scan(3);
    return 0;
}
