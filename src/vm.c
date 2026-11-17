/* Per-process address spaces. Each program gets its own page tables: the kernel is
 * mapped supervisor-only (programs can't touch it) and each program's own memory
 * lives in the user region, invisible to every other program. */
#include "kernel.h"

#define P 1ULL
#define W 2ULL
#define U 4ULL
#define OWNED (1ULL << 9)     /* page belongs to this address space; free it on exit */
#define SHARED (1ULL << 10)   /* page belongs to something else (a Linux program's shared memory): never freed here */
#define NX (1ULL << 63)       /* not executable (EFER.NXE is on) */
#define ADDR(e) ((e) & 0x000FFFFFFFFFF000ULL)
#define USER_PML4_INDEX 1     /* 0x8000000000 - 0x10000000000 */

extern uint64_t boot_pml4[];

/* Programs copy the kernel's top-level page table entries when they're created, so the kernel
 * window must already exist: pre-create its PML4 entries at boot. */
void vm_init(void) {
    uint32_t lo, hi;                                       /* no-execute pages: EFER.NXE (the other cores copy EFER) */
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= 1u << 11;
    __asm__ volatile("wrmsr" :: "c"(0xC0000080), "a"(lo), "d"(hi));
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
        uint64_t pg = upage_alloc();
        if (!pg) return -1;
        *pte = pg | P | W | U | OWNED;
        n++;
    }
    return n;
}

uint64_t vm_phys(uint64_t *pml4, uint64_t va) {
    uint64_t *pte = pte_of(pml4, va, false);
    if (!pte || !(*pte & P)) return 0;
    return (uint64_t)mmio_virt(ADDR(*pte) | (va & 0xFFF));   /* (where the kernel reaches it) */
}

/* Is [p, p+len) entirely mapped user memory (writable if asked)? */
bool user_ok(uint64_t *pml4, const void *p, uint64_t len, bool write) {
    uint64_t a = (uint64_t)p;
    if (a < USER_BASE || a + len > USER_END || a + len < a) return false;
    for (uint64_t pg = a & ~0xFFFULL; pg < a + len; pg += 4096) {
        uint64_t *pte = pte_of(pml4, pg, false);
        if (!pte || !(*pte & P) || !(*pte & U) || (write && !(*pte & W))) {
            /* not there yet (or read-only): an SYS_MMAP area gives it its page now, as an app's own access would */
            struct mm *m = current ? current->mm : 0; uint64_t np = 0;
            if (!m || m->pml4 != pml4 || !mm_fault(m, pg, write, false, &np)) return false;
            if (current->proc) current->proc->pages += np;
        }
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
                    if ((l1[k] & OWNED) && ADDR(l1[k])) upage_free(ADDR(l1[k]));   /* (PROT_NONE pages too) */
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

/* ---------------- program memory: SYS_MMAP areas, threads sharing them ---------------- */
struct mm *mm_new(void) {
    struct mm *m = kmalloc(sizeof *m);
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->pml4 = vm_create();
    if (!m->pml4) { kfree(m); return 0; }
    m->refs = 1;
    return m;
}
void mm_put(struct mm *m) {
    if (!m || --m->refs > 0) return;
    vm_destroy(m->pml4);
    kfree(m->v); kfree(m);
}

/* the areas, sorted and not overlapping */
static bool vma_room(struct mm *m, int more) {
    if (m->nv + more <= m->cap) return true;
    int nc = m->cap ? m->cap * 2 : 64;
    while (nc < m->nv + more) nc *= 2;
    struct vma *n = kmalloc(nc * sizeof *n);
    if (!n) return false;
    if (m->v) memcpy(n, m->v, m->nv * sizeof *n);
    kfree(m->v); m->v = n; m->cap = nc;
    return true;
}
static int vma_find(struct mm *m, uint64_t a) {           /* index of the first area ending after a */
    int lo = 0, hi = m->nv;
    while (lo < hi) { int mid = (lo + hi) / 2; if (m->v[mid].e <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static bool vma_cut(struct mm *m, uint64_t s, uint64_t e) {   /* take [s, e) out of the areas */
    if (!vma_room(m, 1)) return false;
    int i = vma_find(m, s);
    while (i < m->nv && m->v[i].s < e) {
        struct vma *v = &m->v[i];
        if (v->s < s && v->e > e) {                          /* split in two */
            memmove(&m->v[i + 2], &m->v[i + 1], (m->nv - i - 1) * sizeof *v);
            m->v[i + 1] = (struct vma){ e, v->e, v->prot }; v->e = s; m->nv++;
            return true;
        }
        if (v->s < s) { v->e = s; i++; continue; }
        if (v->e > e) { v->s = e; break; }
        memmove(&m->v[i], &m->v[i + 1], (m->nv - i - 1) * sizeof *v); m->nv--;
    }
    return true;
}
static bool vma_add(struct mm *m, uint64_t s, uint64_t e, int prot) {   /* (after vma_cut of the range) */
    if (!vma_room(m, 1)) return false;
    int i = vma_find(m, s);
    if (i > 0 && m->v[i - 1].e == s && m->v[i - 1].prot == prot) {   /* join the one before */
        m->v[i - 1].e = e;
        if (i < m->nv && m->v[i].s == e && m->v[i].prot == prot) { m->v[i - 1].e = m->v[i].e; memmove(&m->v[i], &m->v[i + 1], (m->nv - i - 1) * sizeof *m->v); m->nv--; }
        return true;
    }
    memmove(&m->v[i + 1], &m->v[i], (m->nv - i) * sizeof *m->v);
    m->v[i] = (struct vma){ s, e, prot }; m->nv++;
    return true;
}
static uint64_t pte_flags(int prot) {
    if (!prot) return OWNED | U;                            /* kept, but not present: any access faults */
    return P | U | OWNED | ((prot & DPROT_WRITE) ? W : 0) | ((prot & DPROT_EXEC) ? 0 : NX);
}
/* new protection for a page that's there: keeps whether it's the program's own or shared */
static uint64_t pte_reprot(uint64_t pte, int prot) {
    return ADDR(pte) | (pte_flags(prot) & ~OWNED) | (pte & (OWNED | SHARED));
}
static bool pte_kept(uint64_t pte) { return (pte & (OWNED | SHARED)) && ADDR(pte); }

/* TLB shootdown: other cores running this program's threads drop their cached translations */
static void tlb_shootdown(struct mm *m) {
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    uint64_t gen = __atomic_add_fetch(&m->tlb_gen, 1, __ATOMIC_SEQ_CST);
    cpu_self()->tlb_gen = gen;
    for (int i = 0; i < ncpus_on; i++) {
        struct cpu *c = &cpus[i];
        if (c == cpu_self() || !c->in_user || !c->cur || c->cur->mm != m) continue;
        lapic_ipi(c->apic_id, 0xF1);
        uint64_t t0 = uptime_ms();
        while (c->tlb_ack < gen && c->in_user && uptime_ms() - t0 < 50) cpu_relax();
    }
}
void tlb_ipi(void) {                                       /* (no kernel lock: it may be held by the core waiting for this) */
    struct cpu *c = cpu_self();
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    struct task *t = c->cur;
    if (t && t->mm) { c->tlb_gen = t->mm->tlb_gen; c->tlb_ack = c->tlb_gen; }
    lapic_eoi();
}
void tlb_sync(void) {
    struct cpu *c = cpu_self();
    struct task *t = c->cur;
    c->in_user = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (t && t->mm && c->tlb_gen != t->mm->tlb_gen) {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
        c->tlb_gen = t->mm->tlb_gen;
    }
}

static bool in_area(uint64_t s, uint64_t e) { return s >= USER_MMAP && e <= USER_MMAP_END && s < e; }
/* drop the pages of [s, e) (freeing the program's own) */
static uint64_t drop_pages(struct mm *m, uint64_t s, uint64_t e) {
    uint64_t freed = 0;
    for (uint64_t a = s; a < e; a += 4096) {
        uint64_t *t = next_level(m->pml4, (a >> 39) & 511, false);
        if (t) t = next_level(t, (a >> 30) & 511, false);
        if (!t) { a = (a | ((1ULL << 30) - 1)) + 1 - 4096; continue; }      /* nothing in this GiB */
        t = next_level(t, (a >> 21) & 511, false);
        if (!t) { a = (a | ((1ULL << 21) - 1)) + 1 - 4096; continue; }      /* nothing in this 2 MiB */
        uint64_t *pte = &t[(a >> 12) & 511];
        if (*pte) { if ((*pte & OWNED) && ADDR(*pte)) { upage_free(ADDR(*pte)); freed++; } *pte = 0; }
    }
    return freed;
}
long mm_mmap(struct mm *m, uint64_t addr, uint64_t len, int prot, int flags, uint64_t *pages) {
    len = (len + 4095) & ~4095ULL;
    if (!len || len > USER_MMAP_END - USER_MMAP) return -DERR_INVAL;
    uint64_t s;
    if (flags & DMAP_FIXED) {
        if ((addr & 4095) || !in_area(addr, addr + len)) return -DERR_INVAL;
        s = addr;
        uint64_t fr = drop_pages(m, s, s + len);
        if (pages) *pages -= fr < *pages ? fr : *pages;
        if (!vma_cut(m, s, s + len)) return -DERR_NOMEM;
        tlb_shootdown(m);
    } else {
        /* the first gap big enough, from the hint (or the start of the area) */
        uint64_t from = (addr & ~4095ULL) && in_area(addr & ~4095ULL, (addr & ~4095ULL) + len) ? addr & ~4095ULL : USER_MMAP;
        for (int pass = 0; ; pass++) {
            s = from;
            int i = vma_find(m, s);
            for (; i < m->nv; i++) {
                if (m->v[i].s >= s + len) break;
                if (m->v[i].e > s) s = m->v[i].e;
            }
            if (s + len <= USER_MMAP_END) break;
            if (pass || from == USER_MMAP) return -DERR_NOMEM;
            from = USER_MMAP;
        }
    }
    if (!vma_add(m, s, s + len, prot & 7)) return -DERR_NOMEM;
    return (long)s;
}
long mm_munmap(struct mm *m, uint64_t addr, uint64_t len, uint64_t *pages) {
    len = (len + 4095) & ~4095ULL;
    if ((addr & 4095) || !in_area(addr, addr + len)) return -DERR_INVAL;
    uint64_t fr = drop_pages(m, addr, addr + len);
    if (pages) *pages -= fr < *pages ? fr : *pages;
    if (!vma_cut(m, addr, addr + len)) return -DERR_NOMEM;
    tlb_shootdown(m);
    return 0;
}
long mm_dontneed(struct mm *m, uint64_t addr, uint64_t len, uint64_t *pages) {   /* pages freed; they come back zeroed */
    len = (len + 4095) & ~4095ULL;
    if ((addr & 4095) || !in_area(addr, addr + len)) return 0;
    uint64_t fr = drop_pages(m, addr, addr + len);
    if (pages) *pages -= fr < *pages ? fr : *pages;
    tlb_shootdown(m);
    return 0;
}
long mm_mprotect(struct mm *m, uint64_t addr, uint64_t len, int prot) {
    len = (len + 4095) & ~4095ULL;
    if (addr & 4095) return -DERR_INVAL;
    if (!in_area(addr, addr + len)) return 0;                /* (outside the area: nothing to change) */
    /* every page must be in an area */
    for (uint64_t a = addr; a < addr + len;) {
        int i = vma_find(m, a);
        if (i >= m->nv || m->v[i].s > a) return -DERR_INVAL;
        a = m->v[i].e;
    }
    if (!vma_cut(m, addr, addr + len) || !vma_add(m, addr, addr + len, prot & 7)) return -DERR_NOMEM;
    for (uint64_t a = addr; a < addr + len; a += 4096) {    /* the pages already given */
        uint64_t *t = next_level(m->pml4, (a >> 39) & 511, false);
        if (t) t = next_level(t, (a >> 30) & 511, false);
        if (!t) { a = (a | ((1ULL << 30) - 1)) + 1 - 4096; continue; }
        t = next_level(t, (a >> 21) & 511, false);
        if (!t) { a = (a | ((1ULL << 21) - 1)) + 1 - 4096; continue; }
        uint64_t *pte = &t[(a >> 12) & 511];
        if (pte_kept(*pte)) *pte = pte_reprot(*pte, prot & 7);
    }
    tlb_shootdown(m);
    return 0;
}
/* a page fault (or the kernel checking an address): give the page if its area allows the access */
bool mm_fault(struct mm *m, uint64_t addr, bool write, bool exec, uint64_t *pages) {
    if (!m || !in_area(addr, addr + 1)) return false;
    int i = vma_find(m, addr);
    if (i >= m->nv || m->v[i].s > addr) return false;
    int prot = m->v[i].prot;
    if (!prot || (write && !(prot & DPROT_WRITE)) || (exec && !(prot & DPROT_EXEC))) return false;
    uint64_t va = addr & ~4095ULL;
    uint64_t *pte = pte_of(m->pml4, va, true);
    if (!pte) return false;
    if (pte_kept(*pte)) *pte = pte_reprot(*pte, prot);      /* there already (another core's stale view, or protection back) */
    else {
        uint64_t pg = upage_alloc();
        if (!pg) return false;
        if (m->fill && !m->fill(m, va, pg, pages)) { upage_free(pg); return false; }   /* (filled before anyone can see it) */
        pte = pte_of(m->pml4, va, true);                    /* (fill may have added page tables) */
        if (!pte) { upage_free(pg); return false; }
        if (pte_kept(*pte)) upage_free(pg);                 /* (fill's read-ahead can't have put this one, but be safe) */
        else { *pte = pg | pte_flags(prot); if (pages) (*pages)++; }
    }
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return true;
}
/* a copy of an address space (a Linux program's fork): the same areas; every page of its own is copied, pages that
 * belong to something else (shared memory) are mapped again. *pages: how many pages the copy made. 0: no memory */
struct mm *mm_fork(struct mm *src, uint64_t *pages) {
    struct mm *m = mm_new();
    if (!m) return 0;
    if (src->nv && !vma_room(m, src->nv)) { mm_put(m); return 0; }
    memcpy(m->v, src->v, src->nv * sizeof *m->v); m->nv = src->nv;
    uint64_t *l3 = (src->pml4[USER_PML4_INDEX] & P) ? (uint64_t *)ADDR(src->pml4[USER_PML4_INDEX]) : 0;
    for (int i = 0; l3 && i < 512; i++) {
        if (!(l3[i] & P)) continue;
        uint64_t *l2 = (uint64_t *)ADDR(l3[i]);
        for (int j = 0; j < 512; j++) {
            if (!(l2[j] & P)) continue;
            uint64_t *l1 = (uint64_t *)ADDR(l2[j]);
            for (int k = 0; k < 512; k++) {
                uint64_t e = l1[k];
                if (!pte_kept(e)) continue;
                uint64_t va = ((uint64_t)USER_PML4_INDEX << 39) | ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                uint64_t *pte = pte_of(m->pml4, va, true);
                if (!pte) { mm_put(m); return 0; }
                if (e & SHARED) { *pte = e; continue; }                         /* (its owner keeps it) */
                uint64_t pg = upage_alloc();
                if (!pg) { mm_put(m); return 0; }
                memcpy(mmio_virt(pg), mmio_virt(ADDR(e)), 4096);
                *pte = pg | (e & ~0x000FFFFFFFFFF000ULL);
                if (pages) (*pages)++;
            }
        }
    }
    return m;
}
bool mm_add_page(struct mm *m, uint64_t va, const void *data, uint64_t *pages) {
    if (!m || (va & 4095) || !in_area(va, va + 4096)) return false;
    int i = vma_find(m, va);
    if (i >= m->nv || m->v[i].s > va || !m->v[i].prot) return false;
    uint64_t *pte = pte_of(m->pml4, va, true);
    if (!pte || pte_kept(*pte)) return false;
    uint64_t pg = upage_alloc();
    if (!pg) return false;
    memcpy(mmio_virt(pg), data, 4096);
    *pte = pg | pte_flags(m->v[i].prot);
    if (pages) (*pages)++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return true;
}
/* put a page that belongs to something else (shared memory) at va, inside an area made by mm_mmap; munmap and
 * exit take it out of the page tables but leave the page itself to its owner */
bool mm_map_shared(struct mm *m, uint64_t va, uint64_t pa, int prot) {
    if (!m || (va & 4095) || !in_area(va, va + 4096)) return false;
    uint64_t *pte = pte_of(m->pml4, va, true);
    if (!pte) return false;
    if ((*pte & OWNED) && ADDR(*pte)) upage_free(ADDR(*pte));
    *pte = pa | (pte_flags(prot & 7) & ~OWNED) | SHARED;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return true;
}
int mm_prot_at(struct mm *m, uint64_t addr) {              /* the protection of the area at addr (0 if none) */
    int i = vma_find(m, addr);
    return i < m->nv && m->v[i].s <= addr ? m->v[i].prot : 0;
}
