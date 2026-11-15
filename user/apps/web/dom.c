/* Scripts for Web: connects the JavaScript engine (web/js) to the page.
 * The C side here gives scripts a small set of primitives ("__d": the tree, attributes, text, HTML, timers,
 * networking, storage); the browser APIs scripts expect (document, elements, events, fetch, localStorage...)
 * are written in JavaScript on top of them (web/dom_prelude.js, built into dom_prelude.h). */
#include "web.h"
#include "js/js.h"          /* (uses C's _Bool; the rest of Web uses dunc.h's int bool) */
#undef bool
#include "dom_prelude.h"

static js_rt *rt;
static struct doc *D;
static jsval wrappers;                 /* every node's JS object, by node->jsw - 1 (kept for the page's life) */
static long nwrappers;
static jsval proto_elem, proto_text, proto_comment, api;   /* set by the prelude */
static bool page_on;

/* ---------------- console (about:console) ---------------- */
static char conlog[16384]; static long conlen;
static void con_add(const char *pre, const char *s) {
    if (pre[0] == '[' && pre[1] == 'e') { char l[300]; ksnprintf(l, sizeof l, "script %s%s", pre, s); sys_log(l); }   /* errors also go to the log */
    long n = strlen(pre) + strlen(s) + 1;
    if (n > (long)sizeof conlog / 2) n = sizeof conlog / 2;
    if (conlen + n >= (long)sizeof conlog) { long cut = conlen + n - (long)sizeof conlog / 2; memmove(conlog, conlog + cut, conlen - cut); conlen -= cut; }
    long k = 0; for (const char *p = pre; *p && k < n - 1; p++) conlog[conlen + k++] = *p;
    for (const char *p = s; *p && k < n - 1; p++) conlog[conlen + k++] = *p;
    conlog[conlen + k++] = '\n'; conlen += k; conlog[conlen] = 0;
}
void js_console_lines(char *out, long max) { strncpy0(out, conlog, max); }

/* ---------------- the host ---------------- */
static uint64_t h_now(void *c) { struct rtc_time t; sys_rtc(&t);
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long y = t.year, days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[(t.mon - 1) % 12] + t.day - 1;
    if (t.mon > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return (uint64_t)((days * 86400 + t.hour * 3600 + t.min * 60 + t.sec) * 1000 + sys_time() % 1000); }
static uint64_t h_mono(void *c) { return sys_time(); }
static uint64_t run_start, page_deadline, last_look;
/* How much processor time the page's scripts have used, counted with the cycle counter (timer jobs
 * are often far shorter than a millisecond). A page that keeps scripts busy without end (YouTube's
 * app, for one) has its scripts turned off after PAGE_CPU_MS, so it can't keep a core busy forever. */
#define PAGE_CPU_MS 20000
static uint64_t cyc_begin, page_cycles, cal_tsc0, cal_ms0;
static inline uint64_t cyc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static uint64_t cycles_per_ms(void) {
    uint64_t t = sys_time();
    if (!cal_ms0) { cal_ms0 = t; cal_tsc0 = cyc(); }
    if (t - cal_ms0 < 200) return 3000000;               /* until measured: assume 3 GHz */
    return (cyc() - cal_tsc0) / (t - cal_ms0);
}
static void script_begin(void) { run_start = sys_time(); cyc_begin = cyc(); }
static void script_end(void) { if (cyc_begin) page_cycles += cyc() - cyc_begin; cyc_begin = 0; }
static uint64_t page_cpu_ms(void) { return (page_cycles + (cyc_begin ? cyc() - cyc_begin : 0)) / cycles_per_ms(); }
static bool skip_said;
static struct node *squeue[512]; static int nsq, sqpos; static bool sq_active;
static void scripts_off(const char *why) { con_add("[stopped] ", why); page_on = false; }
/* Called often while a script runs: stops scripts that run too long, and keeps the window answering
 * (the close button quits, Esc stops the page's scripts). */
static _Bool h_interrupt(void *c) {
    uint64_t now = sys_time();
    if (!page_on) return true;
    if (now - run_start > 4000) { scripts_off("a script ran for more than 4 seconds; this page's scripts were turned off"); return true; }
    if (page_cpu_ms() > PAGE_CPU_MS) { scripts_off("this page's scripts kept the processor busy for 20 seconds, so they were turned off"); return true; }
    if (now - last_look > 100) {
        last_look = now;
        struct dunc_event e;
        while (sys_event(&e, 0) > 0) {
            if (e.type == EV_CLOSE) sys_exit(0);
            if (e.type == EV_KEY && e.key == 27) { scripts_off("stopped with Esc"); return true; }
            if (e.type == EV_RESIZE) web_requeue_event(&e);
        }
    }
    return false;
}
static void h_console(void *c, int level, const char *s) {
    static const char *pre[] = { "", "", "[warn] ", "[error] " };
    char *t = malloc(strlen(s) + 1); if (!t) return;
    strncpy0(t, s, strlen(s) + 1); utf8_to_cp1252(t, strlen(t) + 1);
    con_add(pre[level & 3], t);
    free(t);
}
static void h_random(void *c, void *buf, int n) { sys_random(buf, n); }
static const struct js_host host = { 0, h_now, h_mono, h_interrupt, h_console, h_random };

/* ---------------- helpers ---------------- */
static const struct js_class node_class = { "Node", 0, 0, 0, 0 };
static jsval A(int argc, jsval *argv, int i) { return i < argc ? argv[i] : js_undefined(); }
static struct node *N(jsval v) { return js_is_object(v) ? js_host_ptr(v, &node_class) : 0; }
/* a JS string from page text (Windows-1252) */
static void set_held(jsval arr, uint32_t i, jsval v) { js_root(rt, &v); js_set_index(rt, arr, i, v); js_unroot(rt, &v); }
static jsval S(const char *s, long n) {
    if (n < 0) n = s ? strlen(s) : 0;
    char small[512]; char *b = n * 3 + 1 <= (long)sizeof small ? small : malloc(n * 3 + 1);
    if (!b) return js_str(rt, "", 0);
    long k = cp1252_to_utf8(s, n, b, n * 3 + 1);
    jsval r = js_str(rt, b, k);
    if (b != small) free(b);
    return r;
}
/* page text (Windows-1252, malloc'd) from a JS value */
static char *T(jsval v, long *len) {
    char *s = js_to_cstring(rt, v);
    if (!s) { s = malloc(1); if (s) s[0] = 0; }
    long n = utf8_to_cp1252(s, strlen(s));
    s[n] = 0; if (len) *len = n;
    return s;
}
/* keep a value safe from the garbage collector while more engine calls are made */
#define HOLD(v) js_root(rt, &(v))
#define DROP(v) js_unroot(rt, &(v))
static char *ardup(const char *s, long n) { return ar_strndup(&D->ar, s, n); }

static jsval W(struct node *n) {                         /* the node's JS object */
    if (!n) return js_null();
    if (n->jsw) return js_get_index(rt, wrappers, n->jsw - 1);
    jsval o = js_host_object(rt, &node_class, n, n->type == N_TEXT ? proto_text : proto_elem);
    HOLD(o);
    js_set_index(rt, wrappers, nwrappers, o);
    n->jsw = ++nwrappers;
    DROP(o);
    return o;
}

const char *node_attr(const struct node *n, const char *name);
static int find_attr(struct node *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attrs[i].name, name)) return i;
    return -1;
}
static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }
/* keep the parsed fields (id, classes, style) in step with the attributes */
static void refresh_attrs(struct node *n) {
    n->id = node_attr(n, "id");
    n->style_attr = node_attr(n, "style");
    const char *c = node_attr(n, "class");
    n->ncls = 0; n->cls = 0;
    if (c) {
        int cnt = 0; for (const char *p = c; *p;) { while (*p == ' ' || *p == '\t' || *p == '\n') p++; if (!*p) break; cnt++; while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++; }
        if (cnt) {
            n->cls = ar_alloc(&D->ar, cnt * sizeof *n->cls);
            if (!n->cls) return;
            for (const char *p = c; *p;) {
                while (*p == ' ' || *p == '\t' || *p == '\n') p++;
                if (!*p) break;
                const char *s = p; while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                n->cls[n->ncls++] = ardup(s, p - s);
            }
        }
    }
}
static void set_attr(struct node *n, const char *name, const char *val, long vlen) {
    int i = find_attr(n, name);
    if (i < 0) {
        struct attr *na = ar_alloc(&D->ar, (n->nattr + 1) * sizeof *na);
        if (!na) return;
        if (n->nattr) memcpy(na, n->attrs, n->nattr * sizeof *na);
        n->attrs = na; i = n->nattr++;
        n->attrs[i].name = ardup(name, strlen(name));
    }
    n->attrs[i].value = ardup(val, vlen);
    refresh_attrs(n);
}
static void remove_attr(struct node *n, const char *name) {
    int i = find_attr(n, name);
    if (i < 0) return;
    for (int k = i; k + 1 < n->nattr; k++) n->attrs[k] = n->attrs[k + 1];
    n->nattr--;
    refresh_attrs(n);
}
static void set_depth(struct node *n, int d) {
    n->depth = d > 250 ? 250 : d;
    for (struct node *c = n->first; c; c = c->next) set_depth(c, d + 1);
}
static void detach(struct node *n) {
    struct node *p = n->parent;
    if (!p) return;
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = 0;
}
static bool contains(struct node *a, struct node *b) { for (; b; b = b->parent) if (a == b) return true; return false; }
static void insert_before(struct node *p, struct node *n, struct node *ref) {
    if (ref && ref->parent != p) ref = 0;
    detach(n);
    n->parent = p;
    if (ref) { n->next = ref; n->prev = ref->prev; if (ref->prev) ref->prev->next = n; else p->first = n; ref->prev = n; }
    else { n->prev = p->last; n->next = 0; if (p->last) p->last->next = n; else p->first = n; p->last = n; }
    set_depth(n, p->depth + 1);
}
static struct node *new_node(int type, const char *name) {
    struct node *n = ar_alloc(&D->ar, sizeof *n);
    if (!n) return 0;
    n->type = type; n->link = -1;
    if (type == N_ELEM) {
        char t[64]; strncpy0(t, name, sizeof t); lower(t);
        n->name = ardup(t, strlen(t));
        n->tag = tag_lookup(t, strlen(t));
    }
    return n;
}
static void text_of(struct node *e, char **buf, long *len, long *cap, int depth) {
    if (depth > 200) return;
    if (e->type == N_TEXT) {
        if (*len + e->tlen + 1 > *cap) { long nc = (*len + e->tlen + 1) * 2; char *b = realloc(*buf, nc); if (!b) return; *buf = b; *cap = nc; }
        memcpy(*buf + *len, e->text, e->tlen); *len += e->tlen; (*buf)[*len] = 0;
        return;
    }
    for (struct node *c = e->first; c; c = c->next) text_of(c, buf, len, cap, depth + 1);
}

/* ---------------- serialising (innerHTML / outerHTML) ---------------- */
struct sb { char *p; long n, cap; };
static void sb_add(struct sb *b, const char *s, long n) {
    if (n < 0) n = strlen(s);
    if (b->n + n + 1 > b->cap) { long nc = (b->n + n + 1) * 2; char *q = realloc(b->p, nc); if (!q) return; b->p = q; b->cap = nc; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_esc(struct sb *b, const char *s, long n, bool attr) {
    for (long i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_add(b, "&amp;", 5); else if (c == '<' && !attr) sb_add(b, "&lt;", 4); else if (c == '>' && !attr) sb_add(b, "&gt;", 4);
        else if (c == '"' && attr) sb_add(b, "&quot;", 6); else sb_add(b, &c, 1);
    }
}
static bool is_void(int t) { return t == T_BR || t == T_IMG || t == T_INPUT || t == T_HR || t == T_META || t == T_LINK || t == T_WBR || t == T_COL || t == T_AREA || t == T_BASE || t == T_PARAM || t == T_SOURCE || t == T_TRACK || t == T_EMBED; }
static void serialize(struct sb *b, struct node *e, bool outer, int depth) {
    if (depth > 200) return;
    if (e->type == N_TEXT) {
        bool raw = e->parent && (e->parent->tag == T_SCRIPT || e->parent->tag == T_STYLE);
        if (raw) sb_add(b, e->text, e->tlen); else sb_esc(b, e->text, e->tlen, false);
        return;
    }
    if (outer) {
        sb_add(b, "<", 1); sb_add(b, e->name ? e->name : "div", -1);
        for (int i = 0; i < e->nattr; i++) {
            sb_add(b, " ", 1); sb_add(b, e->attrs[i].name, -1);
            sb_add(b, "=\"", 2); sb_esc(b, e->attrs[i].value ? e->attrs[i].value : "", e->attrs[i].value ? strlen(e->attrs[i].value) : 0, true); sb_add(b, "\"", 1);
        }
        sb_add(b, ">", 1);
        if (is_void(e->tag)) return;
    }
    if ((e->tag == T_SCRIPT || e->tag == T_STYLE) && e->text && !e->first) sb_add(b, e->text, e->tlen);
    for (struct node *c = e->first; c; c = c->next) serialize(b, c, true, depth + 1);
    if (outer) { sb_add(b, "</", 2); sb_add(b, e->name ? e->name : "div", -1); sb_add(b, ">", 1); }
}

/* parse an HTML fragment into the page's memory; returns the first new node (siblings follow) */
static struct node *parse_fragment(const char *s, long n) {
    struct doc tmp; memset(&tmp, 0, sizeof tmp);
    tmp.ar = D->ar; strncpy0(tmp.base, D->base, sizeof tmp.base);
    char *b = malloc(n + 1); if (!b) return 0;
    memcpy(b, s, n); b[n] = 0;
    html_parse(&tmp, b, n);
    free(b);
    D->ar = tmp.ar;                                      /* the new nodes live in the page's arena */
    /* gather: anything the parser put in <head> (style, meta...), then the body's children */
    struct node *first = 0, *last = 0;
    struct node *from[2] = { tmp.head, tmp.body };
    for (int k = 0; k < 2; k++) {
        if (!from[k]) continue;
        for (struct node *c = from[k]->first, *nx; c; c = nx) {
            nx = c->next;
            c->parent = 0; c->prev = last; c->next = 0;
            if (last) last->next = c; else first = c;
            last = c;
        }
    }
    return first;
}
static void run_script_node(struct node *s);

/* ---------------- natives: the tree ---------------- */
#define NATIVE(name) static jsval name(js_rt *rt_, jsval self, int argc, jsval *argv, void *data)
NATIVE(n_type) { struct node *n = N(A(argc, argv, 0)); return js_int(!n ? 0 : n->type == N_TEXT ? 3 : 1); }
NATIVE(n_tag) { struct node *n = N(A(argc, argv, 0)); return n && n->name ? js_str(rt, n->name, -1) : js_str(rt, "", 0); }
NATIVE(n_parent) { struct node *n = N(A(argc, argv, 0)); return n ? W(n->parent) : js_null(); }
NATIVE(n_first) { struct node *n = N(A(argc, argv, 0)); return n ? W(n->first) : js_null(); }
NATIVE(n_last) { struct node *n = N(A(argc, argv, 0)); return n ? W(n->last) : js_null(); }
NATIVE(n_next) { struct node *n = N(A(argc, argv, 0)); return n ? W(n->next) : js_null(); }
NATIVE(n_prev) { struct node *n = N(A(argc, argv, 0)); return n ? W(n->prev) : js_null(); }
NATIVE(n_root) { return W(D->root); }
NATIVE(n_head) { return W(D->head); }
NATIVE(n_body) { return W(D->body); }
NATIVE(n_attr) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_null();
    char *k = T(A(argc, argv, 1), 0); lower(k);
    int i = find_attr(n, k); free(k);
    if (i < 0) return js_null();
    return S(n->attrs[i].value ? n->attrs[i].value : "", -1);
}
NATIVE(n_set_attr) {
    struct node *n = N(A(argc, argv, 0)); if (!n || n->type != N_ELEM) return js_undefined();
    char *k = T(A(argc, argv, 1), 0); lower(k);
    long vl; char *v = T(A(argc, argv, 2), &vl);
    set_attr(n, k, v, vl);
    bool init = !strcmp(k, "src") || !strcmp(k, "href") || !strcmp(k, "value") || !strcmp(k, "checked") || !strcmp(k, "type");
    free(k); free(v);
    web_dom_changed(init ? n : 0);
    return js_undefined();
}
NATIVE(n_remove_attr) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_undefined();
    char *k = T(A(argc, argv, 1), 0); lower(k);
    remove_attr(n, k); free(k);
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_attr_names) {
    struct node *n = N(A(argc, argv, 0));
    jsval arr = js_array(rt); HOLD(arr);
    if (n) for (int i = 0; i < n->nattr; i++) { jsval v = js_str(rt, n->attrs[i].name, -1); HOLD(v); js_set_index(rt, arr, i, v); DROP(v); }
    DROP(arr);
    return arr;
}
NATIVE(n_data) { struct node *n = N(A(argc, argv, 0)); return n && n->type == N_TEXT ? S(n->text, n->tlen) : js_null(); }
NATIVE(n_set_data) {
    struct node *n = N(A(argc, argv, 0)); if (!n || n->type != N_TEXT) return js_undefined();
    long l; char *t = T(A(argc, argv, 1), &l);
    n->text = ardup(t, l); n->tlen = l; free(t);
    web_text_changed(n->parent);
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_text_content) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_null();
    if ((n->tag == T_SCRIPT || n->tag == T_STYLE) && n->text && !n->first) return S(n->text, n->tlen);
    char *b = 0; long len = 0, cap = 0;
    text_of(n, &b, &len, &cap, 0);
    jsval r = S(b ? b : "", len);
    free(b);
    return r;
}
NATIVE(n_set_text_content) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_undefined();
    long l; char *t = T(A(argc, argv, 1), &l);
    if (n->type == N_TEXT) { n->text = ardup(t, l); n->tlen = l; }
    else {
        while (n->first) detach(n->first);
        if (n->tag == T_SCRIPT || n->tag == T_STYLE) { n->text = ardup(t, l); n->tlen = l; }
        else if (l) { struct node *tx = new_node(N_TEXT, 0); if (tx) { tx->text = ardup(t, l); tx->tlen = l; insert_before(n, tx, 0); } }
    }
    free(t);
    web_text_changed(n);
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_html) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_str(rt, "", 0);
    struct sb b = { 0, 0, 0 };
    serialize(&b, n, js_truthy(A(argc, argv, 1)), 0);
    jsval r = S(b.p ? b.p : "", b.n);
    free(b.p);
    return r;
}
NATIVE(n_set_html) {
    struct node *n = N(A(argc, argv, 0)); if (!n || n->type != N_ELEM) return js_undefined();
    long l; char *t = T(A(argc, argv, 1), &l);
    bool outer = js_truthy(A(argc, argv, 2));
    struct node *f = parse_fragment(t, l);
    free(t);
    if (outer) {
        struct node *p = n->parent;
        if (p) { for (struct node *c = f, *nx; c; c = nx) { nx = c->next; insert_before(p, c, n); web_dom_changed(c); } detach(n); }
    } else {
        while (n->first) detach(n->first);
        for (struct node *c = f, *nx; c; c = nx) { nx = c->next; insert_before(n, c, 0); web_dom_changed(c); }
    }
    web_text_changed(outer ? n->parent : n);
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_create) {
    char *t = T(A(argc, argv, 0), 0);
    struct node *n = new_node(N_ELEM, t[0] ? t : "div");
    free(t);
    return W(n);
}
NATIVE(n_create_text) {
    long l; char *t = T(A(argc, argv, 0), &l);
    struct node *n = new_node(N_TEXT, 0);
    if (n) { n->text = ardup(t, l); n->tlen = l; }
    free(t);
    return W(n);
}
static bool connected(struct node *n) { while (n->parent) n = n->parent; return n == D->root; }
static void scripts_in(struct node *n, int depth) {      /* run scripts that were just inserted into the page */
    if (depth > 100) return;
    if (n->type == N_ELEM && n->tag == T_SCRIPT) { run_script_node(n); return; }
    for (struct node *c = n->first; c; c = c->next) scripts_in(c, depth + 1);
}
NATIVE(n_insert) {
    struct node *p = N(A(argc, argv, 0)), *n = N(A(argc, argv, 1)), *ref = N(A(argc, argv, 2));
    if (!p || !n || contains(n, p)) return js_throw(rt, "Error", "HierarchyRequestError: can't insert a node inside itself");
    insert_before(p, n, ref);
    web_text_changed(p);
    web_dom_changed(n);
    if (connected(n)) scripts_in(n, 0);
    return W(n);
}
NATIVE(n_remove) { struct node *n = N(A(argc, argv, 0)); if (n) { struct node *p = n->parent; detach(n); web_text_changed(p); web_dom_changed(0); } return js_undefined(); }
NATIVE(n_clone) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_null();
    struct sb b = { 0, 0, 0 };
    if (n->type == N_TEXT) { struct node *t = new_node(N_TEXT, 0); if (t) { t->text = ardup(n->text, n->tlen); t->tlen = n->tlen; } return W(t); }
    if (js_truthy(A(argc, argv, 1))) { serialize(&b, n, true, 0); struct node *f = parse_fragment(b.p ? b.p : "", b.n); free(b.p); return W(f); }
    struct node *c = new_node(N_ELEM, n->name ? n->name : "div");
    if (c) for (int i = 0; i < n->nattr; i++) set_attr(c, n->attrs[i].name, n->attrs[i].value ? n->attrs[i].value : "", n->attrs[i].value ? strlen(n->attrs[i].value) : 0);
    return W(c);
}
static void query(struct node *e, const char *sel, int sl, jsval arr, long *cnt, bool all, int depth) {
    if (depth > 200) return;
    for (struct node *c = e->first; c && (all || !*cnt); c = c->next) {
        if (c->type != N_ELEM) continue;
        if (css_query_match(sel, sl, c)) js_set_index(rt, arr, (*cnt)++, W(c));
        if (all || !*cnt) query(c, sel, sl, arr, cnt, all, depth + 1);
    }
}
NATIVE(n_query) {
    struct node *root = N(A(argc, argv, 0)); if (!root) root = D->root;
    long sl; char *sel = T(A(argc, argv, 1), &sl);
    jsval arr = js_array(rt); long cnt = 0; HOLD(arr);
    query(root, sel, sl, arr, &cnt, js_truthy(A(argc, argv, 2)), 0);
    free(sel);
    DROP(arr);
    return arr;
}
NATIVE(n_matches) {
    struct node *n = N(A(argc, argv, 0)); if (!n || n->type != N_ELEM) return js_bool(false);
    long sl; char *sel = T(A(argc, argv, 1), &sl);
    bool m = css_query_match(sel, sl, n);
    free(sel);
    return js_bool(m);
}
static struct node *by_id(struct node *e, const char *id, int depth) {
    if (depth > 200) return 0;
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->id && !strcmp(c->id, id)) return c;
        struct node *r = by_id(c, id, depth + 1);
        if (r) return r;
    }
    return 0;
}
NATIVE(n_by_id) { char *id = T(A(argc, argv, 0), 0); struct node *n = by_id(D->root, id, 0); free(id); return W(n); }

/* ---------------- natives: form controls ---------------- */
NATIVE(n_value) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_str(rt, "", 0);
    struct fctl *fc = n->fc;
    if (!fc) { const char *v = node_attr(n, "value"); return S(v ? v : "", -1); }
    if (fc->type == FC_SELECT) {
        if (fc->sel < 0 || fc->sel >= fc->nopt) return js_str(rt, "", 0);
        const char *v = node_attr(fc->opts[fc->sel], "value");
        if (v) return S(v, -1);
        char t[256]; form_option_text(fc->opts[fc->sel], t, sizeof t); return S(t, -1);
    }
    if (fc->type == FC_CHECKBOX || fc->type == FC_RADIO) { const char *v = node_attr(n, "value"); return S(v ? v : "on", -1); }
    return S(fc->val ? fc->val : "", fc->len);
}
NATIVE(n_set_value) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_undefined();
    long l; char *t = T(A(argc, argv, 1), &l);
    struct fctl *fc = n->fc;
    if (fc && fc->type == FC_SELECT) {
        for (int i = 0; i < fc->nopt; i++) { const char *v = node_attr(fc->opts[i], "value"); char tx[256]; if (!v) { form_option_text(fc->opts[i], tx, sizeof tx); v = tx; } if (!strcmp(v, t)) { fc->sel = i; break; } }
    } else if (fc && fc->type != FC_CHECKBOX && fc->type != FC_RADIO) {
        if (l + 1 > fc->cap) { char *nv = malloc(l + 64); if (nv) { free(fc->val); fc->val = nv; fc->cap = l + 64; } }
        if (fc->val && l < fc->cap) { memcpy(fc->val, t, l); fc->val[l] = 0; fc->len = l; fc->cur = l; }
    } else set_attr(n, "value", t, l);
    free(t);
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_checked) { struct node *n = N(A(argc, argv, 0)); return js_bool(n && n->fc ? n->fc->checked : n && node_attr(n, "checked")); }
NATIVE(n_set_checked) {
    struct node *n = N(A(argc, argv, 0)); if (!n || !n->fc) return js_undefined();
    n->fc->checked = js_truthy(A(argc, argv, 1));
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_sel_index) { struct node *n = N(A(argc, argv, 0)); return js_int(n && n->fc && n->fc->type == FC_SELECT ? n->fc->sel : -1); }
NATIVE(n_set_sel_index) {
    struct node *n = N(A(argc, argv, 0)); if (!n || !n->fc || n->fc->type != FC_SELECT) return js_undefined();
    int i = (int)js_to_int(rt, A(argc, argv, 1));
    if (i >= -1 && i < n->fc->nopt) n->fc->sel = i;
    web_dom_changed(0);
    return js_undefined();
}
NATIVE(n_disabled) { struct node *n = N(A(argc, argv, 0)); return js_bool(n && n->fc ? n->fc->disabled : n && node_attr(n, "disabled")); }
NATIVE(n_set_disabled) {
    struct node *n = N(A(argc, argv, 0)); if (!n) return js_undefined();
    bool d = js_truthy(A(argc, argv, 1));
    if (n->fc) n->fc->disabled = d;
    if (d) set_attr(n, "disabled", "", 0); else remove_attr(n, "disabled");
    web_dom_changed(0);
    return js_undefined();
}

/* ---------------- natives: the window ---------------- */
NATIVE(n_rect) {
    struct node *n = N(A(argc, argv, 0));
    int x = 0, y = 0, w = 0, h = 0;
    if (n) web_node_rect(n, &x, &y, &w, &h);
    jsval a = js_array(rt); HOLD(a);
    js_set_index(rt, a, 0, js_int(x)); js_set_index(rt, a, 1, js_int(y)); js_set_index(rt, a, 2, js_int(w)); js_set_index(rt, a, 3, js_int(h));
    DROP(a);
    return a;
}
NATIVE(n_display) { struct node *n = N(A(argc, argv, 0)); return js_int(n && n->st ? n->st->display : -1); }
NATIVE(n_viewport) {
    int w, h, s, dh; web_viewport(&w, &h, &s, &dh);
    jsval a = js_array(rt); HOLD(a);
    js_set_index(rt, a, 0, js_int(w)); js_set_index(rt, a, 1, js_int(h)); js_set_index(rt, a, 2, js_int(s)); js_set_index(rt, a, 3, js_int(dh));
    DROP(a);
    return a;
}
NATIVE(n_scroll_to) { web_scroll_to((int)js_to_int(rt, A(argc, argv, 0))); return js_undefined(); }
NATIVE(n_title) { return S(D->title, -1); }
NATIVE(n_set_title) { char *t = T(A(argc, argv, 0), 0); strncpy0(D->title, t, sizeof D->title); web_set_title(D->title); free(t); return js_undefined(); }
NATIVE(n_url) { char u[600]; web_page_url(u, sizeof u); return js_str(rt, u, -1); }
NATIVE(n_base) { return js_str(rt, D->base, -1); }
NATIVE(n_navigate) { char *u = js_to_cstring(rt, A(argc, argv, 0)); if (u) { char abs[600]; resolve_url(D->base, u, abs, sizeof abs); web_navigate(abs); free(u); } return js_undefined(); }
NATIVE(n_alert) { char *t = T(A(argc, argv, 0), 0); web_message("The page says", t); free(t); return js_undefined(); }
NATIVE(n_resolve) { char *u = js_to_cstring(rt, A(argc, argv, 0)); char abs[600] = ""; if (u) { resolve_url(D->base, u, abs, sizeof abs); free(u); } return js_str(rt, abs, -1); }

/* ---------------- natives: timers ---------------- */
#define MAXT 256
struct timer { jsval fn; uint64_t due; long every; int id; bool used; };
static struct timer timers[MAXT];
static int next_tid = 1;
NATIVE(n_set_timer) {
    jsval fn = A(argc, argv, 0);
    if (!js_is_function(fn)) return js_int(0);
    long ms = (long)js_to_int(rt, A(argc, argv, 1)); if (ms < 0) ms = 0;
    bool rep = js_truthy(A(argc, argv, 2));
    if (rep && ms < 10) ms = 10;
    for (int i = 0; i < MAXT; i++) if (!timers[i].used) {
        timers[i].used = true; timers[i].fn = fn; timers[i].due = sys_time() + ms; timers[i].every = rep ? ms : 0; timers[i].id = next_tid++;
        return js_int(timers[i].id);
    }
    return js_int(0);
}
NATIVE(n_clear_timer) {
    int id = (int)js_to_int(rt, A(argc, argv, 0));
    for (int i = 0; i < MAXT; i++) if (timers[i].used && timers[i].id == id) { timers[i].used = false; timers[i].fn = js_undefined(); }
    return js_undefined();
}
long js_timer_wait(void) {
    if (!page_on) return -1;
    long best = -1; uint64_t now = sys_time();
    for (int i = 0; i < MAXT; i++) if (timers[i].used) { long w = timers[i].due > now ? (long)(timers[i].due - now) : 0; if (best < 0 || w < best) best = w; }
    return best;
}
static void report(const char *where) {
    const char *e = js_error_text(rt);
    char *t = malloc(strlen(e) + 1); if (!t) return;
    strncpy0(t, e, strlen(e) + 1); utf8_to_cp1252(t, strlen(t) + 1);
    char pre[80]; ksnprintf(pre, sizeof pre, "[error] %s: ", where);
    con_add(pre, t); free(t);
}
static void after_task(void) { script_end(); script_begin(); js_run_jobs(rt); script_end(); }
void js_run_timers(void) {
    if (!page_on) return;
    uint64_t now = sys_time();
    for (int i = 0; i < MAXT && page_on; i++) {
        if (!timers[i].used || timers[i].due > now) continue;
        jsval fn = timers[i].fn;
        if (timers[i].every) timers[i].due = now + timers[i].every; else { timers[i].used = false; }
        jsval keep = fn; js_root(rt, &keep);
        if (!timers[i].every) timers[i].fn = js_undefined();
        if (page_cpu_ms() > PAGE_CPU_MS) { scripts_off("this page's scripts kept the processor busy for 20 seconds, so they were turned off"); break; }
        script_begin();
        jsval res;
        if (js_call(rt, keep, js_undefined(), 0, 0, &res) < 0) report("timer");
        js_unroot(rt, &keep);
        after_task();
    }
}

/* ---------------- natives: network and storage ---------------- */
NATIVE(n_http) {
    char *method = js_to_cstring(rt, A(argc, argv, 0)), *u = js_to_cstring(rt, A(argc, argv, 1));
    char *body = js_is_undefined(A(argc, argv, 2)) || js_is_null(A(argc, argv, 2)) ? 0 : js_to_cstring(rt, A(argc, argv, 2));
    char *hdr = js_is_string(A(argc, argv, 3)) ? js_to_cstring(rt, A(argc, argv, 3)) : 0;
    char abs[600]; resolve_url(D->base, u ? u : "", abs, sizeof abs);
    char *out = 0, ctype[128] = "", final[600] = ""; int status = 0;
    long n = web_http(method ? method : "GET", abs, body, body ? strlen(body) : 0, hdr, &out, &status, ctype, sizeof ctype, final, sizeof final);
    free(method); free(u); free(body); free(hdr);
    jsval a = js_array(rt); HOLD(a);
    js_set_index(rt, a, 0, js_int(n < 0 ? 0 : status));
    set_held(a, 1, js_str(rt, out && n > 0 ? out : "", n > 0 ? n : 0));        /* the body as sent (UTF-8 for UTF-8 pages) */
    set_held(a, 2, js_str(rt, ctype, -1));
    set_held(a, 3, js_str(rt, final[0] ? final : abs, -1));
    set_held(a, 4, js_str(rt, n < 0 ? http_strerror(n) : "", -1));
    free(out);
    DROP(a);
    return a;
}
static void storage_path(char *p, int n) {
    char host[128] = ""; const char *u = D->base, *s = u;
    for (const char *q = u; *q; q++) if (q[0] == '/' && q[1] == '/') { s = q + 2; break; }
    int k = 0; for (; *s && *s != '/' && *s != ':' && k < 120; s++) host[k++] = (*s >= 'A' && *s <= 'Z') ? *s + 32 : (*s == '.' || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-') ? *s : '_';
    host[k] = 0;
    ksnprintf(p, n, "/home/.web/storage/%s.json", host[0] ? host : "local");
}
NATIVE(n_storage_load) {
    char p[200]; storage_path(p, sizeof p);
    static char buf[65536]; long n = fs_read(p, buf, 0, sizeof buf - 1);
    if (n <= 0) return js_str(rt, "{}", 2);
    buf[n] = 0; return js_str(rt, buf, n);
}
NATIVE(n_storage_save) {
    char p[200]; storage_path(p, sizeof p);
    char *s = js_to_cstring(rt, A(argc, argv, 0)); if (!s) return js_undefined();
    if (strlen(s) < 65000) { fs_mkdir("/home/.web"); fs_mkdir("/home/.web/storage"); fs_write(p, s, strlen(s)); }
    free(s);
    return js_undefined();
}
NATIVE(n_compile) {                                       /* on... attributes: function(event) { <code> } */
    char *code = js_to_cstring(rt, A(argc, argv, 0)); if (!code) return js_null();
    long n = strlen(code);
    char *src = malloc(n + 64); if (!src) { free(code); return js_null(); }
    ksnprintf(src, n + 64, "(function (event) {\n%s\n})", code);
    free(code);
    jsval f;
    int r = js_eval(rt, src, strlen(src), "event handler", &f);
    free(src);
    if (r < 0) { report("event handler"); return js_null(); }
    return f;
}
NATIVE(n_set_protos) {
    proto_elem = A(argc, argv, 0); proto_text = A(argc, argv, 1); proto_comment = A(argc, argv, 2); api = A(argc, argv, 3);
    return js_undefined();
}

/* ---------------- running scripts ---------------- */
static bool js_type_ok(struct node *s) {
    const char *t = node_attr(s, "type");
    if (node_attr(s, "nomodule")) return true;            /* we don't do modules, so run the fallback */
    if (!t || !t[0]) return true;
    char l[64]; strncpy0(l, t, sizeof l); lower(l);
    return !strcmp(l, "text/javascript") || !strcmp(l, "application/javascript") || !strcmp(l, "text/ecmascript") ||
           !strcmp(l, "application/ecmascript") || !strcmp(l, "text/jscript") || !strcmp(l, "application/x-javascript");
}
static void run_code(const char *code, long n, const char *name) {
    script_begin();
    jsval res;
    if (js_eval(rt, code, n, name, &res) < 0) report(name);
    after_task();
}
static void run_script_node(struct node *s) {
    if (!page_on || !js_type_ok(s)) return;
    if (node_attr(s, "data-axiom-ran")) return;
    set_attr(s, "data-axiom-ran", "1", 1);
    const char *src = node_attr(s, "src");
    if (sys_time() > page_deadline) {
        if (!skip_said) { skip_said = true; con_add("[skipped] ", "the page's scripts took too long (over 15 seconds); the rest were skipped"); }
        return;
    }
    if (src && src[0]) {
        char u[600]; resolve_url(D->base, src, u, sizeof u);
        char *t = 0, ct[96], fin[600]; int st = 0;           /* scripts are run as sent (UTF-8), not converted like page text */
        long n = web_http("GET", u, 0, 0, 0, &t, &st, ct, sizeof ct, fin, sizeof fin);
        if (n > 1500000) { char m[700]; ksnprintf(m, sizeof m, "%s is too big to run here (%ld KB)", u, n / 1024); con_add("[skipped] ", m); free(t); }
        else if (n >= 0 && t && st < 400) { run_code(t, n, u); free(t); }
        else { char m[700]; ksnprintf(m, sizeof m, "couldn't load %s", u); con_add("[error] ", m); }
        return;
    }
    char *b = 0; long len = 0, cap = 0;
    if (s->text && !s->first) { b = malloc(s->tlen + 1); if (b) { memcpy(b, s->text, s->tlen); b[s->tlen] = 0; len = s->tlen; } }
    else text_of(s, &b, &len, &cap, 0);
    if (!b) return;
    /* inline scripts were turned into Windows-1252 with the page: back to UTF-8 for the engine */
    char *u8 = malloc(len * 3 + 1);
    if (u8) { long k = cp1252_to_utf8(b, len, u8, len * 3 + 1); run_code(u8, k, "inline script"); free(u8); }
    free(b);
}
static void collect_scripts(struct node *e, struct node **list, int *n, int max, int depth) {
    if (depth > 200) return;
    for (struct node *c = e->first; c && *n < max; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_SCRIPT) { list[(*n)++] = c; continue; }
        collect_scripts(c, list, n, max, depth + 1);
    }
}
static jsval call_api(const char *fname, int argc, jsval *argv) {
    jsval f = js_get(rt, api, fname), res = js_undefined();
    if (js_is_function(f)) { script_begin(); if (js_call(rt, f, api, argc, argv, &res) < 0) { report(fname); res = js_undefined(); } script_end(); }
    return res;
}

static const struct { const char *name; js_native fn; int n; } natives[] = {
    { "type", n_type, 1 }, { "tag", n_tag, 1 }, { "parent", n_parent, 1 }, { "first", n_first, 1 }, { "last", n_last, 1 },
    { "next", n_next, 1 }, { "prev", n_prev, 1 }, { "root", n_root, 0 }, { "head", n_head, 0 }, { "body", n_body, 0 },
    { "attr", n_attr, 2 }, { "setAttr", n_set_attr, 3 }, { "removeAttr", n_remove_attr, 2 }, { "attrNames", n_attr_names, 1 },
    { "data", n_data, 1 }, { "setData", n_set_data, 2 }, { "textContent", n_text_content, 1 }, { "setTextContent", n_set_text_content, 2 },
    { "html", n_html, 2 }, { "setHtml", n_set_html, 3 }, { "create", n_create, 1 }, { "createText", n_create_text, 1 },
    { "insert", n_insert, 3 }, { "remove", n_remove, 1 }, { "clone", n_clone, 2 }, { "query", n_query, 3 }, { "matches", n_matches, 2 },
    { "byId", n_by_id, 1 }, { "value", n_value, 1 }, { "setValue", n_set_value, 2 }, { "checked", n_checked, 1 },
    { "setChecked", n_set_checked, 2 }, { "selIndex", n_sel_index, 1 }, { "setSelIndex", n_set_sel_index, 2 },
    { "disabled", n_disabled, 1 }, { "setDisabled", n_set_disabled, 2 }, { "rect", n_rect, 1 }, { "display", n_display, 1 },
    { "viewport", n_viewport, 0 }, { "scrollTo", n_scroll_to, 1 }, { "title", n_title, 0 }, { "setTitle", n_set_title, 1 },
    { "url", n_url, 0 }, { "base", n_base, 0 }, { "navigate", n_navigate, 1 }, { "alert", n_alert, 1 }, { "resolve", n_resolve, 1 },
    { "setTimer", n_set_timer, 3 }, { "clearTimer", n_clear_timer, 1 }, { "http", n_http, 4 },
    { "storageLoad", n_storage_load, 0 }, { "storageSave", n_storage_save, 1 }, { "compile", n_compile, 1 }, { "setProtos", n_set_protos, 4 },
};

bool js_page_active(void) { return page_on; }

void js_page_start(struct doc *d) {
    js_page_end();
    D = d;
    if (!d || !d->root) return;
    struct node *list[512]; int ns = 0;
    collect_scripts(d->root, list, &ns, 512, 0);
    bool handlers = false;                                /* inline on... attributes need scripts too */
    if (!ns) {
        /* cheap check for onclick= etc. */
        struct node *st[64]; int sp = 0; st[sp++] = d->root;
        while (sp && !handlers) { struct node *e = st[--sp]; for (struct node *c = e->first; c; c = c->next) if (c->type == N_ELEM) { for (int i = 0; i < c->nattr; i++) if (c->attrs[i].name[0] == 'o' && c->attrs[i].name[1] == 'n') handlers = true; if (sp < 64) st[sp++] = c; } }
        if (!handlers) return;                           /* no scripts: no engine needed */
    }
    rt = js_new(&host);
    if (!rt) return;
    page_on = true;
    conlen = 0; conlog[0] = 0;
    wrappers = js_array(rt); nwrappers = 0;
    js_root(rt, &wrappers); js_root(rt, &proto_elem); js_root(rt, &proto_text); js_root(rt, &proto_comment); js_root(rt, &api);
    for (int i = 0; i < MAXT; i++) { timers[i].used = false; timers[i].fn = js_undefined(); js_root(rt, &timers[i].fn); }
    jsval g = js_global(rt), dd = js_object(rt);
    js_root(rt, &dd);
    for (unsigned i = 0; i < sizeof natives / sizeof natives[0]; i++) {
        jsval f = js_function(rt, natives[i].name, natives[i].fn, natives[i].n, 0);
        HOLD(f); js_define(rt, dd, natives[i].name, f, false); DROP(f);
    }
    js_define(rt, g, "__d", dd, false);
    js_unroot(rt, &dd);
    page_deadline = sys_time() + 15000; skip_said = false; page_cycles = 0; cyc_begin = 0;
    run_code(dom_prelude, sizeof dom_prelude - 1, "web prelude");
    call_api("start", 0, 0);
    /* the page's scripts run one at a time from Web's main loop (js_page_step), so the window keeps
     * answering - scrolling, clicks, Esc, closing - while a big site downloads and runs dozens of them */
    for (int i = 0; i < ns; i++) squeue[i] = list[i];
    nsq = ns; sqpos = 0; sq_active = true;
}

bool js_scripts_pending(void) { return sq_active; }
void dom_check_soon(void) { if (rt && page_on) js_check_soon(rt); }
void js_stop_scripts(void) { if (sq_active && page_on) scripts_off("stopped with Esc"); }
/* run the next of the page's scripts; false when there are none left (then "loaded" fires) */
bool js_page_step(char *status, int n) {
    if (!sq_active) return false;
    if (page_on && sqpos < nsq) {
        struct node *s = squeue[sqpos++];
        const char *src = node_attr(s, "src");
        ksnprintf(status, n, "Running the page's scripts: %d of %d%s%s  (Esc stops them)", sqpos, nsq, src ? " - " : "", src ? src : "");
        run_script_node(s);
        return true;
    }
    sq_active = false;
    if (page_on) { call_api("loaded", 0, 0); after_task(); }  /* DOMContentLoaded, then load */
    return false;
}

void js_page_end(void) {
    sq_active = false; nsq = sqpos = 0;
    if (!rt) { page_on = false; return; }
    page_on = false;
    for (int i = 0; i < MAXT; i++) { js_unroot(rt, &timers[i].fn); timers[i].used = false; }
    js_unroot(rt, &wrappers); js_unroot(rt, &proto_elem); js_unroot(rt, &proto_text); js_unroot(rt, &proto_comment); js_unroot(rt, &api);
    if (D) {                                              /* forget the nodes' JS objects */
        struct node *st[256]; int sp = 0; st[sp++] = D->root;
        while (sp) { struct node *e = st[--sp]; e->jsw = 0; for (struct node *c = e->first; c; c = c->next) if (sp < 256) st[sp++] = c; else c->jsw = 0; }
    }
    js_free(rt);
    rt = 0; D = 0;
}

bool js_event(struct node *target, const char *type, int x, int y, int key) {
    if (!page_on || !target) return false;
    jsval args[5] = { js_undefined(), js_undefined(), js_int(x), js_int(y), js_int(key) };
    for (int i = 0; i < 5; i++) js_root(rt, &args[i]);
    args[0] = W(target); args[1] = js_str(rt, type, -1);
    jsval r = call_api("dispatch", 5, args);
    for (int i = 0; i < 5; i++) js_unroot(rt, &args[i]);
    after_task();
    return js_truthy(r);
}

void js_eval_url(const char *code) {
    if (!page_on) return;
    run_code(code, strlen(code), "javascript: link");
}
