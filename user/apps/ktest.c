/* Kernel test for ported software: memory maps (big reservations, pages given when touched, protection,
 * executable code), threads (with futex joins), thread-local storage (%fs) and clocks. Results go to the log. */
#include "../lib/dunc.h"

static int fails;
static void say(const char *what, bool ok, long v) {
    char b[160]; ksnprintf(b, sizeof b, "ktest: %s %s (%ld)", ok ? "PASS" : "FAIL", what, v);
    sys_log(b);
    if (!ok) fails++;
}
static long mmap_(unsigned long a, unsigned long n, int prot, int fl) { return sys_call(SYS_MMAP, (long)a, (long)n, prot, fl, 0); }

/* ---- threads ---- */
#define NT 8
static volatile uint32_t counter;
static volatile uint32_t tids[NT];               /* cleared by the kernel when each thread ends */
static uint64_t tls_blocks[NT][4];
static volatile uint64_t tls_seen[NT];
static volatile uint32_t gate;                   /* threads wait on this futex until it is 1 */

static void worker(long i) {
    uint64_t v; __asm__ volatile("mov %%fs:8, %0" : "=r"(v));     /* its own thread-local block */
    tls_seen[i] = v;
    while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) == 0) sys_call(SYS_FUTEX, FUTEX_OP_WAIT, (long)&gate, 0, 2000, 0);
    for (int k = 0; k < 100000; k++) __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
    sys_call(SYS_THREAD_EXIT, 0, 0, 0, 0, 0);
}
/* new threads start here: the stack holds the function and its argument */
__asm__(".globl thread_start\nthread_start:\n pop %rax\n pop %rdi\n and $-16, %rsp\n call *%rax\n ud2\n");
void thread_start(void);

int main(void) {
    /* 1. a big reservation costs nothing; pages come when touched */
    long big = mmap_(0, 8ull << 30, 0, 0);
    say("reserve 8 GiB", big > 0 && (big & 4095) == 0, big);
    long r = sys_call(SYS_MPROTECT, big + (1ull << 30), 1 << 20, DPROT_READ | DPROT_WRITE, 0, 0);
    say("mprotect 1 MiB of it read/write", r == 0, r);
    volatile uint8_t *p = (uint8_t *)(big + (1ull << 30));
    p[0] = 7; p[4096 * 100] = 9; p[(1 << 20) - 1] = 11;
    say("pages appear when written", p[0] == 7 && p[4096 * 100] == 9 && p[(1 << 20) - 1] == 11 && p[5000] == 0, p[4096 * 100]);
    r = sys_call(SYS_MUNMAP, big, 8ull << 30, 0, 0, 0);
    say("munmap the reservation", r == 0, r);

    /* 2. executable code: write it, make it read+execute, run it */
    long code = mmap_(0, 4096, DPROT_READ | DPROT_WRITE, 0);
    uint8_t *c = (uint8_t *)code;
    c[0] = 0xB8; c[1] = 42; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 0xC3;   /* mov eax, 42; ret */
    r = sys_call(SYS_MPROTECT, code, 4096, DPROT_READ | DPROT_EXEC, 0, 0);
    int (*fn)(void) = (int (*)(void))code;
    int got = fn();
    say("run generated code (JIT-style)", r == 0 && got == 42, got);

    /* 3. fixed address, then replacing it */
    long fx = mmap_(0xD000000000ull, 8192, DPROT_READ | DPROT_WRITE, DMAP_FIXED);
    ((volatile uint8_t *)fx)[0] = 5;
    long fx2 = mmap_(0xD000000000ull, 8192, DPROT_READ | DPROT_WRITE, DMAP_FIXED);
    say("MAP_FIXED gives fresh zeroed memory", fx == 0xD000000000ll && fx2 == fx && ((volatile uint8_t *)fx)[0] == 0, fx);

    /* 4. threads, each with its own %fs, joined by futexes on their tid words */
    uint64_t t0 = sys_call(SYS_CLOCK, 1, 0, 0, 0, 0);
    for (int i = 0; i < NT; i++) {
        long st = mmap_(0, 64 * 1024, DPROT_READ | DPROT_WRITE, 0);
        uint64_t *sp = (uint64_t *)(st + 64 * 1024);
        *--sp = (uint64_t)i; *--sp = (uint64_t)worker;
        tls_blocks[i][0] = (uint64_t)&tls_blocks[i][0]; tls_blocks[i][1] = 1000 + i;
        tids[i] = 1;
        long tid = sys_call(SYS_THREAD, (long)thread_start, (long)sp, (long)&tls_blocks[i][0], (long)&tids[i], 0);
        if (tid <= 0) { say("create thread", false, tid); tids[i] = 0; }
    }
    gate = 1;
    sys_call(SYS_FUTEX, FUTEX_OP_WAKE, (long)&gate, 1 << 30, 0, 0);
    for (int i = 0; i < NT; i++) while (tids[i]) sys_call(SYS_FUTEX, FUTEX_OP_WAIT, (long)&tids[i], 1, 5000, 0);
    uint64_t t1 = sys_call(SYS_CLOCK, 1, 0, 0, 0, 0);
    say("8 threads x 100000 atomic adds", counter == NT * 100000, counter);
    bool tls_ok = true;
    for (int i = 0; i < NT; i++) if (tls_seen[i] != 1000u + i) tls_ok = false;
    say("each thread sees its own %fs block", tls_ok, (long)tls_seen[3]);
    say("threads took (ms)", t1 > t0, (long)((t1 - t0) / 1000000));

    /* 5. futex timeout */
    volatile uint32_t w = 0;
    uint64_t a = sys_call(SYS_CLOCK, 1, 0, 0, 0, 0);
    r = sys_call(SYS_FUTEX, FUTEX_OP_WAIT, (long)&w, 0, 50, 0);
    uint64_t b = sys_call(SYS_CLOCK, 1, 0, 0, 0, 0);
    say("futex wait times out after ~50 ms", r == -DERR_TIMEDOUT && b - a >= 40000000ull && b - a < 500000000ull, (long)((b - a) / 1000000));
    r = sys_call(SYS_FUTEX, FUTEX_OP_WAIT, (long)&w, 1, 50, 0);
    say("futex wait returns at once when the value differs", r == -DERR_AGAIN, r);

    /* 6. clocks */
    long rt = sys_call(SYS_CLOCK, 0, 0, 0, 0, 0);
    say("real time is after 2024", rt / 1000000000l > 1704067200l, rt / 1000000000l);

    char s[80]; ksnprintf(s, sizeof s, "ktest: done, %d failed", fails);
    sys_log(s);
    return fails;
}
