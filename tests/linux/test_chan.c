/* Linux compatibility check: pipes, socket pairs, poll, eventfd, epoll and shared memory (memfd). Build it on Linux as a static PIE and run it on
 * AXIOM ('run test_chan' with the program in /home); it also runs on Linux itself, which must print the same.
 *   gcc -O1 -static-pie -pthread -o test_chan tests/linux/test_chan.c */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)

static long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int wake_fd;
static void *writer(void *arg) {
    struct timespec d = { 0, 200 * 1000000 };
    nanosleep(&d, 0);
    if (write(wake_fd, "x", 1) != 1) printf("FAIL writer\n");
    return arg;
}
static void *big_reader(void *arg) {
    int fd = *(int *)arg; static char buf[300000]; long got = 0, r;
    while ((r = read(fd, buf, sizeof buf)) > 0) got += r;
    return (void *)got;
}

int main(void) {
    char buf[256];
    setvbuf(stdout, 0, _IOLBF, 0);

    /* the standard files, as Rust programs check them at start-up */
    struct pollfd std3[3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };
    CHECK(poll(std3, 3, 0) >= 0);
    CHECK(!(std3[0].revents & POLLNVAL) && !(std3[1].revents & POLLNVAL) && !(std3[2].revents & POLLNVAL));
    struct pollfd bad = { 100, POLLIN, 0 };
    CHECK(poll(&bad, 1, 0) == 1 && bad.revents == POLLNVAL);

    /* a pipe */
    int p[2];
    CHECK(pipe2(p, O_NONBLOCK) == 0);
    CHECK(read(p[0], buf, sizeof buf) == -1 && errno == EAGAIN);
    struct pollfd pf = { p[0], POLLIN, 0 };
    long t0 = ms_now();
    CHECK(poll(&pf, 1, 100) == 0);
    long waited = ms_now() - t0;
    CHECK(waited >= 90 && waited < 1000);
    CHECK(write(p[1], "hello", 5) == 5);
    CHECK(poll(&pf, 1, 0) == 1 && pf.revents == POLLIN);
    int avail = -1; CHECK(ioctl(p[0], FIONREAD, &avail) == 0 && avail == 5);
    CHECK(read(p[0], buf, 3) == 3 && !memcmp(buf, "hel", 3));
    CHECK(read(p[0], buf, sizeof buf) == 2 && !memcmp(buf, "lo", 2));
    CHECK(fcntl(p[0], F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(p[0], F_SETFL, 0) == 0 && !(fcntl(p[0], F_GETFL) & O_NONBLOCK));
    int d = dup(p[1]);
    CHECK(d > 2);
    close(p[1]);
    CHECK(write(d, "z", 1) == 1);                          /* the duplicate keeps the write end open */
    CHECK(read(p[0], buf, sizeof buf) == 1);
    close(d);
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP));
    CHECK(read(p[0], buf, sizeof buf) == 0);              /* no writers left: end of file */
    close(p[0]);

    /* a pipe with nobody to read: EPIPE (SIGPIPE ignored, as Chromium does) */
    signal(SIGPIPE, SIG_IGN);
    CHECK(pipe(p) == 0);
    close(p[0]);
    CHECK(write(p[1], "x", 1) == -1 && errno == EPIPE);
    close(p[1]);

    /* a thread waiting in poll with no time limit is woken by a write from another thread */
    CHECK(pipe(p) == 0);
    wake_fd = p[1];
    pthread_t th; pthread_create(&th, 0, writer, 0);
    pf.fd = p[0]; pf.events = POLLIN; pf.revents = 0;
    t0 = ms_now();
    CHECK(poll(&pf, 1, -1) == 1 && pf.revents == POLLIN);
    waited = ms_now() - t0;
    CHECK(waited >= 150 && waited < 2000);
    pthread_join(th, 0);
    CHECK(read(p[0], buf, 1) == 1 && buf[0] == 'x');
    close(p[0]); close(p[1]);

    /* more than a pipe holds: the writer blocks until a reader thread empties it */
    CHECK(pipe(p) == 0);
    pthread_create(&th, 0, big_reader, &p[0]);
    static char big[1 << 20]; memset(big, 'b', sizeof big);
    CHECK(write(p[1], big, sizeof big) == (long)sizeof big);
    close(p[1]);
    void *got; pthread_join(th, &got);
    CHECK((long)got == (long)sizeof big);
    close(p[0]);

    /* a packet socket pair keeps message boundaries; shutdown as Chromium does it */
    int s[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0);
    CHECK(write(s[0], "one", 3) == 3 && write(s[0], "three", 5) == 5);
    CHECK(read(s[1], buf, sizeof buf) == 3 && !memcmp(buf, "one", 3));
    CHECK(recv(s[1], buf, 2, 0) == 2 && !memcmp(buf, "th", 2));   /* (the rest of that message is dropped) */
    CHECK(recv(s[1], buf, sizeof buf, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    struct iovec iov[2] = { { "ab", 2 }, { "cde", 3 } };
    struct msghdr mh = { 0 }; mh.msg_iov = iov; mh.msg_iovlen = 2;
    CHECK(sendmsg(s[1], &mh, 0) == 5);
    char r1[1], r2[10]; struct iovec riov[2] = { { r1, 1 }, { r2, 10 } };
    struct msghdr rh = { 0 }; rh.msg_iov = riov; rh.msg_iovlen = 2;
    CHECK(recvmsg(s[0], &rh, 0) == 5 && r1[0] == 'a' && !memcmp(r2, "bcde", 4) && rh.msg_flags == 0);
    CHECK(shutdown(s[0], SHUT_RD) == 0 && shutdown(s[1], SHUT_WR) == 0);
    CHECK(write(s[1], "x", 1) == -1 && errno == EPIPE);
    CHECK(read(s[0], buf, sizeof buf) == 0);
    struct pollfd sp = { s[1], POLLIN, 0 };
    CHECK(poll(&sp, 1, 0) == 0);                          /* s[1] can still be written to by s[0]: nothing yet */
    CHECK(write(s[0], "late", 4) == 4);
    CHECK(poll(&sp, 1, 0) == 1 && sp.revents == POLLIN);
    CHECK(read(s[1], buf, sizeof buf) == 4);
    close(s[0]);
    CHECK(read(s[1], buf, sizeof buf) == 0);
    close(s[1]);

    /* a stream socket pair */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, s) == 0);
    CHECK(send(s[0], "ab", 2, MSG_NOSIGNAL) == 2 && send(s[0], "cd", 2, 0) == 2);
    CHECK(recv(s[1], buf, 4, MSG_PEEK) == 4);
    CHECK(read(s[1], buf, sizeof buf) == 4 && !memcmp(buf, "abcd", 4));
    struct pollfd both[2] = { { s[0], POLLIN | POLLOUT, 0 }, { s[1], POLLIN, 0 } };
    CHECK(poll(both, 2, 0) == 1 && both[0].revents == POLLOUT && both[1].revents == 0);
    close(s[0]); close(s[1]);

    /* eventfd */
    int ev = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t v = 0;
    CHECK(ev > 2);
    CHECK(read(ev, &v, 8) == -1 && errno == EAGAIN);
    v = 2; CHECK(write(ev, &v, 8) == 8);
    v = 3; CHECK(write(ev, &v, 8) == 8);
    CHECK(read(ev, &v, 8) == 8 && v == 5);
    CHECK(read(ev, &v, 4) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_eventfd2, 0, ~0) == -1 && errno == EINVAL);   /* Chromium's probe for eventfd */
    CHECK(epoll_create1(~0) == -1 && errno == EINVAL);
    CHECK(pipe2(p, ~0) == -1 && errno == EINVAL);
    int sem = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(read(sem, &v, 8) == 8 && v == 1 && read(sem, &v, 8) == 8 && v == 1 && read(sem, &v, 8) == -1);
    close(sem);

    /* epoll: level-triggered, one-shot, MOD, DEL, closed files dropping out, a waiting thread woken */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep > 2);
    CHECK(pipe(p) == 0);
    struct epoll_event ee = { EPOLLIN, { .u64 = 0x1111 } }, out[8];
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ev, &ee) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ev, &ee) == -1 && errno == EEXIST);
    ee.events = EPOLLIN | EPOLLONESHOT; ee.data.u64 = 0x2222;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ee) == 0);
    CHECK(epoll_wait(ep, out, 8, 0) == 0);
    t0 = ms_now();
    CHECK(epoll_wait(ep, out, 8, 100) == 0);
    waited = ms_now() - t0;
    CHECK(waited >= 90 && waited < 1000);
    v = 1; CHECK(write(ev, &v, 8) == 8);
    CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 0x1111);
    CHECK(epoll_wait(ep, out, 8, 0) == 1);                 /* level-triggered: still there until read */
    CHECK(read(ev, &v, 8) == 8);
    CHECK(epoll_wait(ep, out, 8, 0) == 0);
    CHECK(write(p[1], "q", 1) == 1);
    CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 0x2222);
    CHECK(epoll_wait(ep, out, 8, 0) == 0);                 /* one-shot: off until MOD */
    ee.data.u64 = 0x3333;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ee) == 0);
    CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 0x3333);
    CHECK(read(p[0], buf, 1) == 1);
    struct pollfd epf = { ep, POLLIN, 0 };
    CHECK(poll(&epf, 1, 0) == 0);
    wake_fd = p[1];                                        /* a thread blocked in epoll_wait, woken by a write */
    ee.events = EPOLLIN; ee.data.u64 = 0x4444;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ee) == 0);
    pthread_create(&th, 0, writer, 0);
    t0 = ms_now();
    CHECK(epoll_wait(ep, out, 8, -1) == 1 && out[0].data.u64 == 0x4444);
    waited = ms_now() - t0;
    CHECK(waited >= 150 && waited < 2000);
    pthread_join(th, 0);
    CHECK(poll(&epf, 1, 0) == 1 && epf.revents == POLLIN);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) == -1 && errno == ENOENT);
    CHECK(epoll_wait(ep, out, 8, 0) == 0);
    v = 1; CHECK(write(ev, &v, 8) == 8);
    close(ev);                                             /* closed: gone from the epoll */
    CHECK(epoll_wait(ep, out, 8, 0) == 0);
    close(p[0]); close(p[1]); close(ep);

    /* shared memory: memfd_create, ftruncate, MAP_SHARED */
    int mf = memfd_create("shared-memory-region", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(mf > 2);
    CHECK(ftruncate(mf, 3 * 4096) == 0);
    struct stat sst; CHECK(fstat(mf, &sst) == 0 && sst.st_size == 3 * 4096 && S_ISREG(sst.st_mode));
    char *m1 = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    char *m2 = mmap(0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 4096);
    CHECK(m1 != MAP_FAILED && m2 != MAP_FAILED);
    CHECK(m1[0] == 0 && m1[3 * 4096 - 1] == 0);
    strcpy(m1 + 4096, "shared page");
    CHECK(!strcmp(m2, "shared page"));                     /* the same page, seen at two addresses */
    m2[4096] = 'Z';
    CHECK(m1[2 * 4096] == 'Z');
    CHECK(pwrite(mf, "via write", 10, 2 * 4096 + 100) == 10);
    CHECK(!strcmp(m1 + 2 * 4096 + 100, "via write"));
    CHECK(pread(mf, buf, 11, 4096) == 11 && !memcmp(buf, "shared page", 11));
    char *pv = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, mf, 4096);
    CHECK(pv != MAP_FAILED && !strcmp(pv, "shared page"));
    pv[0] = 'X';
    CHECK(m1[4096] == 's');                                 /* a private copy doesn't change the memfd */
    munmap(pv, 4096);
    CHECK(madvise(m1, 3 * 4096, MADV_DONTNEED) == 0);
    CHECK(!strcmp(m2, "shared page"));                     /* shared memory keeps its contents */
    CHECK(mprotect(m2, 4096, PROT_READ) == 0 && !strcmp(m2, "shared page"));
    CHECK(mprotect(m2, 4096, PROT_READ | PROT_WRITE) == 0);
    m2[0] = 'S';
    CHECK(m1[4096] == 'S');
    CHECK(fcntl(mf, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    char procfd[32]; snprintf(procfd, sizeof procfd, "/proc/self/fd/%d", mf);   /* read-only copy, as Chromium makes them */
    int ro = open(procfd, O_RDONLY | O_CLOEXEC);
    CHECK(ro > 2 && ro != mf);
    CHECK(mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 4096) == MAP_FAILED && errno == EACCES);
    char *rm = mmap(0, 4096, PROT_READ, MAP_SHARED, ro, 4096);
    CHECK(rm != MAP_FAILED && !strcmp(rm, "Shared page"));
    CHECK(fstat(ro, &sst) == 0 && sst.st_size == 3 * 4096);
    close(ro);
    munmap(rm, 4096);
    close(mf);                                             /* the mappings keep it */
    CHECK(munmap(m1, 3 * 4096) == 0);
    CHECK(!strcmp(m2, "Shared page") && m2[4096] == 'Z');
    CHECK(munmap(m2 + 4096, 4096) == 0);                    /* half of it */
    CHECK(!strcmp(m2, "Shared page"));
    CHECK(munmap(m2, 4096) == 0);
    CHECK(syscall(SYS_memfd_create, "", ~0) == -1 && errno == EINVAL);   /* Chromium's probe for memfd */
    mf = memfd_create("sealed", MFD_ALLOW_SEALING);
    CHECK(ftruncate(mf, 8) == 0 && fcntl(mf, F_GET_SEALS) == 0);
    CHECK(fcntl(mf, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
    CHECK(fcntl(mf, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW));
    CHECK(ftruncate(mf, 4) == -1 && errno == EPERM);
    CHECK(ftruncate(mf, 16) == -1 && errno == EPERM);
    char *sw = mmap(0, 8, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    CHECK(sw != MAP_FAILED);
    CHECK(fcntl(mf, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);   /* mapped writable */
    munmap(sw, 8);
    CHECK(fcntl(mf, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL) == 0);
    CHECK(write(mf, "x", 1) == -1 && errno == EPERM);
    CHECK(mmap(0, 8, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0) == MAP_FAILED && errno == EPERM);
    CHECK(fcntl(mf, F_ADD_SEALS, F_SEAL_EXEC) == -1 && errno == EPERM);
    close(mf);
    mf = memfd_create("unsealable", 0);
    CHECK(fcntl(mf, F_GET_SEALS) == F_SEAL_SEAL && fcntl(mf, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);
    close(mf);
    /* a bad pointer is an error, not ignored (Chromium checks this with a read-only page) */
    char *rop = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mprotect(rop, 4096, PROT_READ) == 0);
    CHECK(getrlimit(RLIMIT_NPROC, (struct rlimit *)rop) == -1 && errno == EFAULT);
    munmap(rop, 4096);

    char *anon = mmap(0, 8 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(anon != MAP_FAILED);                             /* (memory freed above can be used again) */
    memset(anon, 7, 8 * 4096);
    CHECK(anon[8 * 4096 - 1] == 7);
    munmap(anon, 8 * 4096);
    mf = memfd_create("big", 0);                           /* a hole punched in a mapping */
    CHECK(ftruncate(mf, 4 * 4096) == 0);
    char *mb = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    mb[0] = 'a'; mb[3 * 4096] = 'd';
    CHECK(munmap(mb + 4096, 2 * 4096) == 0);
    close(mf);
    CHECK(mb[0] == 'a' && mb[3 * 4096] == 'd');
    munmap(mb, 4096); munmap(mb + 3 * 4096, 4096);

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
