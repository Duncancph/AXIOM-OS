#include "kernel.h"
#define COM1 0x3F8
static bool serial_ok;

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);
    serial_ok = inb(COM1 + 5) != 0xFF;   /* 0xFF = no UART present */
}

void serial_putc(char c) {
    if (!serial_ok) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) cpu_relax();
    outb(COM1, (uint8_t)c);
}

int serial_getc(void) {
    if (!serial_ok || !(inb(COM1 + 5) & 1)) return -1;
    return inb(COM1);
}
