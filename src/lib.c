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
    for (;;) __asm__ volatile("cli; hlt");
}
