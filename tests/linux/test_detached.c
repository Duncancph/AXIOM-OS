/* Linux compatibility check: 40000 short-lived detached threads (8 MiB stacks each, 40 alive at a time): their stacks
 * must be reused, not leaked, or the address space runs out.  gcc -O1 -static-pie -pthread -o test_detached tests/linux/test_detached.c */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
static atomic_int alive;
static void *w(void *p) { usleep(200); atomic_fetch_sub(&alive, 1); return p; }
int main(void) {
    int made = 0, fail = 0;
    for (int i = 0; i < 40000; i++) {
        while (atomic_load(&alive) > 40) usleep(100);
        pthread_t t; atomic_fetch_add(&alive, 1);
        int r = pthread_create(&t, 0, w, 0);
        if (r) { atomic_fetch_sub(&alive, 1); if (!fail++) printf("pthread_create failed after %d: %s\n", made, strerror(r)); fflush(stdout); usleep(1000); continue; }
        pthread_detach(t); made++;
        if (made % 10000 == 0) { printf("%d detached threads so far\n", made); fflush(stdout); }
    }
    printf("detached: %d made, %d failed: %s\n", made, fail, fail ? "FAIL" : "PASS");
    return 0;
}
