/* Network helpers and an HTTP/1.1 client for DuncOS apps. */
#include "dunc.h"
#include "crypto/tls.h"
#include "img/img.h"

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

/* ---------------- cookies (RFC 6265, simplified) ---------------- */
int http_options;
#define MAX_COOKIES 300
#define COOKIES_PER_DOMAIN 50
#define COOKIE_MAX 4096
struct cookie { char *name, *value; char domain[128], path[128]; int64_t expires; bool secure, host_only; uint32_t seq; };
static struct cookie *jar[MAX_COOKIES];
static int njar;
static bool jar_loaded, jar_dirty;
static uint32_t cookie_seq;
static const char *COOKIE_FILE = "/home/.web/cookies";

static void cookie_free(int i) { free(jar[i]->name); free(jar[i]->value); free(jar[i]); jar[i] = jar[--njar]; }

/* domains a site may not set cookies for (a few common public suffixes) */
static bool public_suffix(const char *d) {
    static const char *const ps[] = { "com", "net", "org", "edu", "gov", "mil", "int", "info", "biz", "io", "co", "me", "tv", "cc",
        "app", "dev", "xyz", "online", "site", "uk", "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk", "de", "fr", "nl",
        "be", "ch", "at", "it", "es", "pt", "se", "no", "dk", "fi", "pl", "cz", "ru", "ua", "eu", "ie", "us", "ca", "mx", "br", "com.br",
        "ar", "com.ar", "au", "com.au", "net.au", "org.au", "nz", "co.nz", "jp", "co.jp", "ne.jp", "or.jp", "cn", "com.cn", "hk", "com.hk",
        "tw", "com.tw", "kr", "co.kr", "in", "co.in", "sg", "com.sg", "za", "co.za", "tr", "com.tr", "il", "co.il", "local", "localhost",
        "github.io", "gitlab.io", "herokuapp.com", "blogspot.com", "appspot.com", "netlify.app", "vercel.app", "pages.dev", "workers.dev" };
    for (unsigned i = 0; i < sizeof ps / sizeof ps[0]; i++) if (!strcmp(d, ps[i])) return true;
    return false;
}
static bool is_ip(const char *h) { struct ipaddr a; return ip_parse(h, &a); }
static bool domain_match(const char *host, const char *dom) {
    int hl = strlen(host), dl = strlen(dom);
    if (hl == dl) return !strcmp(host, dom);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, dom) && !is_ip(host);
}
static bool path_match(const char *rp, const char *cp) {
    int n = strlen(cp);
    if (strncmp(rp, cp, n)) return false;
    return cp[n - 1] == '/' || rp[n] == 0 || rp[n] == '/' || rp[n] == '?';
}

static int month_of(const char *s) {
    static const char m[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    for (int i = 0; i < 12; i++) if (lower(s[0]) == m[i * 3] && lower(s[1]) == m[i * 3 + 1] && lower(s[2]) == m[i * 3 + 2]) return i + 1;
    return 0;
}
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
/* "Wed, 21 Oct 2015 07:28:00 GMT" and the older "Wednesday, 21-Oct-15 07:28:00 GMT"; 0 if unreadable */
static int64_t parse_http_date(const char *s) {
    int day = -1, mon = 0, year = -1, hh = -1, mm = 0, ss = 0;
    while (*s) {
        while (*s && !((*s >= '0' && *s <= '9') || (lower(*s) >= 'a' && lower(*s) <= 'z'))) s++;
        const char *t = s;
        while (*s && ((*s >= '0' && *s <= '9') || (lower(*s) >= 'a' && lower(*s) <= 'z') || *s == ':')) s++;
        int n = s - t;
        if (!n) break;
        if (n >= 3 && t[0] >= 'A' && !mon && month_of(t)) { mon = month_of(t); continue; }
        if (t[0] < '0' || t[0] > '9') continue;
        bool colon = false; for (int i = 0; i < n; i++) if (t[i] == ':') colon = true;
        if (colon && hh < 0) { hh = katoi(t); const char *c = t; while (*c != ':') c++; mm = katoi(++c); while (c < s && *c != ':') c++; if (c < s) ss = katoi(c + 1); continue; }
        int v = katoi(t);
        if (n <= 2 && day < 0) day = v;
        else if (year < 0) year = n <= 2 ? (v < 70 ? 2000 + v : 1900 + v) : v;
    }
    if (day < 1 || day > 31 || !mon || year < 1601 || hh < 0) return 0;
    return days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

static void jar_save(void) {
    if (!jar_dirty) return;
    jar_dirty = false;
    char *b = malloc(MAX_COOKIES * (COOKIE_MAX + 300));
    if (!b) return;
    long n = 0;
    for (int i = 0; i < njar; i++) {
        struct cookie *c = jar[i];
        if (!c->expires) continue;                     /* session cookies live only while the program runs */
        n += ksnprintf(b + n, COOKIE_MAX + 300, "%s\t%d\t%s\t%d\t%ld\t%s\t%s\n", c->domain, c->host_only, c->path, c->secure, (long)c->expires, c->name, c->value);
    }
    fs_mkdir("/home/.web");
    fs_write(COOKIE_FILE, b, n);
    free(b);
}

static void cookie_store(const char *name, int nl, const char *value, int vl, const char *domain, const char *path, int64_t expires,
                         bool secure, bool host_only) {
    for (int i = 0; i < njar; i++) {                   /* replace a cookie with the same name, domain and path */
        struct cookie *c = jar[i];
        if (!strcmp(c->domain, domain) && !strcmp(c->path, path) && (int)strlen(c->name) == nl && !memcmp(c->name, name, nl)) {
            if (c->expires || expires) jar_dirty = true;
            cookie_free(i);
            break;
        }
    }
    if (expires && expires <= unix_now()) return;      /* deleting */
    int same = 0, oldest = -1;
    for (int i = 0; i < njar; i++) if (!strcmp(jar[i]->domain, domain)) { same++; if (oldest < 0 || jar[i]->seq < jar[oldest]->seq) oldest = i; }
    if (same >= COOKIES_PER_DOMAIN) cookie_free(oldest);
    if (njar >= MAX_COOKIES) { int o = 0; for (int i = 1; i < njar; i++) if (jar[i]->seq < jar[o]->seq) o = i; cookie_free(o); }
    struct cookie *c = calloc(1, sizeof *c);
    if (!c) return;
    c->name = malloc(nl + 1); c->value = malloc(vl + 1);
    if (!c->name || !c->value) { free(c->name); free(c->value); free(c); return; }
    memcpy(c->name, name, nl); c->name[nl] = 0; memcpy(c->value, value, vl); c->value[vl] = 0;
    strncpy0(c->domain, domain, sizeof c->domain); strncpy0(c->path, path, sizeof c->path);
    c->expires = expires; c->secure = secure; c->host_only = host_only; c->seq = ++cookie_seq;
    jar[njar++] = c;
    if (expires) jar_dirty = true;
}

static void jar_load(void) {
    if (jar_loaded) return;
    jar_loaded = true;
    struct dunc_stat st;
    if (fs_stat(COOKIE_FILE, &st) < 0) return;
    long max = MAX_COOKIES * (COOKIE_MAX + 300);
    char *b = malloc(max + 1);
    if (!b) return;
    long n = fs_read(COOKIE_FILE, b, 0, max);
    if (n < 0) n = 0;
    b[n] = 0;
    int64_t now = unix_now();
    for (char *l = b; *l;) {
        char *e = l; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        char *f[7]; int k = 0; f[k++] = l;
        for (char *q = l; *q && k < 7; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (k == 7) {
            int64_t exp = 0; for (const char *q = f[4]; *q >= '0' && *q <= '9'; q++) exp = exp * 10 + (*q - '0');
            if (exp > now && f[0][0] && f[2][0] == '/' && f[5][0])
                cookie_store(f[5], strlen(f[5]), f[6], strlen(f[6]), f[0], f[2], exp, f[3][0] == '1', f[1][0] == '1');
        }
        *e = save; l = *e ? e + 1 : e;
    }
    jar_dirty = false;
    free(b);
}

/* one Set-Cookie header value from host/path */
static void set_cookie(const char *v, const char *host, const char *rpath, bool https) {
    const char *eq = v; while (*eq && *eq != '=' && *eq != ';') eq++;
    if (*eq != '=') return;
    const char *n0 = v, *n1 = eq; while (*n0 == ' ') n0++; while (n1 > n0 && n1[-1] == ' ') n1--;
    const char *v0 = eq + 1, *vend = v0; while (*vend && *vend != ';') vend++;
    const char *v1 = vend;
    while (*v0 == ' ') v0++;
    while (v1 > v0 && v1[-1] == ' ') v1--;
    if (v1 - v0 >= 2 && *v0 == '"' && v1[-1] == '"') { v0++; v1--; }
    int nl = n1 - n0, vl = v1 - v0;
    if (!nl || nl + vl > COOKIE_MAX) return;
    char domain[128] = "", path[128] = "";
    int64_t expires = 0, maxage = 0; bool has_maxage = false, secure = false;
    for (const char *a = vend; *a;) {
        if (*a == ';' || *a == ' ') { a++; continue; }
        const char *ae = a; while (*ae && *ae != ';') ae++;
        const char *av = a; while (av < ae && *av != '=') av++;
        char val[160]; int k = 0;
        if (av < ae) for (const char *q = av + 1; q < ae && k < 159; q++) if (k || *q != ' ') val[k++] = *q;
        while (k && val[k - 1] == ' ') k--;
        val[k] = 0;
        if (starts_ci(a, "domain=")) { const char *d = val; while (*d == '.') d++; int i = 0; for (; d[i] && i < 127; i++) domain[i] = lower(d[i]); domain[i] = 0; }
        else if (starts_ci(a, "path=")) { if (val[0] == '/') strncpy0(path, val, sizeof path); }
        else if (starts_ci(a, "expires=")) expires = parse_http_date(val);
        else if (starts_ci(a, "max-age=")) { has_maxage = true; bool neg = val[0] == '-'; maxage = katoi(val + neg); if (neg) maxage = -1; }
        else if (starts_ci(a, "secure") && (a + 6 == ae || a[6] == ' ' || a[6] == '=')) secure = true;
        a = ae;
    }
    if (secure && !https) return;
    bool host_only = !domain[0];
    if (host_only) strncpy0(domain, host, sizeof domain);
    else {
        if (!domain_match(host, domain)) return;
        if (public_suffix(domain)) { if (strcmp(domain, host)) return; host_only = true; }
    }
    if (!path[0]) {                                    /* default path: the request path's directory */
        int k = 0; for (const char *q = rpath; *q && *q != '?' && k < 127; q++) path[k++] = *q;
        path[k] = 0;
        while (k > 0 && path[k - 1] != '/') path[--k] = 0;
        if (k > 1) path[--k] = 0;
        if (!k) strncpy0(path, "/", sizeof path);
    }
    if (has_maxage) expires = maxage <= 0 ? 1 : unix_now() + maxage;
    cookie_store(n0, nl, v0, vl, domain, path, expires, secure, host_only);
}

/* the Cookie header for a request (most specific paths first); returns its length */
static int cookie_header(char *out, int max, const char *host, const char *path, bool https) {
    int64_t now = unix_now();
    for (int i = 0; i < njar;) if (jar[i]->expires && jar[i]->expires <= now) { cookie_free(i); jar_dirty = true; } else i++;
    struct cookie *m[MAX_COOKIES]; int nm = 0;
    for (int i = 0; i < njar; i++) {
        struct cookie *c = jar[i];
        if (c->secure && !https) continue;
        if (c->host_only ? strcmp(c->domain, host) : !domain_match(host, c->domain)) continue;
        if (!path_match(path, c->path)) continue;
        int j = nm++;
        while (j > 0 && (strlen(m[j - 1]->path) < strlen(c->path) || (strlen(m[j - 1]->path) == strlen(c->path) && m[j - 1]->seq > c->seq))) { m[j] = m[j - 1]; j--; }
        m[j] = c;
    }
    if (!nm) return 0;
    int n = ksnprintf(out, max, "Cookie: ");
    for (int i = 0; i < nm; i++) {
        int need = strlen(m[i]->name) + strlen(m[i]->value) + 4;
        if (n + need + 3 >= max) break;
        n += ksnprintf(out + n, max - n, "%s%s=%s", i ? "; " : "", m[i]->name, m[i]->value);
    }
    n += ksnprintf(out + n, max - n, "\r\n");
    return n;
}

int http_cookie_count(void) { jar_load(); return njar; }
void http_cookies_clear(void) {
    jar_load();
    while (njar) cookie_free(0);
    fs_delete(COOKIE_FILE);
    jar_dirty = false;
}
int http_cookie_list(char *out, int max) {
    jar_load();
    int n = 0; out[0] = 0;
    for (int i = 0; i < njar && n < max - 1; i++)
        n += ksnprintf(out + n, max - n, "%s%s %s %s%s\n", jar[i]->host_only ? "" : ".", jar[i]->domain, jar[i]->path, jar[i]->name,
                       jar[i]->expires ? "" : " (session)");
    return njar;
}

/* ---------------- requests ---------------- */
/* gzip bodies are collected, then decoded and passed on */
struct gzbuf { uint8_t *b; long len, cap; bool over; };
#define GZ_MAX_IN (16l << 20)
#define GZ_MAX_OUT (64l << 20)
static bool gz_sink(const char *d, long n, void *ctx) {
    struct gzbuf *g = ctx;
    if (g->len + n > GZ_MAX_IN) { g->over = true; return false; }
    if (g->len + n > g->cap) {
        long nc = g->cap ? g->cap * 2 : 65536; while (nc < g->len + n) nc *= 2;
        uint8_t *nb = realloc(g->b, nc);
        if (!nb) { g->over = true; return false; }
        g->b = nb; g->cap = nc;
    }
    memcpy(g->b + g->len, d, n); g->len += n;
    return true;
}
static void *gz_alloc(void *c, size_t n) { return malloc(n); }
static void gz_free(void *c, void *p) { free(p); }

/* the request target, with spaces and non-ASCII (Windows-1252) characters percent-encoded as UTF-8 */
static void encode_target(const char *in, char *out, int max) {
    static const char hx[] = "0123456789ABCDEF";
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k < max - 10; p++) {
        if (*p > ' ' && *p < 127) { out[k++] = *p; continue; }
        char u[4]; long n = cp1252_to_utf8((const char *)p, 1, u, 4);
        if (*p == ' ') { u[0] = ' '; n = 1; }
        for (long i = 0; i < n; i++) { out[k++] = '%'; out[k++] = hx[(unsigned char)u[i] >> 4]; out[k++] = hx[u[i] & 15]; }
    }
    out[k] = 0;
}

long http_request(const char *url_in, const struct http_req *rq, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen,
                  char *final_url, int fulen) {
    static char hdr[16384];
    static char req[12288];
    char url[600];
    strncpy0(url, url_in, sizeof url);
    if (total) *total = -1;
    bool cookies = http_options & HTTP_OPT_COOKIES, want_gz = http_options & HTTP_OPT_GZIP;
    if (cookies) jar_load();
    const char *method = rq && rq->method ? rq->method : "GET";
    const char *body = rq ? rq->body : 0; long body_len = body ? rq->body_len : 0;
    for (int hop = 0; hop < 8; hop++) {
        struct url u;
        long e = parse_url(url, &u);
        if (e) return e;
        for (char *h = u.host; *h; h++) *h = lower(*h);
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
        char target[1600]; encode_target(u.path, target, sizeof target);
        int rl = ksnprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: DuncOS/0.3 (Thane kernel) Web/0.3\r\n"
                           "Accept: text/html,application/xhtml+xml,text/css,image/png,image/jpeg,image/gif,*/*;q=0.5\r\n%sConnection: close\r\n",
                           method, target, hp, want_gz ? "Accept-Encoding: gzip\r\n" : "");
        if (rq && rq->referer && rq->referer[0]) rl += ksnprintf(req + rl, sizeof req - rl, "Referer: %s\r\n", rq->referer);
        if (cookies) rl += cookie_header(req + rl, sizeof req - rl - 200, u.host, u.path, u.https);
        if (body) rl += ksnprintf(req + rl, sizeof req - rl, "Content-Type: %s\r\nContent-Length: %ld\r\n",
                                  rq->content_type ? rq->content_type : "application/x-www-form-urlencoded", body_len);
        rl += ksnprintf(req + rl, sizeof req - rl, "\r\n");
        if (c_write(&c, req, rl) < 0 || (body_len && c_write(&c, body, body_len) < 0)) { c_close(&c); return NET_ERESET; }
        long hl = 0, hend = -1;
        while (hend < 0 && hl < (long)sizeof hdr - 1) {
            long n = c_read(&c, hdr + hl, sizeof hdr - 1 - hl, 15000);
            if (n <= 0) { c_close(&c); return n == 0 ? HTTP_EBAD : n; }
            hl += n; hdr[hl] = 0;
            for (long i = 3; i < hl; i++) if (hdr[i - 3] == '\r' && hdr[i - 2] == '\n' && hdr[i - 1] == '\r' && hdr[i] == '\n') { hend = i + 1; break; }
        }
        if (hend < 0 || !starts_ci(hdr, "HTTP/1.")) { c_close(&c); return HTTP_EBAD; }
        int code = 0; { const char *s = hdr + 8; while (*s == ' ') s++; while (*s >= '0' && *s <= '9') code = code * 10 + (*s++ - '0'); }
        long clen = -1; bool chunked = false, gz = false; char loc[600] = "";
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
            else if (starts_ci(line, "content-encoding:") && (starts_ci(v, "gzip") || starts_ci(v, "x-gzip"))) gz = want_gz;
            else if (starts_ci(line, "set-cookie:") && cookies) set_cookie(v, u.host, u.path, u.https);
            *nl = save; line = nl + 1;
        }
        if (cookies) jar_save();
        if (code >= 300 && code < 400 && code != 304 && loc[0]) {
            c_close(&c);
            if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) strncpy0(url, loc, sizeof url);
            else if (loc[0] == '/' && loc[1] == '/') ksnprintf(url, sizeof url, "%s:%s", scheme, loc);
            else if (loc[0] == '/') ksnprintf(url, sizeof url, "%s://%s%s", scheme, hpp, loc);
            else {
                char base[512]; strncpy0(base, u.path, sizeof base);
                char *q = base; while (*q && *q != '?') q++; *q = 0;
                char *sl = base + strlen(base); while (sl > base && sl[-1] != '/') sl--; *sl = 0;
                ksnprintf(url, sizeof url, "%s://%s%s%s", scheme, hpp, base, loc);
            }
            if (code != 307 && code != 308) { method = "GET"; body = 0; body_len = 0; }
            continue;
        }
        if (status) *status = code;
        if (total && !chunked && !gz) *total = clen;
        static char buf[32768];
        struct dechunk dc = { 0 };
        struct gzbuf g = { 0 };
        http_sink out = gz ? gz_sink : sink; void *octx = gz ? (void *)&g : ctx;
        bool stop = false;
        long got = 0;
        long have = hl - hend;
        if (!strcmp(method, "HEAD") || code == 204 || code == 304) have = 0, clen = 0, chunked = false;
        if (have) {
            if (chunked) got += dechunk_feed(&dc, hdr + hend, have, out, octx, &stop);
            else { long k = clen >= 0 && have > clen ? clen : have; if (!out(hdr + hend, k, octx)) stop = true; got += k; }
        }
        while (!stop && (chunked ? dc.state != 3 : (clen < 0 || got < clen))) {
            long n = c_read(&c, buf, sizeof buf, 15000);
            if (n <= 0) break;
            if (chunked) got += dechunk_feed(&dc, buf, n, out, octx, &stop);
            else { if (clen >= 0 && got + n > clen) n = clen - got; if (!out(buf, n, octx)) stop = true; got += n; }
        }
        c_close(&c);
        if (!gz) return got;
        /* decode the gzip body and pass it on */
        long r = HTTP_EBAD;
        long osz = g.over ? -1 : gzip_size(g.b, g.len);
        if (osz >= 0 && osz <= GZ_MAX_OUT) {
            uint8_t *ob = malloc(osz ? osz : 1);
            struct img_alloc al = { 0, gz_alloc, gz_free };
            long n = ob ? gunzip(g.b, g.len, ob, osz, &al) : -1;
            if (n >= 0) {
                if (total) *total = n;
                for (long off = 0; off < n;) { long k = n - off > 32768 ? 32768 : n - off; bool more = sink((const char *)ob + off, k, ctx); off += k; if (!more) break; }
                r = n;
            }
            free(ob);
        }
        free(g.b);
        return r;
    }
    return HTTP_EBAD;
}

long http_fetch(const char *url, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen, char *final_url, int fulen) {
    return http_request(url, 0, sink, ctx, status, total, ctype, ctlen, final_url, fulen);
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
