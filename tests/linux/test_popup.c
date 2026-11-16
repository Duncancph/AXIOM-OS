/* Linux compatibility check: popup windows (menus, lists, tooltips), window positions and pointer shapes, as
 * Chromium's Ozone platform uses them (common/axiom_window.h).
 *   gcc -O1 -static-pie -Icommon -o test_popup tests/linux/test_popup.c
 * Run on AXIOM ('run test_popup'). It opens a window at 300,200 and a red popup over it at +60,+60, and waits
 * up to 40 s for a click in the popup; the QEMU test clicks at the popup's 20,10 (screen 380,270). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "axiom_window.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)
static long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

struct win { int fd; struct axwin_info i; uint32_t *px; };
static int open_win(struct win *w, int x, int y, int cw, int ch, unsigned flags, uint32_t color, const char *title) {
    w->fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
    if (w->fd < 0) return -1;
    struct axwin_rect at = { x, y, 0, 0 };
    if (ioctl(w->fd, AXWIN_MOVE, &at)) return -1;
    struct axwin_create c = { .w = cw, .h = ch, .flags = flags };
    strncpy(c.title, title, sizeof c.title - 1);
    if (ioctl(w->fd, AXWIN_CREATE, &c) < 0 || ioctl(w->fd, AXWIN_INFO, &w->i)) return -1;
    w->px = mmap(0, w->i.stride * w->i.h, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
    if (w->px == MAP_FAILED) return -1;
    for (int j = 0; j < w->i.h; j++) for (int k = 0; k < w->i.w; k++) w->px[j * w->i.stride / 4 + k] = color;
    struct axwin_rect r = { 0, 0, w->i.w, w->i.h };
    return ioctl(w->fd, AXWIN_FLUSH, &r);
}
/* the next event, waiting up to ms; type 0 if none */
static struct axwin_event next(struct win *w, int ms) {
    struct axwin_event e = { 0 };
    struct pollfd p = { w->fd, POLLIN, 0 };
    if (poll(&p, 1, ms) == 1 && read(w->fd, &e, sizeof e) != sizeof e) e.type = 0;
    return e;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    struct win main_w, pop;
    CHECK(open_win(&main_w, 300, 200, 500, 300, AXWIN_MOVE_EVENTS, 0x203040, "Popup test") == 0);
    struct axwin_rect pos;
    CHECK(ioctl(main_w.fd, AXWIN_POS, &pos) == 0 && pos.x == 300 && pos.y == 200 && pos.w == 500 && pos.h == 300);
    printf("window content at %d,%d %dx%d\n", pos.x, pos.y, pos.w, pos.h);
    /* the first events: focus, and where it is */
    int focus = 0, moved = 0;
    for (long end = ms_now() + 1000; ms_now() < end;) {
        struct axwin_event e = next(&main_w, 100);
        if (e.type == AXWIN_EV_FOCUS) focus++;
        if (e.type == AXWIN_EV_MOVE) { moved++; CHECK(e.x == 300 && e.y == 200); }
    }
    printf("first events: %d focus, %d moves\n", focus, moved);
    CHECK(focus == 1 && moved == 1);

    /* a popup: small sizes are fine, no frame, and the window keeps the focus */
    CHECK(open_win(&pop, 360, 260, 120, 40, AXWIN_POPUP, 0xD02020, "") == 0);
    CHECK(pop.i.w == 120 && pop.i.h == 40);
    CHECK(ioctl(pop.fd, AXWIN_POS, &pos) == 0 && pos.x == 360 && pos.y == 260);
    struct axwin_event e = next(&main_w, 300);
    CHECK(e.type == 0);                                        /* no blur */
    CHECK(next(&pop, 100).type == 0);                          /* and the popup gets no focus */

    /* pointer shapes */
    int shape = AXWIN_CURSOR_HAND;
    CHECK(ioctl(pop.fd, AXWIN_CURSOR, &shape) == 0);
    shape = AXWIN_CURSOR_TEXT;
    CHECK(ioctl(main_w.fd, AXWIN_CURSOR, &shape) == 0);

    /* moving the window: an event says where it went */
    struct axwin_rect to = { 320, 210, 0, 0 };
    CHECK(ioctl(main_w.fd, AXWIN_MOVE, &to) == 0);
    e = next(&main_w, 1000);
    CHECK(e.type == AXWIN_EV_MOVE && e.x == 320 && e.y == 210);
    to.x = 300; to.y = 200;
    CHECK(ioctl(main_w.fd, AXWIN_MOVE, &to) == 0);
    e = next(&main_w, 1000);
    CHECK(e.type == AXWIN_EV_MOVE && e.x == 300 && e.y == 200);

    /* hide and show the popup */
    int v = 0;
    CHECK(ioctl(pop.fd, AXWIN_SHOW, &v) == 0);
    usleep(300000);
    v = 1;
    CHECK(ioctl(pop.fd, AXWIN_SHOW, &v) == 0);
    printf("ready: click the popup\n");

    /* a click in the popup goes to the popup, in its own coordinates; the window keeps the focus */
    int got = 0, blurs = 0;
    for (long end = ms_now() + 40000; ms_now() < end && !got;) {
        struct axwin_event pe = next(&pop, 50);
        if (pe.type == AXWIN_EV_MOUSE_DOWN) { printf("popup: mouse down %d at %d,%d\n", pe.button, pe.x, pe.y); CHECK(pe.x == 20 && pe.y == 10); got = 1; }
        struct axwin_event me = next(&main_w, 0);
        if (me.type == AXWIN_EV_BLUR) blurs++;
        if (me.type == AXWIN_EV_MOUSE_DOWN) printf("window: mouse down at %d,%d (should have gone to the popup)\n", me.x, me.y);
    }
    CHECK(got == 1);
    usleep(500000);
    for (struct axwin_event me; (me = next(&main_w, 0)).type;) if (me.type == AXWIN_EV_BLUR) blurs++;
    CHECK(blurs == 0);
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
