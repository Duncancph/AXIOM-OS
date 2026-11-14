/* Per-process address spaces. Each program gets its own page tables: the kernel is
 * mapped supervisor-only (programs can't touch it) and each program's own memory
 * lives in the user region, invisible to every other program. */
#include "kernel.h"

#define P 1ULL
#define W 2ULL
#define U 4ULL
#define OWNED (1ULL << 9)     /* page belongs to this address space; free it on exit */
#define ADDR(e) ((e) & 0x000FFFFFFFFFF000ULL)
#define USER_PML4_INDEX 1     /* 0x8000000000 - 0x10000000000 */

extern uint64_t boot_pml4[];

/* Programs copy the kernel's top-level page table entries when they're created, so the kernel
 * window must already exist: pre-create its PML4 entries at boot. */
void vm_init(void) {
    for (int i = 256; i < 512; i++)
        if (!(boot_pml4[i] & P)) boot_pml4[i] = (uint64_t)page_alloc_or_panic(1) | P | W;
}

uint64_t *vm_create(void) {
    uint64_t *pml4 = page_alloc(1);
    if (!pml4) return 0;
    for (int i = 0; i < 512; i++) if (i != USER_PML4_INDEX) pml4[i] = boot_pml4[i];
    return pml4;
}

static uint64_t *next_level(uint64_t *table, int idx, bool create) {
    if (!(table[idx] & P)) {
        if (!create) return 0;
        uint64_t *n = page_alloc(1);
        if (!n) return 0;
        table[idx] = (uint64_t)n | P | W | U;
    }
    return (uint64_t *)ADDR(table[idx]);
}

static uint64_t *pte_of(uint64_t *pml4, uint64_t va, bool create) {
    uint64_t *t = next_level(pml4, (va >> 39) & 511, create);
    if (!t) return 0;
    t = next_level(t, (va >> 30) & 511, create);
    if (!t) return 0;
    t = next_level(t, (va >> 21) & 511, create);
    if (!t) return 0;
    return &t[(va >> 12) & 511];
}

bool vm_map(uint64_t *pml4, uint64_t va, uint64_t pa, bool writable, bool owned) {
    uint64_t *pte = pte_of(pml4, va, true);
    if (!pte) return false;
    *pte = pa | P | U | (writable ? W : 0) | (owned ? OWNED : 0);
    return true;
}

/* Allocate fresh zeroed pages for [va, va+len). Returns pages allocated, or -1. */
long vm_alloc(uint64_t *pml4, uint64_t va, uint64_t len) {
    long n = 0;
    for (uint64_t a = va & ~0xFFFULL; a < va + len; a += 4096) {
        uint64_t *pte = pte_of(pml4, a, true);
        if (!pte) return -1;
        if (*pte & P) continue;
        void *pg = page_alloc(1);
        if (!pg) return -1;
        *pte = (uint64_t)pg | P | W | U | OWNED;
        n++;
    }
    return n;
}

uint64_t vm_phys(uint64_t *pml4, uint64_t va) {
    uint64_t *pte = pte_of(pml4, va, false);
    if (!pte || !(*pte & P)) return 0;
    return ADDR(*pte) | (va & 0xFFF);
}

/* Is [p, p+len) entirely mapped user memory (writable if asked)? */
bool user_ok(uint64_t *pml4, const void *p, uint64_t len, bool write) {
    uint64_t a = (uint64_t)p;
    if (a < USER_BASE || a + len > USER_END || a + len < a) return false;
    for (uint64_t pg = a & ~0xFFFULL; pg < a + len; pg += 4096) {
        uint64_t *pte = pte_of(pml4, pg, false);
        if (!pte || !(*pte & P) || !(*pte & U) || (write && !(*pte & W))) return false;
    }
    return true;
}

void vm_destroy(uint64_t *pml4) {
    uint64_t *l3 = (pml4[USER_PML4_INDEX] & P) ? (uint64_t *)ADDR(pml4[USER_PML4_INDEX]) : 0;
    if (l3) {
        for (int i = 0; i < 512; i++) {
            if (!(l3[i] & P)) continue;
            uint64_t *l2 = (uint64_t *)ADDR(l3[i]);
            for (int j = 0; j < 512; j++) {
                if (!(l2[j] & P)) continue;
                uint64_t *l1 = (uint64_t *)ADDR(l2[j]);
                for (int k = 0; k < 512; k++)
                    if ((l1[k] & P) && (l1[k] & OWNED)) page_free((void *)ADDR(l1[k]));
                page_free(l1);
            }
            page_free(l2);
        }
        page_free(l3);
    }
    page_free(pml4);
}

/* ---------------- ELF64 program loader ---------------- */
struct ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } __attribute__((packed));

long elf_load(uint64_t *pml4, const uint8_t *img, uint64_t size, uint64_t *entry) {
    const struct ehdr *e = (const void *)img;
    if (size < sizeof *e || memcmp(e->id, "\x7F" "ELF", 4) || e->id[4] != 2 || e->machine != 62) return -1;
    long pages = 0;
    for (int i = 0; i < e->phnum; i++) {
        const struct phdr *ph = (const void *)(img + e->phoff + i * e->phentsize);
        if (ph->type != 1) continue;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > USER_STACK_TOP - 0x100000 || ph->offset + ph->filesz > size) return -1;
        long n = vm_alloc(pml4, ph->vaddr, ph->memsz);
        if (n < 0) return -1;
        pages += n;
        for (uint64_t off = 0; off < ph->filesz;) {
            uint64_t va = ph->vaddr + off;
            uint64_t chunk = 4096 - (va & 0xFFF);
            if (chunk > ph->filesz - off) chunk = ph->filesz - off;
            memcpy((void *)vm_phys(pml4, va), img + ph->offset + off, chunk);
            off += chunk;
        }
    }
    *entry = e->entry;
    return pages;
}

void vm_unmap(uint64_t *pml4, uint64_t va, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t *pte = pte_of(pml4, va + i * 4096, false);
        if (pte) { *pte = 0; __asm__ volatile("invlpg (%0)" ::"r"(va + i * 4096) : "memory"); }
    }
}
