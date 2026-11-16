/* Physical memory: bump allocator over the largest usable RAM region below 4 GiB,
 * plus a free list of recycled single pages. Physical memory is identity mapped. */
#include "kernel.h"

extern char kernel_end[];
extern uint64_t boot_pml4[];
static uint64_t region_start, region_end, next_free, total, free_count;
static void *free_list;

void mem_init(void) {
    uint64_t kend = ((uint64_t)kernel_end + 0xFFFFF) & ~0xFFFFFULL;
    for (int i = 0; i < bootinfo.mod_count; i++)                                       /* keep GRUB's modules safe */
        if (bootinfo.mods[i].end > kend) kend = (bootinfo.mods[i].end + 0xFFFFF) & ~0xFFFFFULL;
    if (bootinfo.mod_end > kend) kend = (bootinfo.mod_end + 0xFFFFF) & ~0xFFFFFULL;
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
    if (pages == 1 && free_list) {
        void *p = free_list;
        free_list = *(void **)p;
        free_count--;
        memset(p, 0, 4096);
        return p;
    }
    uint64_t sz = pages * 4096;
    if (next_free + sz > region_end) return 0;
    void *p = (void *)next_free;
    next_free += sz;
    memset(p, 0, sz);
    return p;
}

void *page_alloc_or_panic(size_t pages) {
    void *p = page_alloc(pages);
    if (!p) panic("out of memory");
    return p;
}

void page_free(void *p) {
    *(void **)p = free_list;
    free_list = p;
    free_count++;
}

/* ---- memory above 4 GiB: programs' pages (and the disk cache, which takes the first part of it) ----
 * It's reached through the kernel's direct map at HHDM + physical address. */
int mem_high_regions(uint64_t *base, uint64_t *len, int max) {     /* RAM above 4 GiB, 2 MiB aligned, in map order */
    int n = 0;
    for (int i = 0; i < bootinfo.mmap_count && n < max; i++) {
        struct mmap_ent *e = &bootinfo.mmap[i];
        if (e->type != 1) continue;
        uint64_t b = (e->base + 0x1FFFFF) & ~0x1FFFFFULL, end = (e->base + e->len) & ~0x1FFFFFULL;
        if (b < 0x100000000ULL) b = 0x100000000ULL;
        if (end <= b) continue;
        base[n] = b; len[n] = end - b; n++;
    }
    return n;
}
static uint64_t hb[16], hl[16]; static int nh, hcur;      /* what's left for programs */
static uint64_t hnext, hfree_list, hfree_count, high_total, high_used;
void mem_high_init(uint64_t skip) {                         /* skip: bytes the disk cache took from the start */
    nh = mem_high_regions(hb, hl, 16);
    for (int i = 0; i < nh; i++) {
        uint64_t t = skip < hl[i] ? skip : hl[i];
        hb[i] += t; hl[i] -= t; skip -= t;
        if (hl[i]) map_mmio_flags(hb[i], hl[i], 0);        /* normal (cached) memory */
        high_total += hl[i];
    }
    hcur = 0; hnext = nh ? hb[0] : 0;
    if (high_total) LOG_OK("Memory above 4 GB: %lu MB for programs\n", high_total >> 20);
}
/* a zeroed page for a program (physical address): above 4 GiB when there is any, else from the kernel's memory */
uint64_t upage_alloc(void) {
    uint64_t pa = 0;
    if (hfree_list) { pa = hfree_list; hfree_list = *(uint64_t *)mmio_virt(pa); hfree_count--; }
    else {
        while (hcur < nh && hnext + 4096 > hb[hcur] + hl[hcur]) { hcur++; if (hcur < nh) hnext = hb[hcur]; }
        if (hcur < nh) { pa = hnext; hnext += 4096; }
    }
    if (pa) { memset(mmio_virt(pa), 0, 4096); high_used += 4096; return pa; }
    void *p = page_alloc(1);
    return (uint64_t)p;
}
void upage_free(uint64_t pa) {
    if (pa < 0x100000000ULL) { page_free((void *)pa); return; }
    *(uint64_t *)mmio_virt(pa) = hfree_list; hfree_list = pa; hfree_count++;
    high_used -= 4096;
}

uint64_t mem_total(void) { return total; }
uint64_t mem_used(void) { return next_free - region_start - free_count * 4096 + high_used; }
uint64_t mem_low_free(void) { return region_end - next_free + free_count * 4096; }   /* the kernel's own (below 4 GiB) */

void map_mmio_flags(uint64_t phys, uint64_t size, uint64_t cache) {
    for (uint64_t a = phys & ~0x1FFFFFULL; a < phys + size; a += 0x200000) {
        if (a < 0x100000000ULL) continue;
        uint64_t *pml4 = boot_pml4, va = HHDM + a;
        int i4 = (va >> 39) & 511, i3 = (va >> 30) & 511, i2 = (va >> 21) & 511;
        if (!(pml4[i4] & 1)) pml4[i4] = (uint64_t)page_alloc_or_panic(1) | 3;
        uint64_t *pdpt = (uint64_t *)(pml4[i4] & ~0xFFFULL);
        if (!(pdpt[i3] & 1)) pdpt[i3] = (uint64_t)page_alloc_or_panic(1) | 3;
        uint64_t *pd = (uint64_t *)(pdpt[i3] & ~0xFFFULL);
        pd[i2] = a | 0x83 | cache;
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    }
}
/* Map device memory (uncached) and return the address to use for it. */
void *map_mmio(uint64_t phys, uint64_t size) { map_mmio_flags(phys, size, 0x18); return mmio_virt(phys); }
