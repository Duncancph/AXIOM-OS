/* Linux compatibility check: an AXIOM window from a Linux program (common/axiom_window.h), driven the way
 * Chromium's Ozone platform will drive it: epoll for events, mmap'd pixels, flush, resize.
 *   gcc -O1 -static-pie -Icommon -o test_window tests/linux/test_window.c
 * Run on AXIOM ('run test_window'); it prints what happens and quits on 'q' (or after 120 s). The QEMU test
 * clicks in the window, types, maximizes it and checks the screen. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include "axiom_window.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)

static int fd;
static struct axwin_info info;
static uint32_t *px;

static void draw(void) {
    int sw = info.stride / 4;
    for (int y = 0; y < info.h; y++)
        for (int x = 0; x < info.w; x++)
            px[y * sw + x] = (uint32_t)(x * 255 / info.w) << 16 | (uint32_t)(y * 255 / info.h) << 8 | 0x60;
    for (int y = 20; y < 60 && y < info.h; y++)                    /* a white box at (20, 20) */
        for (int x = 20; x < 120 && x < info.w; x++) px[y * sw + x] = 0xFFFFFF;
    struct axwin_rect r = { 0, 0, info.w, info.h };
    CHECK(ioctl(fd, AXWIN_FLUSH, &r) == 0);
}
static void map(void) {
    CHECK(ioctl(fd, AXWIN_INFO, &info) == 0);
    px = mmap(0, (size_t)info.stride * info.h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(px != MAP_FAILED);
    printf("window %d: %dx%d (stride %d) on a %dx%d screen\n", info.id, info.w, info.h, info.stride, info.screen_w, info.screen_h);
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
    CHECK(fd > 2);
    struct stat st; CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENODEV);   /* no window yet */
    CHECK(ioctl(fd, AXWIN_INFO, &info) == 0 && info.id == 0 && info.w == 0 && info.screen_w >= 800);   /* the screen, before a window */
    struct axwin_create bad = { 5, 5, 0, "tiny" };
    CHECK(ioctl(fd, AXWIN_CREATE, &bad) == -1 && errno == EINVAL);
    struct axwin_create c = { 480, 320, AXWIN_RESIZABLE, "Linux window test" };
    int id = ioctl(fd, AXWIN_CREATE, &c);
    CHECK(id > 0);
    CHECK(ioctl(fd, AXWIN_CREATE, &c) == -1 && errno == EEXIST);
    map();
    CHECK(info.w == 480 && info.h == 320 && info.stride == 480 * 4 && info.id == id);
    draw();
    CHECK(write(fd, "x", 1) == -1 && errno == EINVAL);
    CHECK(ioctl(fd, AXWIN_RESIZE) == -1 && errno == EBUSY);      /* mapped: must munmap first */
    CHECK(ioctl(fd, AXWIN_TITLE, "Linux window test: click, type, maximize, q") == 0);
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct axwin_event ev[16];
    while (read(fd, ev, sizeof ev) > 0) {}                       /* (focus etc. from opening) */
    CHECK(read(fd, ev, sizeof ev) == -1 && errno == EAGAIN);
    printf("checks: %d, failed %d\n", checks, fails);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ee = { EPOLLIN, { .u64 = 1 } };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ee) == 0);
    time_t start = time(0);
    int clicks = 0, keys = 0, resizes = 0;
    for (int quit = 0; !quit && time(0) - start < 120;) {
        struct epoll_event out;
        if (epoll_wait(ep, &out, 1, 1000) <= 0) continue;
        long n = read(fd, ev, sizeof ev);
        for (long i = 0; i < n / (long)sizeof ev[0]; i++) {
            struct axwin_event *e = &ev[i];
            switch (e->type) {
            case AXWIN_EV_MOUSE_DOWN:
                printf("mouse down %d at %d,%d\n", e->button, e->x, e->y); clicks++;
                for (int y = e->y - 4; y <= e->y + 4; y++)            /* a red square where it was clicked */
                    for (int x = e->x - 4; x <= e->x + 4; x++)
                        if (x >= 0 && y >= 0 && x < info.w && y < info.h) px[y * info.stride / 4 + x] = 0xFF0000;
                { struct axwin_rect r = { e->x - 4, e->y - 4, 9, 9 }; ioctl(fd, AXWIN_FLUSH, &r); }
                break;
            case AXWIN_EV_MOUSE_UP: printf("mouse up %d at %d,%d\n", e->button, e->x, e->y); break;
            case AXWIN_EV_WHEEL: printf("wheel %d at %d,%d\n", e->key, e->x, e->y); break;
            case AXWIN_EV_KEY:
                if (e->key >= 32 && e->key < 127) printf("key '%c'\n", e->key); else printf("key 0x%x\n", e->key);
                keys++;
                if (e->key == 'q') quit = 1;
                break;
            case AXWIN_EV_RESIZE:
                printf("resize asked: %dx%d\n", e->x, e->y);
                munmap(px, (size_t)info.stride * info.h);
                CHECK(ioctl(fd, AXWIN_RESIZE) == 0);
                map();
                CHECK(info.w == e->x && info.h == e->y);
                draw(); resizes++;
                break;
            case AXWIN_EV_CLOSE: printf("close button\n"); quit = 1; break;
            case AXWIN_EV_FOCUS: printf("focus\n"); break;
            case AXWIN_EV_BLUR: printf("blur\n"); break;
            }
        }
    }
    printf("%s: %d checks, %d failed; %d clicks, %d keys, %d resizes\n", fails ? "FAILED" : "PASSED", checks, fails, clicks, keys, resizes);
    return fails != 0;
}
