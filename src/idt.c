/* Interrupt descriptor table and dispatch: CPU exceptions, the scheduler timer and
 * the system call gate (int 0x80, the only door from programs into the kernel). */
#include "kernel.h"

struct idt_entry { uint16_t off0, sel; uint8_t ist, type; uint16_t off1; uint32_t off2, zero; } __attribute__((packed));
static struct idt_entry idt[256];
extern uint64_t isr_table[256];

static const char *names[] = {"Divide error","Debug","NMI","Breakpoint","Overflow","Bound range","Invalid opcode",
    "Device not available","Double fault","Coprocessor","Invalid TSS","Segment not present","Stack fault",
    "General protection fault","Page fault","Reserved","x87 FP error","Alignment check","Machine check","SIMD FP error"};

static void user_fault(struct frame *f) {
    uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    char msg[160];
    const char *what;
    switch (f->vec) {
    case 14:
        if (cr2 < USER_BASE || cr2 >= USER_END) what = "It tried to read or write protected kernel memory";
        else if (cr2 < 0x1000 + USER_BASE) what = "It used an invalid (null) pointer";
        else what = "It accessed memory it doesn't own";
        break;
    case 13: what = "It tried to run a privileged instruction"; break;
    case 6:  what = "It ran an invalid instruction"; break;
    case 0:  what = "It divided by zero"; break;
    default: what = f->vec < 20 ? names[f->vec] : "CPU exception"; break;
    }
    ksnprintf(msg, sizeof msg, "%s and was stopped. Other apps are unaffected.", what);
    kprintf("[protect] '%s' (pid %d): %s at rip=%lx addr=%lx -> terminated\n", current->name, current->pid,
            f->vec < 20 ? names[f->vec] : "exception", f->rip, cr2);
    char title[64]; ksnprintf(title, sizeof title, "%s crashed", current->name);
    desktop_notify(title, msg);
    task_exit(-1);
}

void isr_dispatch(struct frame *f) {
    uint64_t v = f->vec;
    if (v == 32) { sched_tick(f); return; }
    if (v == 0x80) {
        f->rax = syscall_dispatch(f);
        if (current->killed) task_exit(-1);
        return;
    }
    if (v >= 32) return;                       /* spurious / stray legacy IRQs */
    if ((f->cs & 3) == 3) { user_fault(f); return; }

    uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    console_set_color(C_LRED);
    kprintf("\n\n*** THANE KERNEL PANIC: %s (vector %lu) ***\n", v < 20 ? names[v] : "Exception", v);
    console_set_color(C_LGRAY);
    kprintf("err=%lx rip=%lx rsp=%lx cr2=%lx task=%s\n", f->err, f->rip, f->rsp, cr2, current ? current->name : "boot");
    kprintf("rax=%lx rbx=%lx rcx=%lx rdx=%lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("System halted. Press the power button to restart.\n");
    char a[96], b[96], c[96];
    ksnprintf(a, sizeof a, "AXIOM stopped: %s (vector %lu) in '%s'", v < 20 ? names[v] : "exception", v, current ? current->name : "boot");
    ksnprintf(b, sizeof b, "rip=%lx cr2=%lx err=%lx build %d", f->rip, cr2, f->err, OS_BUILD);
    uint64_t *bp = (uint64_t *)f->rbp; int k = ksnprintf(c, sizeof c, "called from");
    for (int i = 0; i < 6 && current && current->kstack && (uint8_t *)bp >= current->kstack && (uint64_t)bp + 16 <= current->kstack_top; i++) { k += ksnprintf(c + k, sizeof c - k, " %lx", bp[1]); bp = (uint64_t *)bp[0]; }
    const char *l[4] = { a, b, c, "Please photograph this for Claude. Press the power button to restart." };
    desktop_emergency(l, 4);
    for (;;) __asm__ volatile("cli; hlt");
}

void idt_init(void) {
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
    for (int i = 0; i < 256; i++) {
        uint64_t a = isr_table[i];
        uint8_t type = i == 0x80 ? 0xEE : 0x8E;    /* 0x80 callable from ring 3 */
        idt[i] = (struct idt_entry){ (uint16_t)a, 0x08, i == 8 ? 1 : 0, type, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
    }
    struct { uint16_t lim; uint64_t base; } __attribute__((packed)) p = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" ::"m"(p));
}
