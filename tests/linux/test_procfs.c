/* /proc as folders and paths relative to an open folder (the *at calls), as Chromium's sandbox code uses
 * them even with --no-sandbox: it opens /proc, counts its threads by the links of self/task, and waits for
 * a stopped thread's self/task/<tid> to go. Prints PASS/FAIL. Build: gcc -static-pie -pthread. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int go, tid;
static void *worker(void *x) {
    pthread_mutex_lock(&mu); tid = (int)syscall(SYS_gettid); pthread_cond_broadcast(&cv);
    while (!go) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu); return 0;
}
static int count_dir(int dfd, const char *want, int *found) {   /* entries (not . or ..); *found if want is one */
    DIR *d = fdopendir(dfd); int n = 0; struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; n++; if (want && !strcmp(e->d_name, want)) *found = 1; }
    closedir(d); return n;
}
int main(void) {
    struct stat st; char b[256];
    int pfd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(pfd >= 0, "open /proc as a folder (%d)", pfd);
    int dot = openat(pfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dot >= 0, "openat(/proc, \".\") (%d, errno %d)", dot, errno);
    int d2 = openat(AT_FDCWD, "/proc/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(d2 >= 0, "openat(AT_FDCWD, \"/proc/\")");
    CHECK(fstatat(dot, "self/task/", &st, 0) == 0 && S_ISDIR(st.st_mode), "fstatat self/task/ is a folder");
    CHECK(st.st_nlink == 3, "one thread: self/task has 3 links (%lu)", (unsigned long)st.st_nlink);
    snprintf(b, sizeof b, "self/task/%d/", (int)syscall(SYS_gettid));
    CHECK(fstatat(pfd, b, &st, 0) == 0, "own thread's %s is there", b);

    pthread_t th; pthread_create(&th, 0, worker, 0);
    pthread_mutex_lock(&mu); while (!tid) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    CHECK(fstatat(pfd, "self/task", &st, 0) == 0 && st.st_nlink == 4, "two threads: 4 links (%lu)", (unsigned long)st.st_nlink);
    snprintf(b, sizeof b, "self/task/%d/", tid);
    CHECK(fstatat(pfd, b, &st, 0) == 0, "the new thread's %s is there", b);
    int t = openat(pfd, "self/task", O_RDONLY | O_DIRECTORY), found = 0; char tn[16]; snprintf(tn, sizeof tn, "%d", tid);
    CHECK(count_dir(t, tn, &found) == 2 && found, "self/task lists both threads");
    pthread_mutex_lock(&mu); go = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(th, 0);
    int gone = 0;
    for (int i = 0; i < 200 && !gone; i++) { if (fstatat(pfd, b, &st, 0) < 0 && errno == ENOENT) gone = 1; else usleep(1000); }
    CHECK(gone, "a joined thread's %s goes", b);
    CHECK(fstatat(pfd, "self/task", &st, 0) == 0 && st.st_nlink == 3, "back to 3 links");
    CHECK(fstat(pfd, &st) == 0 && S_ISDIR(st.st_mode), "fstat /proc: a folder");

    snprintf(b, sizeof b, "/proc/%d", getpid());
    CHECK(stat(b, &st) == 0 && S_ISDIR(st.st_mode), "%s is a folder", b);
    CHECK(stat("/proc/999999", &st) < 0 && errno == ENOENT, "/proc/999999 isn't there");
    CHECK(stat("/proc/self/nothing", &st) < 0 && errno == ENOENT, "/proc/self/nothing isn't there");
    CHECK(access("/proc/self/task", F_OK) == 0, "access /proc/self/task");
    CHECK(open("/proc/self/task", O_RDWR) < 0 && errno == EISDIR, "a /proc folder can't be written");

    int keep = open("/proc/cpuinfo", O_RDONLY); found = 0; snprintf(tn, sizeof tn, "%d", keep);
    int nfd = count_dir(open("/proc/self/fd", O_RDONLY | O_DIRECTORY), tn, &found);
    CHECK(nfd >= 4 && found, "/proc/self/fd lists open files (%d, fd %d listed)", nfd, keep);
    long r = read(keep, b, sizeof b - 1); b[r > 0 ? r : 0] = 0;
    CHECK(strstr(b, "processor") != 0, "/proc/cpuinfo reads");
    int m = openat(pfd, "self/maps", O_RDONLY); r = m >= 0 ? read(m, b, sizeof b - 1) : -1;
    CHECK(r > 0, "openat(/proc, self/maps) reads (%ld)", r);
    char exe[256], exe2[256]; long l1 = readlink("/proc/self/exe", exe, 255), l2 = readlinkat(pfd, "self/exe", exe2, 255);
    CHECK(l1 > 0 && l1 == l2 && !memcmp(exe, exe2, l1), "readlink(at) self/exe (%.*s)", (int)(l1 > 0 ? l1 : 0), exe);
    int e = openat(pfd, "self/exe", O_RDONLY); char elf[4] = { 0 }; if (e >= 0) read(e, elf, 4);
    CHECK(!memcmp(elf, "\177ELF", 4), "open /proc/self/exe: the program itself");
    CHECK(readlink("/proc/self/maps", b, 10) < 0 && errno == EINVAL, "readlink of a file that isn't a link: EINVAL");

    /* *at with a normal folder */
    mkdir("/tmp/pfs", 0755);
    int tfd = open("/tmp/pfs", O_RDONLY | O_DIRECTORY);
    int f = openat(tfd, "a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(f, "hi", 2); close(f);
    CHECK(stat("/tmp/pfs/a.txt", &st) == 0 && st.st_size == 2, "openat(dirfd, a.txt, O_CREAT) made /tmp/pfs/a.txt");
    CHECK(fstatat(tfd, "a.txt", &st, 0) == 0 && st.st_size == 2, "fstatat(dirfd, a.txt)");
    CHECK(faccessat(tfd, "a.txt", R_OK, 0) == 0, "faccessat(dirfd, a.txt)");
    CHECK(fstatat(1, "a.txt", &st, 0) < 0 && errno == ENOTDIR, "fstatat with a dirfd that isn't a folder: ENOTDIR");
    unlink("/tmp/pfs/a.txt");
    printf("%s\n", fails ? "SOME FAILED" : "PASS"); return fails != 0;
}
