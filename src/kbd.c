/* Keyboard input: PS/2 (polled, scancode set 1) plus a shared queue that the
 * USB keyboard driver and the serial port also feed. */
#include "kernel.h"

bool ps2_present, mouse_present;
static int q[256]; static uint8_t qh, qt;
static struct { int16_t dx, dy; uint8_t b; int8_t w; } mq[64]; static uint8_t mh, mt;
static uint8_t mpkt[4]; static int midx;
static bool shift, ctrl, caps, ext;

static const char sc_lo[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const char sc_hi[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};

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
    if (mouse_cmd(0xF6) && mouse_cmd(0xF4)) mouse_present = true;
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);
}

static void ps2_mouse_byte(uint8_t b) {
    if (midx == 0 && !(b & 0x08)) return;       /* resync: bit 3 is always set in byte 0 */
    mpkt[midx++] = b;
    if (midx < 3) return;
    midx = 0;
    if (mpkt[0] & 0xC0) return;                 /* overflow */
    int dx = mpkt[1] - ((mpkt[0] << 4) & 0x100);
    int dy = mpkt[2] - ((mpkt[0] << 3) & 0x100);
    mouse_push(dx, -dy, mpkt[0] & 7, 0);
}

void ps2_poll(void) {
    if (!ps2_present) return;
    for (int n = 0; n < 16; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) return;
        uint8_t sc = inb(0x60);
        if (st & 0x20) { ps2_mouse_byte(sc); continue; }
        if (sc == 0xE0) { ext = true; continue; }
        bool up = sc & 0x80; sc &= 0x7F;
        if (ext) {
            ext = false;
            if (sc == 0x1D) { ctrl = !up; continue; }
            if (up) continue;
            switch (sc) {
            case 0x48: input_push(KEY_UP); break;     case 0x50: input_push(KEY_DOWN); break;
            case 0x4B: input_push(KEY_LEFT); break;   case 0x4D: input_push(KEY_RIGHT); break;
            case 0x47: input_push(KEY_HOME); break;   case 0x4F: input_push(KEY_END); break;
            case 0x53: input_push(KEY_DEL); break;    case 0x1C: input_push('\n'); break;
            case 0x5B: case 0x5C: input_push(KEY_SUPER); break;
            case 0x49: input_push(KEY_PGUP); break;   case 0x51: input_push(KEY_PGDN); break;
            }
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { shift = !up; continue; }
        if (sc == 0x1D) { ctrl = !up; continue; }
        if (up) continue;
        if (sc == 0x3A) { caps = !caps; continue; }
        char c = (shift ? sc_hi : sc_lo)[sc];
        if (!c) continue;
        if (caps && c >= 'a' && c <= 'z') c -= 32;
        else if (caps && c >= 'A' && c <= 'Z') c += 32;
        if (ctrl && ((c | 32) >= 'a' && (c | 32) <= 'z')) c = (c | 32) - 'a' + 1;
        input_push(c);
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
