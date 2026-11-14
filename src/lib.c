#include "kernel.h"

void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_write(buf);
}

void panic(const char *msg) {
    __asm__ volatile("cli");
    kprintf("\n*** KERNEL PANIC: %s ***\nSystem halted.\n", msg);
    const char *l[2] = { "DuncOS stopped (kernel panic). Please photograph this for Claude:", msg };
    desktop_emergency(l, 2);
    for (;;) __asm__ volatile("cli; hlt");
}
