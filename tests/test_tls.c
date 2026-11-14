/* Host-side TLS client test: connects with real sockets to a server (e.g. openssl s_server),
 * verifies it against the given CA file, sends an HTTP request and reports the result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../user/lib/crypto/tls.h"

static long s_send(void *c, const void *b, size_t n) { return send(*(int *)c, b, n, MSG_NOSIGNAL); }
static long s_recv(void *c, void *b, size_t n, long to) {
    struct pollfd p = { *(int *)c, POLLIN, 0 };
    int r = poll(&p, 1, to < 0 ? -1 : (int)to);
    if (r == 0) return -9;
    long k = recv(*(int *)c, b, n, 0);
    return k < 0 ? -1 : k;
}
static void rnd(void *b, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); fread(b, 1, n, f); fclose(f); }

int main(int argc, char **argv) {   /* test_tls <ca.pem> <host> <port> <path> [time_offset_days] */
    FILE *f = fopen(argv[1], "rb"); static char pem[400000]; size_t pl = fread(pem, 1, sizeof pem, f); fclose(f);
    int nr = x509_add_roots_pem(pem, pl);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { AF_INET, htons(atoi(argv[3])) }; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (void *)&a, sizeof a)) { printf("RESULT connect-failed\n"); return 1; }
    struct tls_io io = { &fd, s_send, s_recv, rnd, (int64_t)time(0) + (argc > 5 ? atoi(argv[5]) * 86400LL : 0) };
    static struct tls t;
    int r = tls_connect(&t, &io, argv[2]);
    if (r) { printf("RESULT error %d: %s\n", r, tls_strerror(&t, r)); return 2; }
    char req[512]; int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", argv[4], argv[2]);
    tls_write(&t, req, rl);
    static uint8_t body[20 << 20]; long total = 0;
    for (;;) { long n = tls_read(&t, body + total, sizeof body - total, 10000); if (n <= 0) break; total += n; }
    tls_close(&t);
    uint8_t h[32];
    char *hdr_end = memmem(body, total, "\r\n\r\n", 4);
    long hl = hdr_end ? hdr_end - (char *)body + 4 : 0;
    hash(H_SHA256, body + hl, total - hl, h);
    printf("RESULT ok roots=%d cipher='%s' peer='%s' issuer-root='%s' bytes=%ld body-sha256=", nr, t.cipher, t.peer, t.issuer, total - hl);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf(" status='%.20s'\n", body);
    return 0;
}
