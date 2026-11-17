/* Linux compatibility check: the clipboard through /dev/axiom/window (AXWIN_CLIP_SET / AXWIN_CLIP_GET).
 *   gcc -O1 -static-pie -Icommon -o test_clip tests/linux/test_clip.c
 * 'run test_clip': round trips (UTF-8 kept exactly, partial reads, seq); 'run test_clip set': copies a sample
 * for AXIOM's apps (paste it in Notes); 'run test_clip get': prints what's on the clipboard (copied in Notes). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "axiom_window.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static const char sample[] = "Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x94 caf\xc3\xa9 \xe4\xbd\xa0\xe5\xa5\xbd \xe2\x9c\x93";   /* "Grüße — café 你好 ✓" */
int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    int fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("FAIL: open\n"); return 1; }
    char buf[256];
    if (argc > 1 && !strcmp(argv[1], "set")) {
        struct axwin_clip c = { (uint64_t)sample, sizeof sample - 1, 0 };
        printf("set: %d (seq %llu)\n", ioctl(fd, AXWIN_CLIP_SET, &c), (unsigned long long)c.seq);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "get")) {
        struct axwin_clip c = { (uint64_t)buf, sizeof buf - 1, 0 };
        int r = ioctl(fd, AXWIN_CLIP_GET, &c);
        buf[c.len < sizeof buf - 1 ? c.len : sizeof buf - 1] = 0;
        printf("get: %d, %llu bytes, seq %llu: \"%s\"\nhex:", r, (unsigned long long)c.len, (unsigned long long)c.seq, buf);
        for (unsigned i = 0; i < c.len && i < 64; i++) printf(" %02x", (unsigned char)buf[i]);
        printf("\n");
        return 0;
    }
    struct axwin_clip q = { 0, 0, 0 };
    CHECK(ioctl(fd, AXWIN_CLIP_GET, &q) == 0);
    unsigned long long seq0 = q.seq;
    struct axwin_clip c = { (uint64_t)sample, sizeof sample - 1, 0 };
    CHECK(ioctl(fd, AXWIN_CLIP_SET, &c) == 0);
    CHECK(c.seq == seq0 + 1);
    struct axwin_clip g = { (uint64_t)buf, sizeof buf, 0 };
    memset(buf, 0, sizeof buf);
    CHECK(ioctl(fd, AXWIN_CLIP_GET, &g) == 0);
    CHECK(g.len == sizeof sample - 1 && !memcmp(buf, sample, sizeof sample - 1));   /* exactly what went in */
    CHECK(g.seq == c.seq);
    struct axwin_clip s = { (uint64_t)buf, 5, 0 }; memset(buf, 0, sizeof buf);
    CHECK(ioctl(fd, AXWIN_CLIP_GET, &s) == 0);
    CHECK(s.len == sizeof sample - 1 && !memcmp(buf, sample, 5) && buf[5] == 0);   /* as much as fits, the whole length */
    struct axwin_clip e = { 0, 0, 0 };
    CHECK(ioctl(fd, AXWIN_CLIP_SET, &e) == 0);                                        /* empty */
    struct axwin_clip g2 = { (uint64_t)buf, sizeof buf, 0 };
    CHECK(ioctl(fd, AXWIN_CLIP_GET, &g2) == 0 && g2.len == 0 && g2.seq == c.seq + 1);
    struct axwin_clip bad = { 16, 10, 0 };                                             /* a bad pointer */
    CHECK(ioctl(fd, AXWIN_CLIP_SET, &bad) < 0);
    struct axwin_clip big = { (uint64_t)buf, (2u << 20), 0 };
    CHECK(ioctl(fd, AXWIN_CLIP_SET, &big) < 0);                                       /* over 1 MiB */
    CHECK(ioctl(fd, AXWIN_CLIP_SET, &c) == 0);                                        /* leave the sample there */
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
