/* Interrupt descriptor table and dispatch: CPU exceptions, the scheduler timer and
 * the system call gate (int 0x80, the only door from programs into the kernel). */
#include "kernel.h"

struct idt_entry { uint16_t off0, sel; uint8_t ist, type; uint16_t off1; uint32_t off2, zero; } __attribute__((packed));
static struct idt_entry idt[256];
extern uint64_t isr_table[256];

static const char *names[] = {"Divide error","Debug","NMI","Breakpoint","Overflow","Bound range","Invalid opcode",
    "Device not available","Double fault","Coprocessor","Invalid TSS","Segment not present","Stack fault",
    "General protection fault","Page fault","Reserved","x87 FP error","Alignment check","Machine check","SIMD FP error"};

static void kernel_panic(struct frame *f);
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
    case 13: {                                             /* (int3 from a program faults this way: its own failed check) */
        uint8_t op = 0;
        if (user_ok(current->pml4, (const void *)f->rip, 1, false)) op = *(volatile uint8_t *)f->rip;
        what = op == 0xCC ? "It stopped itself (one of its own checks failed)" : "It used a bad memory address (or a privileged instruction)";
        break;
    }
    case 6:  what = "It ran an invalid instruction"; break;
    case 0:  what = "It divided by zero"; break;
    default: what = f->vec < 20 ? names[f->vec] : "CPU exception"; break;
    }
    ksnprintf(msg, sizeof msg, "%s and was stopped (at %lx). Other apps are unaffected.", what, f->rip);
    kprintf("[protect] '%s' (pid %d): %s at rip=%lx addr=%lx -> terminated\n", current->name, current->pid,
            f->vec < 20 ? names[f->vec] : "exception", f->rip, cr2);
    if (current->lx) {                                     /* a Linux program: which library, and where the stack came from */
        linux_where("crashed in", f->rip);
        uint64_t *sp = (uint64_t *)f->rsp;
        for (int i = 0, shown = 0; i < 64 && shown < 8 && user_ok(current->pml4, sp + i, 8, false); i++)
            if (linux_where_quiet(sp[i])) { linux_where("  called from", sp[i]); shown++; }
    }
    char title[64]; ksnprintf(title, sizeof title, "%s crashed", current->name);
    desktop_notify(title, msg);
    task_exit(-1);
}

static void dispatch(struct frame *f) {
    uint64_t v = f->vec;
    if (v == 32) { sched_tick(f); return; }
    if (v == 0x80) {
        f->rax = syscall_dispatch(f);
        if (current->killed) task_exit(-1);
        return;
    }
    if (v == 0x81) {                           /* a Linux program's system call */
        f->rax = linux_syscall(f);
        if (current->killed) task_exit(-1);
        return;
    }
    if (v == 0xF0) { lapic_eoi(); return; }    /* another core woke this one */
    if (v >= 32) return;                       /* spurious / stray legacy IRQs */
    if (v == 14 && (f->cs & 3) == 3 && current->mm) {   /* a page of a memory area, used for the first time */
        uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        uint64_t dummy = 0;
        if (mm_fault(current->mm, cr2, f->err & 2, f->err & 16, current->proc ? &current->proc->pages : &dummy)) return;
    }
    if ((f->cs & 3) == 3) { user_fault(f); return; }
    kernel_panic(f);
}

/* Every way into the kernel: take the kernel lock unless this core already holds it (it was in the
 * kernel); give it back on the way out. The task may come back on another core: whichever core it is,
 * it holds the lock while in the kernel, and returning to the app is where it lets go. */
void isr_dispatch(struct frame *f) {
    if (f->vec == 0xF1) { tlb_ipi(); return; }     /* TLB shootdown: without the lock (its sender holds it) */
    if ((f->cs & 3) == 3) cpu_self()->in_user = 0;
    if ((f->cs & 3) == 3 && current) current->last_rip = f->rip;
    bool took = !cpu_self()->bkl_held;
    if (took) bkl_lock();
    dispatch(f);
    if ((f->cs & 3) == 3 && current && current->lx && (current->sig_pending & ~current->sig_blocked)) linux_signals(f);
    if (took) {
        if ((f->cs & 3) == 3) tlb_sync();          /* back to the app: fresh translations if its memory changed */
        bkl_unlock();
    }
}

static void kernel_panic(struct frame *f) {
    uint64_t v = f->vec;

    uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    console_set_color(C_LRED);
    kprintf("\n\n*** THANE KERNEL PANIC: %s (vector %lu) ***\n", v < 20 ? names[v] : "Exception", v);
    console_set_color(C_LGRAY);
    kprintf("err=%lx rip=%lx rsp=%lx cr2=%lx task=%s\n", f->err, f->rip, f->rsp, cr2, current ? current->name : "boot");
    kprintf("rax=%lx rbx=%lx rcx=%lx rdx=%lx pid=%d last syscall %lu\n", f->rax, f->rbx, f->rcx, f->rdx, current ? current->pid : 0, current ? current->sig_sysno : 0);
    {   /* kernel code addresses on its stack (for finding where it came from) */
        extern char kernel_end[]; uint64_t *sp = (uint64_t *)f->rsp; int shown = 0;
        kprintf("stack:");
        for (int i = 0; i < 256 && shown < 16; i++) {
            if (!current || !current->kstack || (uint8_t *)(sp + i) < current->kstack || (uint64_t)(sp + i) + 8 > current->kstack_top) break;
            uint64_t w = sp[i]; if (w >= 0x100000 && w < (uint64_t)kernel_end) { kprintf(" %lx", w); shown++; }
        }
        kprintf("\n");
    }
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

void idt_load(void) {
    struct { uint16_t lim; uint64_t base; } __attribute__((packed)) p = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" ::"m"(p));
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
