/* Network helpers and an HTTP/1.1 client for DuncOS apps. */
#include "dunc.h"

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
    }
    return "Network error";
}

const char *http_strerror(long e) {
    if (e == HTTP_EHTTPS) return "Secure (https://) sites need TLS, which DuncOS doesn't have yet";
    if (e == HTTP_EURL) return "That address doesn't look right";
    if (e == HTTP_EBAD) return "The server sent something DuncOS couldn't understand";
    return net_strerror((int)e);
}

void ip_str(uint32_t ip, char *out) { ksnprintf(out, 16, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24); }

bool ip_parse(const char *s, uint32_t *out) {
    uint32_t v[4];
    for (int k = 0; k < 4; k++) {
        if (*s < '0' || *s > '9') return false;
        v[k] = 0;
        while (*s >= '0' && *s <= '9') { v[k] = v[k] * 10 + (*s++ - '0'); if (v[k] > 255) return false; }
        if (k < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = IP4(v[0], v[1], v[2], v[3]);
    return true;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool starts_ci(const char *s, const char *p) { for (; *p; s++, p++) if (lower(*s) != lower(*p)) return false; return true; }

struct url { char host[128], path[512]; int port; };
static long parse_url(const char *u, struct url *o) {
    while (*u == ' ') u++;
    if (starts_ci(u, "https://")) return HTTP_EHTTPS;
    if (starts_ci(u, "http://")) u += 7;
    int k = 0;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && k < 127) o->host[k++] = *u++;
    o->host[k] = 0;
    if (!k) return HTTP_EURL;
    o->port = 80;
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

long http_fetch(const char *url_in, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen, char *final_url, int fulen) {
    static char hdr[16384];
    char url[600];
    strncpy0(url, url_in, sizeof url);
    if (total) *total = -1;
    for (int hop = 0; hop < 6; hop++) {
        struct url u;
        long e = parse_url(url, &u);
        if (e) return e;
        if (final_url) {
            if (u.port != 80) ksnprintf(final_url, fulen, "http://%s:%d%s", u.host, u.port, u.path);
            else ksnprintf(final_url, fulen, "http://%s%s", u.host, u.path);
        }
        uint32_t ip;
        int r = sys_resolve(u.host, &ip);
        if (r) return r;
        int h = tcp_open(ip, u.port);
        if (h < 0) return h;
        char req[900];
        int rl = ksnprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: DuncOS/0.1 (Thane kernel)\r\nAccept: */*\r\nConnection: close\r\n\r\n", u.path, u.host);
        if (tcp_write(h, req, rl) < 0) { tcp_shut(h); return NET_ERESET; }
        long hl = 0, hend = -1;
        while (hend < 0 && hl < (long)sizeof hdr - 1) {
            long n = tcp_read(h, hdr + hl, sizeof hdr - 1 - hl, 15000);
            if (n <= 0) { tcp_shut(h); return n == 0 ? HTTP_EBAD : n; }
            hl += n; hdr[hl] = 0;
            for (long i = 3; i < hl; i++) if (hdr[i - 3] == '\r' && hdr[i - 2] == '\n' && hdr[i - 1] == '\r' && hdr[i] == '\n') { hend = i + 1; break; }
        }
        if (hend < 0 || !starts_ci(hdr, "HTTP/1.")) { tcp_shut(h); return HTTP_EBAD; }
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
            tcp_shut(h);
            if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) strncpy0(url, loc, sizeof url);
            else if (loc[0] == '/') ksnprintf(url, sizeof url, "http://%s:%d%s", u.host, u.port, loc);
            else {
                char base[512]; strncpy0(base, u.path, sizeof base);
                char *sl = base + strlen(base); while (sl > base && sl[-1] != '/') sl--; *sl = 0;
                ksnprintf(url, sizeof url, "http://%s:%d%s%s", u.host, u.port, base, loc);
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
            long n = tcp_read(h, buf, sizeof buf, 15000);
            if (n <= 0) break;
            if (chunked) got += dechunk_feed(&dc, buf, n, sink, ctx, &stop);
            else { if (clen >= 0 && got + n > clen) n = clen - got; if (!sink(buf, n, ctx)) stop = true; got += n; }
        }
        tcp_shut(h);
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
