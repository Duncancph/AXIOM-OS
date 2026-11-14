/* Kernel heap: power-of-two buckets for small objects, whole pages for big ones. */
#include "kernel.h"

#define MAGIC 0x4B4D414CU
struct hdr { uint32_t magic, bucket; uint64_t pages; };   /* 16 bytes */
static void *freel[8];                                      /* 32 .. 4096-16 */
static const uint32_t bsize[8] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };

void *kmalloc(size_t n) {
    size_t need = n + sizeof(struct hdr);
    for (int b = 0; b < 8; b++) {
        if (need > bsize[b]) continue;
        if (!freel[b]) {
            uint8_t *pg = page_alloc(1);
            if (!pg) return 0;
            for (uint32_t off = 0; off + bsize[b] <= 4096; off += bsize[b]) {
                *(void **)(pg + off) = freel[b];
                freel[b] = pg + off;
            }
        }
        struct hdr *h = freel[b];
        freel[b] = *(void **)h;
        h->magic = MAGIC; h->bucket = b; h->pages = 0;
        memset(h + 1, 0, n);
        return h + 1;
    }
    size_t pages = (need + 4095) / 4096;
    struct hdr *h = page_alloc(pages);
    if (!h) return 0;
    h->magic = MAGIC; h->bucket = 99; h->pages = pages;
    return h + 1;
}

void kfree(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != MAGIC) { kprintf("[kmalloc] bad free %p\n", p); return; }
    h->magic = 0;
    if (h->bucket < 8) { *(void **)h = freel[h->bucket]; freel[h->bucket] = h; return; }
    for (uint64_t i = 0; i < h->pages; i++) page_free((uint8_t *)h + i * 4096);
}
