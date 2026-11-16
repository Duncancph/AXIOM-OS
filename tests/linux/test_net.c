/* Linux internet sockets on AXIOM (src/linux.c over the kernel's TCP/IP, src/net.c): name lookups through the
 * C library (UDP to the DNS server in /etc/resolv.conf), TCP connects with and without waiting, epoll, HTTP,
 * MSG_PEEK, a refused connection, UDP sendto/recvfrom and an IPv6 socket reaching an IPv4 address.
 * Needs the internet (QEMU's user networking is enough). Build it on Linux as an ordinary program (the C
 * library's name lookups load parts of it at run time) and run it on AXIOM ('run test_net' with the
 * program in /home); it also runs on Linux itself, which must print the same.
 *   gcc -O1 -pthread -o test_net tests/linux/test_net.c */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); } } while (0)
static long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static const char *HOST = "example.com";

/* read everything until the far end closes, waiting with poll */
static long read_all(int s, char *buf, long max) {
    long n = 0;
    for (;;) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 15000) != 1) return -1;
        long r = read(s, buf + n, max - 1 - n);
        if (r < 0 && errno == EAGAIN) continue;
        if (r <= 0) break;
        n += r;
        if (n >= max - 1) break;
    }
    buf[n] = 0;
    return n;
}

int main(void) {
    static char buf[1 << 20];
    setvbuf(stdout, 0, _IONBF, 0);
    long t0 = ms_now();

    /* 1. a name lookup (the C library: /etc/resolv.conf, a UDP query) */
    struct addrinfo hints = { 0 }, *ai = 0;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(HOST, "80", &hints, &ai);
    CHECK(g == 0 && ai);
    if (g || !ai) { printf("getaddrinfo: %s\n", gai_strerror(g)); printf("FAILED: %d checks, %d failed\n", checks, fails); return 1; }
    struct sockaddr_in sa = *(struct sockaddr_in *)ai->ai_addr;
    char ip[64]; inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
    printf("%s is %s (%ld ms)\n", HOST, ip, ms_now() - t0);
    freeaddrinfo(ai);

    /* 2. connect without waiting; epoll says when it's done; SO_ERROR; names */
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(s >= 0);
    int one = 1;
    CHECK(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    CHECK(setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) == 0);
    long t1 = ms_now();
    int r = connect(s, (struct sockaddr *)&sa, sizeof sa);
    CHECK(r == -1 && errno == EINPROGRESS);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLOUT, .data.u64 = 7 }, got;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0);
    CHECK(epoll_wait(ep, &got, 1, 15000) == 1 && got.data.u64 == 7 && (got.events & EPOLLOUT));
    int err = -1; socklen_t el = sizeof err;
    CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    printf("connected in %ld ms\n", ms_now() - t1);
    struct sockaddr_in me, peer; socklen_t ml = sizeof me, pl = sizeof peer;
    CHECK(getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_family == AF_INET && me.sin_port != 0);
    CHECK(getpeername(s, (struct sockaddr *)&peer, &pl) == 0 && peer.sin_addr.s_addr == sa.sin_addr.s_addr && peer.sin_port == htons(80));

    /* 3. HTTP over it; MSG_PEEK; FIONREAD; the end of the stream */
    int len = snprintf(buf, sizeof buf, "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: test_net\r\n\r\n", HOST);
    CHECK(write(s, buf, len) == len);
    ev.events = EPOLLIN; CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, s, &ev) == 0);
    CHECK(epoll_wait(ep, &got, 1, 15000) == 1 && (got.events & EPOLLIN));
    char pk[5] = { 0 };
    CHECK(recv(s, pk, 4, MSG_PEEK) == 4 && !memcmp(pk, "HTTP", 4));
    int avail = 0; CHECK(ioctl(s, FIONREAD, &avail) == 0 && avail >= 4);
    long n = read_all(s, buf, sizeof buf);
    CHECK(n > 100 && !strncmp(buf, "HTTP/1.1 ", 9));
    CHECK(strstr(buf, "Example Domain") != 0);
    printf("HTTP: %ld bytes, \"%.*s\"\n", n, (int)(strchr(buf, '\r') ? strchr(buf, '\r') - buf : 20), buf);
    CHECK(read(s, buf, 10) == 0);                               /* still the end */
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, s, 0) == 0);
    close(s);

    /* 4. a refused connection (QEMU's router, a closed port), waiting this time: only in QEMU, where AXIOM made
     * /etc/resolv.conf */
    FILE *mine = fopen("/etc/resolv.conf", "r"); char first[64] = "";
    if (mine) { if (!fgets(first, sizeof first, mine)) first[0] = 0; fclose(mine); }
    if (strstr(first, "made by AXIOM")) {
    struct sockaddr_in rf = { .sin_family = AF_INET, .sin_port = htons(1) };
    inet_pton(AF_INET, "10.0.2.2", &rf.sin_addr);
    s = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(s, (struct sockaddr *)&rf, sizeof rf);
    CHECK(r == -1 && (errno == ECONNREFUSED || errno == ETIMEDOUT));
    printf("closed port: %s\n", r ? strerror(errno) : "connected?");
    close(s);
    }

    /* 5. UDP: a DNS question of our own, to the server in /etc/resolv.conf */
    char ns[64] = "";
    FILE *rc = fopen("/etc/resolv.conf", "r");
    CHECK(rc != 0);
    if (rc) { char line[200]; while (fgets(line, sizeof line, rc)) if (sscanf(line, "nameserver %63s", ns) == 1) break; fclose(rc); }
    CHECK(ns[0] != 0);
    printf("nameserver %s\n", ns);
    int u = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(u >= 0);
    struct sockaddr_in any = { .sin_family = AF_INET };
    CHECK(bind(u, (struct sockaddr *)&any, sizeof any) == 0);
    ml = sizeof me; CHECK(getsockname(u, (struct sockaddr *)&me, &ml) == 0 && me.sin_port != 0);
    struct sockaddr_in dns = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_pton(AF_INET, ns, &dns.sin_addr);
    unsigned char q[64] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    int ql = 12;
    for (const char *p = HOST; *p;) { const char *d = strchr(p, '.'); int l = d ? d - p : (int)strlen(p); q[ql++] = l; memcpy(q + ql, p, l); ql += l; p += l + (d ? 1 : 0); }
    q[ql++] = 0; q[ql++] = 0; q[ql++] = 1; q[ql++] = 0; q[ql++] = 1;
    CHECK(recvfrom(u, buf, 512, 0, 0, 0) == -1 && errno == EAGAIN);
    CHECK(sendto(u, q, ql, 0, (struct sockaddr *)&dns, sizeof dns) == ql);
    struct pollfd pu = { u, POLLIN, 0 };
    CHECK(poll(&pu, 1, 5000) == 1);
    struct sockaddr_in from; socklen_t fl = sizeof from;
    n = recvfrom(u, buf, 512, 0, (struct sockaddr *)&from, &fl);
    CHECK(n > 12 && (unsigned char)buf[0] == 0x12 && (unsigned char)buf[1] == 0x34 && from.sin_port == htons(53));
    printf("UDP: %ld byte DNS answer\n", n);
    /* connected UDP: send / recv, as the C library's resolver does */
    CHECK(connect(u, (struct sockaddr *)&dns, sizeof dns) == 0);
    q[1] = 0x35;
    CHECK(send(u, q, ql, 0) == ql);
    CHECK(poll(&pu, 1, 5000) == 1);
    n = recv(u, buf, 512, 0);
    CHECK(n > 12 && (unsigned char)buf[1] == 0x35);
    close(u);

    /* 6. an IPv6 socket reaching the IPv4 address (::ffff:a.b.c.d), waiting to connect */
    int s6 = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(s6 >= 0);
    struct sockaddr_in6 m6 = { .sin6_family = AF_INET6, .sin6_port = htons(80) };
    m6.sin6_addr.s6_addr[10] = m6.sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&m6.sin6_addr.s6_addr[12], &sa.sin_addr, 4);
    CHECK(connect(s6, (struct sockaddr *)&m6, sizeof m6) == 0);
    struct sockaddr_in6 p6; socklen_t p6l = sizeof p6;
    CHECK(getpeername(s6, (struct sockaddr *)&p6, &p6l) == 0 && p6.sin6_family == AF_INET6 && p6l == sizeof p6 &&
          !memcmp(&p6.sin6_addr, &m6.sin6_addr, 16));
    len = snprintf(buf, sizeof buf, "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", HOST);
    CHECK(send(s6, buf, len, MSG_NOSIGNAL) == len);
    n = read_all(s6, buf, sizeof buf);
    CHECK(n > 0 && !strncmp(buf, "HTTP/1.1 ", 9));
    close(s6);

    /* 7. getaddrinfo with both families, as browsers ask */
    hints.ai_family = AF_UNSPEC; hints.ai_flags = AI_ADDRCONFIG;
    CHECK(getaddrinfo("www.google.com", "443", &hints, &ai) == 0);
    if (ai) freeaddrinfo(ai);

    close(ep);
    printf("%s: %d checks, %d failed (%ld ms)\n", fails ? "FAILED" : "PASSED", checks, fails, ms_now() - t0);
    return fails != 0;
}
