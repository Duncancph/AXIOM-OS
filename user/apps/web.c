/* Web 0.3 - a web browser for AXIOM (installed with: clan install web).
 * Fetches pages over HTTP and HTTPS (TLS 1.3 with certificate checks), with cookies and gzip, and shows them with
 * style sheets (a useful part of CSS 2 and a little of CSS 3), images (PNG, JPEG, GIF, BMP) and forms. No scripts.
 * The parts: web/html.c (document tree), web/css.c (styles), web/layout.c (layout); this file does the window,
 * loading, history, images, forms and drawing. */
#include "web/web.h"

static int WW = 900, WH = 640;
#define BAR_H 52
#define STATUS_H 24
#define SCROLLBAR 12
#define MAXPAGE (4l << 20)
#define MAXIMG (8l << 20)
#define MAXCSS (1l << 20)

static struct app a;
static char url[600] = "about:home", status_text[220];
static char addr[600]; static int alen; static bool addr_focus;
static int scroll, hover_link = -1, hover_btn = -1;
static struct http_security sec;
static char page_status[220];
static struct doc *doc;
static struct layout lay;
static bool need_layout, need_cascade, need_draw, quit, loading, abort_fetch, styles_loading;
struct doc *web_doc;
static int fetch_depth;

/* history: back and forward */
struct hent { char url[600]; int scroll; };
#define MAXHIST 64
static struct hent hist[MAXHIST]; static int hpos = -1, hcount;

/* a navigation asked for while something was loading */
static bool pend; static char pend_url[600]; static char *pend_body; static long pend_blen; static bool pend_post; static int pend_mode, pend_scroll;

struct link *links; int nlinks, links_cap;

/* forms */
static struct node *focus;                 /* the focused form control */
static struct node *popup;                 /* an open <select> list */
static int popup_hover = -1, popup_first;

static void draw(void);
static void handle_event(struct dunc_event *e);
static void request_nav(const char *u, bool post, const char *body, long blen, int mode, int restore);

/* ---------------- URLs ---------------- */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool pre_ci(const char *s, const char *p) { for (; *p; s++, p++) if (lc(*s) != *p) return false; return true; }

static int scheme_len(const char *s) {                  /* "https:" -> 5, or 0 */
    int i = 0;
    if (!(lc(s[0]) >= 'a' && lc(s[0]) <= 'z')) return 0;
    while (s[i] && ((lc(s[i]) >= 'a' && lc(s[i]) <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    return s[i] == ':' ? i : 0;
}

/* remove "." and ".." segments from a path (in place, at most 600 bytes) */
static void norm_path(char *p) {
    static char out[8300], tail[16384];                  /* p holds up to 16384 (resolve_url's res) */
    int o = 0;
    char *q = p; while (*q && *q != '?' && *q != '#') q++;
    strncpy0(tail, q, sizeof tail); *q = 0;
    for (char *s = p; *s;) {
        char *e = s; while (*e && *e != '/') e++;
        int n = e - s;
        if (n == 1 && s[0] == '.') { }
        else if (n == 2 && s[0] == '.' && s[1] == '.') { if (o > 1) { o--; while (o > 0 && out[o - 1] != '/') o--; } }
        else if (n || !o) { if (o + n + 2 < 8190) { memcpy(out + o, s, n); o += n; if (*e) out[o++] = '/'; } }
        if (!*e) break;
        s = e + 1;
    }
    out[o] = 0;
    if (!o || out[0] != '/') { memmove(out + 1, out, o + 1); out[0] = '/'; }
    ksnprintf(p, 16384, "%s%s", out, tail);
}

void resolve_url(const char *base, const char *href_in, char *out, int n) {
    static char href[8192]; int k = 0;                  /* addresses can be long (lists of modules) */
    while (*href_in == ' ' || *href_in == '\n' || *href_in == '\t' || *href_in == '\r') href_in++;
    for (const char *s = href_in; *s && k < (int)sizeof href - 2; s++) if (*s != '\n' && *s != '\r' && *s != '\t') href[k++] = *s;
    while (k && href[k - 1] == ' ') k--;
    href[k] = 0;
    if (scheme_len(href)) {
        if (pre_ci(href, "http:") || pre_ci(href, "https:") || pre_ci(href, "about:") || pre_ci(href, "file:") || pre_ci(href, "data:")) {
            strncpy0(out, href, n); return;
        }
        strncpy0(out, "about:unsupported", n); return;
    }
    if (pre_ci(base, "about:") || pre_ci(base, "data:") || !scheme_len(base)) { strncpy0(out, href[0] == '#' || !href[0] ? base : "about:blank", n); return; }
    int bs = scheme_len(base);
    char scheme[16]; int s2 = bs < 15 ? bs : 15;
    for (int i = 0; i < s2; i++) scheme[i] = lc(base[i]);
    scheme[s2] = 0;
    const char *h = base + bs + 1; if (h[0] == '/' && h[1] == '/') h += 2;
    char origin[300]; int o = 0;
    while (*h && *h != '/' && *h != '?' && *h != '#' && o < 299) origin[o++] = *h++;
    origin[o] = 0;
    static char path[8192]; ksnprintf(path, sizeof path, "%s%s", *h == '/' ? "" : "/", h);
    static char res[16384];
    if (href[0] == '/' && href[1] == '/') { ksnprintf(out, n, "%s:%s", scheme, href); return; }
    if (href[0] == '/') ksnprintf(res, sizeof res, "%s", href);
    else if (href[0] == '#') { char *q = path; while (*q && *q != '#') q++; *q = 0; ksnprintf(res, sizeof res, "%s%s", path, href); }
    else if (href[0] == '?') { char *q = path; while (*q && *q != '?' && *q != '#') q++; *q = 0; ksnprintf(res, sizeof res, "%s%s", path, href); }
    else if (!href[0]) { char *q = path; while (*q && *q != '#') q++; *q = 0; ksnprintf(res, sizeof res, "%s", path); }
    else {
        char *q = path; while (*q && *q != '?' && *q != '#') q++; *q = 0;
        char *d = path + strlen(path); while (d > path && d[-1] != '/') d--; *d = 0;
        ksnprintf(res, sizeof res, "%s%s", path, href);
    }
    norm_path(res);
    ksnprintf(out, n, "%s://%s%s", scheme, origin, res);
}

static void strip_fragment(const char *u, char *out, int n) { strncpy0(out, u, n); for (char *q = out; *q; q++) if (*q == '#') { *q = 0; break; } }
static const char *fragment_of(const char *u) { for (; *u; u++) if (*u == '#') return u + 1; return ""; }

/* ---------------- fetching ---------------- */
struct fbuf { char *b; long len, max; bool trunc; };
static uint64_t last_poll;
static void poll_events(void) {
    uint64_t now = sys_time();
    if (now - last_poll < 30) return;
    last_poll = now;
    struct dunc_event e;
    for (int i = 0; i < 64 && sys_event(&e, 0) > 0; i++) {
        /* A page's script is waiting for a server: handling a click or a key now could run the page's
         * scripts again in the middle of that one (or leave the page), which the script engine can't
         * survive. Only closing and Esc act now; clicks and keys wait until the script has finished. */
        if (web_script_waiting) {
            if (e.type == EV_CLOSE) sys_exit(0);
            if (e.type == EV_KEY && e.key == 27) { abort_fetch = true; continue; }
            if (e.type != EV_MOUSE_MOVE) web_requeue_event(&e);
            continue;
        }
        handle_event(&e);
    }
}
static dunc_jmp stuck_jb; static uint64_t stuck_rip; static int stuck_count;
static void on_stuck_alarm(void *frame) {
    if (malloc_busy) { sys_alarm(20, on_stuck_alarm); sys_sigreturn(frame); }   /* not in the middle of changing the heap */
    stuck_rip = ((uint64_t *)frame)[17];                    /* where it was (struct frame: 15 registers, vec, err, rip) */
    dunc_longjmp(stuck_jb, 1);
}
static void recover_from_stuck(void) {
    stuck_count++;
    js_abandon(stuck_rip);
    loading = false; fetch_depth = 0; abort_fetch = false; web_fetch_deadline = 0; web_fetch_timeout = 0; pend = false;
    need_cascade = false; need_layout = stuck_count < 2;     /* if laying the page out is what got stuck, don't try again */
    ksnprintf(status_text, sizeof status_text, "This page made Arc get stuck (at %lx), so its scripts were turned off. Details: about:console", stuck_rip);
    strncpy0(page_status, status_text, sizeof page_status);
    need_draw = true;
}
void web_show_status(const char *s) { strncpy0(status_text, s, sizeof status_text); draw(); }
/* while waiting for a server: keep answering (close, Esc, scrolling) and give up when asked or too late */
uint64_t web_fetch_deadline;
int web_time_scale = 1;
int web_script_waiting;
static bool web_wait(void) {
    poll_events();
    if (abort_fetch || quit) return false;
    if (web_fetch_deadline && sys_time() > web_fetch_deadline) return false;
    return true;
}
static bool fbuf_sink(const char *d, long n, void *ctx) {
    struct fbuf *f = ctx;
    long k = f->len + n > f->max ? f->max - f->len : n;
    if (k > 0) { memcpy(f->b + f->len, d, k); f->len += k; }
    if (k < n) f->trunc = true;
    poll_events();
    return !f->trunc && !abort_fetch && !quit && !(web_fetch_deadline && sys_time() > web_fetch_deadline);
}
static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1; }
static int hex2(const char *q) { int h = 0; for (int i = 0; i < 2; i++) { int x = lc(q[i]); if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f'))) return -1; h = h * 16 + (x >= 'a' ? x - 'a' + 10 : x - '0'); } return h; }

#define ENOFILE -1000
/* any URL (http, https, file, data) into a new malloc'd buffer; returns the length or a negative error */
/* The web cache: pictures, style sheets, scripts and fonts this window has downloaded stay in memory,
 * so going back or opening more pages of the same site doesn't download them again. Pages themselves
 * and data asked for by scripts aren't kept (they change); reload (R) downloads everything again. */
struct wcache { struct wcache *next; char *url, *data, ctype[80]; long len; uint64_t used; };
long web_fetch_timeout;
static struct wcache *wc_list; static long wc_bytes, wc_max; static uint64_t wc_tick; static bool cache_bypass;
static bool wc_kind(const char *ct) {
    char l[40]; int k = 0; for (; ct[k] && ct[k] != ';' && k < 39; k++) l[k] = lc(ct[k]); l[k] = 0;
    return pre_ci(l, "image/") || pre_ci(l, "font/") || !strcmp(l, "text/css") || !strcmp(l, "text/javascript") ||
           !strcmp(l, "application/javascript") || !strcmp(l, "application/x-javascript") || !strcmp(l, "application/font-woff");
}
static struct wcache *wc_find(const char *u) { for (struct wcache *c = wc_list; c; c = c->next) if (!strcmp(c->url, u)) return c; return 0; }
static void wc_put(const char *u, const char *data, long len, const char *ct) {
    if (!wc_max) { struct sysinfo si; sys_sysinfo(&si); wc_max = (long)(si.mem_total / 8 < (256l << 20) ? si.mem_total / 8 : (256l << 20)); }
    if (len <= 0 || len > wc_max / 8 || wc_find(u)) return;
    while (wc_list && wc_bytes + len > wc_max) {            /* make room: the least recently used goes */
        struct wcache **pp = &wc_list, **old = &wc_list;
        for (; *pp; pp = &(*pp)->next) if ((*pp)->used < (*old)->used) old = pp;
        struct wcache *c = *old; *old = c->next; wc_bytes -= c->len; free(c->url); free(c->data); free(c);
    }
    struct wcache *c = calloc(1, sizeof *c); if (!c) return;
    c->url = malloc(strlen(u) + 1); c->data = malloc(len + 1);
    if (!c->url || !c->data) { free(c->url); free(c->data); free(c); return; }
    memcpy(c->url, u, strlen(u) + 1); memcpy(c->data, data, len); c->len = len; strncpy0(c->ctype, ct, sizeof c->ctype); c->used = ++wc_tick;
    c->next = wc_list; wc_list = c; wc_bytes += len;
}

static long fetch_any(const char *u, long cap, char **out, char *ctype, int ctlen, int *status, char *final, int fulen,
                      const char *method, const char *body, long blen) {
    *out = 0; if (ctype) ctype[0] = 0; if (status) *status = 0;
    if (final) strncpy0(final, u, fulen);
    if (pre_ci(u, "data:")) {
        const char *c = u + 5; while (*c && *c != ',') c++;
        if (!*c) return HTTP_EURL;
        bool b64 = false; for (const char *q = u + 5; q < c; q++) if (pre_ci(q, ";base64")) b64 = true;
        if (ctype) { int k = 0; for (const char *q = u + 5; q < c && *q != ';' && k < ctlen - 1; q++) ctype[k++] = *q; ctype[k] = 0; }
        long n = strlen(c + 1);
        char *b = malloc(n + 1); if (!b) return NET_ENOMEM;
        long o = 0;
        if (b64) { int acc = 0, bits = 0; for (const char *q = c + 1; *q; q++) { int v = b64v(*q); if (v < 0) continue; acc = (acc << 6 | v) & 0xFFFFFF; bits += 6; if (bits >= 8) { bits -= 8; b[o++] = (char)(acc >> bits); } } }
        else for (const char *q = c + 1; *q; q++) { int h; if (*q == '%' && q[1] && q[2] && (h = hex2(q + 1)) >= 0) { b[o++] = (char)h; q += 2; } else b[o++] = *q; }
        *out = b; if (status) *status = 200;
        return o;
    }
    if (pre_ci(u, "file:")) {
        const char *p = u + 5; while (p[0] == '/' && p[1] == '/') p++;
        char path[400]; int k = 0;
        for (; *p && *p != '?' && *p != '#' && k < 399; p++) { int h; if (*p == '%' && p[1] && p[2] && (h = hex2(p + 1)) >= 0) { path[k++] = (char)h; p += 2; } else path[k++] = *p; }
        path[k] = 0;
        struct dunc_stat st;
        if (fs_stat(path, &st) < 0) return ENOFILE;
        long n = (long)st.size; if (n > cap) n = cap;
        char *b = malloc(n + 1); if (!b) return NET_ENOMEM;
        long r = n ? fs_read(path, b, 0, n) : 0;
        if (r < 0) { free(b); return ENOFILE; }
        *out = b; if (status) *status = 200;
        if (ctype) {
            const char *e = path + strlen(path); while (e > path && e[-1] != '.' && e[-1] != '/') e--;
            const char *t = pre_ci(e, "htm") || pre_ci(e, "xhtml") ? "text/html" : pre_ci(e, "css") ? "text/css" : pre_ci(e, "png") ? "image/png" :
                            pre_ci(e, "jpg") || pre_ci(e, "jpeg") ? "image/jpeg" : pre_ci(e, "gif") ? "image/gif" : pre_ci(e, "bmp") ? "image/bmp" : "text/plain";
            strncpy0(ctype, t, ctlen);
        }
        return r;
    }
    if (!pre_ci(u, "http:") && !pre_ci(u, "https:")) return HTTP_EURL;
    bool get = (!method || !strcmp(method, "GET")) && !body;
    struct wcache *hit = get && !cache_bypass ? wc_find(u) : 0;
    if (hit && hit->len <= cap) {                           /* from the web cache */
        char *b = malloc(hit->len + 1); if (!b) return NET_ENOMEM;
        memcpy(b, hit->data, hit->len); hit->used = ++wc_tick;
        if (ctype) strncpy0(ctype, hit->ctype, ctlen);
        if (status) *status = 200;
        *out = b; return hit->len;
    }
    struct fbuf f = { malloc(cap + 1), 0, cap, false };
    if (!f.b) return NET_ENOMEM;
    struct http_req rq = { method, body, blen, "application/x-www-form-urlencoded", 0, 0, web_fetch_timeout };
    fetch_depth++;
    int st_local = 0; char ct_local[80] = "";
    if (!status) status = &st_local;                       /* the cache needs them even when the caller doesn't */
    if (!ctype) { ctype = ct_local; ctlen = sizeof ct_local; }
    long r = http_request(u, &rq, fbuf_sink, &f, status, 0, ctype, ctlen, final, fulen);
    fetch_depth--;
    if (r < 0 && !f.len) { free(f.b); return r; }
    char ctbuf[80] = ""; if (ctype) strncpy0(ctbuf, ctype, sizeof ctbuf);
    if (get && r >= 0 && !f.trunc && status && *status == 200 && wc_kind(ctbuf)) wc_put(u, f.b, f.len, ctbuf);
    *out = f.b;
    return f.len;
}

char *web_fetch_text(const char *u, long *len, long cap) {
    char *b; char ct[80];
    long n = fetch_any(u, cap, &b, ct, sizeof ct, 0, 0, 0, 0, 0, 0);
    if (n < 0 || !b) return 0;
    b[n] = 0;
    n = utf8_to_cp1252(b, n);
    *len = n;
    return b;
}

/* ---------------- images ---------------- */
static struct imgent *images; static long img_bytes;
#define IMG_CACHE_KEEP (48l << 20)
#define IMG_CACHE_MAX (320l << 20)
struct imgent *img_get(const char *u) {
    for (struct imgent *e = images; e; e = e->next) if (!strcmp(e->url, u)) { e->used = true; return e; }
    struct imgent *e = calloc(1, sizeof *e);
    if (!e) return 0;
    int n = strlen(u);
    e->url = malloc(n + 1);
    if (!e->url) { free(e); return 0; }
    memcpy(e->url, u, n + 1);
    e->state = IS_PENDING; e->used = true;
    e->next = images; images = e;
    return e;
}
static void img_free_pixels(struct imgent *e) {
    if (e->px) img_bytes -= (long)e->pw * e->ph * 4;
    if (e->spx) img_bytes -= (long)e->sw * e->sh * 4;
    free(e->px); free(e->spx); e->px = e->spx = 0; e->sw = e->sh = 0;
}
static void img_evict(void) {                       /* after a navigation: forget images the new page doesn't use */
    for (struct imgent **pp = &images; *pp;) {
        struct imgent *e = *pp;
        if (!e->used && (img_bytes > IMG_CACHE_KEEP || e->state != IS_OK)) { *pp = e->next; img_free_pixels(e); free(e->url); free(e); }
        else pp = &e->next;
    }
}
static void *ia(void *c, size_t n) { return malloc(n); }
static void ifr(void *c, void *p) { free(p); }

/* area-averaging (down) / nearest (up) scaling of 0xAARRGGBB pixels */
static uint32_t *scale_px(const uint32_t *src, int sw, int sh, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return 0;
    uint32_t *d = malloc((size_t)dw * dh * 4);
    if (!d) return 0;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh); if (y1 <= y0) y1 = y0 + 1; if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw); if (x1 <= x0) x1 = x0 + 1; if (x1 > sw) x1 = sw;
            if (x1 - x0 == 1 && y1 - y0 == 1) { d[y * dw + x] = src[y0 * sw + x0]; continue; }
            uint32_t A = 0, R = 0, G = 0, B = 0, n = 0;
            int sy = (y1 - y0) > 8 ? (y1 - y0) / 8 : 1, sx = (x1 - x0) > 8 ? (x1 - x0) / 8 : 1;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx) {
                    uint32_t p = src[yy * sw + xx], al = p >> 24;
                    A += al; R += (p >> 16 & 255) * al; G += (p >> 8 & 255) * al; B += (p & 255) * al; n++;
                }
            d[y * dw + x] = A ? (A / n) << 24 | (R / A) << 16 | (G / A) << 8 | (B / A) : 0;
        }
    }
    return d;
}

static bool decode_into(struct imgent *e, const uint8_t *data, long n) {
    int w, h;
    if (img_probe(data, n, &w, &h) != IMG_OK || w <= 0 || h <= 0 || (long)w * h > 16l * 1024 * 1024 || img_bytes > IMG_CACHE_MAX) return false;
    struct img_alloc al = { 0, ia, ifr };
    struct image im;
    if (img_decode(data, n, &im, &al) != IMG_OK) return false;
    e->w = im.w; e->h = im.h;
    if (im.w > 2048 || im.h > 4096) {                  /* keep a smaller copy of huge pictures */
        int dw = im.w, dh = im.h;
        if (dw > 2048) { dh = (int)((long)dh * 2048 / dw); dw = 2048; }
        if (dh > 4096) { dw = (int)((long)dw * 4096 / dh); dh = 4096; }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        uint32_t *s = scale_px(im.px, im.w, im.h, dw, dh);
        free(im.px);
        if (!s) return false;
        im.px = s; im.w = dw; im.h = dh;
    }
    e->px = im.px; e->pw = im.w; e->ph = im.h;
    e->opaque = true;
    for (long i = 0; i < (long)im.w * im.h; i++) if ((im.px[i] >> 24) != 255) { e->opaque = false; break; }
    img_bytes += (long)im.w * im.h * 4;
    e->state = IS_OK;
    return true;
}

/* the next picture to load: one on screen first */
static bool images_stopped;
static struct imgent *next_image(void) {
    struct imgent *first = 0;
    if (images_stopped) return 0;
    int top = scroll, bot = scroll + WH;
    for (int i = 0; i < lay.nit; i++) {
        struct ditem *it = &lay.it[i];
        struct imgent *im = it->kind == DI_IMG ? it->n->img : it->kind == DI_BGIMG ? it->img : 0;
        if (!im || im->state != IS_PENDING) continue;
        if (it->y + it->h >= top && it->y <= bot) return im;
        if (!first) first = im;
    }
    return first;
}
static uint64_t last_layout_t;
static void load_image(struct imgent *e) {
    e->state = IS_LOADING;
    char ct[80]; char *b; int status;
    abort_fetch = false;
    long n = fetch_any(e->url, MAXIMG, &b, ct, sizeof ct, &status, 0, 0, 0, 0, 0);
    if (pend || quit) { free(b); e->state = IS_PENDING; return; }
    if (abort_fetch) { free(b); e->state = IS_FAILED; images_stopped = true; return; }
    if (n <= 0 || !b || (status && status >= 400)) { free(b); e->state = IS_FAILED; need_layout = true; return; }
    if (!decode_into(e, (const uint8_t *)b, n)) e->state = IS_FAILED;
    free(b);
    need_layout = true;
}

/* ---------------- forms ---------------- */
static bool text_like(int t) { return t == FC_TEXT || t == FC_PASSWORD || t == FC_NUMBER || t == FC_TEXTAREA; }

static void text_content(struct node *e, char *out, int max, int *k, int depth) {
    for (struct node *c = e->first; c && depth < 20; c = c->next) {
        if (c->type == N_TEXT) {
            for (int i = 0; i < c->tlen && *k < max - 1; i++) {
                char ch = c->text[i];
                if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
                if (ch == ' ' && (!*k || out[*k - 1] == ' ')) continue;
                out[(*k)++] = ch;
            }
        } else text_content(c, out, max, k, depth + 1);
    }
    out[*k] = 0;
}
static void gather_options(struct fctl *fc, struct node *e, struct node **tmp, int depth) {
    for (struct node *c = e->first; c && depth < 4; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_OPTION) { if (fc->nopt < 500) tmp[fc->nopt++] = c; }
        else gather_options(fc, c, tmp, depth + 1);
    }
}

void form_init_control(struct doc *d, struct node *n) {
    struct fctl *fc = ar_alloc(&d->ar, sizeof *fc);
    if (!fc) return;
    const char *t = node_attr(n, "type");
    char ty[16] = ""; if (t) { int k = 0; for (; t[k] && k < 15; k++) ty[k] = lc(t[k]); ty[k] = 0; }
    if (n->tag == T_TEXTAREA) fc->type = FC_TEXTAREA;
    else if (n->tag == T_SELECT) fc->type = FC_SELECT;
    else if (n->tag == T_BUTTON) fc->type = !strcmp(ty, "reset") ? FC_RESET : !strcmp(ty, "button") ? FC_BUTTON : FC_SUBMIT;
    else fc->type = !strcmp(ty, "password") ? FC_PASSWORD : !strcmp(ty, "hidden") ? FC_HIDDEN : !strcmp(ty, "submit") ? FC_SUBMIT :
                    !strcmp(ty, "reset") ? FC_RESET : !strcmp(ty, "button") ? FC_BUTTON : !strcmp(ty, "checkbox") ? FC_CHECKBOX :
                    !strcmp(ty, "radio") ? FC_RADIO : !strcmp(ty, "number") || !strcmp(ty, "range") ? FC_NUMBER : !strcmp(ty, "image") ? FC_IMAGE :
                    !strcmp(ty, "file") ? FC_FILE : FC_TEXT;
    fc->cap = fc->type == FC_TEXTAREA ? 16384 : 1024;
    const char *ml = node_attr(n, "maxlength");
    if (ml && katoi(ml) > 0 && katoi(ml) + 1 < fc->cap) fc->cap = katoi(ml) + 1;
    fc->val = ar_alloc(&d->ar, fc->cap + 1);
    if (!fc->val) return;
    const char *v = node_attr(n, "value");
    if (fc->type == FC_TEXTAREA) {
        for (struct node *c = n->first; c; c = c->next) if (c->type == N_TEXT) for (int i = 0; i < c->tlen && fc->len < fc->cap - 1; i++) if (c->text[i] != '\r') fc->val[fc->len++] = c->text[i];
    } else if (v) { int k = strlen(v); if (k > fc->cap - 1) k = fc->cap - 1; memcpy(fc->val, v, k); fc->len = k; }
    fc->val[fc->len] = 0; fc->cur = fc->len;
    fc->checked = node_attr(n, "checked") != 0;
    fc->disabled = node_attr(n, "disabled") != 0;
    fc->readonly = node_attr(n, "readonly") != 0;
    const char *s = node_attr(n, "size"); fc->size = s ? katoi(s) : 0; if (fc->size > 200) fc->size = 200;
    if (fc->type == FC_NUMBER && !s) fc->size = 10;
    s = node_attr(n, "rows"); fc->rows = s ? katoi(s) : 2; if (fc->rows < 1) fc->rows = 1; if (fc->rows > 60) fc->rows = 60;
    s = node_attr(n, "cols"); fc->cols = s ? katoi(s) : 20; if (fc->cols < 2) fc->cols = 2; if (fc->cols > 200) fc->cols = 200;
    if (fc->type == FC_SELECT) {
        static struct node *tmp[500];
        gather_options(fc, n, tmp, 0);
        fc->opts = ar_alloc(&d->ar, (fc->nopt ? fc->nopt : 1) * sizeof(struct node *));
        if (!fc->opts) fc->nopt = 0;
        for (int i = 0; i < fc->nopt; i++) { fc->opts[i] = tmp[i]; if (node_attr(tmp[i], "selected")) fc->sel = i; }
    }
    if (n->tag == T_BUTTON) {
        char b[200]; int k = 0; text_content(n, b, sizeof b, &k, 0);
        while (k && b[k - 1] == ' ') b[--k] = 0;
        fc->label = ar_strndup(&d->ar, b[0] == ' ' ? b + 1 : b, strlen(b[0] == ' ' ? b + 1 : b));
        if (!b[0]) fc->label = "Button";
    } else if (fc->type == FC_SUBMIT) fc->label = v && v[0] ? v : "Submit";
    else if (fc->type == FC_RESET) fc->label = v && v[0] ? v : "Reset";
    else if (fc->type == FC_BUTTON) fc->label = v ? v : "";
    else if (fc->type == FC_IMAGE) { const char *alt = node_attr(n, "alt"); fc->label = alt && alt[0] ? alt : "Submit"; }
    else if (fc->type == FC_FILE) { fc->label = "Choose file (not supported)"; fc->disabled = true; }
    n->fc = fc;
}

/* application/x-www-form-urlencoded, from Windows-1252 text sent as UTF-8 */
static long enc_append(char *out, long o, long max, const char *s, long n) {
    static const char hx[] = "0123456789ABCDEF";
    for (long i = 0; i < n && o < max - 10; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '*') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else if (c == '\n') { if (!(i && s[i - 1] == '\r')) { memcpy(out + o, "%0D%0A", 6); o += 6; } }
        else if (c == '\r') { memcpy(out + o, "%0D%0A", 6); o += 6; }
        else {
            char u[4]; long k = cp1252_to_utf8((const char *)&c, 1, u, 4);
            for (long j = 0; j < k; j++) { out[o++] = '%'; out[o++] = hx[(unsigned char)u[j] >> 4]; out[o++] = hx[u[j] & 15]; }
        }
    }
    return o;
}
__attribute__((noinline)) static long add_pair(char *out, long o, long max, const char *name, const char *val, long vl) {
    if (o && o < max - 1) out[o++] = '&';
    o = enc_append(out, o, max, name, strlen(name));
    if (o < max - 1) out[o++] = '=';
    return enc_append(out, o, max, val, vl);
}
static long collect_form(struct node *e, struct node *form, struct node *submitter, char *out, long o, long max, int depth) {
    for (struct node *c = e->first; c && depth < 200; c = c->next) {
        if (c->type != N_ELEM) continue;
        struct fctl *fc = c->fc;
        if (fc && c->form == form && !fc->disabled) {
            const char *name = node_attr(c, "name");
            if (name && name[0]) {
                switch (fc->type) {
                case FC_TEXT: case FC_PASSWORD: case FC_HIDDEN: case FC_NUMBER: case FC_TEXTAREA: o = add_pair(out, o, max, name, fc->val, fc->len); break;
                case FC_CHECKBOX: case FC_RADIO: if (fc->checked) { const char *v = node_attr(c, "value"); if (!v) v = "on"; o = add_pair(out, o, max, name, v, strlen(v)); } break;
                case FC_SELECT: if (fc->nopt) {
                        const char *v = node_attr(fc->opts[fc->sel], "value"); static char t[300];
                        if (!v) { form_option_text(fc->opts[fc->sel], t, sizeof t); v = t; }
                        o = add_pair(out, o, max, name, v, strlen(v));
                    } break;
                case FC_SUBMIT: case FC_BUTTON:
                    if (c == submitter) { const char *v = node_attr(c, "value"); if (!v) v = c->tag == T_BUTTON ? "" : fc->label; o = add_pair(out, o, max, name, v, strlen(v)); }
                    break;
                case FC_IMAGE:
                    if (c == submitter) { static char nm[200]; ksnprintf(nm, sizeof nm, "%s.x", name); o = add_pair(out, o, max, nm, "1", 1); ksnprintf(nm, sizeof nm, "%s.y", name); o = add_pair(out, o, max, nm, "1", 1); }
                    break;
                }
            }
        }
        o = collect_form(c, form, submitter, out, o, max, depth + 1);
    }
    return o;
}
static void submit(struct node *form, struct node *submitter) {
    if (!doc) return;
    if (!form && submitter) form = submitter->form;
    if (!form) return;
    if (js_event(form, "submit", 0, 0, 0)) return;          /* a script handles it */
    const char *action = submitter && node_attr(submitter, "formaction") ? node_attr(submitter, "formaction") : node_attr(form, "action");
    const char *method = submitter && node_attr(submitter, "formmethod") ? node_attr(submitter, "formmethod") : node_attr(form, "method");
    char target[600];
    resolve_url(doc->base, action && action[0] ? action : url, target, sizeof target);
    if (!action || !action[0]) strncpy0(target, url, sizeof target);
    long max = 256 * 1024;
    char *body = malloc(max);
    if (!body) return;
    long n = collect_form(doc->root, form, submitter, body, 0, max, 0);
    body[n] = 0;
    if (method && pre_ci(method, "post")) { request_nav(target, true, body, n, 0, 0); free(body); return; }
    char u[1200]; strip_fragment(target, u, sizeof u);
    for (char *q = u; *q; q++) if (*q == '?') { *q = 0; break; }
    int l = strlen(u);
    ksnprintf(u + l, sizeof u - l, "?%s", body);
    free(body);
    if (strlen(u) >= 599) { strncpy0(status_text, "That form's answers are too long to send in an address.", sizeof status_text); draw(); return; }
    request_nav(u, false, 0, 0, 0, 0);
}

/* controls in document order, for Tab */
static struct node *tab_next(struct node *e, struct node *after, bool *seen, int depth) {
    for (struct node *c = e->first; c && depth < 200; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->fc && c->st && c->st->display != D_NONE && !c->fc->disabled && c->fc->type != FC_HIDDEN) {
            if (*seen || !after) return c;
            if (c == after) *seen = true;
        }
        if (c->st && c->st->display == D_NONE) continue;
        struct node *r = tab_next(c, after, seen, depth + 1);
        if (r) return r;
    }
    return 0;
}

static int ctl_item(struct node *n) { for (int i = 0; i < lay.nit; i++) if (lay.it[i].kind == DI_CTRL && lay.it[i].n == n) return i; return -1; }
static void ensure_visible(struct node *n) {
    int i = ctl_item(n);
    if (i < 0) return;
    int view = WH - BAR_H - STATUS_H;
    if (lay.it[i].y < scroll + 10) scroll = lay.it[i].y - 40;
    else if (lay.it[i].y + lay.it[i].h > scroll + view - 10) scroll = lay.it[i].y + lay.it[i].h - view + 40;
    if (scroll < 0) scroll = 0;
}

static void ins_text(struct fctl *fc, const char *s, long n, bool multiline) {
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c == '\n' && !multiline) break;
        if (c != '\n' && !key_is_char(c)) continue;
        if (fc->type == FC_NUMBER && !((c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+' || c == 'e')) continue;
        if (fc->len >= fc->cap - 1) break;
        memmove(fc->val + fc->cur + 1, fc->val + fc->cur, fc->len - fc->cur + 1);
        fc->val[fc->cur++] = (char)c; fc->len++;
    }
    fc->val[fc->len] = 0;
}

static void control_key(int k) {
    struct fctl *fc = focus->fc;
    int code = KEY_CODE(k);
    if (k == 27) { focus = 0; return; }
    if (fc->type == FC_MEDIA && k != '\t') { if (k == ' ' || k == '\n') media_toggle(focus); return; }
    if (k == '\t') { bool seen = false; struct node *nx = tab_next(doc->root, focus, &seen, 0); if (!nx) { seen = false; nx = tab_next(doc->root, 0, &seen, 0); } focus = nx; if (nx) { if (nx->fc) nx->fc->cur = nx->fc->len; ensure_visible(nx); } return; }
    if (fc->type == FC_CHECKBOX || fc->type == FC_RADIO) {
        if (k == ' ') {
            if (fc->type == FC_CHECKBOX) fc->checked = !fc->checked;
            else { for (int i = 0; i < lay.nit; i++) { struct node *o = lay.it[i].n; if (lay.it[i].kind == DI_CTRL && o->fc && o->fc->type == FC_RADIO && o->form == focus->form && o != focus) { const char *a1 = node_attr(o, "name"), *a2 = node_attr(focus, "name"); if (a1 && a2 && !strcmp(a1, a2)) o->fc->checked = false; } } fc->checked = true; }
        } else if (k == '\n') submit(focus->form, 0);
        return;
    }
    if (fc->type == FC_SUBMIT || fc->type == FC_IMAGE) { if (k == '\n' || k == ' ') submit(focus->form, focus); return; }
    if (fc->type == FC_RESET || fc->type == FC_BUTTON || fc->type == FC_FILE) return;
    if (fc->type == FC_SELECT) {
        if (code == KEY_DOWN && fc->sel + 1 < fc->nopt) fc->sel++;
        else if (code == KEY_UP && fc->sel > 0) fc->sel--;
        else if (k == '\n' || k == ' ') { popup = focus; popup_hover = fc->sel; popup_first = fc->sel > 6 ? fc->sel - 6 : 0; }
        return;
    }
    if (fc->readonly && k != 3) return;
    bool multi = fc->type == FC_TEXTAREA;
    if (k == '\n') { if (multi) ins_text(fc, "\n", 1, true); else submit(focus->form, 0); return; }
    if (k == '\b') { if (fc->cur > 0) { memmove(fc->val + fc->cur - 1, fc->val + fc->cur, fc->len - fc->cur + 1); fc->cur--; fc->len--; } return; }
    if (code == KEY_DEL) { if (fc->cur < fc->len) { memmove(fc->val + fc->cur, fc->val + fc->cur + 1, fc->len - fc->cur); fc->len--; } return; }
    if (code == KEY_LEFT) { if (fc->cur > 0) fc->cur--; return; }
    if (code == KEY_RIGHT) { if (fc->cur < fc->len) fc->cur++; return; }
    if (code == KEY_HOME) { if (multi) { while (fc->cur > 0 && fc->val[fc->cur - 1] != '\n') fc->cur--; } else fc->cur = 0; return; }
    if (code == KEY_END) { if (multi) { while (fc->cur < fc->len && fc->val[fc->cur] != '\n') fc->cur++; } else fc->cur = fc->len; return; }
    if (multi && (code == KEY_UP || code == KEY_DOWN)) {
        int ls = fc->cur; while (ls > 0 && fc->val[ls - 1] != '\n') ls--;
        int col = fc->cur - ls;
        if (code == KEY_UP) { if (!ls) return; int ps = ls - 1; while (ps > 0 && fc->val[ps - 1] != '\n') ps--; int plen = ls - 1 - ps; fc->cur = ps + (col < plen ? col : plen); }
        else { int le = fc->cur; while (le < fc->len && fc->val[le] != '\n') le++; if (le >= fc->len) return; int ns = le + 1, ne = ns; while (ne < fc->len && fc->val[ne] != '\n') ne++; fc->cur = ns + (col < ne - ns ? col : ne - ns); }
        return;
    }
    if (k == 22) {                                         /* Ctrl+V */
        static char b[16384];
        long n = clip_get(b, sizeof b - 1);
        if (n > 0) { n = utf8_to_cp1252(b, n); ins_text(fc, b, n, multi); }
        return;
    }
    if (k == 3) { if (fc->type != FC_PASSWORD) clip_set(fc->val, fc->len); return; }   /* Ctrl+C: the whole field */
    if (k == 1) { fc->cur = fc->len; return; }
    if (key_is_char(k)) { char c = (char)k; ins_text(fc, &c, 1, multi); }
}

/* ---------------- pages ---------------- */
static void free_doc(struct doc *d) { if (!d) return; ar_reset(&d->ar); free(d); }

int add_link(const char *href, struct node *n) {
    if (nlinks >= links_cap) {
        int nc = links_cap ? links_cap * 2 : 256;
        if (nc > 40000) return -1;
        struct link *l = realloc(links, nc * sizeof *l);
        if (!l) return -1;
        links = l; links_cap = nc;
    }
    links[nlinks].href = href; links[nlinks].n = n;
    return nlinks++;
}

static void init_node(struct doc *d, struct node *c) {
        if ((c->tag == T_A || c->tag == T_AREA) && node_attr(c, "href")) {
            static char u[600]; resolve_url(d->base, node_attr(c, "href"), u, sizeof u);
            char *s = ar_strndup(&d->ar, u, strlen(u));
            if (s) c->link = add_link(s, c);
        }
        if (c->tag == T_INPUT || c->tag == T_TEXTAREA || c->tag == T_SELECT || c->tag == T_BUTTON) form_init_control(d, c);
        if (c->tag == T_VIDEO || c->tag == T_AUDIO) media_attach(d, c);
        if (c->tag == T_IMG) {
            const char *src = node_attr(c, "src");
            const char *lazy = node_attr(c, "data-src"); if (!lazy) lazy = node_attr(c, "data-original");
            if (lazy && (!src || !src[0] || pre_ci(src, "data:"))) src = lazy;
            static char first[600];
            if (!src || !src[0]) {                       /* srcset only: take its first candidate */
                const char *ss = node_attr(c, "srcset");
                if (ss) { int k = 0; while (*ss == ' ') ss++; while (*ss && *ss != ' ' && *ss != ',' && k < 599) first[k++] = *ss++; first[k] = 0; src = first; }
            }
            if (src && src[0]) { static char u[600]; resolve_url(d->base, src, u, sizeof u); c->img = img_get(u); }
        }
}
static void walk_init(struct doc *d, struct node *e, int depth) {
    if (depth > 200) return;
    if (e->shadow) walk_init(d, e->shadow, depth + 1);
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        init_node(d, c);
        walk_init(d, c, depth + 1);
    }
}

/* <style> and <link rel=stylesheet>, in document order */
static int nsheets;
/* a shadow root's <style>: scoped to its host's tag, and each different one only once (every instance of a
 * component carries the same sheet) */
static uint32_t scoped_seen[512]; static int nscoped;
static void add_style_node(struct doc *d, struct node *c) {
    c->css_done = 1;
    char *b = 0; long len = 0, cap = 0;
    if (c->text && !c->first) { b = malloc(c->tlen + 1); if (b) { memcpy(b, c->text, c->tlen); b[c->tlen] = 0; len = c->tlen; } }
    else {                                              /* its text children */
        for (struct node *t = c->first; t; t = t->next) if (t->type == N_TEXT) cap += t->tlen;
        b = malloc(cap + 1);
        if (b) { for (struct node *t = c->first; t; t = t->next) if (t->type == N_TEXT) { memcpy(b + len, t->text, t->tlen); len += t->tlen; } b[len] = 0; }
    }
    if (!b || !len) { free(b); return; }
    struct node *h = css_tree_host(c);
    if (h && h->name) {
        uint32_t hash = 2166136261u;
        for (const char *q = h->name; *q; q++) hash = (hash ^ (uint8_t)*q) * 16777619u;
        for (long i = 0; i < len; i++) hash = (hash ^ (uint8_t)b[i]) * 16777619u;
        for (int i = 0; i < nscoped; i++) if (scoped_seen[i] == hash) { free(b); return; }
        if (nscoped < 512) scoped_seen[nscoped++] = hash;
        long sl; char *sc = css_scope_shadow(b, len, h->name, &sl);
        free(b); b = sc; len = sl;
        if (!b) return;
    }
    char *copy = ar_strndup(&d->ar, b, len); free(b);
    if (copy) css_add_sheet(copy, len, d->base, 0);
}
static void walk_styles(struct doc *d, struct node *e, int depth) {
    if (depth > 200) return;
    for (struct node *c = e->first; c && !abort_fetch && !quit && !pend; c = c->next) {
        if (c->type != N_ELEM) continue;
        const char *media = node_attr(c, "media");
        bool ok = !media || css_media_matches(media, strlen(media), WW - SCROLLBAR);
        if (c->tag == T_STYLE) { if (ok) add_style_node(d, c); else c->css_done = 1; continue; }
        if (c->tag == T_LINK && ok && nsheets < 24) {
            const char *rel = node_attr(c, "rel"), *href = node_attr(c, "href");
            bool sheet = false;
            if (rel) for (const char *q = rel; *q; q++) if (pre_ci(q, "stylesheet")) sheet = true;
            if (rel && pre_ci(rel, "alternate")) sheet = false;
            c->css_done = 1;
            if (sheet && href) {
                static char u[600]; resolve_url(d->base, href, u, sizeof u);
                ksnprintf(status_text, sizeof status_text, "Loading style sheet %s", u);
                draw();
                long n; char *t = web_fetch_text(u, &n, MAXCSS);
                nsheets++;
                if (t) { char *copy = ar_strndup(&d->ar, t, n); free(t); if (copy) css_add_sheet(copy, n, u, 0); }
            }
        }
        if (c->shadow) walk_styles(d, c->shadow, depth + 1);
        walk_styles(d, c, depth + 1);
    }
}

static void find_charset(const char *s, long n, char *cs, int max) {
    long lim = n < 4096 ? n : 4096;
    for (long i = 0; i + 8 < lim; i++) {
        if (pre_ci(s + i, "charset=")) {
            long j = i + 8; while (j < lim && (s[j] == '"' || s[j] == '\'' || s[j] == ' ')) j++;
            int k = 0; while (j < lim && k < max - 1 && s[j] != '"' && s[j] != '\'' && s[j] != ' ' && s[j] != '>' && s[j] != ';' && s[j] != '/') cs[k++] = lc(s[j++]);
            cs[k] = 0;
            return;
        }
    }
}

/* window titles are drawn with the 8x16 font by the desktop, which shows only ASCII there: spell accents out */
static void title_ascii(const char *in, char *out, int max) {
    static const char hi[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    int k = 0;
    for (; *in && k < max - 1; in++) {
        unsigned c = (unsigned char)*in;
        if (c >= 0xC0) c = hi[c - 0xC0];
        else if (c >= 0x80) c = c == 0x80 ? 'E' : c == 0x91 || c == 0x92 ? '\'' : c == 0x93 || c == 0x94 ? '"' : c == 0x96 || c == 0x97 ? '-' :
                                c == 0x85 ? '.' : c == 0xA0 ? ' ' : c == 0xAB ? '<' : c == 0xBB ? '>' : c == 0x95 || c == 0xB7 ? '*' : '?';
        out[k++] = (char)c;
    }
    out[k] = 0;
}

static void set_title(void) {
    char t[64];
    char tbuf[160]; title_ascii(doc && doc->title[0] ? doc->title : url, tbuf, sizeof tbuf);
    const char *tt = tbuf;
    int n = strlen(tt);
    if (n > 50) { char s[48]; memcpy(s, tt, 47); s[47] = 0; ksnprintf(t, sizeof t, "%s... - Arc", s); }
    else ksnprintf(t, sizeof t, "%s - Arc", tt);
    app_title(&a, t);
}

static int page_w(void) { return WW - SCROLLBAR > 120 ? WW - SCROLLBAR : 120; }
static bool styles_scan, styles_reload;
static const bool web_load_late_links = false;
static void load_styles(struct doc *d);
static void scan_new_styles(struct doc *d, struct node *e, int depth) {     /* <style>/<link> that scripts added */
    if (depth > 200) return;
    if (e->shadow) scan_new_styles(d, e->shadow, depth + 1);
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_STYLE) { if (!c->css_done) { const char *m = node_attr(c, "media"); if (!m || css_media_matches(m, strlen(m), WW - SCROLLBAR)) add_style_node(d, c); else c->css_done = 1; } continue; }
        /* style sheets linked by scripts later (lazy-loaded parts of a site) are left out for now: they are
         * written for layout features Web doesn't have yet and made pages like npr.org worse, not better */
        if (c->tag == T_LINK && !c->css_done && !web_load_late_links) { c->css_done = 1; continue; }
        if (c->tag == T_LINK && !c->css_done) {
            c->css_done = 1;
            const char *rel = node_attr(c, "rel"), *href = node_attr(c, "href");
            bool sheet = false;
            if (rel) for (const char *q = rel; *q; q++) if (pre_ci(q, "stylesheet")) sheet = true;
            if (sheet && href && nsheets < 40) {
                static char u[600]; resolve_url(d->base, href, u, sizeof u);
                long n; char *t = web_fetch_text(u, &n, MAXCSS); nsheets++;
                if (t) { char *copy = ar_strndup(&d->ar, t, n); free(t); if (copy) css_add_sheet(copy, n, u, 0); }
            }
            continue;
        }
        scan_new_styles(d, c, depth + 1);
    }
}
static void relayout(void) {
    if (!doc) return;
    if (styles_reload && !styles_loading) { styles_reload = styles_scan = false; load_styles(doc); need_cascade = true; }
    if (styles_scan && !styles_loading) { styles_scan = false; scan_new_styles(doc, doc->root, 0); }
    if (need_cascade && !styles_loading) { css_cascade(doc, page_w()); need_cascade = false; }
    layout_doc(doc, &lay, page_w());
    need_layout = false;
    last_layout_t = sys_time();
}
static int view_h(void) { return WH - BAR_H - STATUS_H > 20 ? WH - BAR_H - STATUS_H : 20; }
static void scroll_by(int d) {
    scroll += d;
    int mx = lay.doc_h - view_h();
    if (scroll > mx) scroll = mx;
    if (scroll < 0) scroll = 0;
}
static struct node *find_id(struct node *e, const char *id, int depth) {
    if (depth > 200) return 0;
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if ((c->id && !strcmp(c->id, id)) || (c->tag == T_A && node_attr(c, "name") && !strcmp(node_attr(c, "name"), id))) return c;
        struct node *r = find_id(c, id, depth + 1);
        if (r) return r;
    }
    return 0;
}
static void scroll_to_fragment(const char *f) {
    if (!doc || !f[0]) return;
    char id[200]; int k = 0;
    for (; *f && k < 199; f++) { int h; if (*f == '%' && f[1] && f[2] && (h = hex2(f + 1)) >= 0) { id[k++] = (char)h; f += 2; } else id[k++] = *f; }
    id[k] = 0;
    struct node *n = find_id(doc->root, id, 0);
    if (n) { scroll = n->y - 8; scroll_by(0); }
}

/* replace the shown document */
static void show_doc(struct doc *d) {
    struct doc *old = doc;
    js_page_end();                                         /* the old page's scripts stop */
    media_page_end();                                      /* and its videos and sounds */
    doc = d; web_doc = d;
    free_doc(old);
    nlinks = 0; hover_link = -1; focus = 0; popup = 0;
    for (struct imgent *e = images; e; e = e->next) e->used = false;
    walk_init(d, d->root, 0);
    img_evict();
    need_cascade = true;
    relayout();
    set_title();
    if (d->root) {                                         /* then run its scripts (they may change it) */
        need_draw = true;
        js_page_start(d);                                  /* starts them; Web's main loop runs them one by one */
        if (need_layout || need_cascade) relayout();
    }
}

static struct doc *parse_page(char *s, long n, const char *ctype, const char *base) {
    struct doc *d = calloc(1, sizeof *d);
    if (!d) return 0;
    /* text encoding: UTF-8 unless the page says otherwise */
    char cs[32] = "";
    for (const char *c = ctype; c && *c; c++) if (pre_ci(c, "charset=")) {
        int k = 0; c += 8; while (*c == '"' || *c == ' ') c++;
        while (*c && *c != ';' && *c != ' ' && *c != '"' && k < 31) cs[k++] = lc(*c++);
        cs[k] = 0; break;
    }
    if (!cs[0]) find_charset(s, n, cs, sizeof cs);
    bool latin = pre_ci(cs, "iso-8859-1") || pre_ci(cs, "iso8859-1") || pre_ci(cs, "windows-1252") || pre_ci(cs, "latin1") ||
                 pre_ci(cs, "iso-8859-15") || pre_ci(cs, "us-ascii") || pre_ci(cs, "cp1252") || pre_ci(cs, "ascii");
    if (pre_ci(cs, "utf-8") || pre_ci(cs, "utf8")) n = utf8_lenient_to_cp1252(s, n);
    else if (!latin) n = utf8_to_cp1252(s, n);
    strncpy0(d->charset, cs[0] ? cs : "(not given)", sizeof d->charset);
    strncpy0(d->base, base, sizeof d->base);
    html_parse(d, s, n);
    for (struct node *e = d->head ? d->head->first : 0; e; e = e->next)          /* <base href> */
        if (e->type == N_ELEM && e->tag == T_BASE && node_attr(e, "href")) { char b[600]; resolve_url(base, node_attr(e, "href"), b, sizeof b); strncpy0(d->base, b, sizeof d->base); break; }
    return d;
}

static void load_styles(struct doc *d) {
    css_reset(&d->ar);
    nsheets = 0; nscoped = 0;
    styles_loading = true;
    walk_styles(d, d->root, 0);
    styles_loading = false;
}

static const char PAGE_CSS[] =
    "<style>html{background:#F4F1EA}body{color:#23201B;margin:0;padding:24px 40px;line-height:1.45}"
    "h1{font-size:36px;font-weight:normal;color:#1B2945;margin:4px 0 12px}h2{font-size:20px;color:#1B2945;margin:20px 0 6px}"
    "a{color:#1A6FC4}.card{background:#FFFFFF;border:1px solid #DDD6C8;padding:2px 18px;margin:12px 0}"
    "code{background:#E8E3D8;padding:0 3px}.dim{color:#7A7468}li{margin:3px 0}</style>";

static void html_doc(const char *h, const char *base) {
    char *b = malloc(strlen(h) + 1);
    if (!b) return;
    memcpy(b, h, strlen(h) + 1);
    struct doc *d = parse_page(b, strlen(b), "text/html; charset=windows-1252", base);
    free(b);
    if (!d) return;
    load_styles(d);
    show_doc(d);
}

static void message_doc(const char *head, const char *body) {
    static char h[4096];
    ksnprintf(h, sizeof h, "<title>%s</title>%s<h1>%s</h1><div class=card><p>%s</p></div><p><a href=\"about:home\">Go to the start page</a></p>",
              head, PAGE_CSS, head, body);
    html_doc(h, "about:blank");
}

static void about_page(const char *u) {
    static char h[16384];
    if (pre_ci(u, "about:unsupported")) {
        message_doc("Not supported", "That kind of link (email, telephone or script) doesn't work in Arc.");
        strncpy0(status_text, "Not supported", sizeof status_text);
        return;
    }
    if (pre_ci(u, "about:cookies")) {
        bool clear = false; for (const char *q = u; *q; q++) if (pre_ci(q, "?clear")) clear = true;
        if (clear) http_cookies_clear();
        static char list[8192]; int n = http_cookie_list(list, sizeof list);
        int o = ksnprintf(h, sizeof h, "<title>Cookies</title>%s<h1>Cookies</h1><p>Arc keeps %d cookie%s. Cookies that don't end when Arc closes are saved in "
                          "<code>/home/.web/cookies</code>.</p><div class=card><pre>", PAGE_CSS, n, n == 1 ? "" : "s");
        for (char *q = list; *q && o < (int)sizeof h - 400; q++) {
            if (*q == '<') o += ksnprintf(h + o, sizeof h - o, "&lt;"); else if (*q == '&') o += ksnprintf(h + o, sizeof h - o, "&amp;"); else h[o++] = *q;
        }
        ksnprintf(h + o, sizeof h - o, "</pre></div><p><a href=\"about:cookies?clear\">Delete all cookies</a> - <a href=\"about:home\">Start page</a></p>");
        strncpy0(status_text, "Cookies", sizeof status_text);
    } else if (pre_ci(u, "about:console")) {
        static char lines[16384]; js_console_lines(lines, sizeof lines);
        int o = ksnprintf(h, sizeof h, "<title>Console</title>%s<h1>Console</h1><p>Messages and errors from the last page's scripts.</p><div class=card><pre>", PAGE_CSS);
        for (char *q = lines; *q && o < (int)sizeof h - 200; q++) {
            if (*q == '<') o += ksnprintf(h + o, sizeof h - o, "&lt;"); else if (*q == '&') o += ksnprintf(h + o, sizeof h - o, "&amp;"); else h[o++] = *q;
        }
        ksnprintf(h + o, sizeof h - o, "</pre></div>");
        strncpy0(status_text, "Console", sizeof status_text);
    } else if (pre_ci(u, "about:blank")) {
        strncpy0(h, "<title>Blank</title>", sizeof h);
        status_text[0] = 0;
    } else {
        ksnprintf(h, sizeof h, "<title>Start page</title>%s"
            "<h1>Arc <span class=dim style=\"font-size:22px\">0.4</span></h1>"
            "<p>A browser for <b>AXIOM</b>, running on the Axiom Core kernel's own TCP/IP stack. Secure <b>https://</b> sites use TLS 1.3, "
            "and every site's certificate is checked against the trusted authorities.</p>"
            "<div class=card><h2>Try</h2><ul>"
            "<li><a href=\"https://text.npr.org/\">text.npr.org</a> - news in plain text</li>"
            "<li><a href=\"https://lite.duckduckgo.com/lite/\">lite.duckduckgo.com</a> - web search, lite edition</li>"
            "<li><a href=\"https://news.ycombinator.com/\">news.ycombinator.com</a> - Hacker News</li>"
            "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a></li>"
            "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> - the world's first website (1991)</li></ul></div>"
            "<h2>What Arc can show</h2><p>Text with style sheets (CSS), pictures (PNG, JPEG, GIF and BMP), tables, lists, forms, and video and sound (MP4, MP3, WAV). "
            "Sites can keep you signed in with cookies (<a href=\"about:cookies\">see cookies</a>). "
            "Open files on this computer with addresses like <code>file:///home/page.html</code>.</p>"
            "<h2>Not shown</h2><p class=dim>Web fonts, WebP pictures, streamed video (YouTube) and floating layouts; JavaScript runs, but big web apps need more than Web has (see <a href=\"about:console\">the console</a> for script errors). Sites that only allow TLS 1.2 or older can't be opened yet.</p>"
            "<p class=dim>Keys: Ctrl+L address bar, Backspace or Alt+Left back, Alt+Right forward, F5 reload, Tab next form field, Esc stop.</p>", PAGE_CSS);
        strncpy0(status_text, "Start page", sizeof status_text);
    }
    html_doc(h, "about:home");
}

/* a text file shown as it is */
static void text_doc(char *s, long n, const char *base) {
    n = utf8_to_cp1252(s, n);
    struct doc *d = calloc(1, sizeof *d);
    if (!d) return;
    static const char shell[] = "<html><body style=\"margin:10px\"><pre style=\"white-space:pre-wrap\"></pre></body></html>";
    strncpy0(d->base, base, sizeof d->base);
    html_parse(d, shell, sizeof shell - 1);
    struct node *pre = d->body ? d->body->first : 0;
    while (pre && pre->tag != T_PRE) pre = pre->next;
    if (pre) {
        struct node *t = ar_alloc(&d->ar, sizeof *t);
        char *txt = ar_strndup(&d->ar, s, n);
        if (t && txt) { t->type = N_TEXT; t->link = -1; t->text = txt; t->tlen = n; t->parent = pre; t->depth = pre->depth + 1; pre->first = pre->last = t; }
    }
    const char *nm = base + strlen(base); while (nm > base && nm[-1] != '/') nm--;
    strncpy0(d->title, nm, sizeof d->title);
    load_styles(d);
    show_doc(d);
}

/* ---------------- navigation ---------------- */
static void push_history(void) {
    if (hpos >= 0 && !strcmp(hist[hpos].url, url)) return;
    if (hpos + 1 >= MAXHIST) { memmove(&hist[0], &hist[1], sizeof hist[0] * (MAXHIST - 1)); hpos--; }
    hpos++;
    strncpy0(hist[hpos].url, url, sizeof hist[hpos].url);
    hist[hpos].scroll = 0;
    hcount = hpos + 1;
}

/* mode: 0 a new page (history entry), 1 reload / back / forward (no new entry; restore: scroll position) */
static void navigate(const char *target, bool post, const char *body, long blen, int mode, int restore) {
    stuck_count = 0;
    cache_bypass = mode == 1;                              /* reload: everything fresh from the network */
    char u[600]; strncpy0(u, target, sizeof u);
    if (hpos >= 0) hist[hpos].scroll = scroll;
    addr_focus = false; popup = 0;
    /* a link to another place on the same page */
    char a1[600], a2[600]; strip_fragment(u, a1, sizeof a1); strip_fragment(url, a2, sizeof a2);
    if (!post && doc && mode == 0 && fragment_of(u)[0] && !strcmp(a1, a2)) {
        strncpy0(url, u, sizeof url);
        push_history();
        scroll_to_fragment(fragment_of(u));
        draw();
        return;
    }
    abort_fetch = false; images_stopped = false;
    if (pre_ci(u, "about:")) {
        memset(&sec, 0, sizeof sec);
        strncpy0(url, u, sizeof url);
        about_page(u);
        scroll = 0;
    } else {
        loading = true;
        ksnprintf(status_text, sizeof status_text, "Loading %s ...", u);
        draw();
        char ctype[96], final[600]; int status = 0; char *b = 0;
        long n = fetch_any(u, MAXPAGE, &b, ctype, sizeof ctype, &status, final, sizeof final, post ? "POST" : "GET", body, blen);
        loading = false;
        if (quit) { free(b); return; }
        if (pend || (abort_fetch && n <= 0)) {           /* stopped, or another page was asked for */
            free(b);
            strncpy0(status_text, pend ? "" : "Stopped", sizeof status_text);
            if (!pend) draw();
            return;
        }
        if (n < 0 || !b) {
            free(b);
            memset(&sec, 0, sizeof sec);
            strncpy0(url, u, sizeof url);
            char m[700];
            if (n <= -40 && n >= -68) {
                ksnprintf(m, sizeof m, "AXIOM couldn't set up a secure connection to <b>%s</b>: %s.</p><p>This protects you from impostor sites."
                          " If the computer's clock is wrong, fix it and try again.", u, http_strerror(n) + 26);
                message_doc("Connection isn't secure", m);
                strncpy0(status_text, "Not secure: the connection was stopped", sizeof status_text);
            } else if (n == ENOFILE) {
                ksnprintf(m, sizeof m, "There's no file at <b>%s</b>.", u);
                message_doc("File not found", m);
                strncpy0(status_text, u, sizeof status_text);
            } else {
                ksnprintf(m, sizeof m, "Couldn't open %s: %s.", u, http_strerror(n));
                message_doc("Can't reach this page", m);
                strncpy0(status_text, http_strerror(n), sizeof status_text);
            }
        } else {
            bool web = pre_ci(u, "http");
            if (web) sec = http_sec; else memset(&sec, 0, sizeof sec);
            const char *fr = fragment_of(u);
            if (fr[0] && !fragment_of(final)[0]) ksnprintf(url, sizeof url, "%s#%s", final, fr); else strncpy0(url, final, sizeof url);
            const char *ct = ctype;
            while (*ct == ' ') ct++;
            if (pre_ci(ct, "image/")) {
                struct imgent *e = img_get(final);
                if (e && e->state != IS_OK) { if (decode_into(e, (const uint8_t *)b, n)) e->state = IS_OK; else e->state = IS_FAILED; }
                free(b);
                static char h[1400];
                const char *nm = final + strlen(final); while (nm > final && nm[-1] != '/') nm--;
                ksnprintf(h, sizeof h, "<title>%s</title><body style=\"margin:0;background:#2A2A2A;text-align:center\"><img style=\"margin-top:12px\" src=\"%s\" alt=\"%s\">", nm, final, nm);
                html_doc(h, final);
            } else if (!ct[0] || pre_ci(ct, "text/html") || pre_ci(ct, "application/xhtml")) {
                b[n] = 0;
                struct doc *d = parse_page(b, n, ct, url);
                free(b);
                if (d) {
                    void *saved = css_save();
                    load_styles(d);
                    if (pend || quit) { free_doc(d); css_restore(saved); return; }   /* the old page stays */
                    free(saved);
                    show_doc(d);
                }
            } else if (pre_ci(ct, "text/") || pre_ci(ct, "application/json") || pre_ci(ct, "application/javascript") || pre_ci(ct, "application/xml")) {
                text_doc(b, n, url);
                free(b);
            } else {
                free(b);
                message_doc("Can't show this file", "This address leads to a file Arc can't show (it might be a download). Use <b>wget</b> in the terminal to save it.");
            }
            if (sec.secure) ksnprintf(status_text, sizeof status_text, "Secure: %s - certificate for %s, from %s", sec.cipher, sec.subject, sec.issuer);
            else if (web) ksnprintf(status_text, sizeof status_text, "Not secure (http) - HTTP %d - %ld bytes", status, n);
            else ksnprintf(status_text, sizeof status_text, "%s - %ld bytes", url, n);
            if (web && status >= 400 && doc && !doc->title[0]) { char t[40]; ksnprintf(t, sizeof t, "HTTP %d", status); strncpy0(doc->title, t, sizeof doc->title); set_title(); }
        }
        scroll = 0;
    }
    if (mode == 0) push_history();
    strncpy0(page_status, status_text, sizeof page_status);
    if (restore > 0) { scroll = restore; scroll_by(0); }
    else if (fragment_of(url)[0]) scroll_to_fragment(fragment_of(url));
    set_title();
    draw();
}

static void request_nav(const char *u, bool post, const char *body, long blen, int mode, int restore) {
    if (fetch_depth || loading || styles_loading) {
        pend = true; strncpy0(pend_url, u, sizeof pend_url);
        free(pend_body); pend_body = 0; pend_blen = 0; pend_post = post; pend_mode = mode; pend_scroll = restore;
        if (post && body) { pend_body = malloc(blen + 1); if (pend_body) { memcpy(pend_body, body, blen); pend_blen = blen; } }
        abort_fetch = true;
        return;
    }
    navigate(u, post, body, blen, mode, restore);
}

static void go_back(void) { if (hpos > 0) { hist[hpos].scroll = scroll; hpos--; request_nav(hist[hpos].url, false, 0, 0, 1, hist[hpos].scroll); } }
static void go_forward(void) { if (hpos + 1 < hcount) { hist[hpos].scroll = scroll; hpos++; request_nav(hist[hpos].url, false, 0, 0, 1, hist[hpos].scroll); } }
static void reload(void) { request_nav(url, false, 0, 0, 1, scroll); }

static void open_typed(const char *t) {
    char u[600];
    while (*t == ' ') t++;
    if (t[0] == '/') ksnprintf(u, sizeof u, "file://%s", t);
    else if (pre_ci(t, "mailto:") || pre_ci(t, "javascript:") || pre_ci(t, "tel:")) strncpy0(u, "about:unsupported", sizeof u);
    else if (scheme_len(t) && (pre_ci(t, "http") || pre_ci(t, "about:") || pre_ci(t, "file:") || pre_ci(t, "data:"))) strncpy0(u, t, sizeof u);
    else ksnprintf(u, sizeof u, "https://%s", t);
    request_nav(u, false, 0, 0, 0, 0);
}

/* ---------------- drawing ---------------- */
static void draw_text(struct canvas *c, const struct font *f, int x, int base, uint32_t col, const char *s, int n) {
    for (int k = 0; k < n; k++) {
        unsigned ch = (unsigned char)s[k];
        if (ch < 32 || ch == 127) ch = '?';
        const struct glyph *g = &f->g[ch - 32];
        int gx = x + g->xoff, gy = base + g->yoff;
        if (gx >= c->cx1) break;
        if (gx + g->w > c->cx0 && gy + g->h > c->cy0 && gy < c->cy1) {
            const uint8_t *b = f->bits + g->off;
            for (int j = 0; j < g->h; j++) {
                int py = gy + j;
                if (py < c->cy0 || py >= c->cy1) continue;
                uint32_t *p = c->px + py * c->stride;
                for (int i = 0; i < g->w; i++) {
                    int px = gx + i, al = b[j * g->w + i];
                    if (!al || px < c->cx0 || px >= c->cx1) continue;
                    p[px] = al == 255 ? col : gfx_mix(p[px], col, al);
                }
            }
        }
        x += g->adv;
    }
}
static int text_width(const struct font *f, const char *s, int n) { int w = 0; for (int i = 0; i < n; i++) { unsigned ch = (unsigned char)s[i]; if (ch < 32 || ch == 127) ch = '?'; w += f->g[ch - 32].adv; } return w; }

static void blit_img(struct canvas *c, const uint32_t *px, int w, int h, int x, int y, bool opaque) {
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + w > c->cx1 ? c->cx1 : x + w;
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        const uint32_t *s = px + (long)(j - y) * w;
        uint32_t *d = c->px + j * c->stride;
        for (int i = x0; i < x1; i++) {
            uint32_t p = s[i - x], al = p >> 24;
            if (opaque || al == 255) d[i] = p & 0xFFFFFF;
            else if (al) d[i] = gfx_mix(d[i], p & 0xFFFFFF, al);
        }
    }
}

static void draw_image(struct canvas *c, struct ditem *it, int x, int y) {
    struct imgent *e = it->n->img;
    if (e && e->state == IS_OK && e->px && it->w > 0 && it->h > 0) {
        const uint32_t *px = e->px;
        if (e->pw != it->w || e->ph != it->h) {
            if (!(e->spx && e->sw == it->w && e->sh == it->h)) {
                if (e->spx) img_bytes -= (long)e->sw * e->sh * 4;
                free(e->spx); e->spx = 0;
                if ((long)it->w * it->h <= 8l * 1024 * 1024) { e->spx = scale_px(e->px, e->pw, e->ph, it->w, it->h); e->sw = it->w; e->sh = it->h; if (e->spx) img_bytes += (long)it->w * it->h * 4; }
            }
            px = e->spx;
        }
        if (px) { blit_img(c, px, it->w, it->h, x, y, e->opaque); return; }
    }
    /* still loading, or failed: a frame with the alt text */
    if (it->w < 2 || it->h < 2) return;
    const char *alt = node_attr(it->n, "alt");
    bool failed = !e || e->state == IS_FAILED;
    gfx_rrect_outline(c, x, y, it->w, it->h, 2, failed ? 0xC0B8B0 : 0xD0D0D0, 255);
    int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
    int nx0 = x + 1 > cx0 ? x + 1 : cx0, ny0 = y + 1 > cy0 ? y + 1 : cy0, nx1 = x + it->w - 1 < cx1 ? x + it->w - 1 : cx1, ny1 = y + it->h - 1 < cy1 ? y + it->h - 1 : cy1;
    if (nx1 > nx0 && ny1 > ny0) {
        c->cx0 = nx0; c->cy0 = ny0; c->cx1 = nx1; c->cy1 = ny1;
        /* a small picture sign, then the alt text */
        int ix = x + 5, iy = y + (it->h > 20 ? 5 : 2);
        gfx_rrect_outline(c, ix, iy, 12, 10, 1, 0x9A9A9A, 255);
        gfx_line(c, ix + 2, iy + 8, ix + 5, iy + 4, 0x9A9A9A, 1); gfx_line(c, ix + 5, iy + 4, ix + 9, iy + 8, 0x9A9A9A, 1);
        if (alt && alt[0]) { const struct font *f = web_font(13, false, false); draw_text(c, f, x + 21, iy + 10, 0x6A6460, alt, strlen(alt)); }
        c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
    }
}

static void draw_control(struct canvas *c, struct ditem *it, int x, int y) {
    struct node *n = it->n; struct fctl *fc = n->fc;
    if (!fc) return;
    if (fc->type == FC_MEDIA) { media_draw(c, n, x, y, it->w, it->h); return; }
    bool foc = n == focus;
    const struct font *f = web_font(14, false, false);
    uint32_t border = foc ? HL_ACCENT : 0x8F8F8F, txt = fc->disabled ? 0x9A9A9A : 0x1E1E1E;
    int w = it->w, h = it->h;
    switch (fc->type) {
    case FC_CHECKBOX:
        gfx_rrect(c, x, y, w, h, 3, fc->checked ? HL_ACCENT : 0xFFFFFF, 255);
        gfx_rrect_outline(c, x, y, w, h, 3, fc->checked ? HL_ACCENT : border, 255);
        if (fc->checked) { gfx_line(c, x + 3, y + 7, x + 6, y + 10, 0xFFFFFF, 2); gfx_line(c, x + 6, y + 10, x + 11, y + 4, 0xFFFFFF, 2); }
        if (foc) gfx_rrect_outline(c, x - 2, y - 2, w + 4, h + 4, 4, HL_ACCENT, 160);
        return;
    case FC_RADIO:
        gfx_circle(c, x + w / 2, y + h / 2, w / 2, border, 255);
        gfx_circle(c, x + w / 2, y + h / 2, w / 2 - 1, 0xFFFFFF, 255);
        if (fc->checked) gfx_circle(c, x + w / 2, y + h / 2, w / 2 - 4, HL_ACCENT, 255);
        if (foc) gfx_circle(c, x + w / 2, y + h / 2, w / 2 + 2, HL_ACCENT, 60);
        return;
    case FC_SUBMIT: case FC_RESET: case FC_BUTTON: case FC_IMAGE: case FC_FILE: {
        uint32_t bg = n->st && n->st->bg ? n->st->bg & 0xFFFFFF : 0xEFEDE8;
        uint32_t tc = n->st && n->st->bg && n->st->color != 0 ? n->st->color : txt;
        gfx_rrect(c, x, y, w, h, 5, bg, 255);
        gfx_rrect_outline(c, x, y, w, h, 5, foc ? HL_ACCENT : 0xA09A92, 255);
        const char *l = fc->label ? fc->label : "";
        int tw = text_width(f, l, strlen(l));
        int cx0 = c->cx0, cx1 = c->cx1; if (c->cx0 < x + 2) c->cx0 = x + 2; if (c->cx1 > x + w - 2) c->cx1 = x + w - 2;
        draw_text(c, f, x + (w - tw) / 2, y + h / 2 + 5, fc->disabled ? 0x9A9A9A : tc, l, strlen(l));
        c->cx0 = cx0; c->cx1 = cx1;
        return;
    }
    case FC_SELECT: {
        gfx_rrect(c, x, y, w, h, 4, 0xFFFFFF, 255);
        gfx_rrect_outline(c, x, y, w, h, 4, border, 255);
        char t[200] = ""; if (fc->nopt) form_option_text(fc->opts[fc->sel], t, sizeof t);
        int cx1 = c->cx1; if (c->cx1 > x + w - 22) c->cx1 = x + w - 22;
        draw_text(c, f, x + 8, y + h / 2 + 5, txt, t, strlen(t));
        c->cx1 = cx1;
        int ax = x + w - 16, ay = y + h / 2 - 2;
        for (int i = 0; i < 4; i++) gfx_fill(c, ax + i, ay + i, 8 - 2 * i, 1, 0x5A5A5A);
        return;
    }
    case FC_TEXTAREA: {
        gfx_fill(c, x, y, w, h, 0xFFFFFF);
        gfx_rrect_outline(c, x, y, w, h, 3, border, 255);
        int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
        if (c->cx0 < x + 4) c->cx0 = x + 4;
        if (c->cx1 > x + w - 4) c->cx1 = x + w - 4;
        if (c->cy0 < y + 3) c->cy0 = y + 3;
        if (c->cy1 > y + h - 3) c->cy1 = y + h - 3;
        int rows = (h - 10) / MONO_H; if (rows < 1) rows = 1;
        int cl = 0; for (int i = 0; i < fc->cur; i++) if (fc->val[i] == '\n') cl++;
        if (cl < fc->scroll) fc->scroll = cl;
        if (cl >= fc->scroll + rows) fc->scroll = cl - rows + 1;
        int line = 0, col = 0;
        int cols = (w - 14) / MONO_W;
        for (int i = 0; i <= fc->len; i++) {
            if (foc && i == fc->cur && line >= fc->scroll && line < fc->scroll + rows) gfx_fill(c, x + 6 + col * MONO_W, y + 5 + (line - fc->scroll) * MONO_H, 2, MONO_H, HL_ACCENT);
            if (i == fc->len) break;
            char ch = fc->val[i];
            if (ch == '\n' || col >= cols) { line++; col = 0; if (ch == '\n') continue; }
            if (line >= fc->scroll && line < fc->scroll + rows) gfx_mono(c, x + 6 + col * MONO_W, y + 5 + (line - fc->scroll) * MONO_H, ch, txt);
            col++;
        }
        c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
        return;
    }
    default: {
        gfx_fill(c, x, y, w, h, fc->disabled || fc->readonly ? 0xF2F2F2 : 0xFFFFFF);
        gfx_rrect_outline(c, x, y, w, h, 3, border, 255);
        if (foc) gfx_rrect_outline(c, x - 1, y - 1, w + 2, h + 2, 4, HL_ACCENT, 120);
        static char shown[1100];
        int n2 = fc->len < 1024 ? fc->len : 1024;
        for (int i = 0; i < n2; i++) shown[i] = fc->type == FC_PASSWORD ? '*' : fc->val[i];
        int inner = w - 12;
        int curw = text_width(f, shown, fc->cur < n2 ? fc->cur : n2);
        int off = foc && curw > inner - 2 ? curw - inner + 2 : 0;
        int cx0 = c->cx0, cx1 = c->cx1;
        if (c->cx0 < x + 3) c->cx0 = x + 3;
        if (c->cx1 > x + w - 3) c->cx1 = x + w - 3;
        int base = y + h / 2 + 5;
        if (!n2 && !foc) { const char *ph = node_attr(n, "placeholder"); if (ph) draw_text(c, f, x + 6, base, 0x9A9690, ph, strlen(ph)); }
        draw_text(c, f, x + 6 - off, base, txt, shown, n2);
        if (foc) gfx_fill(c, x + 6 - off + curw, y + 5, 1, h - 10, 0x1E1E1E);
        c->cx0 = cx0; c->cx1 = cx1;
    }
    }
}

static void popup_geom(int *px, int *py, int *pw, int *ph, int *vis) {
    int i = ctl_item(popup);
    struct fctl *fc = popup->fc;
    *vis = fc->nopt < 12 ? fc->nopt : 12;
    if (i < 0) { *px = 40; *py = BAR_H + 40; *pw = 200; *ph = *vis * 22 + 4; return; }
    struct ditem *it = &lay.it[i];
    int w = it->w < 160 ? 160 : it->w;
    *px = it->x; *py = it->y - scroll + BAR_H + it->h + 2; *pw = w; *ph = *vis * 22 + 4;
    if (*py + *ph > WH - STATUS_H) *py = it->y - scroll + BAR_H - *ph - 2;
    if (*py < BAR_H) *py = BAR_H;
    if (*px + w > WW - 4) *px = WW - 4 - w;
    if (*px < 0) *px = 0;
}

static void draw_toolbar(struct canvas *c) {
    gfx_fill(c, 0, 0, WW, BAR_H, HL_WIN_BG);
    gfx_fill(c, 0, BAR_H - 1, WW, 1, HL_WIN_BORDER);
    ui_button(c, 12, 10, 34, 32, "<", BTN_NORMAL, hover_btn == 0, hpos <= 0);
    ui_button(c, 50, 10, 34, 32, ">", BTN_NORMAL, hover_btn == 4, hpos + 1 >= hcount);
    ui_button(c, 88, 10, 34, 32, loading ? "X" : "R", BTN_NORMAL, hover_btn == 1, false);
    int ax = 130, aw = WW - ax - 12 - 64;
    if (aw < 60) aw = 60;
    gfx_rrect(c, ax, 10, aw, 32, 8, HL_TERM_BG, 255);
    gfx_rrect_outline(c, ax, 10, aw, 32, 8, addr_focus ? HL_ACCENT : HL_WIN_BORDER, 255);
    int tx = ax + 12;
    if (!addr_focus && sec.secure) {                       /* padlock */
        gfx_rrect_outline(c, ax + 12, 17, 10, 10, 4, HL_GREEN, 255);
        gfx_fill(c, ax + 11, 22, 12, 9, HL_GREEN);
        gfx_fill(c, ax + 16, 25, 2, 3, HL_TERM_BG);
        tx = ax + 32;
    }
    gfx_clip(c, tx - 4, 10, aw - (tx - ax) - 4, 32);
    int vis = (aw - 24 - (tx - ax - 12)) / 8, start = addr_focus && alen > vis ? alen - vis : 0;
    gfx_mono_text(c, tx, 18, (addr_focus ? addr : url) + (addr_focus ? start : 0), addr_focus ? HL_TEXT : HL_TEXT_DIM);
    if (addr_focus) gfx_fill(c, tx + (alen - start) * 8, 18, 2, 16, HL_ACCENT);
    gfx_noclip(c);
    ui_button(c, WW - 12 - 56, 10, 56, 32, "Go", BTN_PRIMARY, hover_btn == 2, false);
}

static void draw(void) {
    struct canvas *c = &a.c;
    uint32_t bg = doc ? lay.canvas_bg : 0xFFFFFF;
    gfx_fill(c, 0, BAR_H, WW, WH - BAR_H - STATUS_H, bg);
    gfx_clip(c, 0, BAR_H, WW - SCROLLBAR, WH - BAR_H - STATUS_H);
    int oy = BAR_H - scroll;
    int top = scroll - 64, bottom = scroll + WH;
    for (int i = 0; doc && i < lay.nit; i++) {
        struct ditem *it = &lay.it[i];
        if (it->y > bottom || it->y + it->h < top) continue;
        if (it->deco & 0x80) continue;                 /* visibility: hidden */
        int x = it->x, y = it->y + oy;
        switch (it->kind) {
        case DI_RECT: gfx_fill(c, x, y, it->w, it->h, it->col); break;
        case DI_FRAME: {
            int t = it->asc > 0 ? it->asc : 1;
            gfx_fill(c, x, y, it->w, t, it->col); gfx_fill(c, x, y + it->h - t, it->w, t, it->col);
            gfx_fill(c, x, y, t, it->h, it->col); gfx_fill(c, x + it->w - t, y, t, it->h, it->col);
            break;
        }
        case DI_TEXT: case DI_MONO: {
            uint32_t col = it->col;
            bool hov = it->link >= 0 && it->link == hover_link;
            if (hov) col = gfx_mix(col, 0x000000, 60);
            const char *s = lay.tp + it->off;
            int base = y + it->asc;
            if (it->kind == DI_MONO) { for (uint32_t k = 0; k < it->len; k++) { int cx = x + k * MONO_W; if (cx >= c->cx1) break; if (cx + MONO_W > c->cx0) gfx_mono(c, cx, y, s[k], col); } base = y + 13; }
            else draw_text(c, it->f, x, base, col, s, it->len);
            if ((it->deco & DECO_UNDER) || hov) gfx_fill(c, x, base + 2, it->w, 1, col);
            if (it->deco & DECO_STRIKE) gfx_fill(c, x, base - it->asc * 3 / 10, it->w, 1, col);
            if (it->deco & DECO_OVER) gfx_fill(c, x, y, it->w, 1, col);
            break;
        }
        case DI_MARK:
            if (it->mark == LS_DISC) gfx_circle(c, x + it->w / 2, y + it->h / 2, it->w / 2, it->col, 255);
            else if (it->mark == LS_CIRCLE) {                /* a ring */
                int r2 = it->w * it->w, in2 = (it->w - 2) * (it->w - 2);
                for (int j = 0; j < it->h; j++) for (int i = 0; i < it->w; i++) {
                    int dx = 2 * i + 1 - it->w, dy = 2 * j + 1 - it->h, d2 = dx * dx + dy * dy;
                    if (d2 <= r2 && d2 >= in2) gfx_fill(c, x + i, y + j, 1, 1, it->col);
                }
            }
            else gfx_fill(c, x, y, it->w, it->h, it->col);
            break;
        case DI_IMG:
            draw_image(c, it, x, y);
            if (it->link >= 0 && it->link == hover_link) gfx_rrect_outline(c, x - 1, y - 1, it->w + 2, it->h + 2, 2, 0x1A6FC4, 200);
            break;
        case DI_CTRL: draw_control(c, it, x, y); break;
        case DI_BGIMG: {                                  /* a background picture, tiled or once */
            struct imgent *im = it->img;
            if (!im || im->state != IS_OK || !im->px || im->pw <= 0 || im->ph <= 0) break;
            int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
            if (c->cx0 < x) c->cx0 = x;
            if (c->cy0 < y) c->cy0 = y;
            if (c->cx1 > x + it->w) c->cx1 = x + it->w;
            if (c->cy1 > y + it->h) c->cy1 = y + it->h;
            int rep = it->mark;
            int sx = x, sy = y;
            if (it->asc) { sx = x + (it->w - im->pw) / 2; if (rep == 1 || rep == 3) sy = y; if (rep == 1) sy = y + (it->h - im->ph) / 2; }
            int nx = rep == 0 || rep == 2 ? (it->w + im->pw - 1) / im->pw + 1 : 1, ny = rep == 0 || rep == 3 ? (it->h + im->ph - 1) / im->ph + 1 : 1;
            if (rep == 0 || rep == 2) { while (sx > x) sx -= im->pw; }
            if ((long)im->pw * im->ph <= 16 && rep == 0) { if (im->opaque) gfx_fill(c, x, y, it->w, it->h, im->px[0] & 0xFFFFFF); }
            else if (c->cy1 > c->cy0 && c->cx1 > c->cx0) {
                int ty0 = c->cy0 > sy ? (c->cy0 - sy) / im->ph : 0, tx0 = c->cx0 > sx ? (c->cx0 - sx) / im->pw : 0;
                for (int ty = ty0; ty < ny; ty++) {
                    int yy = sy + ty * im->ph;
                    if (yy >= c->cy1) break;
                    for (int tx = tx0; tx < nx; tx++) { int xx = sx + tx * im->pw; if (xx >= c->cx1) break; blit_img(c, im->px, im->pw, im->ph, xx, yy, im->opaque); }
                }
            }
            c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
            break;
        }
        }
    }
    gfx_noclip(c);
    /* scroll bar */
    int vh = view_h();
    gfx_fill(c, WW - SCROLLBAR, BAR_H, SCROLLBAR, vh, gfx_mix(bg, 0x808080, 40));
    if (lay.doc_h > vh && lay.doc_h > 0) {
        int th = (int)((long)vh * vh / lay.doc_h); if (th < 24) th = 24;
        int ty = BAR_H + (int)((long)(vh - th) * scroll / (lay.doc_h - vh > 0 ? lay.doc_h - vh : 1));
        gfx_rrect(c, WW - SCROLLBAR + 2, ty, SCROLLBAR - 4, th, 4, 0x9A968E, 255);
    }
    /* an open <select> list */
    if (popup && popup->fc) {
        int px, py, pw, ph, vis; popup_geom(&px, &py, &pw, &ph, &vis);
        gfx_rrect(c, px + 2, py + 3, pw, ph, 6, 0x000000, 50);
        gfx_rrect(c, px, py, pw, ph, 6, 0xFFFFFF, 255);
        gfx_rrect_outline(c, px, py, pw, ph, 6, 0x8F8F8F, 255);
        const struct font *f = web_font(14, false, false);
        gfx_clip(c, px + 1, py + 1, pw - 2, ph - 2);
        for (int k = 0; k < vis; k++) {
            int i = popup_first + k; if (i >= popup->fc->nopt) break;
            int yy = py + 2 + k * 22;
            if (i == popup_hover) gfx_fill(c, px + 2, yy, pw - 4, 22, HL_ACCENT);
            char t[200]; form_option_text(popup->fc->opts[i], t, sizeof t);
            draw_text(c, f, px + 10, yy + 16, i == popup_hover ? 0xFFFFFF : i == popup->fc->sel ? HL_ACCENT : 0x1E1E1E, t, strlen(t));
        }
        gfx_noclip(c);
    }
    draw_toolbar(c);
    /* status bar */
    gfx_fill(c, 0, WH - STATUS_H, WW, STATUS_H, HL_WIN_BG);
    gfx_fill(c, 0, WH - STATUS_H, WW, 1, HL_WIN_BORDER);
    const char *st = hover_link >= 0 && hover_link < nlinks ? links[hover_link].href : status_text;
    gfx_clip(c, 0, WH - STATUS_H, WW - 10, STATUS_H);
    gfx_mono_text(c, 10, WH - 20, st, loading ? HL_GOLD : HL_TEXT_FAINT);
    gfx_noclip(c);
    app_flush(&a);
}

/* ---------------- input ---------------- */
static int item_at(int x, int y, bool links_only) {
    int dy = y - BAR_H + scroll;
    if (y < BAR_H || y >= WH - STATUS_H || x >= WW - SCROLLBAR) return -1;
    for (int i = lay.nit - 1; i >= 0; i--) {
        struct ditem *it = &lay.it[i];
        if (it->deco & 0x80) continue;
        if (it->kind == DI_CTRL ? links_only : (it->link < 0 || (it->kind != DI_TEXT && it->kind != DI_MONO && it->kind != DI_IMG))) continue;
        if (x >= it->x && x < it->x + it->w && dy >= it->y - 1 && dy < it->y + it->h + 1) return i;
    }
    return -1;
}
static int link_at(int x, int y) { int i = item_at(x, y, true); return i >= 0 ? lay.it[i].link : -1; }

static int btn_at(int x, int y) {
    if (ui_in(x, y, 12, 10, 34, 32)) return 0;
    if (ui_in(x, y, 50, 10, 34, 32)) return 4;
    if (ui_in(x, y, 88, 10, 34, 32)) return 1;
    if (ui_in(x, y, WW - 68, 10, 56, 32)) return 2;
    if (ui_in(x, y, 130, 10, WW - 130 - 76, 32)) return 3;
    return -1;
}

static void reset_form(struct node *e, struct node *form, int depth) {      /* back to the values in the page */
    for (struct node *c = e->first; c && depth < 200; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->fc && c->form == form) form_init_control(doc, c);
        reset_form(c, form, depth + 1);
    }
}

static void click_control(struct node *n, int mx, int my) {
    struct fctl *fc = n->fc;
    if (!fc || fc->disabled) return;
    int i = ctl_item(n);
    focus = n;
    switch (fc->type) {
    case FC_CHECKBOX: fc->checked = !fc->checked; break;
    case FC_RADIO: control_key(' '); break;
    case FC_SUBMIT: case FC_IMAGE: submit(n->form, n); break;
    case FC_RESET: if (n->form) reset_form(doc->root, n->form, 0); break;
    case FC_MEDIA: if (i >= 0) media_click(n, mx - lay.it[i].x, my - (lay.it[i].y - scroll + BAR_H), lay.it[i].w, lay.it[i].h); break;
    case FC_SELECT: popup = n; popup_hover = fc->sel; popup_first = fc->sel > 6 ? fc->sel - 6 : 0;
        if (popup_first + 12 > fc->nopt) popup_first = fc->nopt > 12 ? fc->nopt - 12 : 0;
        break;
    case FC_TEXTAREA:
        if (i >= 0) {
            int line = (my - (lay.it[i].y - scroll + BAR_H) - 5) / MONO_H + fc->scroll, col = (mx - lay.it[i].x - 6 + MONO_W / 2) / MONO_W;
            int cols = (lay.it[i].w - 14) / MONO_W, l = 0, cc = 0, k = 0;
            for (; k < fc->len; k++) {
                if (l == line && cc >= col) break;
                if (fc->val[k] == '\n') { if (l == line) break; l++; cc = 0; continue; }
                if (cc >= cols) { if (l == line) break; l++; cc = 0; }
                cc++;
            }
            fc->cur = k;
        }
        break;
    case FC_TEXT: case FC_PASSWORD: case FC_NUMBER:
        if (i >= 0) {
            const struct font *f = web_font(14, false, false);
            int inner = lay.it[i].w - 12, curw = 0;
            for (int k = 0; k < fc->cur; k++) curw += f->g[(unsigned char)(fc->type == FC_PASSWORD ? '*' : fc->val[k]) - 32].adv;
            int off = curw > inner - 2 ? curw - inner + 2 : 0, rx = mx - lay.it[i].x - 6 + off, w = 0, k = 0;
            for (; k < fc->len; k++) { int g = f->g[(unsigned char)(fc->type == FC_PASSWORD ? '*' : fc->val[k]) - 32].adv; if (w + g / 2 >= rx) break; w += g; }
            fc->cur = k;
        }
        break;
    default: break;
    }
}

/* ---------------- for scripts (web/dom.c) ---------------- */
static struct dunc_event requeued[8]; static int nrequeued;
void web_requeue_event(const struct dunc_event *e) {
    if (nrequeued && e->type == EV_RESIZE && requeued[nrequeued - 1].type == EV_RESIZE) { requeued[nrequeued - 1] = *e; return; }
    if (nrequeued < 8) requeued[nrequeued++] = *e;
}
void web_dom_changed(struct node *added) {
    if (added && doc && added->type == N_ELEM) { init_node(doc, added); walk_init(doc, added, 0); }
    need_cascade = true; need_layout = true; styles_scan = true;
}
void web_text_changed(struct node *n) {                    /* a <button>'s label is drawn from its text */
    for (struct node *p = n; p && p->type == N_ELEM; p = 0) if (p->tag == T_STYLE && p->css_done) styles_reload = true;   /* a sheet was rewritten */
    if (n && n->parent && n->type == N_TEXT && n->parent->tag == T_STYLE && n->parent->css_done) styles_reload = true;
    for (int k = 0; n && k < 20; n = n->parent, k++)
        if (n->type == N_ELEM && n->tag == T_BUTTON && n->fc && doc) {
            char b[200]; int l = 0; text_content(n, b, sizeof b, &l, 0);
            while (l && b[l - 1] == ' ') b[--l] = 0;
            const char *t = b[0] == ' ' ? b + 1 : b;
            n->fc->label = t[0] ? ar_strndup(&doc->ar, t, strlen(t)) : "Button";
            return;
        }
}
void web_navigate(const char *u) {                         /* later, from the main loop: a script may still be running */
    pend = true; strncpy0(pend_url, u, sizeof pend_url);
    free(pend_body); pend_body = 0; pend_blen = 0; pend_post = false; pend_mode = 0; pend_scroll = 0;
}
void web_page_url(char *out, int n) { strncpy0(out, url, n); }

/* ---------------- for <video>/<audio> (web/media.c) ---------------- */
void web_redraw_node(struct node *n) {
    if (!doc || popup) return;
    int i = ctl_item(n);
    if (i < 0 || (lay.it[i].deco & 0x80)) return;
    int x = lay.it[i].x, y = lay.it[i].y - scroll + BAR_H, w = lay.it[i].w, h = lay.it[i].h;
    int vx0 = 0, vy0 = BAR_H, vx1 = WW - SCROLLBAR, vy1 = WH - STATUS_H;
    int cx0 = x > vx0 ? x : vx0, cy0 = y > vy0 ? y : vy0, cx1 = x + w < vx1 ? x + w : vx1, cy1 = y + h < vy1 ? y + h : vy1;
    if (cx1 <= cx0 || cy1 <= cy0) return;
    gfx_clip(&a.c, cx0, cy0, cx1 - cx0, cy1 - cy0);
    media_draw(&a.c, n, x, y, w, h);
    gfx_noclip(&a.c);
    app_flush_rect(&a, cx0, cy0, cx1 - cx0, cy1 - cy0);
}
void web_media_resized(void) { need_layout = true; }
struct dlbuf { char *b; long len, cap, max; bool over; long total; uint64_t last; char *msg; int msglen; struct node *n; };
static bool dl_sink(const char *d, long n, void *ctx) {
    struct dlbuf *f = ctx;
    if (f->len + n > f->max) { f->over = true; return false; }
    if (f->len + n > f->cap) {
        long nc = f->cap * 2; while (nc < f->len + n) nc *= 2;
        if (nc > f->max) nc = f->max;
        char *nb = realloc(f->b, nc + 1); if (!nb) { f->over = true; return false; }
        f->b = nb; f->cap = nc;
    }
    memcpy(f->b + f->len, d, n); f->len += n;
    uint64_t now = sys_time();
    if (now - f->last > 300) {
        f->last = now;
        if (f->total > 0) ksnprintf(f->msg, f->msglen, "Downloading... %ld of %ld MB (Esc stops)", f->len >> 20, f->total >> 20);
        else ksnprintf(f->msg, f->msglen, "Downloading... %ld MB (Esc stops)", f->len >> 20);
        web_redraw_node(f->n);
    }
    poll_events();
    return !abort_fetch && !quit && !pend;
}
long web_download(const char *u, long cap, char **out, int *status, char *ctype, int ctlen, char *msg, int msglen, struct node *n) {
    *out = 0; *status = 0;
    if (!pre_ci(u, "http:") && !pre_ci(u, "https:")) {          /* file: and data: */
        long r = fetch_any(u, cap, out, ctype, ctlen, status, 0, 0, 0, 0, 0);
        if (r >= 0 && !*status) *status = 200;
        return r;
    }
    struct dlbuf f = { malloc(1 << 20), 0, 1 << 20, cap, false, 0, 0, msg, msglen, n };
    if (!f.b) return NET_ENOMEM;
    struct http_req rq = { "GET", 0, 0, 0, 0, 0, 0 };
    abort_fetch = false;
    fetch_depth++;
    long r = http_request(u, &rq, dl_sink, &f, status, &f.total, ctype, ctlen, 0, 0);
    fetch_depth--;
    bool stopped = abort_fetch; abort_fetch = false;
    if (f.over) { free(f.b); return -2; }
    if (stopped || (r < 0 && !f.len) || quit || pend) { free(f.b); return r < 0 ? r : -1; }
    *out = f.b;
    return f.len;
}
void web_set_title(const char *t) { set_title(); }
/* Scripts asking for sizes and positions after changing the page need it laid out again. Some (animation
 * libraries) change and measure in a loop; on a big page each layout is slow, so it's redone at most 4
 * times a second (scripts get the last one's numbers in between) and the time limits are checked next. */
static uint64_t last_script_layout;
static void layout_for_script(void) {
    if (!(need_layout || need_cascade)) return;
    uint64_t now = sys_time();
    if (now - last_script_layout < 250) return;
    relayout(); last_script_layout = sys_time();
    dom_check_soon();
}
void web_viewport(int *w, int *h, int *s, int *dh) {
    layout_for_script();
    *w = page_w(); *h = view_h(); *s = scroll; *dh = lay.doc_h;
}
void web_scroll_to(int y) { scroll = y; scroll_by(0); need_draw = true; }
void web_message(const char *title, const char *msg) {
    char m[200]; strncpy0(m, msg, sizeof m);
    sys_notify(title, m);
    ksnprintf(status_text, sizeof status_text, "The page says: %s", m); need_draw = true;
}
bool web_node_rect(struct node *n, int *x, int *y, int *w, int *h) {
    layout_for_script();
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < lay.nit; i++) {
        struct node *s = lay.it[i].src; int k = 0;
        while (s && s != n && k++ < 120) s = s->parent;
        if (s != n) continue;
        struct ditem *it = &lay.it[i];
        if (it->x < x0) x0 = it->x;
        if (it->y < y0) y0 = it->y;
        if (it->x + it->w > x1) x1 = it->x + it->w;
        if (it->y + it->h > y1) y1 = it->y + it->h;
    }
    if (x1 < x0) { *x = *y = *w = *h = 0; return false; }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}
long web_http_script(const char *u, char **out, int *status, char *ctype, int ctlen, char *final, int fulen) {
    return fetch_any(u, 24l << 20, out, ctype, ctlen, status, final, fulen, "GET", 0, 0);
}
long web_http(const char *method, const char *u, const char *body, long blen, const char *headers,
              char **out, int *status, char *ctype, int ctlen, char *final, int fulen) {
    return fetch_any(u, 8l << 20, out, ctype, ctlen, status, final, fulen, method, body, blen);
}
/* the element under a point (for scripts' mouse events) */
static struct node *elem_at(int x, int y) {
    int dy = y - BAR_H + scroll;
    for (int i = lay.nit - 1; i >= 0; i--) {
        struct ditem *it = &lay.it[i];
        if (x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h && it->src) return it->src;
    }
    return doc ? doc->body : 0;
}

static void handle_event(struct dunc_event *e) {
    if (e->type == EV_CLOSE) { quit = true; abort_fetch = true; return; }
    if (e->type == EV_RESIZE) {
        if (app_resize(&a, e)) {
            WW = a.wi.w; WH = a.wi.h;
            if (css_media_dependent() && !styles_loading) need_cascade = true;
            need_layout = true;
            if (fetch_depth) { relayout(); scroll_by(0); draw(); }
        }
        return;
    }
    if (e->type == EV_WHEEL) {
        if (popup) { popup_first += e->key; if (popup_first > popup->fc->nopt - 12) popup_first = popup->fc->nopt - 12; if (popup_first < 0) popup_first = 0; draw(); return; }
        scroll_by(e->key * 60); draw(); return;
    }
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_LEAVE) {
        if (popup && e->type == EV_MOUSE_MOVE) {
            int px, py, pw, ph, vis; popup_geom(&px, &py, &pw, &ph, &vis);
            int h = ui_in(e->x, e->y, px, py, pw, ph) ? popup_first + (e->y - py - 2) / 22 : -1;
            if (h >= popup->fc->nopt) h = -1;
            if (h != popup_hover && h >= 0) { popup_hover = h; draw(); }
            return;
        }
        int l = e->type == EV_MOUSE_MOVE ? link_at(e->x, e->y) : -1, b = e->type == EV_MOUSE_MOVE ? btn_at(e->x, e->y) : -1;
        if (l != hover_link || b != hover_btn) { hover_link = l; hover_btn = b; draw(); }
        return;
    }
    if (e->type == EV_MOUSE_DOWN && e->button == 1) {
        if (popup) {
            int px, py, pw, ph, vis; popup_geom(&px, &py, &pw, &ph, &vis);
            if (ui_in(e->x, e->y, px, py, pw, ph)) { int h = popup_first + (e->y - py - 2) / 22; if (h >= 0 && h < popup->fc->nopt) popup->fc->sel = h; }
            popup = 0; draw(); return;
        }
        int b = btn_at(e->x, e->y);
        if (b == 0) go_back();
        else if (b == 4) go_forward();
        else if (b == 1) { if (loading) abort_fetch = true; else reload(); }
        else if (b == 2) { if (addr_focus && alen) open_typed(addr); else reload(); }
        else if (b == 3) { addr_focus = true; focus = 0; strncpy0(addr, url, sizeof addr); alen = strlen(addr); }
        else if (e->x >= WW - SCROLLBAR && e->y > BAR_H && e->y < WH - STATUS_H) {     /* the scroll bar: jump */
            int vh = view_h();
            scroll = (int)((long)(e->y - BAR_H) * (lay.doc_h - vh) / (vh > 0 ? vh : 1)); scroll_by(0);
        } else if (e->y > BAR_H && e->y < WH - STATUS_H && doc) {
            addr_focus = false;
            if (js_page_active()) {                          /* scripts see the click first and may cancel what it would do */
                struct node *t = elem_at(e->x, e->y);
                int px = e->x, py = e->y - BAR_H + scroll;
                js_event(t, "mousedown", px, py, 0);
                js_event(t, "mouseup", px, py, 0);
                if (js_event(t, "click", px, py, 0)) { draw(); return; }
                if (doc != web_doc || pend) return;          /* a script went to another page */
            }
            int i = item_at(e->x, e->y, false);
            if (i >= 0 && lay.it[i].kind == DI_CTRL) {
                struct node *cn = lay.it[i].n;
                click_control(cn, e->x, e->y);
                if (cn->fc && (cn->fc->type == FC_CHECKBOX || cn->fc->type == FC_RADIO)) { js_event(cn, "input", 0, 0, 0); js_event(cn, "change", 0, 0, 0); }
            } else {
                focus = 0;
                if (i >= 0 && lay.it[i].link >= 0 && lay.it[i].link < nlinks) {
                    char u[600]; strncpy0(u, links[lay.it[i].link].href, sizeof u);
                    const char *raw = node_attr(links[lay.it[i].link].n, "href");
                    if (raw && pre_ci(raw, "javascript:")) {          /* a script link */
                        char code[2000]; int k = 0;
                        for (const char *q = raw + 11; *q && k < 1999; q++) { int h; if (*q == '%' && q[1] && q[2] && (h = hex2(q + 1)) >= 0) { code[k++] = (char)h; q += 2; } else code[k++] = *q; }
                        code[k] = 0; js_eval_url(code); draw(); return;
                    }
                    request_nav(u, false, 0, 0, 0, 0); return;
                }
            }
        }
        draw();
        return;
    }
    if (e->type == EV_KEY) {
        int k = e->key, code = KEY_CODE(k);
        if (popup) {
            struct fctl *fc = popup->fc;
            if (code == KEY_DOWN && popup_hover + 1 < fc->nopt) popup_hover++;
            else if (code == KEY_UP && popup_hover > 0) popup_hover--;
            else if (k == '\n' || k == ' ') { if (popup_hover >= 0) fc->sel = popup_hover; popup = 0; }
            else if (k == 27) popup = 0;
            if (popup) { if (popup_hover < popup_first) popup_first = popup_hover; if (popup_hover >= popup_first + 12) popup_first = popup_hover - 11; }
            draw(); return;
        }
        if (k == 27 && (loading || fetch_depth)) { abort_fetch = true; js_stop_scripts(); return; }
        if (k == 27 && js_scripts_pending()) { js_stop_scripts(); return; }
        if (addr_focus) {
            if (k == '\n') { if (alen) open_typed(addr); return; }
            else if (k == 27) addr_focus = false;
            else if (k == '\b') { if (alen) addr[--alen] = 0; }
            else if (k == 22) clip_paste_line(addr, &alen, 590);
            else if (k == 3) clip_set(addr, alen);
            else if (key_is_char(k) && alen < 590) { addr[alen++] = k; addr[alen] = 0; }
            if (addr_focus) { draw_toolbar(&a.c); app_flush_rect(&a, 0, 0, WW, BAR_H); return; }
        } else if (js_page_active() && doc && js_event(focus ? focus : doc->body, "keydown", 0, 0, k)) {
            /* a script handled the key */
        } else if (focus && doc && !(focus->fc && focus->fc->type == FC_MEDIA && k != ' ' && k != '\n' && k != '\t' && k != 27)) {
            struct node *was = focus; int len0 = focus->fc ? focus->fc->len : 0, sel0 = focus->fc ? focus->fc->sel : 0;
            control_key(k);
            if (was->fc && (was->fc->len != len0 || (was->fc->type == FC_SELECT && was->fc->sel != sel0))) { js_event(was, "input", 0, 0, k); if (was->fc->type == FC_SELECT) js_event(was, "change", 0, 0, 0); }
            if (focus) ensure_visible(focus);
        } else {
            int vh = view_h();
            if (code == KEY_DOWN) scroll_by(40); else if (code == KEY_UP) scroll_by(-40);
            else if (code == KEY_PGDN || k == ' ') scroll_by(vh - 60); else if (code == KEY_PGUP) scroll_by(-(vh - 60));
            else if (code == KEY_HOME) scroll = 0;
            else if (code == KEY_END) scroll_by(lay.doc_h);
            else if ((code == KEY_LEFT && (k & KEYMOD_ALT)) || k == '\b') { go_back(); return; }
            else if (code == KEY_RIGHT && (k & KEYMOD_ALT)) { go_forward(); return; }
            else if (code == KEY_F5 || k == 18) { reload(); return; }
            else if (k == 12 || k == '/') { addr_focus = true; addr[0] = 0; alen = 0; }   /* Ctrl+L */
            else if (k == '\t' && doc) { bool seen = false; focus = tab_next(doc->root, 0, &seen, 0); if (focus) { if (focus->fc) focus->fc->cur = focus->fc->len; ensure_visible(focus); } }
        }
        draw();
    }
}

int main(void) {
    char arg[256];
    http_options = HTTP_OPT_COOKIES | HTTP_OPT_GZIP;
    { char ts[8] = ""; sys_setting_get("web.test_slow", ts, sizeof ts); if (ts[0] == '1') web_time_scale = 40; }   /* emulator testing only */
    css_init();
    if (!app_open_ex(&a, WW, WH, "Arc", WIN_RESIZABLE)) return 1;
    http_wait_hook = web_wait;
    WW = a.wi.w; WH = a.wi.h;
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0]) open_typed(arg); else navigate("about:home", false, 0, 0, 0, 0);
    draw();
    while (!quit) {
        /* If one turn of the loop uses 8 s of processor time (a page's script making Web loop somewhere in its
         * own code), the alarm brings it back here: that page's scripts are turned off and Web carries on. */
        if (dunc_setjmp(stuck_jb)) recover_from_stuck();
        sys_alarm(8000 * web_time_scale, on_stuck_alarm);
        if (pend) {
            pend = false; abort_fetch = false;
            char u[600]; strncpy0(u, pend_url, sizeof u);
            char *b = pend_body; long bl = pend_blen; pend_body = 0;
            navigate(u, pend_post, b, bl, pend_mode, pend_scroll);
            free(b);
            continue;
        }
        struct dunc_event e;
        if (js_scripts_pending()) {                        /* the page's scripts, one per turn, answering events in between */
            while (sys_event(&e, 0) > 0 && !quit && !pend) handle_event(&e);
            if (quit || pend) continue;
            static bool img_turn;                          /* pictures and scripts take turns, so pictures show up while scripts run */
            struct imgent *pim = (img_turn = !img_turn) ? next_image() : 0;
            if (pim && !loading) {
                ksnprintf(status_text, sizeof status_text, "Loading picture %s", pim->url);
                load_image(pim);
                draw();
                continue;
            }
            static uint64_t last_lay;
            char st[220];
            bool more = js_page_step(st, sizeof st);
            if (more) {
                strncpy0(status_text, st, sizeof status_text);
                if ((need_layout || need_cascade) && sys_time() - last_lay > 1000) { relayout(); last_lay = sys_time(); }
            } else {
                strncpy0(status_text, page_status, sizeof status_text);
                if (need_layout || need_cascade) relayout();
            }
            draw();
            continue;
        }
        if (need_layout) {                                 /* lay out again once the events (e.g. a resize drag) settle */
            if (sys_event(&e, 0) > 0) { handle_event(&e); continue; }
            if (sys_time() - last_layout_t < 200 && next_image()) { /* images still coming: not too often */ }
            else { relayout(); scroll_by(0); draw(); continue; }
        }
        struct imgent *im = next_image();
        if (im && !loading) {
            while (sys_event(&e, 0) > 0 && !quit) handle_event(&e);
            if (quit || pend) continue;
            ksnprintf(status_text, sizeof status_text, "Loading picture %s", im->url);
            draw();
            load_image(im);
            if (!next_image()) strncpy0(status_text, page_status, sizeof status_text);
            draw();
            continue;
        }
        /* changes made by scripts are drawn at most ~30 times a second (a page that changes itself
         * constantly would otherwise keep the desktop redrawing it all the time) */
        static uint64_t last_draw;
        uint64_t tnow = sys_time();
        if (need_draw && tnow - last_draw >= 33) { need_draw = false; last_draw = tnow; draw(); }
        if (nrequeued) { struct dunc_event q = requeued[0]; for (int i = 1; i < nrequeued; i++) requeued[i - 1] = requeued[i]; nrequeued--; handle_event(&q); continue; }
        long wait = js_timer_wait();                    /* scripts' timers */
        long mw = media_tick();                         /* a video or sound playing */
        if (mw >= 0 && (wait < 0 || mw < wait)) wait = mw;
        if (mw == 0) { if (sys_event(&e, 0) > 0) handle_event(&e); js_run_timers(); continue; }
        if (need_draw) { long dw = 33 - (long)(tnow - last_draw); if (dw < 1) dw = 1; if (wait < 0 || dw < wait) wait = dw; }
        if (sys_event(&e, wait) > 0) handle_event(&e);
        js_run_timers();
    }
    return 0;
}
