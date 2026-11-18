/* Linux compatibility check: inotify. Watches a folder and a file in it, makes changes (create, write, close,
 * rename, mkdir, delete) and checks the events, as Linux gives them: names, IN_ISDIR, rename cookies, a file's
 * own events, IN_IGNORED when a watch ends, one event for repeated writes, IN_ONESHOT, poll/epoll, FIONREAD,
 * a buffer too small, and a blocked read woken by another process's change. Prints PASS/FAIL.
 *   gcc -O1 -static-pie -o test_inotify tests/linux/test_inotify.c   (also runs on Linux itself) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); fflush(stdout); } while (0)

struct ev { int wd; unsigned mask, cookie; char name[64]; };
static struct ev evs[64]; static int nev;
static void collect(int fd, int ms) {                       /* the events there now (waiting up to ms for the first) */
    nev = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return;
    char buf[4096] __attribute__((aligned(8)));
    long n = read(fd, buf, sizeof buf);
    for (long o = 0; o < n && nev < 64;) {
        struct inotify_event *e = (struct inotify_event *)(buf + o);
        evs[nev].wd = e->wd; evs[nev].mask = e->mask; evs[nev].cookie = e->cookie;
        snprintf(evs[nev].name, sizeof evs[nev].name, "%s", e->len ? e->name : ""); nev++;
        o += sizeof *e + e->len;
    }
}
static int find(int from, int wd, unsigned mask, const char *name) {   /* index of a matching event at or after from */
    for (int i = from; i < nev; i++) if (evs[i].wd == wd && evs[i].mask == mask && !strcmp(evs[i].name, name)) return i;
    return -1;
}
static int count(unsigned mask) { int c = 0; for (int i = 0; i < nev; i++) if (evs[i].mask == mask) c++; return c; }
static void show(void) { for (int i = 0; i < nev; i++) printf("     ev wd=%d mask=%x cookie=%u name=%s\n", evs[i].wd, evs[i].mask, evs[i].cookie, evs[i].name); }

int main(void) {
    const char *dir = "/home/inotify-test";
    char f1[200], f2[200], sub[200];
    snprintf(f1, sizeof f1, "%s/a.txt", dir); snprintf(f2, sizeof f2, "%s/b.txt", dir); snprintf(sub, sizeof sub, "%s/sub", dir);
    unlink(f1); unlink(f2); rmdir(sub); rmdir(dir);
    CHECK(mkdir(dir, 0755) == 0, "made %s", dir);

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0, "inotify_init1 gives a file (%d)", fd);
    char tiny[8];
    CHECK(read(fd, tiny, sizeof tiny) < 0 && errno == EAGAIN, "nothing yet: EAGAIN (non-blocking)");
    CHECK(inotify_add_watch(fd, "/home/no-such-folder", IN_ALL_EVENTS) < 0 && errno == ENOENT, "watching a missing path: ENOENT");
    CHECK(inotify_add_watch(fd, dir, 0) < 0 && errno == EINVAL, "an empty mask: EINVAL");
    int wd = inotify_add_watch(fd, dir, IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB);
    CHECK(wd > 0, "watching the folder (wd %d)", wd);
    CHECK(inotify_add_watch(fd, dir, IN_CREATE | IN_MASK_ADD | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB) == wd,
          "the same path again: the same wd");
    CHECK(inotify_add_watch(fd, dir, IN_CREATE | IN_MASK_CREATE) < 0 && errno == EEXIST, "IN_MASK_CREATE on a watched path: EEXIST");

    /* create, write three times, close */
    int f = open(f1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(f, "one\n", 4); write(f, "two\n", 4); write(f, "three\n", 6); close(f);
    collect(fd, 1000); show();
    int c = find(0, wd, IN_CREATE, "a.txt"), m = find(0, wd, IN_MODIFY, "a.txt"), cw = find(0, wd, IN_CLOSE_WRITE, "a.txt");
    CHECK(c >= 0 && m > c && cw > m, "create, write, close: IN_CREATE, IN_MODIFY, IN_CLOSE_WRITE with the name, in that order");
    CHECK(count(IN_MODIFY) == 1, "three writes in a row: one IN_MODIFY (%d)", count(IN_MODIFY));

    /* FIONREAD and a buffer too small */
    f = open(f1, O_WRONLY | O_APPEND); write(f, "four\n", 5); close(f);
    struct pollfd pp = { fd, POLLIN, 0 };
    CHECK(poll(&pp, 1, 1000) == 1 && (pp.revents & POLLIN), "poll: readable when events wait");
    int avail = 0; ioctl(fd, FIONREAD, &avail);
    CHECK(avail >= (int)(2 * sizeof(struct inotify_event)), "FIONREAD: %d bytes waiting", avail);
    CHECK(read(fd, tiny, sizeof tiny) < 0 && errno == EINVAL, "a buffer smaller than the next event: EINVAL");
    collect(fd, 100);

    /* a watch on the file itself */
    int wf = inotify_add_watch(fd, f1, IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
    CHECK(wf > 0 && wf != wd, "watching the file too (wd %d)", wf);
    CHECK(inotify_add_watch(fd, f1, IN_MODIFY | IN_ONLYDIR) < 0 && errno == ENOTDIR, "IN_ONLYDIR on a file: ENOTDIR");
    f = open(f1, O_WRONLY | O_APPEND); write(f, "five\n", 5); close(f);
    collect(fd, 1000); show();
    CHECK(find(0, wf, IN_MODIFY, "") >= 0 && find(0, wd, IN_MODIFY, "a.txt") >= 0, "a write: IN_MODIFY to the file's watch (no name) and the folder's (with it)");

    /* rename */
    CHECK(rename(f1, f2) == 0, "renamed a.txt to b.txt");
    collect(fd, 1000); show();
    int mf = find(0, wd, IN_MOVED_FROM, "a.txt"), mt = find(0, wd, IN_MOVED_TO, "b.txt");
    CHECK(mf >= 0 && mt > mf && evs[mf].cookie && evs[mf].cookie == evs[mt].cookie, "IN_MOVED_FROM a.txt then IN_MOVED_TO b.txt, with the same cookie");
    CHECK(find(0, wf, IN_MOVE_SELF, "") >= 0, "the file's own watch: IN_MOVE_SELF");

    /* the watch follows the file: a write to b.txt still reaches it */
    f = open(f2, O_WRONLY | O_APPEND); write(f, "six\n", 4); close(f);
    collect(fd, 1000);
    CHECK(find(0, wf, IN_MODIFY, "") >= 0, "after the rename, the file's watch still sees its writes");

    /* attributes */
    struct timespec ts[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    utimensat(AT_FDCWD, f2, ts, 0);
    collect(fd, 1000);
    CHECK(find(0, wd, IN_ATTRIB, "b.txt") >= 0, "changing its date: IN_ATTRIB");

    /* a folder */
    CHECK(mkdir(sub, 0755) == 0, "made sub/");
    collect(fd, 1000); show();
    CHECK(find(0, wd, IN_CREATE | IN_ISDIR, "sub") >= 0, "a new folder: IN_CREATE | IN_ISDIR");

    /* delete: the folder hears IN_DELETE, the file's watch IN_DELETE_SELF then IN_IGNORED */
    CHECK(unlink(f2) == 0, "deleted b.txt");
    collect(fd, 1000); show();
    int ds = find(0, wf, IN_DELETE_SELF, ""), ig = find(0, wf, IN_IGNORED, "");
    CHECK(find(0, wd, IN_DELETE, "b.txt") >= 0, "the folder's watch: IN_DELETE b.txt");
    CHECK(ds >= 0 && ig > ds, "the file's watch: IN_DELETE_SELF, then IN_IGNORED");
    CHECK(inotify_rm_watch(fd, wf) < 0 && errno == EINVAL, "its watch is gone: rm_watch says EINVAL");

    /* rm_watch */
    CHECK(inotify_rm_watch(fd, wd) == 0, "rm_watch on the folder");
    collect(fd, 1000);
    CHECK(find(0, wd, IN_IGNORED, "") >= 0, "... gives IN_IGNORED");
    f = open(f1, O_WRONLY | O_CREAT, 0644); close(f);
    collect(fd, 300);
    CHECK(nev == 0, "after that, changes there give nothing (%d events)", nev);

    /* IN_ONESHOT */
    int wo = inotify_add_watch(fd, dir, IN_CREATE | IN_DELETE | IN_ONESHOT);
    unlink(f1);
    f = open(f2, O_WRONLY | O_CREAT, 0644); close(f);
    collect(fd, 1000); show();
    CHECK(find(0, wo, IN_DELETE, "a.txt") >= 0 && find(0, wo, IN_IGNORED, "") >= 0 && find(0, wo, IN_CREATE, "b.txt") < 0,
          "IN_ONESHOT: one event, then IN_IGNORED, then nothing");

    /* epoll, and a blocked read woken by another process */
    int wd2 = inotify_add_watch(fd, dir, IN_CREATE);
    int ep = epoll_create1(0);
    struct epoll_event ee = { .events = EPOLLIN, .data.u32 = 7 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ee) == 0, "an inotify file in epoll");
    pid_t kid = fork();
    if (kid == 0) { usleep(300000); int k = open("/home/inotify-test/c.txt", O_WRONLY | O_CREAT, 0644); close(k); _exit(0); }
    struct epoll_event got;
    int r = epoll_wait(ep, &got, 1, 5000);
    CHECK(r == 1 && got.data.u32 == 7, "epoll_wait woke when another process made a file");
    int bf = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, bf & ~O_NONBLOCK);
    char buf[512] __attribute__((aligned(8)));
    long n = read(fd, buf, sizeof buf);
    struct inotify_event *e = (struct inotify_event *)buf;
    CHECK(n > 0 && e->wd == wd2 && (e->mask & IN_CREATE) && !strcmp(e->name, "c.txt"), "... and read gives IN_CREATE c.txt");
    waitpid(kid, 0, 0);
    kid = fork();
    if (kid == 0) { usleep(300000); unlink("/home/inotify-test/c.txt"); _exit(0); }
    inotify_add_watch(fd, dir, IN_DELETE);
    n = read(fd, buf, sizeof buf);                               /* (blocks until the child deletes it) */
    CHECK(n > 0 && (e->mask & IN_DELETE) && !strcmp(e->name, "c.txt"), "a blocking read waits for the change");
    waitpid(kid, 0, 0);

    close(ep); close(fd);
    unlink(f1); unlink(f2); rmdir(sub); rmdir(dir);
    printf("%d checks, %s\n", checks, fails ? "SOME FAILED" : "PASS");
    return fails != 0;
}
