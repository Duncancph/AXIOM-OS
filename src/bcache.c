/* The disk cache: recently read parts of every drive are kept in memory, so opening the same files,
 * apps and folders again doesn't go back to the disk. It uses memory above 4 GiB when the computer has
 * it (the rest of AXIOM lives below 4 GiB), else a share of the free memory below.
 *
 * It sits in front of each drive's read/write functions (bcache_attach, from disk_add). Writes go to
 * the drive first and then update the cached copy ("write-through"), so nothing is lost on power-off.
 * Cached in 4 KiB chunks; least recently used chunks make room for new ones. */
#include "kernel.h"
#include "blockdev.h"

#define CH 4096                                   /* bytes per cached chunk */
#define MAX_RUN 64                                /* chunks read in one go (256 KiB) */
#define NIL 0xFFFFFFFFu
struct ent { uint64_t key; uint32_t prev, next, hnext; };   /* key: device id << 48 | chunk number */
static struct ent *ents; static uint32_t nents, *htab, hmask, lru_head = NIL, lru_tail = NIL, free_head = NIL;
struct pool { uint32_t first; uint8_t *va; };
static struct pool pools[16]; static int npools;
static uint64_t hits, misses, used_chunks;
static uint8_t *bounce;

struct cwrap {
    bool used; int id; uint32_t gen;              /* gen: bumped by every write (see c_read) */
    struct blockdev *bd; void *ctx;
    int (*read)(void *, uint64_t, uint32_t, void *);
    int (*write)(void *, uint64_t, uint32_t, const void *);
    int (*flush)(void *);
    void (*release)(void *);
};
static struct cwrap wraps[16];

static uint8_t *slot(uint32_t i) {
    int p = npools - 1; while (p > 0 && pools[p].first > i) p--;
    return pools[p].va + (uint64_t)(i - pools[p].first) * CH;
}
static uint32_t hashk(uint64_t k) { k ^= k >> 29; k *= 0xBF58476D1CE4E5B9ULL; k ^= k >> 32; return (uint32_t)k & hmask; }
static uint32_t find(uint64_t k) { for (uint32_t i = htab[hashk(k)]; i != NIL; i = ents[i].hnext) if (ents[i].key == k) return i; return NIL; }
static void lru_unlink(uint32_t i) {
    struct ent *e = &ents[i];
    if (e->prev != NIL) ents[e->prev].next = e->next; else lru_head = e->next;
    if (e->next != NIL) ents[e->next].prev = e->prev; else lru_tail = e->prev;
}
static void lru_front(uint32_t i) { ents[i].prev = NIL; ents[i].next = lru_head; if (lru_head != NIL) ents[lru_head].prev = i; lru_head = i; if (lru_tail == NIL) lru_tail = i; }
static void hash_remove(uint32_t i) {
    uint32_t *pp = &htab[hashk(ents[i].key)];
    while (*pp != NIL && *pp != i) pp = &ents[*pp].hnext;
    if (*pp == i) *pp = ents[i].hnext;
}
static uint32_t take_slot(void) {                 /* a free slot, or the least recently used one */
    uint32_t i;
    if (free_head != NIL) { i = free_head; free_head = ents[i].next; used_chunks++; return i; }
    i = lru_tail;
    if (i == NIL) return NIL;
    lru_unlink(i); hash_remove(i);
    return i;
}
static void insert(uint64_t k, const uint8_t *data) {
    if (!nents) return;
    uint32_t i = find(k);
    if (i == NIL) {
        i = take_slot(); if (i == NIL) return;
        ents[i].key = k; uint32_t h = hashk(k); ents[i].hnext = htab[h]; htab[h] = i;
    } else lru_unlink(i);
    memcpy(slot(i), data, CH);
    lru_front(i);
}

/* ---- the setup: find the memory ---- */
static uint64_t cache_setup(void) {                  /* returns the bytes it took from memory above 4 GiB */
    uint64_t want = 0; struct { uint64_t base, len; } r[16]; int nr = 0;
    uint64_t hb[16], hl[16]; nr = mem_high_regions(hb, hl, 16);          /* RAM above 4 GiB */
    for (int i = 0; i < nr; i++) { r[i].base = hb[i]; r[i].len = hl[i]; want += hl[i]; }
    uint64_t high = want;
    uint64_t cap = 4ULL << 30;                                           /* at most 4 GiB of cache, and a quarter of it: */
    if (cap > high / 4) cap = high / 4 & ~(uint64_t)(CH - 1);             /* the rest is for programs (mem_high_init) */
    if (want > cap) want = cap;
    if (want < (64ULL << 20)) {                                          /* little or none: borrow some low memory */
        nr = 0;
        uint64_t freelow = mem_low_free();
        uint64_t sz = freelow / 8; if (sz > (256ULL << 20)) sz = 256ULL << 20;
        sz &= ~(uint64_t)(CH - 1);
        void *p = sz >= (8ULL << 20) ? page_alloc(sz / 4096) : 0;
        if (!p) { LOG_INFO("Disk cache: off (not enough memory)\n"); return 0; }
        r[0].base = (uint64_t)p; r[0].len = sz; nr = 1; want = sz;
    }
    nents = (uint32_t)(want / CH);
    uint32_t hs = 1; while (hs < nents) hs <<= 1;
    ents = page_alloc(((uint64_t)nents * sizeof *ents + 4095) / 4096);
    htab = page_alloc(((uint64_t)hs * 4 + 4095) / 4096);
    bounce = page_alloc(MAX_RUN * CH / 4096);
    if (!ents || !htab || !bounce) { nents = 0; LOG_INFO("Disk cache: off (not enough memory for its index)\n"); return 0; }
    hmask = hs - 1;
    for (uint32_t i = 0; i < hs; i++) htab[i] = NIL;
    uint64_t left = want; uint32_t first = 0;
    for (int k = 0; k < nr && left; k++) {
        uint64_t len = r[k].len < left ? r[k].len : left;
        uint8_t *va;
        if (r[k].base >= 0x100000000ULL) { map_mmio_flags(r[k].base, len, 0); va = (uint8_t *)(HHDM + r[k].base); }   /* normal (cached) memory */
        else va = (uint8_t *)r[k].base;
        pools[npools].first = first; pools[npools].va = va; npools++;
        first += (uint32_t)(len / CH); left -= len;
    }
    nents = first;
    for (uint32_t i = 0; i < nents; i++) ents[i].next = i + 1 < nents ? i + 1 : NIL;
    free_head = 0;
    LOG_OK("Disk cache: %lu MB of memory%s\n", (uint64_t)nents * CH >> 20, r[0].base >= 0x100000000ULL ? " (above 4 GB)" : "");
    return r[0].base >= 0x100000000ULL ? want : 0;
}
/* the disk cache takes its part of the memory above 4 GiB first; the rest is for programs */
void bcache_init(void) { mem_high_init(cache_setup()); }

/* ---- in front of a drive ---- */
static int c_read(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    struct cwrap *w = ctx;
    uint32_t bs = w->bd->bsize;
    if (!nents || !n || bs > CH || CH % bs) return w->read(w->ctx, lba, n, buf);
    uint32_t per = CH / bs;                                       /* blocks per chunk */
    uint64_t c0 = lba / per, c1 = (lba + n - 1) / per;
    uint8_t *out = buf;
    for (uint64_t c = c0; c <= c1;) {
        uint64_t key = (uint64_t)w->id << 48 | c;
        uint32_t i = find(key);
        if (i != NIL) {                                           /* in memory */
            hits++;
            uint64_t from = c * per > lba ? c * per : lba, to = (c + 1) * per < lba + n ? (c + 1) * per : lba + n;
            memcpy(out + (from - lba) * bs, slot(i) + (from - c * per) * bs, (to - from) * bs);
            lru_unlink(i); lru_front(i);
            c++; continue;
        }
        /* a run of chunks that aren't cached: read them in one go */
        uint64_t e = c; while (e < c1 && e - c + 1 < MAX_RUN && find((uint64_t)w->id << 48 | (e + 1)) == NIL) e++;
        uint64_t rl = c * per, rn = (e - c + 1) * per;
        if (rl + rn > w->bd->blocks) rn = w->bd->blocks > rl ? w->bd->blocks - rl : 0;   /* the end of the drive */
        if (!rn) return w->read(w->ctx, lba, n, buf);
        uint32_t gen = w->gen;
        int r = w->read(w->ctx, rl, (uint32_t)rn, bounce);
        if (r) return r;
        misses += e - c + 1;
        uint64_t from = rl > lba ? rl : lba, to = rl + rn < lba + n ? rl + rn : lba + n;
        memcpy(out + (from - lba) * bs, bounce + (from - rl) * bs, (to - from) * bs);
        /* keep them, unless the drive was written to while this read was waiting (the data could be older) */
        if (gen == w->gen && w->used)
            for (uint64_t k = c; k <= e && (k - c + 1) * per <= rn; k++) insert((uint64_t)w->id << 48 | k, bounce + (k - c) * CH);
        c = e + 1;
    }
    return 0;
}
static int c_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) {
    struct cwrap *w = ctx;
    w->gen++;
    int r = w->write(w->ctx, lba, n, buf);
    uint32_t bs = w->bd->bsize;
    if (!nents || !n || bs > CH || CH % bs) return r;
    uint32_t per = CH / bs;
    for (uint64_t c = lba / per; c <= (lba + n - 1) / per; c++) {
        uint32_t i = find((uint64_t)w->id << 48 | c);
        if (i == NIL) continue;
        if (r) { lru_unlink(i); hash_remove(i); ents[i].key = 0; ents[i].next = free_head; free_head = i; used_chunks--; continue; }   /* failed: forget it */
        uint64_t from = c * per > lba ? c * per : lba, to = (c + 1) * per < lba + n ? (c + 1) * per : lba + n;
        memcpy(slot(i) + (from - c * per) * bs, (const uint8_t *)buf + (from - lba) * bs, (to - from) * bs);
    }
    w->gen++;
    return r;
}
static int c_flush(void *ctx) { struct cwrap *w = ctx; return w->flush ? w->flush(w->ctx) : 0; }
static void c_release(void *ctx) { struct cwrap *w = ctx; if (w->release) w->release(w->ctx); }

void bcache_attach(struct blockdev *bd) {
    if (bd->read == c_read) return;
    struct cwrap *w = 0; static int next_id = 1;
    for (int i = 0; i < 16; i++) if (!wraps[i].used) { w = &wraps[i]; break; }
    if (!w) return;
    w->used = true; w->id = next_id++; w->gen = 0; w->bd = bd;
    w->ctx = bd->ctx; w->read = bd->read; w->write = bd->write; w->flush = bd->flush; w->release = bd->release;
    bd->ctx = w; bd->read = c_read; bd->write = c_write;
    bd->flush = w->flush ? c_flush : 0; bd->release = w->release ? c_release : 0;
}
/* the drive is gone: forget what was cached for it */
void bcache_detach(struct blockdev *bd) {
    if (bd->read != c_read) return;
    struct cwrap *w = bd->ctx;
    for (uint32_t i = 0; i < nents; i++)
        if (ents[i].key && ents[i].key >> 48 == (uint64_t)w->id) {          /* key 0: a free slot */
            lru_unlink(i); hash_remove(i); ents[i].key = 0; ents[i].next = free_head; free_head = i; used_chunks--;
        }
    bd->ctx = w->ctx; bd->read = w->read; bd->write = w->write; bd->flush = w->flush; bd->release = w->release;
    w->used = false;
}

uint64_t bcache_bytes(void) { return used_chunks * CH; }
uint64_t bcache_capacity(void) { return (uint64_t)nents * CH; }
void bcache_stats(uint64_t *h, uint64_t *m) { *h = hits; *m = misses; }
