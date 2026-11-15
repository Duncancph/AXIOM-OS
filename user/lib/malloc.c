/* App memory allocator. */
#ifdef MALLOC_TEST
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
long sys_call(long n, long a, long b, long c, long d, long e);
#define SYS_MEM_GROW 0
#define malloc malloc_d
#define free free_d
#define realloc realloc_d
#define calloc calloc_d
#else
#include "dunc.h"
#endif

/* ---- memory: a boundary-tag heap with a free list, growing with SYS_MEM_GROW ----
 * Every block: [size_t prev_size][size_t size | used] payload... ; free blocks keep next/prev links in the payload. */
struct blk { size_t prev, size; struct blk *next, *prevf; };
#define HDR 16
#define USED 1ul
#define BSIZE(b) ((b)->size & ~USED)
/* Free blocks are kept in bins by size, so a fitting one is found at once instead of walking one long list:
 * bins 0..63 hold one size each (multiples of 16 below 1024), the rest four per power of two. A bitmap tells
 * which bins have anything. */
#define NBINS 160
static struct blk *bins[NBINS], *heap_end;   /* heap_end: the zero-size "end" header */
static uint64_t binmap[3];
static int bin_of(size_t sz) {
    if (sz < 1024) return (int)(sz >> 4);
    int lg = 63 - __builtin_clzl(sz);
    int i = 64 + (lg - 10) * 4 + (int)((sz >> (lg - 2)) & 3);
    return i >= NBINS ? NBINS - 1 : i;
}

static struct blk *after(struct blk *b) { return (struct blk *)((uint8_t *)b + BSIZE(b)); }
static struct blk *before(struct blk *b) { return (struct blk *)((uint8_t *)b - b->prev); }
static void fl_add(struct blk *b) {
    int i = bin_of(BSIZE(b));
    b->next = bins[i]; b->prevf = 0; if (bins[i]) bins[i]->prevf = b; bins[i] = b;
    binmap[i >> 6] |= 1ul << (i & 63);
}
static void fl_del(struct blk *b) {
    int i = bin_of(BSIZE(b));
    if (b->prevf) b->prevf->next = b->next; else bins[i] = b->next;
    if (b->next) b->next->prevf = b->prevf;
    if (!bins[i]) binmap[i >> 6] &= ~(1ul << (i & 63));
}
/* a free block of at least need bytes: from need's own bin if one there fits, else the first of a bigger bin */
static struct blk *find_fit(size_t need) {
    int i = bin_of(need);
    int tries = 0;
    for (struct blk *b = bins[i]; b && tries < 64; b = b->next, tries++) if (BSIZE(b) >= need) return b;
    for (i++; i < NBINS; i++) {
        uint64_t m = binmap[i >> 6] >> (i & 63);
        if (!m) { i |= 63; continue; }
        i += __builtin_ctzl(m);
        if (i >= NBINS) break;
        for (struct blk *b = bins[i]; b; b = b->next) if (BSIZE(b) >= need) return b;   /* (the last bin holds mixed sizes) */
    }
    return 0;
}

static bool grow(size_t need) {
    size_t len = need + HDR > (1u << 20) ? ((need + HDR + 65535) & ~(size_t)65535) : (1u << 20);
    uint8_t *m = (uint8_t *)sys_call(SYS_MEM_GROW, (long)len, 0, 0, 0, 0);
    if (!m) return false;
    struct blk *b;
    if (heap_end && (uint8_t *)heap_end + HDR == m) {       /* contiguous: the old end header becomes the new block */
        b = heap_end; b->size = len;
        if (b->prev && !(before(b)->size & USED)) { struct blk *p = before(b); fl_del(p); p->size += len; b = p; }
    } else {
        b = (struct blk *)m; b->prev = 0; b->size = len - HDR;
    }
    heap_end = after(b); heap_end->prev = BSIZE(b); heap_end->size = USED;   /* size 0, in use */
    fl_add(b);
    return true;
}

static void *malloc_(size_t n) {
    if (n > (512u << 20)) return 0;
    size_t need = ((n + 15) & ~(size_t)15) + HDR;
    if (need < sizeof(struct blk)) need = sizeof(struct blk);
    for (int pass = 0; pass < 2; pass++) {
        struct blk *b = find_fit(need);
        if (b) {
            fl_del(b);
            if (BSIZE(b) - need >= 64) {                     /* split off the rest */
                struct blk *r = (struct blk *)((uint8_t *)b + need);
                r->size = BSIZE(b) - need; r->prev = need;
                after(r)->prev = BSIZE(r);
                b->size = need;
                fl_add(r);
            }
            b->size |= USED;
            return (uint8_t *)b + HDR;
        }
        if (pass == 0 && !grow(need)) return 0;
    }
    return 0;
}

static void free_(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((uint8_t *)p - HDR);
    if (!(b->size & USED)) return;                        /* double free: ignore */
    b->size &= ~USED;
    struct blk *nx = after(b);
    if (!(nx->size & USED)) { fl_del(nx); b->size += BSIZE(nx); }
    if (b->prev && !(before(b)->size & USED)) { struct blk *pv = before(b); fl_del(pv); pv->size += BSIZE(b); b = pv; }
    after(b)->prev = BSIZE(b);
    fl_add(b);
}

/* malloc_busy: set while the heap is being changed (an alarm function must not jump out then) */
volatile int malloc_busy;
void *malloc(size_t n) { malloc_busy++; void *p = malloc_(n); malloc_busy--; return p; }
void free(void *p) { malloc_busy++; free_(p); malloc_busy--; }

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    struct blk *b = (struct blk *)((uint8_t *)p - HDR);
    size_t have = BSIZE(b) - HDR;
    if (have >= n) return p;
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, have);
    free(p);
    return q;
}

void *calloc(size_t a, size_t b) {
    if (b && a > (size_t)-1 / b) return 0;
    void *p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

