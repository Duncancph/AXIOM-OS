/* CPU setup: GDT with user segments, TSS, local APIC timer, write-combining framebuffer. */
#include "kernel.h"

static inline uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static uint8_t bsp_df_stack[8192] __attribute__((aligned(16)));

/* Each core has its own GDT (for its own TSS: the kernel stack to use when an app is interrupted) and
 * keeps a pointer to its struct cpu in %gs (swapped with the app's on every way in and out of the kernel). */
void cpu_setup(struct cpu *c) {
    uint64_t *gdt = c->gdt;
    struct tss64 *tss = &c->tss;
    c->self = c;
    if (!c->df_stack) c->df_stack = bsp_df_stack;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data (DPL 3) */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code (DPL 3) */
    uint64_t b = (uint64_t)tss, l = sizeof *tss - 1;
    gdt[5] = (l & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((l >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
    gdt[6] = b >> 32;
    tss->iomap = sizeof *tss;          /* no I/O permission bitmap: user code can't touch ports */
    tss->ist[0] = (uint64_t)c->df_stack + 8192;
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) p = { 7 * 8 - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "xor %%eax, %%eax\n mov %%ax, %%fs\n mov %%ax, %%gs\n"
        "pushq $0x08\n lea 1f(%%rip), %%rax\n pushq %%rax\n lretq\n1:\n"
        "mov $0x28, %%ax\n ltr %%ax\n" ::"m"(p) : "rax", "memory");
    wrmsr(0xC0000101, (uint64_t)c);   /* GS base: this core */
    wrmsr(0xC0000102, 0);             /* the apps' GS base (swapgs) */
    linux_cpu_init();
}

void gdt_init(void) { cpu_setup(&cpus[0]); }

void tss_set_rsp0(uint64_t rsp) { struct cpu *c = cpu_self(); c->tss.rsp0 = rsp; c->sc_kstack = rsp; }

/* ---------------- local APIC ---------------- */
static volatile uint32_t *lapic;
static bool x2apic;
static uint32_t timer_count;
static uint32_t lapic_r(uint32_t reg) { return x2apic ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : lapic[reg / 4]; }
static void lapic_w(uint32_t reg, uint32_t v) { if (x2apic) wrmsr(0x800 + (reg >> 4), v); else lapic[reg / 4] = v; }

void lapic_eoi(void) { lapic_w(0xB0, 0); }

/* Set up this core's local APIC (and measure its timer) without starting the timer yet. */
static bool lapic_ready;
void lapic_init(uint32_t hz) {
    if (lapic_ready) return;
    lapic_ready = true;
    /* remap + mask the legacy PICs so stray IRQs land on harmless vectors 0x20-0x2F */
    outb(0x20, 0x11); outb(0xA0, 0x11); outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 4); outb(0xA1, 2); outb(0x21, 1); outb(0xA1, 1);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);

    uint64_t base = rdmsr(0x1B);
    x2apic = base & (1 << 10);
    wrmsr(0x1B, base | (1 << 11));
    if (!x2apic) {
        uint64_t phys = base & 0xFFFFF000ULL;
        extern uint64_t boot_pd[];
        if (phys < 0x100000000ULL) boot_pd[phys >> 21] |= 0x18;   /* uncached */
        else map_mmio(phys, 4096);
        __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
        lapic = mmio_virt(phys);
    }
    lapic_w(0xF0, 0x1FF);              /* enable, spurious vector 0xFF */
    lapic_w(0x80, 0);                  /* task priority: accept all */
    lapic_w(0x3E0, 0x3);               /* divide by 16 */
    lapic_w(0x320, 0x10000 | 32);      /* masked, one-shot, for calibration */
    lapic_w(0x380, 0xFFFFFFFF);
    sleep_ms(10);
    uint32_t per10ms = 0xFFFFFFFF - lapic_r(0x390);
    timer_count = (uint32_t)((uint64_t)per10ms * 100 / hz);
    lapic_w(0x320, 0x10000 | 32);      /* masked until the scheduler starts */
}

void lapic_timer_start(uint32_t hz) {
    lapic_init(hz);
    lapic_w(0x320, (1 << 17) | 32);    /* periodic, vector 32 */
    lapic_w(0x380, timer_count);
}

/* the same on another core: its own local APIC, same timer rate */
void lapic_ap_init(void) {
    uint64_t base = rdmsr(0x1B);
    wrmsr(0x1B, base | (1 << 11) | (x2apic ? 1 << 10 : 0));
    lapic_w(0xF0, 0x1FF);
    lapic_w(0x80, 0);
    lapic_w(0x3E0, 0x3);
    lapic_w(0x320, (1 << 17) | 32);
    lapic_w(0x380, timer_count);
}
uint32_t lapic_id(void) { return x2apic ? lapic_r(0x20) : lapic_r(0x20) >> 24; }
void lapic_ipi(uint32_t id, uint32_t low) {
    if (x2apic) { wrmsr(0x830, (uint64_t)id << 32 | low); return; }
    lapic_w(0x310, id << 24);
    lapic_w(0x300, low);
    for (int i = 0; i < 100000 && (lapic_r(0x300) & (1 << 12)); i++) cpu_relax();   /* delivered */
}

/* Make the framebuffer write-combining (much faster screen updates on real hardware). */
void fb_write_combining(uint64_t addr, uint64_t size) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1 << 16))) return;                     /* no PAT */
    uint64_t pat = rdmsr(0x277);
    pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);   /* PA1 = write-combining */
    wrmsr(0x277, pat);
    extern uint64_t boot_pd[];
    for (uint64_t p = addr & ~0x1FFFFFULL; p < addr + size; p += 0x200000) {
        if (p < 0x100000000ULL && (boot_pd[p >> 21] & 0x80)) boot_pd[p >> 21] = (boot_pd[p >> 21] & ~0x18ULL) | 0x08;
        else if (p < 0x100000000ULL) {                     /* (split into 4 KiB pages: mem_protect_holes) the screen's ones */
            uint64_t *pt = (uint64_t *)(boot_pd[p >> 21] & ~0xFFFULL);
            for (int k = 0; k < 512; k++) { uint64_t a = p + ((uint64_t)k << 12); if (a >= (addr & ~0xFFFULL) && a < addr + size) pt[k] = (pt[k] & ~0x18ULL) | 0x08; }
        }
        else map_mmio_flags(p, 0x200000, 0x08);
    }
    __asm__ volatile("wbinvd; mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
}
