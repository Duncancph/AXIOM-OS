/* Linux compatibility check: a window's file passed to another process (SCM_RIGHTS), as Vertex's browser
 * passes its windows to its GPU process, which maps the pixels, draws, flushes and resizes them (AXWIN_RESIZE_TO).
 *   gcc -O1 -static-pie -Icommon -o test_winpass tests/linux/test_winpass.c
 * Run on AXIOM ('run test_winpass'): a window appears, turns green at 400x300, then 500x350; prints PASS/FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "axiom_window.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fflush(stdout); } } while (0)

static void send_fd(int s, int fd) {
    char b = 'f'; struct iovec io = { &b, 1 };
    union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.c, .msg_controllen = sizeof u.c };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    sendmsg(s, &m, 0);
}
static int recv_fd(int s) {
    char b; struct iovec io = { &b, 1 };
    union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.c, .msg_controllen = sizeof u.c };
    if (recvmsg(s, &m, 0) != 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd;
}
static void fill(uint32_t *px, struct axwin_info *i, uint32_t color) {
    for (int y = 0; y < i->h; y++) for (int x = 0; x < i->w; x++) px[y * (i->stride / 4) + x] = color;
}

int main(void) {
    int fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
    struct axwin_create c = { .w = 400, .h = 300, .flags = AXWIN_RESIZABLE };
    strcpy(c.title, "test_winpass");
    CHECK(fd >= 0 && ioctl(fd, AXWIN_CREATE, &c) > 0);
    struct axwin_info pi; CHECK(ioctl(fd, AXWIN_INFO, &pi) == 0 && pi.w == 400 && pi.h == 300);
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    pid_t pid = fork();
    if (pid == 0) {                                   /* the "GPU process": draws what the parent's window shows */
        close(sv[0]);
        int w = recv_fd(sv[1]);
        CHECK(w >= 0);
        struct axwin_info i; CHECK(ioctl(w, AXWIN_INFO, &i) == 0 && i.id == pi.id && i.w == 400);
        uint32_t *px = mmap(0, (size_t)i.stride * i.h, PROT_READ | PROT_WRITE, MAP_SHARED, w, 0);
        CHECK(px != MAP_FAILED);
        fill(px, &i, 0x0000c000);
        struct axwin_rect all = { 0, 0, i.w, i.h };
        CHECK(ioctl(w, AXWIN_FLUSH, &all) == 0);
        struct axwin_rect to = { 0, 0, 500, 350 };
        errno = 0; CHECK(ioctl(w, AXWIN_RESIZE_TO, &to) < 0 && errno == EBUSY);   /* its own mapping */
        munmap(px, (size_t)i.stride * i.h);
        CHECK(ioctl(w, AXWIN_RESIZE_TO, &to) == 0);
        CHECK(ioctl(w, AXWIN_INFO, &i) == 0 && i.w == 500 && i.h == 350 && i.stride == 2000);
        px = mmap(0, (size_t)i.stride * i.h, PROT_READ | PROT_WRITE, MAP_SHARED, w, 0);
        CHECK(px != MAP_FAILED);
        fill(px, &i, 0x0000e000);
        all = (struct axwin_rect){ 0, 0, i.w, i.h };
        CHECK(ioctl(w, AXWIN_FLUSH, &all) == 0);
        char b = 'm'; write(sv[1], &b, 1);            /* mapped: the parent checks it can't resize now */
        read(sv[1], &b, 1);
        CHECK(px[349 * 500 + 499] == 0x0000e000);
        _exit(fails ? 1 : 0);                         /* (exit unmaps and closes) */
    }
    close(sv[1]);
    send_fd(sv[0], fd);
    char b = 0; CHECK(read(sv[0], &b, 1) == 1 && b == 'm');
    struct axwin_info i; CHECK(ioctl(fd, AXWIN_INFO, &i) == 0 && i.w == 500 && i.h == 350);   /* the same window */
    errno = 0; CHECK(ioctl(fd, AXWIN_RESIZE, 0) < 0 && errno == EBUSY);   /* mapped in the other process */
    usleep(1500 * 1000);                              /* (time to see it) */
    write(sv[0], &b, 1);
    int st = -1; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    struct axwin_rect small = { 0, 0, 10, 10 };
    CHECK(ioctl(fd, AXWIN_RESIZE_TO, &small) == 0);
    CHECK(ioctl(fd, AXWIN_INFO, &i) == 0 && i.w > 10 && i.h > 10);   /* made to fit: AXIOM's smallest */
    struct axwin_rect bad = { 0, 0, 0, 100 };
    errno = 0; CHECK(ioctl(fd, AXWIN_RESIZE_TO, &bad) < 0 && errno == EINVAL);
    int pfd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
    struct axwin_create pc = { .w = 100, .h = 50, .flags = AXWIN_POPUP };
    CHECK(ioctl(pfd, AXWIN_CREATE, &pc) > 0);
    errno = 0; CHECK(ioctl(pfd, AXWIN_RESIZE_TO, &small) < 0 && errno == EINVAL);   /* a popup's size is fixed */
    close(pfd); close(fd);
    printf("%d checks, %s\n", checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
