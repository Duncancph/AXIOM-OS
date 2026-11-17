/* Long file names in one folder: 400 similar names created, renamed and read back (their 8.3 names must
 * all differ; fsck.fat on the disk afterwards checks that). Also times it.
 *   gcc -static-pie -O2 -o test_names test_names.c */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
static void check(int ok, const char *what, int i) { checks++; if (!ok) { fails++; if (fails < 20) printf("FAIL %s %d\n", what, i); } }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "names";
    enum { N = 400 };
    mkdir(dir, 0755);
    char a[256], b[256], buf[64];
    double t0 = now();
    for (int i = 0; i < N; i++) {
        snprintf(a, sizeof a, "%s/cachefileentry_%04d.data", dir, i);
        int fd = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        check(fd >= 0, "create", i);
        if (fd < 0) continue;
        int n = snprintf(buf, sizeof buf, "file %d", i);
        check(write(fd, buf, n) == n, "write", i);
        close(fd);
    }
    double t1 = now();
    for (int i = 0; i < N; i++) {
        snprintf(a, sizeof a, "%s/cachefileentry_%04d.data", dir, i);
        snprintf(b, sizeof b, "%s/cachefilerenamed_%04d.data", dir, i);
        check(rename(a, b) == 0, "rename", i);
    }
    double t2 = now();
    for (int i = 0; i < N; i++) {
        snprintf(b, sizeof b, "%s/cachefilerenamed_%04d.data", dir, i);
        int fd = open(b, O_RDONLY);
        check(fd >= 0, "open", i);
        if (fd < 0) continue;
        char want[64]; int n = snprintf(want, sizeof want, "file %d", i);
        memset(buf, 0, sizeof buf);
        check(read(fd, buf, sizeof buf) == n && !memcmp(buf, want, n), "content", i);
        close(fd);
        snprintf(a, sizeof a, "%s/cachefileentry_%04d.data", dir, i);
        check(access(a, F_OK) != 0, "old name gone", i);
    }
    for (int i = 0; i < N; i += 2) {                     /* delete half, then create again into the gaps */
        snprintf(b, sizeof b, "%s/cachefilerenamed_%04d.data", dir, i);
        check(unlink(b) == 0, "unlink", i);
    }
    for (int i = 0; i < N; i += 2) {
        snprintf(a, sizeof a, "%s/cachefileagain_%04d.data", dir, i);
        int fd = open(a, O_CREAT | O_WRONLY, 0644);
        check(fd >= 0, "create again", i);
        if (fd >= 0) close(fd);
    }
    double t3 = now();
    printf("names: create %.2f s, rename %.2f s, check+delete+create %.2f s\n", t1 - t0, t2 - t1, t3 - t2);
    printf("names: %d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
