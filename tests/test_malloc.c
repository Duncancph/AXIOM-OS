/* Stress test for user/lib/malloc.c: random malloc/free/realloc, checking that blocks never overlap and keep their contents. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
void *malloc_d(size_t), free_d(void *), *realloc_d(void *, size_t);
static uint8_t arena[256 << 20]; static size_t used;
long sys_call(long n, long a, long b, long c, long d, long e) {        /* SYS_MEM_GROW: contiguous, like the kernel */
    size_t len = (a + 4095) & ~4095ul;
    if (used + len > sizeof arena) return 0;
    void *p = arena + used; used += len; return (long)p;
}
#define N 4000
static uint8_t *ptr[N]; static size_t sz[N]; static uint8_t tag[N];
static void check(int i) { for (size_t k = 0; k < sz[i]; k++) if (ptr[i][k] != tag[i]) { printf("FAIL: block %d corrupted at %zu\n", i, k); exit(1); } }
int main(void) {
    srand(42);
    long ops = 0;
    for (int round = 0; round < 400000; round++) {
        int i = rand() % N, r = rand() % 10;
        size_t n = rand() % 8 == 0 ? rand() % 300000 : rand() % 600;
        if (ptr[i] && r < 4) { check(i); free_d(ptr[i]); ptr[i] = 0; }
        else if (ptr[i] && r < 6) {
            check(i);
            uint8_t *q = realloc_d(ptr[i], n);
            if (!q && n) { printf("FAIL: realloc %zu\n", n); return 1; }
            size_t keep = n < sz[i] ? n : sz[i];
            for (size_t k = 0; k < keep; k++) if (q[k] != tag[i]) { printf("FAIL: realloc lost data\n"); return 1; }
            ptr[i] = q; sz[i] = n; if (n) memset(q, tag[i], n);
        } else if (!ptr[i]) {
            ptr[i] = malloc_d(n);
            if (!ptr[i]) { printf("FAIL: malloc %zu returned 0 (arena used %zu MB)\n", n, used >> 20); return 1; }
            if ((uintptr_t)ptr[i] % 16) { printf("FAIL: misaligned\n"); return 1; }
            sz[i] = n; tag[i] = rand(); memset(ptr[i], tag[i], n);
        }
        ops++;
    }
    for (int i = 0; i < N; i++) if (ptr[i]) { check(i); free_d(ptr[i]); }
    void *big = malloc_d(100 << 20);      /* after freeing everything, a big block must fit in what was used */
    printf("malloc: %ld random operations, arena grew to %zu MB, big block after free-all %s\n", ops, used >> 20, big ? "ok" : "FAILED");
    return big ? 0 : 1;
}
