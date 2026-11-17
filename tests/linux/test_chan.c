/* Linux compatibility check: pipes, socket pairs, poll, eventfd, epoll, shared memory (memfd), /dev, passing files (SCM_RIGHTS)
 * and /proc/self/maps. Build it on Linux as a static PIE and run it on
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
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)

static long long ns_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int send_fds(int s, const char *data, int n, const int *fds, int nfds) {
    char cbuf[CMSG_SPACE(8 * sizeof(int))]; memset(cbuf, 0, sizeof cbuf);
    struct iovec io = { (void *)data, (size_t)n };
    struct msghdr m = { 0 }; m.msg_iov = &io; m.msg_iovlen = 1;
    if (nfds) {
        m.msg_control = cbuf; m.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(nfds * sizeof(int)); memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
    }
    return (int)sendmsg(s, &m, 0);
}
static int recv_fds(int s, char *data, int n, int *fds, int *nfds, int room, int *flags) {
    char cbuf[CMSG_SPACE(8 * sizeof(int))];
    struct iovec io = { data, (size_t)n };
    struct msghdr m = { 0 }; m.msg_iov = &io; m.msg_iovlen = 1;
    m.msg_control = room ? cbuf : 0; m.msg_controllen = room ? CMSG_SPACE(room * sizeof(int)) : 0;
    int r = (int)recvmsg(s, &m, 0);
    *nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); r >= 0 && c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + *nfds, CMSG_DATA(c), k * sizeof(int)); *nfds += k;
        }
    *flags = m.msg_flags;
    return r;
}
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

/* a read waiting on a socket another thread shuts down (as Chromium stops its audio thread) */
static volatile long sd_r = -9;
static void *sd_reader(void *a) { char c; sd_r = read(*(int *)a, &c, 1); return 0; }
static volatile long sd_w = -9; static volatile int sd_errno;
static void *sd_writer(void *a) {
    static char big[1 << 20]; long r = write(*(int *)a, big, sizeof big);   /* (more than fits: it waits) */
    sd_errno = r < 0 ? errno : 0; sd_w = r; return 0;
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
    {                                                            /* a socket shut down is still the same watched file (Mojo) */
        int sp[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);
        struct epoll_event se = { EPOLLIN, { .u64 = 9 } };
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, sp[0], &se) == 0);
        CHECK(shutdown(sp[0], SHUT_RDWR) == 0);
        struct epoll_event got; CHECK(epoll_wait(ep, &got, 1, 0) == 1 && got.data.u64 == 9);   /* (end of file: readable) */
        CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, sp[0], &se) == 0);
        CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, sp[0], 0) == 0);
        close(sp[0]); close(sp[1]);
    }
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

    /* /dev */
    int dn = open("/dev/null", O_RDWR | O_CLOEXEC);
    CHECK(dn > 2 && write(dn, "gone", 4) == 4 && read(dn, buf, sizeof buf) == 0);
    struct stat dst; CHECK(fstat(dn, &dst) == 0 && S_ISCHR(dst.st_mode) && major(dst.st_rdev) == 1 && minor(dst.st_rdev) == 3);
    close(dn);
    CHECK(stat("/dev/urandom", &dst) == 0 && S_ISCHR(dst.st_mode) && access("/dev/null", W_OK) == 0);
    struct statfs sfs;                                           /* /dev/shm: memory, with room (Chromium checks) */
    CHECK(statfs("/dev/shm", &sfs) == 0 && sfs.f_type == 0x01021994 && sfs.f_bavail > 0 && sfs.f_bsize > 0);
    {                                                            /* /proc/cpuinfo, /proc/meminfo (Chromium reads both) */
        static char pb[8192]; int pf = open("/proc/cpuinfo", O_RDONLY | O_CLOEXEC); long pn = pf >= 0 ? read(pf, pb, sizeof pb - 1) : -1;
        CHECK(pn > 0 && (pb[pn] = 0, strstr(pb, "processor\t: 0") && strstr(pb, "model name\t: ")));
        if (pf >= 0) close(pf);
        pf = open("/proc/meminfo", O_RDONLY | O_CLOEXEC); pn = pf >= 0 ? read(pf, pb, sizeof pb - 1) : -1;
        unsigned long mt = 0;
        CHECK(pn > 0 && (pb[pn] = 0, sscanf(pb, "MemTotal: %lu kB", &mt) == 1) && mt > 1000 && strstr(pb, "MemAvailable:"));
        if (pf >= 0) close(pf);
    }
    int du = open("/dev/urandom", O_RDONLY);
    uint64_t u1 = 0, u2 = 0;
    CHECK(read(du, &u1, 8) == 8 && read(du, &u2, 8) == 8 && u1 != u2);
    close(du);
    int dz = open("/dev/zero", O_RDONLY);
    memset(buf, 1, 16); CHECK(read(dz, buf, 16) == 16 && buf[0] == 0 && buf[15] == 0);
    char *zm = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, dz, 0);
    CHECK(zm != MAP_FAILED && zm[100] == 0);
    munmap(zm, 4096); close(dz);
    int df = open("/dev/full", O_WRONLY);
    CHECK(write(df, "x", 1) == -1 && errno == ENOSPC);
    close(df);

    /* passing files over a socket pair (SCM_RIGHTS), as Mojo does */
    {
        int sp2[2], pp[2], got[8], ng, fl;
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp2) == 0 && pipe(pp) == 0);
        CHECK(send_fds(sp2[0], "A", 1, &pp[1], 1) == 1);
        close(pp[1]);                                       /* the copy in flight keeps the write end open */
        CHECK(recv_fds(sp2[1], buf, sizeof buf, got, &ng, 4, &fl) == 1 && buf[0] == 'A' && ng == 1 && fl == 0);
        CHECK(ng == 1 && got[0] > 2 && write(got[0], "via passed fd", 13) == 13);
        CHECK(read(pp[0], buf, sizeof buf) == 13 && !memcmp(buf, "via passed fd", 13));
        close(got[0]);
        CHECK(read(pp[0], buf, sizeof buf) == 0);           /* that was the last write end */
        close(pp[0]);
        int mfd = memfd_create("passed", 0);                /* shared memory across the socket */
        CHECK(ftruncate(mfd, 4096) == 0 && pwrite(mfd, "shm!", 4, 0) == 4);
        CHECK(send_fds(sp2[0], "B", 1, &mfd, 1) == 1);
        CHECK(recv_fds(sp2[1], buf, sizeof buf, got, &ng, 4, &fl) == 1 && ng == 1);
        char *pm = ng == 1 ? mmap(0, 4096, PROT_READ, MAP_SHARED, got[0], 0) : MAP_FAILED;
        CHECK(pm != MAP_FAILED && !memcmp(pm, "shm!", 4));
        if (pm != MAP_FAILED) munmap(pm, 4096);
        if (ng == 1) close(got[0]);
        close(mfd);
        /* where a stream read stops when files were sent along */
        int e1 = eventfd(0, 0), e2 = eventfd(0, 0);
        CHECK(write(sp2[0], "xy", 2) == 2 && send_fds(sp2[0], "zw", 2, &e1, 1) == 2 && send_fds(sp2[0], "q", 1, &e2, 1) == 1);
        CHECK(write(sp2[0], "t", 1) == 1);
        CHECK(recv_fds(sp2[1], buf, sizeof buf, got, &ng, 4, &fl) == 4 && !memcmp(buf, "xyzw", 4) && ng == 1);   /* as Linux: */
        if (ng == 1) close(got[0]);                         /* plain bytes, then the ones sent with files, then stop */
        CHECK(recv_fds(sp2[1], buf, sizeof buf, got, &ng, 0, &fl) == 1 && buf[0] == 'q' && ng == 0 && (fl & MSG_CTRUNC));   /* no room: dropped */
        CHECK(recv_fds(sp2[1], buf, sizeof buf, got, &ng, 4, &fl) == 1 && buf[0] == 't' && ng == 0);
        CHECK(send_fds(sp2[0], "r", 1, &e2, 1) == 1 && read(sp2[1], buf, sizeof buf) == 1);              /* read(): dropped */
        close(e1); close(e2);
        CHECK(send_fds(sp2[0], "s", 1, (int[]){ 99 }, 1) == -1 && errno == EBADF);
        close(sp2[0]); close(sp2[1]);
        int sq[2];                                          /* packet sockets: files with their message */
        CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sq) == 0 && pipe(pp) == 0);
        int two[2] = { pp[0], pp[1] };
        CHECK(send_fds(sq[0], "msg", 3, two, 2) == 3);
        CHECK(recv_fds(sq[1], buf, sizeof buf, got, &ng, 4, &fl) == 3 && ng == 2);
        if (ng == 2) { CHECK(write(got[1], "k", 1) == 1 && read(pp[0], buf, 1) == 1 && buf[0] == 'k'); close(got[0]); close(got[1]); }
        close(pp[0]); close(pp[1]); close(sq[0]); close(sq[1]);
    }

    /* /proc/self/maps: main() is in an executable range of this program's own file; the stack is named */
    {
        char exe[256] = { 0 }, line[512];
        CHECK(readlink("/proc/self/exe", exe, sizeof exe - 1) > 0);
        FILE *mf2 = fopen("/proc/self/maps", "r");
        CHECK(mf2 != 0);
        int found_main = 0, found_stack = 0, lines = 0;
        while (mf2 && fgets(line, sizeof line, mf2)) {
            unsigned long lo, hi; char perms[8] = { 0 }, path[300] = { 0 };
            lines++;
            if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %299s", &lo, &hi, perms, path) < 3) continue;
            if ((unsigned long)(uintptr_t)&main >= lo && (unsigned long)(uintptr_t)&main < hi && perms[2] == 'x' && !strcmp(path, exe)) found_main = 1;
            if (!strcmp(path, "[stack]")) found_stack = 1;
        }
        if (mf2) fclose(mf2);
        CHECK(lines > 3 && found_main && found_stack);
    }

    /* timed waits never end early (Linux guarantees it; Chromium's timers count on it) */
    long long early_ns = 0, t1;
    int ep2 = epoll_create1(0); struct epoll_event eo;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t cv; pthread_condattr_t ca;
    pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&cv, &ca);
    for (int i = 0; i < 200; i++) {
        struct timespec one = { 0, 1000000 };
        t1 = ns_now(); nanosleep(&one, 0);
        if (ns_now() - t1 < 1000000 && 1000000 - (ns_now() - t1) > early_ns) early_ns = 1000000 - (ns_now() - t1);
        t1 = ns_now(); poll(0, 0, 1);
        if (ns_now() - t1 < 1000000 && 1000000 - (ns_now() - t1) > early_ns) early_ns = 1000000 - (ns_now() - t1);
        t1 = ns_now(); epoll_wait(ep2, &eo, 1, 1);
        if (ns_now() - t1 < 1000000 && 1000000 - (ns_now() - t1) > early_ns) early_ns = 1000000 - (ns_now() - t1);
        struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
        t1 = dl.tv_sec * 1000000000LL + dl.tv_nsec + 1500000;
        dl.tv_nsec += 1500000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        pthread_mutex_lock(&mu); pthread_cond_timedwait(&cv, &mu, &dl); pthread_mutex_unlock(&mu);
        if (ns_now() < t1 && t1 - ns_now() > early_ns) early_ns = t1 - ns_now();
    }
    close(ep2);
    if (early_ns) printf("timed waits ended up to %lld ns early\n", early_ns);
    CHECK(early_ns == 0);

    {
        signal(SIGPIPE, SIG_IGN);
        int sd[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sd) == 0);
        pthread_t t; pthread_create(&t, 0, sd_reader, &sd[0]); usleep(50000);
        CHECK(sd_r == -9);                                       /* (still waiting) */
        CHECK(shutdown(sd[0], SHUT_RDWR) == 0);
        for (int i = 0; i < 200 && sd_r == -9; i++) usleep(10000);
        CHECK(sd_r == 0);                                        /* the shutdown ended the read: end of file */
        pthread_join(t, 0);
        close(sd[0]); close(sd[1]); CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sd) == 0);
        pthread_create(&t, 0, sd_writer, &sd[1]); usleep(50000);
        CHECK(sd_w == -9);
        CHECK(shutdown(sd[1], SHUT_WR) == 0);
        for (int i = 0; i < 200 && sd_w == -9; i++) usleep(10000);
        CHECK(sd_w != -9);                                       /* the shutdown ended the write */
        pthread_join(t, 0);
        close(sd[0]); close(sd[1]);
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
