/* Keyboard input: PS/2 (polled, scancode set 1) plus a shared queue that the
 * USB keyboard driver and the serial port also feed. */
#include "kernel.h"

bool ps2_present, mouse_present;
static int q[256]; static uint8_t qh, qt;
static struct { int16_t dx, dy; uint8_t b; int8_t w; } mq[64]; static uint8_t mh, mt;
static uint8_t mpkt[4]; static int midx, mpkt_len = 3;   /* 4: an IntelliMouse (its wheel in byte 3) */

void input_push(int c) { if ((uint8_t)(qh + 1) != qt) { q[qh++] = c; } }

int input_pop(void) { if (qh == qt) return -1; return q[qt++]; }

void mouse_push(int dx, int dy, int buttons, int wheel) {
    if (((mh + 1) & 63) == mt) return;
    mq[mh].dx = dx; mq[mh].dy = dy; mq[mh].b = buttons; mq[mh].w = wheel;
    mh = (mh + 1) & 63;
}
bool mouse_pop(int *dx, int *dy, int *buttons, int *wheel) {
    if (mh == mt) return false;
    *dx = mq[mt].dx; *dy = mq[mt].dy; *buttons = mq[mt].b; *wheel = mq[mt].w;
    mt = (mt + 1) & 63;
    return true;
}

static bool ps2_wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true; return false; }
static int ps2_read_timeout(void) {
    uint64_t end = uptime_ms() + 50;
    while (uptime_ms() < end) if (inb(0x64) & 1) return inb(0x60);
    return -1;
}
static bool mouse_cmd(uint8_t c) {
    if (!ps2_wait_write()) return false;
    outb(0x64, 0xD4);
    if (!ps2_wait_write()) return false;
    outb(0x60, c);
    return ps2_read_timeout() == 0xFA;
}

void ps2_init(void) {
    uint8_t st = inb(0x64);
    ps2_present = st != 0xFF;
    if (!ps2_present) return;
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);   /* flush */
    /* Enable the auxiliary (mouse/touchpad) port. */
    if (!ps2_wait_write()) return;
    outb(0x64, 0xA8);
    ps2_wait_write(); outb(0x64, 0x20);
    int cfg = ps2_read_timeout();
    if (cfg >= 0) {
        cfg = (cfg | 0x01 | 0x02) & ~0x20;   /* keep IRQ bits (harmless, masked), enable mouse clock */
        cfg &= ~0x03;                        /* we poll: no IRQs */
        ps2_wait_write(); outb(0x64, 0x60);
        ps2_wait_write(); outb(0x60, (uint8_t)cfg);
    }
    if (mouse_cmd(0xF6)) {
        /* the IntelliMouse sequence: sample rates 200, 100, 80; a mouse with a wheel then gives ID 3 (or 4) and sends
         * its wheel in a fourth byte. (A USB mouse the firmware presents as PS/2 does this too.) */
        if (mouse_cmd(0xF3) && mouse_cmd(200) && mouse_cmd(0xF3) && mouse_cmd(100) && mouse_cmd(0xF3) && mouse_cmd(80) && mouse_cmd(0xF2)) {
            int id = ps2_read_timeout();
            if (id == 3 || id == 4) mpkt_len = 4;
        }
        mouse_cmd(0xF3); mouse_cmd(100);                    /* (back to the usual 100 reports a second) */
        if (mouse_cmd(0xF4)) { mouse_present = true; LOG_INFO("PS/2 mouse: %s\n", mpkt_len == 4 ? "with a wheel" : "no wheel"); }
    }
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);
}

static void ps2_mouse_byte(uint8_t b) {
    if (midx == 0 && !(b & 0x08)) return;       /* resync: bit 3 is always set in byte 0 */
    mpkt[midx++] = b;
    if (midx < mpkt_len) return;
    midx = 0;
    if (mpkt[0] & 0xC0) return;                 /* overflow */
    int dx = mpkt[1] - ((mpkt[0] << 4) & 0x100);
    int dy = mpkt[2] - ((mpkt[0] << 3) & 0x100);
    int w = mpkt_len == 4 ? (int8_t)(mpkt[3] << 4) >> 4 : 0;   /* (4 bits, signed: ID 4 uses the rest for buttons 4-5) */
    mouse_push(dx, -dy, mpkt[0] & 7, w);        /* (positive: down, as AXIOM's wheel) */
}

void ps2_poll(void) {
    if (!ps2_present) return;
    for (int n = 0; n < 16; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) return;
        uint8_t sc = inb(0x60);
        if (st & 0x20) { ps2_mouse_byte(sc); continue; }
        kbd_ps2_scancode(sc);
    }
}

int input_poll(void) {
    ps2_poll();
    xhci_poll();
    int s = serial_getc();
    if (s >= 0) {
        if (s == '\r') s = '\n';
        else if (s == 127) s = '\b';
        input_push(s);
    }
    if (qh == qt) return -1;
    return q[qt++];
}

int input_getc(void) {
    int c;
    while ((c = input_poll()) < 0) cpu_relax();
    return c;
}
