/* Network helpers and an HTTP/1.1 client for DuncOS apps. */
#include "dunc.h"
#include "crypto/tls.h"

struct http_security http_sec;

/* ---------------- TLS plumbing ---------------- */
static struct tls tls_conn;
static bool roots_loaded;
static long t_send(void *c, const void *b, size_t n) { return tcp_write(*(int *)c, b, (uint32_t)n); }
static long t_recv(void *c, void *b, size_t n, long to) { return tcp_read(*(int *)c, b, (uint32_t)n, to); }
static void t_rand(void *b, size_t n) { sys_call(SYS_RANDOM, (long)b, (long)n, 0, 0, 0); }

static int64_t unix_now(void) {
    struct rtc_time t; sys_rtc(&t);
    int64_t y = t.year - (t.mon <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), m = t.mon;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + (int64_t)doe - 719468) * 86400 + t.hour * 3600 + t.min * 60 + t.sec - t.utc_offset_min * 60LL;   /* to UTC */
}

static void load_roots(void) {
    if (roots_loaded) return;
    roots_loaded = true;
    static char pem[420000];
    long n = fs_read("/system/certs/ca-bundle.crt", pem, 0, sizeof pem);
    if (n > 0) x509_add_roots_pem(pem, n);
    static struct dunc_dirent d[32];
    int k = fs_list("/system/certs/user", d, 32);
    for (int i = 0; i < k; i++) {
        char p[128]; ksnprintf(p, sizeof p, "/system/certs/user/%s", d[i].name);
        n = fs_read(p, pem, 0, sizeof pem);
        if (n > 0) x509_add_roots_pem(pem, n);
    }
}

int http_trusted_roots(void) { load_roots(); return x509_root_count(); }
int http_trust_pem(const char *pem, long n) { load_roots(); return x509_add_roots_pem(pem, n); }

/* a connection: plain TCP or TLS on top of it */
struct conn { int h; bool tls; };
static long c_write(struct conn *c, const void *b, uint32_t n) { return c->tls ? tls_write(&tls_conn, b, n) : tcp_write(c->h, b, n); }
static long c_read(struct conn *c, void *b, uint32_t n, long to) { return c->tls ? tls_read(&tls_conn, b, n, to) : tcp_read(c->h, b, n, to); }
static void c_close(struct conn *c) { if (c->tls) tls_close(&tls_conn); tcp_shut(c->h); }

const char *net_strerror(int e) {
    switch (e) {
    case NET_ENODEV: return "No network adapter found";
    case NET_ENOTCONN: return "Not connected to a network";
    case NET_ETIMEDOUT: return "Timed out";
    case NET_EREFUSED: return "Connection refused";
    case NET_ERESET: return "Connection reset";
    case NET_ENOTFOUND: return "Name not found";
    case NET_EINVAL: return "Invalid request";
    case NET_ENOMEM: return "Too many connections";
    case NET_EAGAIN: return "Busy, try again";
    case NET_EUNREACH: return "Can't reach that address from this network";
    }
    return "Network error";
}

const char *http_strerror(long e) {
    if (e <= -40 && e >= -68) {   /* TLS and certificate errors */
        static char m[220];
        ksnprintf(m, sizeof m, "Secure connection failed: %s", tls_strerror(&tls_conn, (int)e));
        return m;
    }
    if (e == HTTP_EURL) return "That address doesn't look right";
    if (e == HTTP_EBAD) return "The server sent something DuncOS couldn't understand";
    return net_strerror((int)e);
}

void ip_str(uint32_t ip, char *out) { ksnprintf(out, 16, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24); }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool starts_ci(const char *s, const char *p) { for (; *p; s++, p++) if (lower(*s) != lower(*p)) return false; return true; }

struct url { char host[128], path[512]; int port; bool https, v6lit; };
static long parse_url(const char *u, struct url *o) {
    while (*u == ' ') u++;
    o->https = false;
    if (starts_ci(u, "https://")) { u += 8; o->https = true; }
    else if (starts_ci(u, "http://")) u += 7;
    int k = 0;
    o->v6lit = *u == '[';
    if (o->v6lit) {                                            /* http://[2001:db8::1]:8080/ */
        u++;
        while (*u && *u != ']' && k < 127) o->host[k++] = *u++;
        if (*u++ != ']') return HTTP_EURL;
        o->host[k] = 0;
        struct ipaddr a;
        if (!ip_parse(o->host, &a) || !a.v6) return HTTP_EURL;
    } else {
        while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && k < 127) o->host[k++] = *u++;
        o->host[k] = 0;
    }
    if (!k) return HTTP_EURL;
    o->port = o->https ? 443 : 80;
    if (*u == ':') { u++; o->port = 0; while (*u >= '0' && *u <= '9') o->port = o->port * 10 + (*u++ - '0'); if (!o->port || o->port > 65535) return HTTP_EURL; }
    k = 0;
    if (*u != '/') o->path[k++] = '/';
    while (*u && *u != '#' && k < 511) { char c = *u++; if (c == ' ') { if (k < 509) { o->path[k++] = '%'; o->path[k++] = '2'; o->path[k++] = '0'; } } else o->path[k++] = c; }
    o->path[k] = 0;
    return 0;
}

static long hex(const char *s) { long v = 0; for (;; s++) { int c = lower(*s); if (c >= '0' && c <= '9') v = v * 16 + c - '0'; else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10; else return v; } }

/* chunked transfer decoding, streaming */
struct dechunk { int state; long left; char hex[20]; int hl; };   /* 0 size line, 1 data, 2 data CRLF, 3 done */
static long dechunk_feed(struct dechunk *d, const char *in, long n, http_sink sink, void *ctx, bool *stop) {
    long out = 0;
    for (long i = 0; i < n && d->state != 3 && !*stop;) {
        if (d->state == 0) {
            char c = in[i++];
            if (c == '\n') { d->hex[d->hl] = 0; d->left = hex(d->hex); d->hl = 0; d->state = d->left ? 1 : 3; }
            else if (c != '\r' && d->hl < 19) d->hex[d->hl++] = c;
        } else if (d->state == 1) {
            long k = n - i < d->left ? n - i : d->left;
            if (!sink(in + i, k, ctx)) *stop = true;
            out += k; i += k; d->left -= k;
            if (!d->left) d->state = 2;
        } else { if (in[i++] == '\n') d->state = 0; }
    }
    return out;
}

/* host[:port] as it appears in a URL or Host header */
static void hostport(const struct url *u, char *out, int n, bool port) {
    const char *l = u->v6lit ? "[" : "", *r = u->v6lit ? "]" : "";
    if (port) ksnprintf(out, n, "%s%s%s:%d", l, u->host, r, u->port); else ksnprintf(out, n, "%s%s%s", l, u->host, r);
}

/* connect to the first address that answers. With several addresses (IPv6 and IPv4), each gets a short
 * time before moving on, so a broken IPv6 path falls back to IPv4 quickly. */
static int connect_any(const char *host, int port) {
    struct ipaddr a[8];
    int n = sys_resolve(host, a, 8);
    if (n < 0) return n;
    if (n == 0) return NET_ENOTFOUND;
    int err = NET_ETIMEDOUT;
    for (int i = 0; i < n; i++) {
        bool other_family_left = false;
        for (int j = i + 1; j < n; j++) if (a[j].v6 != a[i].v6) other_family_left = true;
        int h = tcp_open(&a[i], port, other_family_left ? 3000 : i + 1 < n ? 6000 : 15000);
        if (h >= 0) { ip_format(&a[i], http_sec.addr); return h; }
        err = h;
        if (h != NET_ETIMEDOUT && h != NET_EUNREACH && h != NET_EREFUSED && h != NET_ENOTCONN) break;
    }
    return err;
}

long http_fetch(const char *url_in, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen, char *final_url, int fulen) {
    static char hdr[16384];
    char url[600];
    strncpy0(url, url_in, sizeof url);
    if (total) *total = -1;
    for (int hop = 0; hop < 6; hop++) {
        struct url u;
        long e = parse_url(url, &u);
        if (e) return e;
        const char *scheme = u.https ? "https" : "http";
        bool default_port = u.port == (u.https ? 443 : 80);
        char hp[160], hpp[160];
        hostport(&u, hp, sizeof hp, !default_port); hostport(&u, hpp, sizeof hpp, true);
        if (final_url) ksnprintf(final_url, fulen, "%s://%s%s", scheme, hp, u.path);
        memset(&http_sec, 0, sizeof http_sec);
        struct conn c = { connect_any(u.host, u.port), u.https };
        if (c.h < 0) return c.h;
        if (u.https) {
            load_roots();
            struct tls_io io = { &c.h, t_send, t_recv, t_rand, unix_now() };
            int tr = tls_connect(&tls_conn, &io, u.host);
            if (tr) { tcp_shut(c.h); return tr; }
            http_sec.secure = 1;
            strncpy0(http_sec.cipher, tls_conn.cipher, sizeof http_sec.cipher);
            strncpy0(http_sec.subject, tls_conn.peer, sizeof http_sec.subject);
            strncpy0(http_sec.issuer, tls_conn.issuer, sizeof http_sec.issuer);
        }
        char req[900];
        int rl = ksnprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: DuncOS/0.1 (Thane kernel)\r\nAccept: */*\r\nConnection: close\r\n\r\n", u.path, hp);
        if (c_write(&c, req, rl) < 0) { c_close(&c); return NET_ERESET; }
        long hl = 0, hend = -1;
        while (hend < 0 && hl < (long)sizeof hdr - 1) {
            long n = c_read(&c, hdr + hl, sizeof hdr - 1 - hl, 15000);
            if (n <= 0) { c_close(&c); return n == 0 ? HTTP_EBAD : n; }
            hl += n; hdr[hl] = 0;
            for (long i = 3; i < hl; i++) if (hdr[i - 3] == '\r' && hdr[i - 2] == '\n' && hdr[i - 1] == '\r' && hdr[i] == '\n') { hend = i + 1; break; }
        }
        if (hend < 0 || !starts_ci(hdr, "HTTP/1.")) { c_close(&c); return HTTP_EBAD; }
        int code = 0; { const char *s = hdr + 8; while (*s == ' ') s++; while (*s >= '0' && *s <= '9') code = code * 10 + (*s++ - '0'); }
        long clen = -1; bool chunked = false; char loc[600] = "";
        if (ctype && ctlen) ctype[0] = 0;
        for (char *line = hdr; line < hdr + hend;) {
            char *nl = line; while (nl < hdr + hend && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *v = line; while (*v && *v != ':') v++;
            if (*v) { v++; while (*v == ' ') v++; char *e2 = v + strlen(v); while (e2 > v && (e2[-1] == '\r' || e2[-1] == ' ')) *--e2 = 0; }
            if (starts_ci(line, "content-length:")) clen = katoi(v);
            else if (starts_ci(line, "transfer-encoding:") && starts_ci(v, "chunked")) chunked = true;
            else if (starts_ci(line, "location:")) strncpy0(loc, v, sizeof loc);
            else if (starts_ci(line, "content-type:") && ctype) strncpy0(ctype, v, ctlen);
            *nl = save; line = nl + 1;
        }
        if (code >= 300 && code < 400 && loc[0]) {
            c_close(&c);
            if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) strncpy0(url, loc, sizeof url);
            else if (loc[0] == '/' && loc[1] == '/') ksnprintf(url, sizeof url, "%s:%s", scheme, loc);
            else if (loc[0] == '/') ksnprintf(url, sizeof url, "%s://%s%s", scheme, hpp, loc);
            else {
                char base[512]; strncpy0(base, u.path, sizeof base);
                char *sl = base + strlen(base); while (sl > base && sl[-1] != '/') sl--; *sl = 0;
                ksnprintf(url, sizeof url, "%s://%s%s%s", scheme, hpp, base, loc);
            }
            continue;
        }
        if (status) *status = code;
        if (total && !chunked) *total = clen;
        static char buf[32768];
        struct dechunk dc = { 0 };
        bool stop = false;
        long got = 0;
        long have = hl - hend;
        if (have) {
            if (chunked) got += dechunk_feed(&dc, hdr + hend, have, sink, ctx, &stop);
            else { long k = clen >= 0 && have > clen ? clen : have; if (!sink(hdr + hend, k, ctx)) stop = true; got += k; }
        }
        while (!stop && (chunked ? dc.state != 3 : (clen < 0 || got < clen))) {
            long n = c_read(&c, buf, sizeof buf, 15000);
            if (n <= 0) break;
            if (chunked) got += dechunk_feed(&dc, buf, n, sink, ctx, &stop);
            else { if (clen >= 0 && got + n > clen) n = clen - got; if (!sink(buf, n, ctx)) stop = true; got += n; }
        }
        c_close(&c);
        return got;
    }
    return HTTP_EBAD;
}

struct membuf { char *b; long max, len; };
static bool mem_sink(const char *d, long n, void *ctx) {
    struct membuf *m = ctx;
    long k = m->len + n > m->max ? m->max - m->len : n;
    memcpy(m->b + m->len, d, k); m->len += k;
    return m->len < m->max;
}

long http_get(const char *url, char *body, long max, int *status, char *ctype, int ctlen, char *final_url, int fulen) {
    struct membuf m = { body, max, 0 };
    long r = http_fetch(url, mem_sink, &m, status, 0, ctype, ctlen, final_url, fulen);
    return r < 0 ? r : m.len;
}
