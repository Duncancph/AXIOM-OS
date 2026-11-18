/* Linux compatibility check: futex operations beyond WAIT/WAKE. FUTEX_WAKE_OP (which Qt 5's semaphores use: a Qt
 * app hung at its first widget without it), with each operation and comparison, OPARG_SHIFT, a waiter on each
 * word, and the count it returns; FUTEX_WAIT_BITSET with an absolute timeout; REQUEUE. Prints PASS/FAIL.
 *   gcc -O1 -static-pie -pthread -o test_futex tests/linux/test_futex.c   (also runs on Linux itself) */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)

static long fx(uint32_t *a, int op, uint32_t v, uintptr_t v2, uint32_t *a2, uint32_t v3) {
    return syscall(SYS_futex, a, op, v, v2, a2, v3);
}
struct waiter { uint32_t *word; uint32_t expect; volatile int woke; };
static void *wait_thread(void *p) {
    struct waiter *w = p;
    struct timespec t = { 5, 0 };                           /* (a missed wake shows as a timeout, not a hang) */
    long r = fx(w->word, FUTEX_WAIT_PRIVATE, w->expect, (uintptr_t)&t, 0, 0);
    w->woke = r == 0 ? 1 : -1;
    return 0;
}
static void start(pthread_t *t, struct waiter *w) { pthread_create(t, 0, wait_thread, w); usleep(200000); }

int main(void) {
    /* WAKE_OP: *b = *b OP arg; wake one on a; if old *b CMP cmparg, wake one on b */
    uint32_t a = 0, b = 5;
    struct waiter wa = { &a, 0, 0 }, wb = { &b, 5, 0 };
    pthread_t ta, tb; start(&ta, &wa);
    pthread_t tb2; (void)tb2;
    start(&tb, &wb);
    long r = fx(&a, FUTEX_WAKE_OP_PRIVATE, 1, 1, &b, FUTEX_OP(FUTEX_OP_ADD, 3, FUTEX_OP_CMP_EQ, 5));
    pthread_join(ta, 0); pthread_join(tb, 0);
    CHECK(r == 2, "WAKE_OP returns how many it woke (%ld)", r);
    CHECK(b == 8, "WAKE_OP ADD 3: the second word 5 -> %u", b);
    CHECK(wa.woke == 1 && wb.woke == 1, "the waiter on each word woke (the old value matched)");

    /* comparison false: only the first word's waiter wakes */
    a = 0; b = 8; wa.woke = wb.woke = 0; wb.expect = 8;
    start(&ta, &wa);
    r = fx(&a, FUTEX_WAKE_OP_PRIVATE, 1, 1, &b, FUTEX_OP(FUTEX_OP_SET, 1, FUTEX_OP_CMP_GT, 100));
    pthread_join(ta, 0);
    CHECK(r == 1 && wa.woke == 1 && b == 1, "SET 1 with old 8 > 100 false: one woken (%ld), the word set (%u)", r, b);

    /* the other operations, OPARG_SHIFT, signed arguments */
    struct { int op, arg; uint32_t from, to; const char *name; } ops[] = {
        { FUTEX_OP_OR, 0x30, 0x0f, 0x3f, "OR" }, { FUTEX_OP_ANDN, 0x03, 0x0f, 0x0c, "ANDN" },
        { FUTEX_OP_XOR, 0xff, 0x0f, 0xf0, "XOR" }, { FUTEX_OP_ADD, -2, 10, 8, "ADD -2" },
        { FUTEX_OP_OR | FUTEX_OP_OPARG_SHIFT, 31, 0, 0x80000000u, "OR 1<<31 (OPARG_SHIFT)" },
    };
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        b = ops[i].from;
        r = fx(&a, FUTEX_WAKE_OP_PRIVATE, 0, 0, &b, FUTEX_OP(ops[i].op, ops[i].arg & 0xfff, FUTEX_OP_CMP_EQ, 0));
        CHECK(r == 0 && b == ops[i].to, "%s: %#x -> %#x", ops[i].name, ops[i].from, b);
    }
    struct { int cmp, arg; uint32_t old; int want; const char *name; } cmps[] = {
        { FUTEX_OP_CMP_NE, 3, 4, 1, "NE" }, { FUTEX_OP_CMP_LT, 5, 4, 1, "LT" }, { FUTEX_OP_CMP_LE, 4, 4, 1, "LE" },
        { FUTEX_OP_CMP_GE, 5, 4, 0, "GE" }, { FUTEX_OP_CMP_LT, -1, 4, 0, "LT -1" },
    };
    for (unsigned i = 0; i < sizeof cmps / sizeof cmps[0]; i++) {
        b = cmps[i].old; wb.woke = 0; wb.expect = cmps[i].old + 100;   /* (SET old+100: the waiter's value) */
        uint32_t tmp = b; b = wb.expect; start(&tb, &wb); b = tmp;
        r = fx(&a, FUTEX_WAKE_OP_PRIVATE, 0, 1, &b, FUTEX_OP(FUTEX_OP_SET, (cmps[i].old + 100) & 0xfff, cmps[i].cmp, cmps[i].arg & 0xfff));
        if (!cmps[i].want) { fx(&b, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
        pthread_join(tb, 0);
        CHECK(r == cmps[i].want, "comparison %s on old %u: %s (%ld woken)", cmps[i].name, cmps[i].old, cmps[i].want ? "wakes" : "doesn't", r);
    }

    /* WAIT_BITSET with an absolute CLOCK_MONOTONIC timeout */
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec until = { now.tv_sec, now.tv_nsec + 200000000 };
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    a = 0;
    r = fx(&a, FUTEX_WAIT_BITSET_PRIVATE, 0, (uintptr_t)&until, 0, FUTEX_BITSET_MATCH_ANY);
    CHECK(r < 0 && errno == ETIMEDOUT, "WAIT_BITSET with an absolute time: ETIMEDOUT");
    a = 1;
    r = fx(&a, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
    CHECK(r < 0 && errno == EAGAIN, "WAIT on a value that changed: EAGAIN");

    /* CMP_REQUEUE: wakes one; the rest wait on the other word, or are woken (either is allowed) */
    a = 0; b = 0; wa.woke = 0; wa.expect = 0;
    start(&ta, &wa);
    r = fx(&a, FUTEX_CMP_REQUEUE_PRIVATE, 1, 1, &b, 0);
    pthread_join(ta, 0);
    CHECK(r >= 1 && wa.woke == 1, "CMP_REQUEUE woke the waiter (%ld)", r);

    printf("%d checks, %s\n", checks, fails ? "SOME FAILED" : "PASS");
    return fails != 0;
}
