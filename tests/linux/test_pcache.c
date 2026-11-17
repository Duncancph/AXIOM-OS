/* The page cache: read-only file mappings share pages between processes. Four children map the same 8 MB file
 * (this program's own file, which has 8 MB of read-only data in it: programs and libraries are what's shared)
 * and touch every page; memory should grow by about one copy, not four. One child makes its mapping writable and
 * writes to it: the others still see the file. When they end, the memory comes back. A file that's rewritten
 * (same size, within FAT's 2 s date step) is mapped with its new contents, not the old pages. Prints PASS/FAIL. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)
#define SZ (8 << 20)
__attribute__((used)) static const unsigned char big[SZ] = {1};        /* (in the file: 8 MB, initialised) */
static long exe_size;
static long memfree_kb(void) {
    char b[4096]; int f = open("/proc/meminfo", O_RDONLY); if (f < 0) return -1;
    long n = read(f, b, sizeof b - 1); close(f); if (n <= 0) return -1; b[n] = 0;
    char *p = strstr(b, "MemFree:"); return p ? atol(p + 8) : -1;
}
static const char *path = "/proc/self/exe", *rw = "/home/pcache-test.bin";
static int touch_check(const unsigned char *m, int fd) {   /* every page read, the same as the file */
    unsigned char b[4096];
    for (long i = 0; i + 4096 <= exe_size; i += 4096)
        if (pread(fd, b, 4096, i) != 4096 || memcmp(m + i, b, 4096)) return 0;
    return 1;
}
static int rewrite_check(unsigned char v) {                /* write the file full of v, then map it: v seen */
    int f = open(rw, O_RDWR | O_CREAT | O_TRUNC, 0644);
    unsigned char b[4096]; memset(b, v, sizeof b);
    for (int i = 0; i < 64; i++) write(f, b, sizeof b);
    close(f);
    f = open(rw, O_RDONLY);
    unsigned char *m = mmap(0, 64 * 4096, PROT_READ, MAP_PRIVATE, f, 0);
    int ok = m != MAP_FAILED;
    for (int i = 0; ok && i < 64 * 4096; i += 4096) ok = m[i] == v && m[i + 4095] == v;
    if (m != MAP_FAILED) munmap(m, 64 * 4096);
    close(f); return ok;
}
int main(void) {
    int e = open(path, O_RDONLY); exe_size = lseek(e, 0, SEEK_END) & ~4095L; close(e);
    CHECK(exe_size >= SZ && ((volatile const unsigned char *)big)[0] == 1, "this program's file is %ld kB", exe_size >> 10);
    long m0 = memfree_kb();
    CHECK(m0 > 0, "MemFree readable (%ld kB)", m0);
    int go[2], ready[2]; pipe(go); pipe(ready);
    pid_t kids[4];
    for (int k = 0; k < 4; k++) {
        if ((kids[k] = fork()) == 0) {
            int fd = open(path, O_RDONLY);
            unsigned char *m = mmap(0, exe_size, PROT_READ, MAP_PRIVATE, fd, 0);
            int ok = m != MAP_FAILED && touch_check(m, fd);
            if (k == 3 && ok) {                             /* this one writes to its own copy */
                unsigned char was = m[4096 + 1], next = m[2 * 4096 + 1];
                ok = mprotect(m, exe_size, PROT_READ | PROT_WRITE) == 0;
                if (ok) { m[4096 + 1] = was + 1; ok = m[4096 + 1] == (unsigned char)(was + 1) && m[2 * 4096 + 1] == next; }
            }
            write(ready[1], ok ? "y" : "n", 1);
            char c; read(go[0], &c, 1);                     /* (stay until the parent has measured) */
            _exit(ok && touch_check(m, fd) == (k != 3) ? 0 : 1);
        }
    }
    int good = 0; for (int k = 0; k < 4; k++) { char c; read(ready[0], &c, 1); good += c == 'y'; }
    long m1 = memfree_kb();
    CHECK(good == 4, "four children read the file right through their mappings (%d)", good);
    long used = m0 - m1;                                    /* (one shared copy ~8 MB, + the writer's own 8 MB, + small change) */
    CHECK(used < 3 * 8192 + 4096, "four mappings of the 8 MB file use %ld kB (one copy shared, one private: under 28 MB; private copies would be 40 MB)", used);
    for (int k = 0; k < 4; k++) write(go[1], "x", 1);
    int st, kid_ok = 1; for (int k = 0; k < 4; k++) { waitpid(kids[k], &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) kid_ok = 0; }
    CHECK(kid_ok, "the writer's change stayed its own; the others still saw the file");
    long m2 = memfree_kb();
    CHECK(m0 - m2 < 1024, "memory came back when they ended (%ld kB still used)", m0 - m2);
    CHECK(rewrite_check(0x41) && rewrite_check(0x42) && rewrite_check(0x43), "a rewritten file maps with its new contents");
    unlink(rw);
    printf("%s\n", fails ? "SOME FAILED" : "PASS"); return fails != 0;
}
