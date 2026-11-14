/* CPU setup: GDT with user segments, TSS, local APIC timer, write-combining framebuffer. */
#include "kernel.h"

static uint64_t gdt[7];
static struct tss {
    uint32_t r0; uint64_t rsp0, rsp1, rsp2, r1; uint64_t ist[7]; uint64_t r2; uint16_t r3, iomap;
} __attribute__((packed)) tss;
static uint8_t double_fault_stack[8192] __attribute__((aligned(16)));

static inline uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data (DPL 3) */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code (DPL 3) */
    uint64_t b = (uint64_t)&tss, l = sizeof tss - 1;
    gdt[5] = (l & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((l >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
    gdt[6] = b >> 32;
    tss.iomap = sizeof tss;           /* no I/O permission bitmap: user code can't touch ports */
    tss.ist[0] = (uint64_t)double_fault_stack + sizeof double_fault_stack;
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) p = { sizeof gdt - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "pushq $0x08\n lea 1f(%%rip), %%rax\n pushq %%rax\n lretq\n1:\n"
        "mov $0x28, %%ax\n ltr %%ax\n" ::"m"(p) : "rax", "memory");
}

void tss_set_rsp0(uint64_t rsp) { tss.rsp0 = rsp; }

/* ---------------- local APIC ---------------- */
static volatile uint32_t *lapic;
static bool x2apic;
static uint32_t lapic_r(uint32_t reg) { return x2apic ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : lapic[reg / 4]; }
static void lapic_w(uint32_t reg, uint32_t v) { if (x2apic) wrmsr(0x800 + (reg >> 4), v); else lapic[reg / 4] = v; }

void lapic_eoi(void) { lapic_w(0xB0, 0); }

void lapic_timer_start(uint32_t hz) {
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
    lapic_w(0x320, (1 << 17) | 32);    /* periodic, vector 32 */
    lapic_w(0x380, (uint32_t)((uint64_t)per10ms * 100 / hz));
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
        if (p < 0x100000000ULL) boot_pd[p >> 21] = (boot_pd[p >> 21] & ~0x18ULL) | 0x08;
        else map_mmio_flags(p, 0x200000, 0x08);
    }
    __asm__ volatile("wbinvd; mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
}
