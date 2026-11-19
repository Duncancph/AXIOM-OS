/* Physical memory: the largest usable RAM region below 4 GiB (identity mapped), one bit per page. A freed page
 * is free again for runs of pages as well as single ones (a bump pointer with a list of freed single pages,
 * as this was, never gave a run back: FAT's buffers, pipe rings, kernel stacks and window buffers slowly used
 * up every run, and a long session ran out of memory with most of it free). Searches start after the last page
 * given out (next fit), so they're short. */
#include "kernel.h"

extern char kernel_end[];
extern uint64_t boot_pml4[];
static uint64_t region_start, region_end, total;
#define MAXPAGES (1u << 20)                                   /* 4 GiB of pages */
static uint64_t pbits[MAXPAGES / 64];                         /* 1: in use */
static uint32_t npages, used_pages, hint;
static inline bool pused(uint32_t i) { return pbits[i >> 6] >> (i & 63) & 1; }

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
    npages = (uint32_t)((region_end - region_start) / 4096 < MAXPAGES ? (region_end - region_start) / 4096 : MAXPAGES);
    region_end = region_start + (uint64_t)npages * 4096;
}
/* The first 4 GiB are mapped at boot as 2 MiB pages, all cached - RAM, but also what isn't: the graphics chip's
 * own part of the memory (right after the last RAM below 4 GiB), the BIOS's, and devices. The processor may read
 * cached memory ahead of time on its own (a copy near the end of RAM reads into the next page), and reading the
 * graphics memory that way stops it with a machine check: the user's PC did, playing a video, once memory was used
 * up to there. So only RAM stays cached: a 2 MiB piece with no RAM is uncached; one that is partly RAM is split
 * into 4 KiB pages, each cached only if it is RAM. (Devices are still reached there, uncached.) */
static int ram_cover(uint64_t s, uint64_t e) {             /* 2: [s,e) all RAM, 0: none of it, 1: part */
    uint64_t in = 0;
    for (int i = 0; i < bootinfo.mmap_count; i++) {
        struct mmap_ent *m = &bootinfo.mmap[i];
        if (m->type != 1) continue;
        uint64_t a = m->base > s ? m->base : s, b = m->base + m->len < e ? m->base + m->len : e;
        if (b > a) in += b - a;
    }
    return in >= e - s ? 2 : in ? 1 : 0;
}
void mem_protect_holes(void) {
    extern uint64_t boot_pd[];
    int uc = 0, split = 0;
    for (uint64_t i = 0; i < 2048; i++) {
        uint64_t s = i << 21;
        int c = ram_cover(s, s + 0x200000);
        if (c == 2) continue;
        if (c == 0) { boot_pd[i] |= 0x18; uc++; continue; }   /* (PCD | PWT: uncached) */
        uint64_t *pt = page_alloc(1);
        if (!pt) { boot_pd[i] |= 0x18; uc++; continue; }
        for (int k = 0; k < 512; k++) {
            uint64_t a = s + ((uint64_t)k << 12);
            pt[k] = a | 0x3 | (ram_cover(a, a + 4096) == 2 ? 0 : 0x18);
        }
        boot_pd[i] = (uint64_t)pt | 0x3;
        split++;
    }
    __asm__ volatile("wbinvd; mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
    LOG_INFO("Memory below 4 GB: %d MB that isn't RAM uncached (%d pieces split)\n", uc * 2, split);
}
static void pmark(uint32_t i, uint32_t n, bool on) {
    for (uint32_t k = i; k < i + n; k++) { if (on) pbits[k >> 6] |= 1ULL << (k & 63); else pbits[k >> 6] &= ~(1ULL << (k & 63)); }
}
/* the first run of n free pages at or after from (whole words in use are skipped); npages if none */
static uint32_t pfind(uint32_t from, uint32_t to, uint32_t n) {
    uint32_t run = 0, start = from;
    for (uint32_t i = from; i < to;) {
        if (!(i & 63) && pbits[i >> 6] == ~0ULL) { i += 64; run = 0; start = i; continue; }
        if (pused(i)) { i++; run = 0; start = i; continue; }
        if (++run == n) return start;
        i++;
    }
    return npages;
}

void *page_alloc(size_t pages) {
    if (!pages || pages > npages) return 0;
    uint32_t n = (uint32_t)pages;
    uint32_t i = pfind(hint, npages, n);
    if (i == npages) i = pfind(0, hint + n < npages ? hint + n : npages, n);   /* (wrap around) */
    if (i == npages) return 0;
    pmark(i, n, true); used_pages += n; hint = i + n < npages ? i + n : 0;
    void *p = (void *)(region_start + (uint64_t)i * 4096);
    memset(p, 0, pages * 4096);
    return p;
}

void *page_alloc_or_panic(size_t pages) {
    void *p = page_alloc(pages);
    if (!p) panic("out of memory");
    return p;
}

void page_free(void *p) {
    uint64_t a = (uint64_t)p;
    if (a < region_start || a >= region_end || (a & 4095)) { kprintf("[mem] page_free of %p: not a kernel page\n", p); return; }
    uint32_t i = (uint32_t)((a - region_start) / 4096);
    if (!pused(i)) { kprintf("[mem] page_free of %p: already free\n", p); return; }
    pmark(i, 1, false); used_pages--;
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
uint64_t mem_used(void) { return (uint64_t)used_pages * 4096 + high_used; }
uint64_t mem_low_free(void) { return (uint64_t)(npages - used_pages) * 4096; }   /* the kernel's own (below 4 GiB) */

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
