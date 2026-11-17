/* Signals on AXIOM's Linux layer: handlers, siginfo, masks, EINTR, and a Boehm-GC-style stop-the-world
 * (pthread_kill to a busy thread and a blocked one, sigsuspend until a restart signal). Prints PASS/FAIL. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)
static volatile int got1, got_info, info_ok; static volatile double fpv;
static void h1(int s) { got1++; fpv = 1.0; for (int i = 0; i < 10; i++) fpv *= 3.5; }   /* (uses the FPU) */
static void hinfo(int s, siginfo_t *si, void *uc) { got_info = s; info_ok = si->si_signo == s && uc != 0; }
/* stop-the-world, as Boehm GC does it */
static volatile int suspended, resumed_count; static sigset_t wait_mask;
static void h_suspend(int s) { __atomic_add_fetch(&suspended, 1, __ATOMIC_SEQ_CST); sigsuspend(&wait_mask); __atomic_add_fetch(&resumed_count, 1, __ATOMIC_SEQ_CST); }
static void h_restart(int s) {}
static volatile int stop_busy; static volatile long work;
static void *busy(void *a) { while (!stop_busy) work++; return 0; }
static int pfd[2]; static volatile int read_errno;
static void *reader(void *a) { char c; long r = read(pfd[0], &c, 1); read_errno = r < 0 ? errno : 0; return 0; }
static volatile long restart_r = -2; static volatile int restart_h;
static void h_count(int s) { restart_h++; }
static void *reader2(void *a) { char c; restart_r = read(pfd[0], &c, 1); return 0; }
static void *sleeper(void *a) { char c; while (read(pfd[0], &c, 1) < 0 && errno == EINTR) ; return 0; }   /* (as code that retries) */
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = h1; sigaction(SIGUSR1, &sa, 0);
    double x = 2.25;
    kill(getpid(), SIGUSR1);
    CHECK(got1 == 1, "handler ran (kill to itself)");
    CHECK(x == 2.25 && fpv > 270000 && fpv < 280000, "FPU kept across the handler (%g, %g)", x, (double)fpv);
    memset(&sa, 0, sizeof sa); sa.sa_sigaction = hinfo; sa.sa_flags = SA_SIGINFO; sigaction(SIGUSR2, &sa, 0);
    raise(SIGUSR2);
    CHECK(got_info == SIGUSR2 && info_ok, "SA_SIGINFO: signal number and context");
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
    kill(getpid(), SIGUSR1);
    sigset_t p; sigpending(&p);
    CHECK(got1 == 1 && sigismember(&p, SIGUSR1), "blocked: pending, not run");
    sigprocmask(SIG_UNBLOCK, &m, 0);
    CHECK(got1 == 2, "unblocked: runs");
    /* EINTR: a thread blocked in read() */
    pipe(pfd);
    memset(&sa, 0, sizeof sa); sa.sa_handler = h_restart; sigaction(SIGURG, &sa, 0);   /* (no SA_RESTART) */
    pthread_t rt; pthread_create(&rt, 0, reader, 0); msleep(100);
    pthread_kill(rt, SIGURG); pthread_join(rt, 0);
    CHECK(read_errno == EINTR, "a blocked read stops with EINTR (%d)", read_errno);
    memset(&sa, 0, sizeof sa); sa.sa_handler = h_count; sa.sa_flags = SA_RESTART; sigaction(SIGUSR2, &sa, 0);
    pthread_t rt2; pthread_create(&rt2, 0, reader2, 0); msleep(100);
    pthread_kill(rt2, SIGUSR2); msleep(100);
    int still = restart_r == -2;
    write(pfd[1], "y", 1); pthread_join(rt2, 0);
    CHECK(restart_h == 1 && still && restart_r == 1, "SA_RESTART: the handler runs and the read goes on (%d %d %ld)", restart_h, still, (long)restart_r);
    /* stop-the-world: a busy thread and a blocked one */
    memset(&sa, 0, sizeof sa); sa.sa_handler = h_suspend; sigfillset(&sa.sa_mask); sigaction(SIGPWR, &sa, 0);
    memset(&sa, 0, sizeof sa); sa.sa_handler = h_restart; sigaction(SIGXCPU, &sa, 0);
    sigfillset(&wait_mask); sigdelset(&wait_mask, SIGXCPU);
    pthread_t b, s; pthread_create(&b, 0, busy, 0); pthread_create(&s, 0, sleeper, 0); msleep(100);
    for (int round = 0; round < 20; round++) {
        suspended = 0;
        pthread_kill(b, SIGPWR); pthread_kill(s, SIGPWR);
        for (int i = 0; i < 2000 && suspended < 2; i++) msleep(1);
        long w0 = work; msleep(20);
        if (suspended < 2 || work != w0) { CHECK(0, "round %d: suspended %d, busy thread still working %d", round, suspended, work != w0); break; }
        int r0 = resumed_count;
        pthread_kill(b, SIGXCPU); pthread_kill(s, SIGXCPU);
        for (int i = 0; i < 2000 && resumed_count < r0 + 2; i++) msleep(1);
        if (resumed_count < r0 + 2) { CHECK(0, "round %d: resumed %d of 2", round, resumed_count - r0); break; }
        if (round == 19) CHECK(1, "stop-the-world 20 times: both threads stop (the busy one stops working) and go on");
    }
    stop_busy = 1; pthread_join(b, 0); write(pfd[1], "x", 1); pthread_join(s, 0);
    printf("%s\n", fails ? "SOME FAILED" : "PASS"); return fails != 0;
}
