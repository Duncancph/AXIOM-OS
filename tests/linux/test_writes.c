/* Linux compatibility check: file writes reach the disk. 40 files written in pieces, overwritten in place at odd
 * offsets (as a database does), appended to, one deleted; read back. Run, shut down, and compare the disk with the same
 * program run on Linux (the disk cache writes back).  gcc -O1 -static-pie -o test_writes tests/linux/test_writes.c */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned char pat(int f, long i) { return (unsigned char)(f * 31 + i * 7 + (i >> 9)); }
int main(void) {
    mkdir("/home/wt", 0755);
    char p[64]; static unsigned char buf[300000];
    for (int f = 0; f < 40; f++) {                          /* new files, written in pieces */
        snprintf(p, sizeof p, "/home/wt/f%02d.bin", f);
        long sz = 1000 + f * 7000;
        for (long i = 0; i < sz; i++) buf[i] = pat(f, i);
        int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        for (long o = 0; o < sz; o += 777) write(fd, buf + o, sz - o < 777 ? sz - o : 777);
        close(fd);
    }
    for (int f = 0; f < 40; f += 3) {                       /* overwrite in place at odd offsets (as a database does) */
        snprintf(p, sizeof p, "/home/wt/f%02d.bin", f);
        int fd = open(p, O_RDWR);
        for (long o = 13; o < 1000 + f * 7000 - 50; o += 1031) { unsigned char x[50]; for (int k = 0; k < 50; k++) x[k] = (unsigned char)(o + k) ^ 0x5A; pwrite(fd, x, 50, o); }
        close(fd);
    }
    unlink("/home/wt/f39.bin");
    int fd = open("/home/wt/f00.bin", O_WRONLY | O_APPEND); write(fd, "TAIL", 4); close(fd);
    /* read back now (through the cache) */
    int bad = 0;
    for (int f = 0; f < 39; f++) {
        snprintf(p, sizeof p, "/home/wt/f%02d.bin", f);
        long sz = 1000 + f * 7000;
        int r = open(p, O_RDONLY); long n = read(r, buf, sizeof buf); close(r);
        if (n != sz + (f == 0 ? 4 : 0)) { bad++; continue; }
        for (long i = 0; i < sz; i++) {
            unsigned char want = pat(f, i);
            if (f % 3 == 0) { long o = (i - 13) % 1031; if (i >= 13 && o < 50 && i - o + 50 <= sz - 50 + 50 && i - o < sz - 50) want = (unsigned char)((i - o) + o) ^ 0x5A; }
            if (buf[i] != want) { bad++; break; }
        }
    }
    printf("wtest: %d bad: %s\n", bad, bad ? "FAIL" : "PASS");
    return 0;
}
