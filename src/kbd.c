/* Keyboard input: PS/2 (polled, scancode set 1) plus a shared queue that the
 * USB keyboard driver and the serial port also feed. */
#include "kernel.h"

bool ps2_present;
static int q[256]; static uint8_t qh, qt;
static bool shift, ctrl, caps, ext;

static const char sc_lo[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const char sc_hi[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};

void input_push(int c) { if ((uint8_t)(qh + 1) != qt) { q[qh++] = c; } }

void ps2_init(void) {
    uint8_t st = inb(0x64);
    ps2_present = st != 0xFF;
    if (!ps2_present) return;
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);   /* flush */
}

static void ps2_poll(void) {
    if (!ps2_present) return;
    for (int n = 0; n < 16; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) return;
        uint8_t sc = inb(0x60);
        if (st & 0x20) continue;   /* mouse byte */
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
