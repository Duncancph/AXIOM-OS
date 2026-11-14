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
static struct blk *freelist, *heap_end;      /* heap_end: the zero-size "end" header */

static struct blk *after(struct blk *b) { return (struct blk *)((uint8_t *)b + BSIZE(b)); }
static struct blk *before(struct blk *b) { return (struct blk *)((uint8_t *)b - b->prev); }
static void fl_add(struct blk *b) { b->next = freelist; b->prevf = 0; if (freelist) freelist->prevf = b; freelist = b; }
static void fl_del(struct blk *b) { if (b->prevf) b->prevf->next = b->next; else freelist = b->next; if (b->next) b->next->prevf = b->prevf; }

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

void *malloc(size_t n) {
    if (n > (512u << 20)) return 0;
    size_t need = ((n + 15) & ~(size_t)15) + HDR;
    if (need < sizeof(struct blk)) need = sizeof(struct blk);
    for (int pass = 0; pass < 2; pass++) {
        for (struct blk *b = freelist; b; b = b->next) {
            if (BSIZE(b) < need) continue;
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

void free(void *p) {
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

