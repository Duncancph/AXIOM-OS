/* axwl's windows on AXIOM: one /dev/axiom/window per Wayland toplevel (see common/axiom_window.h). */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "backend.h"
struct bwin { int fd; uint32_t *px; struct axwin_info info; size_t maplen; };
static int clipfd = -1;
int be_init(void) { clipfd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC); return clipfd < 0 ? -1 : 0; }
void be_screen(int *w, int *h) { struct axwin_info i = { 0 }; ioctl(clipfd, AXWIN_INFO, &i); *w = i.screen_w; *h = i.screen_h; }
static int map(struct bwin *b) {
    if (ioctl(b->fd, AXWIN_INFO, &b->info) < 0) return -1;
    b->maplen = (size_t)b->info.stride * b->info.h;
    b->px = mmap(0, b->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    return b->px == MAP_FAILED ? (b->px = 0, -1) : 0;
}
struct bwin *be_create(int w, int h, const char *title) {
    struct bwin *b = calloc(1, sizeof *b);
    b->fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    struct axwin_create c = { w, h, AXWIN_RESIZABLE, "" };
    strncpy(c.title, title ? title : "", sizeof c.title - 1);
    if (b->fd < 0 || ioctl(b->fd, AXWIN_CREATE, &c) < 0 || map(b) < 0) { if (b->fd >= 0) close(b->fd); free(b); return 0; }
    return b;
}
void be_destroy(struct bwin *b) { if (b->px) munmap(b->px, b->maplen); close(b->fd); free(b); }
uint32_t *be_pixels(struct bwin *b, int *w, int *h, int *stride) { *w = b->info.w; *h = b->info.h; *stride = b->info.stride / 4; return b->px; }
void be_flush(struct bwin *b, int x, int y, int w, int h) { struct axwin_rect r = { x, y, w, h }; ioctl(b->fd, AXWIN_FLUSH, &r); }
void be_title(struct bwin *b, const char *t) { char c[64] = { 0 }; strncpy(c, t, 63); ioctl(b->fd, AXWIN_TITLE, c); }
int be_fd(struct bwin *b) { return b->fd; }
/* (the file is non-blocking: this returns 0 once the queue is empty, so the event loop goes back to waiting) */
int be_event(struct bwin *b, struct axwin_event *e) { return read(b->fd, e, sizeof *e) == (ssize_t)sizeof *e; }
int be_resize(struct bwin *b) {
    if (b->px) { munmap(b->px, b->maplen); b->px = 0; }
    int r = ioctl(b->fd, AXWIN_RESIZE, 0);
    map(b);
    return r;
}
long be_clip_get(char *buf, long max, uint64_t *seq) {
    struct axwin_clip c = { (uint64_t)(uintptr_t)buf, (uint64_t)(max > 0 ? max : 0), 0 };
    if (!buf) c.data = 0;
    if (ioctl(clipfd, AXWIN_CLIP_GET, &c) < 0) return -1;
    if (seq) *seq = c.seq;
    return (long)c.len;
}
void be_clip_set(const char *s, long n) { struct axwin_clip c = { (uint64_t)(uintptr_t)s, (uint64_t)n, 0 }; ioctl(clipfd, AXWIN_CLIP_SET, &c); }
int be_control_fd(void) { return -1; }
void be_control(void) {}
