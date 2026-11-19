/* Linux compatibility check: the lock-free clock calls. CLOCK_MONOTONIC never goes back across threads, realtime
 * agrees with time() and gettimeofday(), a 100 ms sleep takes 100-130 ms.  gcc -O1 -static-pie -pthread -o test_clocks tests/linux/test_clocks.c */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static int bad;
static void *w(void *p) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 200000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) bad++;
        a = b;
    }
    if (syscall(SYS_gettid) == getpid() && p) bad++;
    return 0;
}
int main(void) {
    pthread_t t[4]; for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, w, (void *)1);
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    struct timespec r; clock_gettime(CLOCK_REALTIME, &r); time_t s = time(0);
    struct timeval tv; gettimeofday(&tv, 0);
    long d1 = (long)(s - r.tv_sec), d2 = (long)(tv.tv_sec - r.tv_sec);
    struct timespec m0, m1; clock_gettime(CLOCK_MONOTONIC, &m0); usleep(100000); clock_gettime(CLOCK_MONOTONIC, &m1);
    long ms = (m1.tv_sec - m0.tv_sec) * 1000 + (m1.tv_nsec - m0.tv_nsec) / 1000000;
    printf("clocks: backwards %d, realtime-time %ld s, gettimeofday-realtime %ld s, 100 ms sleep took %ld ms, year %ld: %s\n",
           bad, d1, d2, ms, 1970 + r.tv_sec / 31557600, !bad && d1 >= 0 && d1 <= 1 && d2 >= 0 && d2 <= 1 && ms >= 100 && ms < 130 && r.tv_sec > 1700000000 ? "PASS" : "FAIL");
    return 0;
}
