/* Physical memory: a simple bump allocator over the largest usable RAM region
 * below 4 GiB (memory is identity mapped, so physical == virtual). */
#include "kernel.h"

extern char kernel_end[];
extern uint64_t boot_pml4[];
static uint64_t region_start, region_end, next_free, total;

void mem_init(void) {
    uint64_t kend = ((uint64_t)kernel_end + 0xFFFFF) & ~0xFFFFFULL;  /* leave 1 MiB slack after kernel */
    for (int i = 0; i < bootinfo.mmap_count; i++) {
        struct mmap_ent *e = &bootinfo.mmap[i];
        if (e->type != 1) continue;
        total += e->len;
        uint64_t s = e->base, end = e->base + e->len;
        if (s < kend) s = kend;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        s = (s + 0xFFF) & ~0xFFFULL; end &= ~0xFFFULL;
        if (end > s && end - s > region_end - region_start) { region_start = s; region_end = end; }
    }
    next_free = region_start;
}

void *page_alloc(size_t pages) {
    uint64_t sz = pages * 4096;
    if (next_free + sz > region_end) {
        kprintf("PANIC: out of memory\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    void *p = (void *)next_free;
    next_free += sz;
    memset(p, 0, sz);
    return p;
}

uint64_t mem_total(void) { return total; }
uint64_t mem_used(void) { return next_free - region_start; }

/* Identity-map a device region that lies above 4 GiB (the first 4 GiB are mapped at boot),
 * as uncached 2 MiB pages. */
void map_mmio(uint64_t phys, uint64_t size) {
    for (uint64_t a = phys & ~0x1FFFFFULL; a < phys + size; a += 0x200000) {
        if (a < 0x100000000ULL) continue;
        uint64_t *pml4 = boot_pml4;
        int i4 = (a >> 39) & 511, i3 = (a >> 30) & 511, i2 = (a >> 21) & 511;
        if (!(pml4[i4] & 1)) pml4[i4] = (uint64_t)page_alloc(1) | 3;
        uint64_t *pdpt = (uint64_t *)(pml4[i4] & ~0xFFFULL);
        if (!(pdpt[i3] & 1)) pdpt[i3] = (uint64_t)page_alloc(1) | 3;
        uint64_t *pd = (uint64_t *)(pdpt[i3] & ~0xFFFULL);
        pd[i2] = a | 0x83 | 0x18;   /* present, writable, huge, cache-disable */
        __asm__ volatile("invlpg (%0)" ::"r"(a) : "memory");
    }
}
