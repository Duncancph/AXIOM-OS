/* Linux compatibility check: 600 threads alive at once (more than the old 256-task limit Vertex hit), then 1800 more in rounds, so ended threads give their slots back.
 *   gcc -O1 -static-pie -pthread -o test_manythreads tests/linux/test_manythreads.c   (also runs on Linux itself) */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t c = PTHREAD_COND_INITIALIZER; static int go;
static void *w(void *p) { pthread_mutex_lock(&m); while (!go) pthread_cond_wait(&c, &m); pthread_mutex_unlock(&m); return p; }
int main(void) {
    static pthread_t t[600]; int n = 0;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 << 10);
    for (; n < 600; n++) if (pthread_create(&t[n], &a, w, 0)) break;
    printf("started %d threads at once\n", n); fflush(stdout);
    pthread_mutex_lock(&m); go = 1; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    int k = 0;                                                  /* and again: the ended ones' slots come back */
    for (int r = 0; r < 3; r++) { go = 0; int s = 0;
        for (; s < 600; s++) if (pthread_create(&t[s], &a, w, 0)) break;
        pthread_mutex_lock(&m); go = 1; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
        for (int i = 0; i < s; i++) pthread_join(t[i], 0); k += s; }
    printf("then %d more in 3 rounds: %s\n", k, n == 600 && k == 1800 ? "PASS" : "FAIL");
    return 0;
}
