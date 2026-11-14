/* Exception handling. EmberOS runs with hardware interrupts off and polls
 * devices, which is far more robust across real PCs (legacy PIC vs APIC). */
#include "kernel.h"

struct idt_entry { uint16_t off0, sel; uint8_t ist, type; uint16_t off1; uint32_t off2, zero; } __attribute__((packed));
static struct idt_entry idt[32];
extern uint64_t isr_table[32];

struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err, rip, cs, rflags, rsp, ss;
};

static const char *names[] = {"Divide error","Debug","NMI","Breakpoint","Overflow","Bound range","Invalid opcode",
    "Device not available","Double fault","Coprocessor","Invalid TSS","Segment not present","Stack fault",
    "General protection fault","Page fault","Reserved","x87 FP error","Alignment check","Machine check","SIMD FP error"};

void isr_handler(struct frame *f) {
    uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    console_set_color(C_LRED);
    kprintf("\n\n*** KERNEL PANIC: %s (vector %lu) ***\n", f->vec < 20 ? names[f->vec] : "Exception", f->vec);
    console_set_color(C_LGRAY);
    kprintf("err=%lx rip=%lx rsp=%lx cr2=%lx\n", f->err, f->rip, f->rsp, cr2);
    kprintf("rax=%lx rbx=%lx rcx=%lx rdx=%lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("System halted. Press the power button to restart.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void idt_init(void) {
    /* Mask the legacy PICs; we never enable interrupts. */
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
    for (int i = 0; i < 32; i++) {
        uint64_t a = isr_table[i];
        idt[i] = (struct idt_entry){ (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
    }
    struct { uint16_t lim; uint64_t base; } __attribute__((packed)) p = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" ::"m"(p));
}
