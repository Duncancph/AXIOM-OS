/* Web: layout - block boxes, lines of inline content, lists, tables and simple flex rows, into a display list. */
#include "web.h"
#include "../../../common/webfont_data.h"

/* ---------------- fonts ---------------- */
const struct font *web_font(int px, bool bold, bool italic) {
    const struct font *const *set = bold ? wf_b_set : wf_r_set; const int *sz = bold ? wf_b_sizes : wf_r_sizes; int n = 7;
    if (italic && !bold && px >= 13 && px <= 19) { set = wf_i_set; sz = wf_i_sizes; n = 3; }
    int best = 0;
    for (int i = 1; i < n; i++) {
        int d = sz[i] - px, bd = sz[best] - px;
        if (d < 0) d = -d;
        if (bd < 0) bd = -bd;
        if (d <= bd) best = i;
    }
    return set[best];
}

/* ---------------- display list ---------------- */
static struct layout *L;
static struct doc *DOC;
static int gen;
static int depth;
static struct node *canvas_node;
static int last_base; static int nflush;
static int table_depth, flex_depth;
#define MAX_ITEMS 400000
#define MAX_DEPTH 90
static struct ditem dummy_item;
static struct node *cur_src;           /* the element being laid out: recorded in each display item */

static int push(int kind) {
    if (L->nit >= L->cap) {
        if (L->cap >= MAX_ITEMS) return -1;
        int nc = L->cap ? L->cap * 2 : 4096;
        struct ditem *n = realloc(L->it, nc * sizeof *n);
        if (!n) return -1;
        L->it = n; L->cap = nc;
    }
    struct ditem *d = &L->it[L->nit];
    memset(d, 0, sizeof *d);
    d->kind = kind; d->link = -1; d->src = cur_src;
    return L->nit++;
}
static struct ditem *IT(int i) { return i >= 0 && i < L->nit ? &L->it[i] : &dummy_item; }
static long tp_add(const char *s, long n) {
    if (L->tlen + n + 1 > L->tcap) {
        long nc = L->tcap ? L->tcap * 2 : 65536;
        while (nc < L->tlen + n + 1) nc *= 2;
        if (nc > (64l << 20)) return -1;
        char *p = realloc(L->tp, nc);
        if (!p) return -1;
        L->tp = p; L->tcap = nc;
    }
    memcpy(L->tp + L->tlen, s, n);
    long o = L->tlen; L->tlen += n;
    return o;
}

/* ---------------- text measuring ---------------- */
static int tchar(int c, int tt, bool *word_start) {
    unsigned u = (unsigned char)c;
    bool ws = *word_start;
    *word_start = c == ' ';
    if (tt == TT_UPPER || (tt == TT_CAP && ws)) {
        if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7)) return u - 32;
        if (u == 0x9A || u == 0x9C || u == 0x9E) return u - 16;
        if (u == 0xFF) return 0x9F;
    } else if (tt == TT_LOWER) {
        if ((u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7)) return u + 32;
        if (u == 0x8A || u == 0x8C || u == 0x8E) return u + 16;
        if (u == 0x9F) return 0xFF;
    }
    return u;
}
static int glyph_w(const struct font *f, int c) { unsigned u = (unsigned char)c; if (u < 32 || u == 127) u = '?'; return f->g[u - 32].adv; }
static int text_w(const struct style *st, const char *s, int n) {
    if (st->mono) return n * MONO_W;
    const struct font *f = web_font(st->fsize, st->bold, st->italic);
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph_w(f, s[i]);
    return w;
}
static int space_w(const struct style *st) { return st->mono ? MONO_W : glyph_w(web_font(st->fsize, st->bold, st->italic), ' '); }

/* ascent/descent of a style's line box (with its line-height), and of its glyphs */
static void metrics(const struct style *st, int *asc, int *desc, int *gasc, int *gdesc) {
    int a, d;
    if (st->mono) { a = 13; d = 3; if (st->fsize > 16) { a += (st->fsize - 16) / 2; d += (st->fsize - 16) / 2; } }
    else { const struct font *f = web_font(st->fsize, st->bold, st->italic); a = f->ascent; d = f->descent; if (d < 2) d = 2; }
    if (gasc) { *gasc = a; *gdesc = d; }
    int lh = st->lh_num ? st->fsize * st->lh_num / 100 : st->lh;
    if (lh > 0) {
        int min = st->mono ? 14 : st->fsize;
        if (lh < min) lh = min;
        int extra = lh - (a + d);
        a += extra / 2; d += extra - extra / 2;
    }
    *asc = a; *desc = d;
}

static bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

/* ---------------- lengths ---------------- */
static int rlen(struct len l, int base) {
    if (l.t == L_PX) return l.v;
    if (l.t == L_PCT) return (int)((long)base * l.v / 10000);
    return 0;
}
static int bwid(const struct style *st, int i) { return st->bs[i] ? st->bw[i] : 0; }
static int hextra(const struct style *st, int cbw) {   /* horizontal padding + border + margins */
    int m = 0;
    for (int i = 1; i <= 3; i += 2) m += rlen(st->p[i], cbw) + bwid(st, i) + (st->m[i].t == L_AUTO ? 0 : rlen(st->m[i], cbw));
    return m;
}

/* ---------------- replaced elements: images and form controls ---------------- */
static int attr_px(const struct node *n, const char *a) {
    const char *v = node_attr(n, a);
    if (!v || v[0] < '0' || v[0] > '9') return -1;
    int k = 0; while (*v >= '0' && *v <= '9' && k < 100000) k = k * 10 + (*v++ - '0');
    if (*v == '%') return -1;
    return k;
}
static void img_size(struct node *n, int cbw, int *w, int *h) {
    const struct style *st = n->st;
    int iw = 0, ih = 0;
    if (n->img && n->img->w > 0 && n->img->h > 0) { iw = n->img->w; ih = n->img->h; }
    int W = -1, H = -1;
    if (st->w.t == L_PX) W = st->w.v; else if (st->w.t == L_PCT) W = rlen(st->w, cbw); else W = attr_px(n, "width");
    if (st->h.t == L_PX) H = st->h.v; else H = attr_px(n, "height");
    if (W >= 0 && H < 0) H = iw ? (int)((long)W * ih / iw) : -1;
    else if (H >= 0 && W < 0) W = ih ? (int)((long)H * iw / ih) : -1;
    if (W < 0 && H < 0 && iw) { W = iw; H = ih; }
    if (W < 0 || H < 0) {                                  /* not known yet: room for the alt text */
        const char *alt = node_attr(n, "alt");
        int tw = alt && alt[0] ? text_w(st, alt, strlen(alt) > 60 ? 60 : strlen(alt)) + 26 : 20;
        if (W < 0) W = tw > 400 ? 400 : tw;
        if (H < 0) H = alt && alt[0] ? 22 : 20;
    }
    int maxw = st->maxw.t == L_PX || st->maxw.t == L_PCT ? rlen(st->maxw, cbw) : -1;
    if (maxw >= 0 && W > maxw) { if (W) H = (int)((long)H * maxw / W); W = maxw; }
    int room = cbw - hextra(st, cbw);
    if (room > 16 && W > room) { if (W) H = (int)((long)H * room / W); W = room; }     /* scale down to fit */
    if (W > 8000) W = 8000;
    if (H > 20000) H = 20000;
    if (W < 0) W = 0;
    if (H < 0) H = 0;
    *w = W; *h = H;
}

void form_option_text(struct node *opt, char *out, int max) {
    int k = 0; bool sp = false;
    for (struct node *t = opt->first; t; t = t->next) {
        if (t->type != N_TEXT) continue;
        for (int i = 0; i < t->tlen && k < max - 1; i++) {
            char c = t->text[i];
            if (is_ws(c)) { sp = k > 0; continue; }
            if (sp && k < max - 2) out[k++] = ' ';
            sp = false; out[k++] = c;
        }
    }
    out[k] = 0;
}

static const struct style *ctl_style(void) {
    static struct style s; s.fsize = 14; s.vis = 1;
    return &s;
}
static void control_size(struct node *n, int cbw, int *w, int *h, int *base) {
    struct fctl *fc = n->fc;
    const struct style *st = n->st;
    int W = 150, H = 26;
    const struct style *cs = ctl_style();
    if (!fc) { *w = 0; *h = 0; *base = 0; return; }
    if (fc->type == FC_MEDIA) {                          /* <video>/<audio>: its own size, scaled to the width the page gives it */
        int bw, bh; media_box(n, &bw, &bh);
        W = bw; H = bh;
        if (st->w.t == L_PX) W = st->w.v; else if (st->w.t == L_PCT) W = rlen(st->w, cbw) - hextra(st, cbw);
        int maxw = st->maxw.t == L_PX || st->maxw.t == L_PCT ? rlen(st->maxw, cbw) : -1;
        if (maxw >= 0 && W > maxw) W = maxw;
        if (W > cbw - hextra(st, cbw) && cbw > 60) W = cbw - hextra(st, cbw);
        if (W < 20) W = 20;
        if (n->tag == T_VIDEO) {
            if (st->h.t == L_PX && st->h.v > 0) H = st->h.v;
            else if (W != bw && bw > 0) H = (int)((long)bh * W / bw);
        }
        if (H < 20) H = 20;
        *w = W; *h = H; *base = n->tag == T_AUDIO ? H / 2 + 6 : H;
        return;
    }
    switch (fc->type) {
    case FC_CHECKBOX: case FC_RADIO: W = 15; H = 15; break;
    case FC_SUBMIT: case FC_RESET: case FC_BUTTON: case FC_IMAGE: case FC_FILE:
        W = text_w(cs, fc->label ? fc->label : "", fc->label ? strlen(fc->label) : 0) + 26; if (W < 40) W = 40; break;
    case FC_SELECT: {
        int mw = 30;
        for (int i = 0; i < fc->nopt; i++) { char t[120]; form_option_text(fc->opts[i], t, sizeof t); int tw = text_w(cs, t, strlen(t)); if (tw > mw) mw = tw; }
        W = mw + 34; if (W > 400) W = 400; break;
    }
    case FC_TEXTAREA: W = (fc->cols > 0 ? fc->cols : 20) * MONO_W + 14; H = (fc->rows > 0 ? fc->rows : 2) * MONO_H + 10; break;
    case FC_HIDDEN: W = 0; H = 0; break;
    default: W = (fc->size > 0 ? fc->size : 20) * 7 + 14; break;
    }
    if (fc->type != FC_CHECKBOX && fc->type != FC_RADIO) {
        if (st->w.t == L_PX) W = st->w.v - (st->bsizing ? 0 : 0); else if (st->w.t == L_PCT) W = rlen(st->w, cbw) - hextra(st, cbw);
        if (st->h.t == L_PX && st->h.v >= 16 && st->h.v < 600) H = st->h.v;
        int maxw = st->maxw.t == L_PX || st->maxw.t == L_PCT ? rlen(st->maxw, cbw) : -1;
        if (maxw >= 0 && W > maxw) W = maxw;
        if (W > cbw - hextra(st, cbw) && cbw > 60) W = cbw - hextra(st, cbw);
        if (W < 20) W = 20;
    }
    *w = W; *h = H;
    *base = fc->type == FC_CHECKBOX || fc->type == FC_RADIO ? 13 : fc->type == FC_TEXTAREA ? 17 : H / 2 + 5;
}
static bool is_control(const struct node *n) { return n->tag == T_INPUT || n->tag == T_TEXTAREA || n->tag == T_SELECT || n->tag == T_BUTTON ||
                                                     ((n->tag == T_VIDEO || n->tag == T_AUDIO) && n->fc); }

/* ---------------- intrinsic widths (min-content, max-content), cached per layout ---------------- */
static void intrinsic(struct node *e, int cbw, int *mn, int *mx);
static void table_intrinsic(struct node *t, int cbw, int *mn, int *mx);

static void text_intrinsic(const struct style *st, const char *s, int n, int *line, int *mn, int *mx) {
    bool pre = st->ws == WS_PRE || st->ws == WS_PREWRAP, nowrap = st->ws == WS_NOWRAP || st->ws == WS_PRE;
    const struct font *f = st->mono ? 0 : web_font(st->fsize, st->bold, st->italic);
    int sw = f ? glyph_w(f, ' ') : MONO_W;
    int word = 0; bool ws_pending = false, wstart = true;
    for (int i = 0; i <= n; i++) {
        char c = i < n ? s[i] : ' ';
        if (pre && c == '\n') {
            *line += word; if (word > *mn && !nowrap) *mn = word; word = 0;
            if (*line > *mx) *mx = *line;
            if (nowrap && *line > *mn) *mn = *line;
            *line = 0; continue;
        }
        if (is_ws(c)) {
            if (word) {
                *line += word; if (word > *mn) *mn = word; word = 0;
                if (nowrap && *line > *mn) *mn = *line;
            }
            if (pre) { if (i < n) *line += c == '\t' ? sw * 4 : sw; }
            else if (!ws_pending && *line && i < n) *line += sw;      /* one space for a run of white space */
            ws_pending = true;
            continue;
        }
        ws_pending = false;
        int ch = tchar(c, st->tt, &wstart);
        word += f ? glyph_w(f, ch) : MONO_W;
    }
    if (*line > *mx) *mx = *line;
}

static void inline_intrinsic(struct node *e, int cbw, int *line, int *mn, int *mx, int d) {
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type == N_TEXT) { text_intrinsic(e->st, c->text, c->tlen, line, mn, mx); continue; }
        const struct style *st = c->st;
        if (!st || st->display == D_NONE) continue;
        int dsp = st->display;
        if (c->tag == T_BR) { if (*line > *mx) *mx = *line; *line = 0; continue; }
        if (dsp == D_INLINE && c->tag != T_IMG && !is_control(c) && d < MAX_DEPTH) {
            int ex = hextra(st, cbw);
            *line += ex;
            inline_intrinsic(c, cbw, line, mn, mx, d + 1);
            continue;
        }
        int cmn, cmx;
        intrinsic(c, cbw, &cmn, &cmx);
        if (dsp == D_INLINE || dsp == D_INLINE_BLOCK || dsp == D_INLINE_TABLE || dsp == D_INLINE_FLEX) {
            *line += cmx; if (cmn > *mn) *mn = cmn;
        } else {
            if (*line > *mx) *mx = *line;
            *line = 0;
            if (cmx > *mx) *mx = cmx;
            if (cmn > *mn) *mn = cmn;
        }
    }
}

static void intrinsic(struct node *e, int cbw, int *mn, int *mx) {
    if (e->iw_gen == gen) { *mn = e->minw; *mx = e->maxw; return; }
    const struct style *st = e->st;
    int a = 0, b = 0;
    if (!st || st->display == D_NONE) { *mn = *mx = 0; return; }
    int ex = hextra(st, cbw);
    if (e->tag == T_IMG) { int w, h; img_size(e, 100000, &w, &h); a = b = w + ex; if (st->maxw.t == L_PCT || st->w.t == L_PCT) a = 0; }
    else if (is_control(e)) { int w, h, bs; control_size(e, 100000, &w, &h, &bs); a = b = w + ex; }
    else if (st->display == D_TABLE || st->display == D_INLINE_TABLE) { table_intrinsic(e, cbw, &a, &b); a += ex; b += ex; }
    else if (st->w.t == L_PX) {
        int w = st->w.v + (st->bsizing ? 0 : rlen(st->p[1], 0) + rlen(st->p[3], 0) + bwid(st, 1) + bwid(st, 3));
        a = b = w + ex - (rlen(st->p[1], 0) + rlen(st->p[3], 0) + bwid(st, 1) + bwid(st, 3));
    } else if (depth < MAX_DEPTH) {
        int line = 0;
        depth++;
        if (st->display == D_FLEX && !st->flex_col) {
            /* a row: widths add up */
            for (struct node *c = e->first; c; c = c->next) {
                if (c->type != N_ELEM || !c->st || c->st->display == D_NONE) continue;
                int cmn, cmx; intrinsic(c, cbw, &cmn, &cmx);
                b += cmx + st->gap; if (st->flex_wrap) { if (cmn > a) a = cmn; } else a += cmn + st->gap;
            }
        } else {
            inline_intrinsic(e, cbw, &line, &a, &b, depth);
            if (line > b) b = line;
        }
        depth--;
        if (st->maxw.t == L_PX && b > st->maxw.v) b = st->maxw.v;
        if (st->minw.t == L_PX && a < st->minw.v) a = st->minw.v;
        if (a > b) b = a;
        a += ex; b += ex;
    }
    if (a > 100000) a = 100000;
    if (b > 100000) b = 100000;
    if (a < 0) a = 0;
    if (b < a) b = a;
    e->minw = a; e->maxw = b; e->iw_gen = gen;
    *mn = a; *mx = b;
}

/* ---------------- flow context ---------------- */
struct ictx { const struct style *st; int deco; int link; const struct style *bgst; bool hidden; };
/* floats of one block formatting context, in its coordinates */
#define MAXFLOATS 24
struct flist { int n; struct { int x0, x1, top, bottom; uint8_t side; } f[MAXFLOATS]; };
struct fctx {
    int left, width, y, pm;
    int x, line_first, asc, desc; bool has, space;
    const struct style *bst;
    int counter;
    int mark, mark_num; const struct style *mark_st;
    int ll, lr;                       /* the current line's left and right ends (beside floats) */
    struct flist *fl;
    struct node *pfloat; struct ictx pfic;    /* a float waiting for the end of the line */
    const struct style *last_bg; int last_bg_rect; int bg_pad_left;
};

static struct flist *new_flist(void) { return ar_alloc(&L->ar, sizeof(struct flist)); }
static int fl_left(struct fctx *c, int y, int h) {
    int l = c->left;
    if (c->fl) for (int i = 0; i < c->fl->n; i++)
        if (c->fl->f[i].side == 1 && c->fl->f[i].top < y + h && c->fl->f[i].bottom > y && c->fl->f[i].x1 > l) l = c->fl->f[i].x1;
    return l;
}
static int fl_right(struct fctx *c, int y, int h) {
    int r = c->left + c->width;
    if (c->fl) for (int i = 0; i < c->fl->n; i++)
        if (c->fl->f[i].side == 2 && c->fl->f[i].top < y + h && c->fl->f[i].bottom > y && c->fl->f[i].x0 < r) r = c->fl->f[i].x0;
    return r;
}
static int fl_next_bottom(struct fctx *c, int y) {         /* the nearest float bottom below y, or -1 */
    int b = -1;
    if (c->fl) for (int i = 0; i < c->fl->n; i++) if (c->fl->f[i].bottom > y && (b < 0 || c->fl->f[i].bottom < b)) b = c->fl->f[i].bottom;
    return b;
}
static int fl_clear(struct flist *fl, int sides) {
    int b = 0;
    if (fl) for (int i = 0; i < fl->n; i++) if ((fl->f[i].side & sides) && fl->f[i].bottom > b) b = fl->f[i].bottom;
    return b;
}

static void flow_children(struct fctx *c, struct node *e, struct ictx ic);
static void layout_block(struct fctx *c, struct node *n, struct ictx ic, int forced_w);

static void line_metrics(struct fctx *c, int asc, int desc) { if (asc > c->asc) c->asc = asc; if (desc > c->desc) c->desc = desc; }

static void marker_text(int type, int num, char *out) {
    if (type == LS_DECIMAL) { ksnprintf(out, 16, "%d.", num); return; }
    if (type == LS_LALPHA || type == LS_UALPHA) {
        char b[8]; int k = 0; int v = num < 1 ? 1 : num;
        while (v > 0 && k < 6) { b[k++] = (char)((type == LS_LALPHA ? 'a' : 'A') + (v - 1) % 26); v = (v - 1) / 26; }
        int o = 0; while (k) out[o++] = b[--k];
        out[o++] = '.'; out[o] = 0; return;
    }
    static const char *const r[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    static const int rv[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    int v = num < 1 ? 1 : num > 3999 ? 3999 : num, o = 0;
    for (int i = 0; i < 13; i++) while (v >= rv[i]) { for (const char *q = r[i]; *q; q++) out[o++] = type == LS_UROMAN ? *q - 32 : *q; v -= rv[i]; }
    out[o++] = '.'; out[o] = 0;
}

/* the list marker, placed left of x at baseline base */
__attribute__((noinline)) static void draw_marker(struct fctx *c, int base) {
    const struct style *st = c->mark_st;
    int type = c->mark - 1;
    c->mark = 0;
    if (!st || type == LS_NONE || !st->vis) return;
    int ga, gd, a, d; metrics(st, &a, &d, &ga, &gd);
    if (type <= LS_SQUARE) {
        int s = st->fsize * 5 / 16; if (s < 4) s = 4; if (s > 10) s = 10;
        int i = push(DI_MARK); if (i < 0) return;
        struct ditem *it = IT(i);
        it->mark = type; it->w = s; it->h = s; it->x = c->left - 8 - s; it->y = base - ga / 3 - s / 2 - 1; it->col = st->color;
        return;
    }
    char t[24]; marker_text(type, c->mark_num, t);
    int n = strlen(t), w = text_w(st, t, n);
    long off = tp_add(t, n); if (off < 0) return;
    int i = push(st->mono ? DI_MONO : DI_TEXT); if (i < 0) return;
    struct ditem *it = IT(i);
    it->f = st->mono ? 0 : web_font(st->fsize, st->bold, st->italic);
    it->x = c->left - 6 - w; it->w = w; it->asc = ga; it->h = ga + gd; it->y = base - ga; it->off = off; it->len = n; it->col = st->color;
}

static void do_float(struct fctx *c, struct node *n, struct ictx ic);
static void flush_line(struct fctx *c, bool force) {
    if (!c->has && !force) {
        c->x = c->left; c->space = false; c->line_first = L->nit; c->asc = c->desc = 0; c->last_bg = 0;
        if (c->pfloat) { struct node *f = c->pfloat; c->pfloat = 0; do_float(c, f, c->pfic); }
        return;
    }
    int sa, sd; metrics(c->bst, &sa, &sd, 0, 0);
    int asc = c->asc > sa ? c->asc : sa, desc = c->desc > sd ? c->desc : sd;
    int base = c->y + asc;
    int used = c->x - c->ll, room = c->lr - c->ll, shift = 0;
    if (!c->has) used = room = 0;
    if (c->bst->align == TA_CENTER) shift = (room - used) / 2;
    else if (c->bst->align == TA_RIGHT) shift = room - used;
    if (shift < 0) shift = 0;
    for (int i = c->line_first; i < L->nit; i++) { L->it[i].x += shift; L->it[i].y += base; }
    if (c->mark) draw_marker(c, base);
    last_base = base; nflush++;
    c->y = base + desc;
    c->x = c->left; c->has = false; c->space = false; c->line_first = L->nit; c->asc = c->desc = 0; c->last_bg = 0;
    if (c->pfloat) { struct node *f = c->pfloat; c->pfloat = 0; do_float(c, f, c->pfic); }
}

static int line_right(struct fctx *c) { return c->has ? c->lr : fl_right(c, c->y, 16); }
/* a new line starts: find its ends beside the floats, moving below them if need is too wide */
static void begin_line(struct fctx *c, int need) {
    if (c->has) return;
    for (int k = 0; k < MAXFLOATS + 1; k++) {
        c->ll = fl_left(c, c->y, 16); c->lr = fl_right(c, c->y, 16);
        if (need <= c->lr - c->ll || (c->ll == c->left && c->lr == c->left + c->width)) break;
        int b = fl_next_bottom(c, c->y);
        if (b < 0) break;
        c->y = b;
    }
    if (c->x < c->ll) c->x = c->ll;
}

/* place one word (already transformed) */
static void add_word(struct fctx *c, const struct ictx *ic, const char *w, int n, int ww, bool wrap) {
    const struct style *st = ic->st;
    int sw = c->space && c->has ? space_w(st) : 0;
    if (c->has && wrap && c->x + sw + ww > line_right(c)) { flush_line(c, false); sw = 0; }
    if (!c->has) begin_line(c, wrap ? ww : 0);
    int ga, gd, a, d; metrics(st, &a, &d, &ga, &gd);
    int dy = st->valign == VA_SUPER ? -st->fsize / 2 : st->valign == VA_SUB ? st->fsize / 4 : 0;
    if (dy < 0) a -= dy; else d += dy;
    line_metrics(c, a, d);
    int x = c->x + sw;
    bool mono = st->mono;
    const struct font *f = mono ? 0 : web_font(st->fsize, st->bold, st->italic);
    /* inline background */
    if (ic->bgst && !ic->hidden) {
        int pt = rlen(ic->bgst->p[0], 0), pb = rlen(ic->bgst->p[2], 0);
        int rx = x, rw = ww;
        if (c->last_bg == ic->bgst && sw) { rx -= sw; rw += sw; }
        if (c->bg_pad_left) { rx -= c->bg_pad_left; rw += c->bg_pad_left; c->bg_pad_left = 0; }
        int ri = push(DI_RECT);
        if (ri >= 0) { struct ditem *r = IT(ri); r->x = rx; r->w = rw; r->y = -ga - pt + dy; r->h = ga + gd + pt + pb; r->col = ic->bgst->bg & 0xFFFFFF; }
        c->last_bg = ic->bgst; c->last_bg_rect = ri;
    } else c->last_bg = 0;
    /* merge with the previous word when it looks the same */
    if (L->nit > c->line_first && !ic->bgst) {
        struct ditem *p = &L->it[L->nit - 1];
        if ((p->kind == DI_TEXT || p->kind == DI_MONO) && (p->kind == DI_MONO) == mono && p->f == f && p->col == st->color &&
            p->deco == (ic->deco | (ic->hidden ? 0x80 : 0)) && p->link == ic->link && p->off + p->len == (uint32_t)L->tlen &&
            p->x + p->w <= x && p->y == -ga + dy && p->len + n + 1 < 60000) {
            int gap = x - (p->x + p->w);
            int spaces = mono ? gap / MONO_W : (sw ? 1 : 0);
            if (gap == 0 || (sw && gap == sw) || (mono && gap == spaces * MONO_W)) {
                for (int k = 0; k < spaces; k++) tp_add(" ", 1);
                if (tp_add(w, n) >= 0) { p->len += n + spaces; p->w = x + ww - p->x; c->x = x + ww; c->has = true; c->space = false; return; }
            }
        }
    }
    long off = tp_add(w, n);
    if (off < 0) return;
    int i = push(mono ? DI_MONO : DI_TEXT);
    if (i < 0) return;
    struct ditem *it = IT(i);
    it->f = f; it->x = x; it->y = -ga + dy; it->w = ww; it->h = ga + gd; it->asc = ga; it->off = off; it->len = n;
    it->col = st->color; it->deco = ic->deco | (ic->hidden ? 0x80 : 0); it->link = ic->link;
    c->x = x + ww; c->has = true; c->space = false;
}

__attribute__((noinline)) static void inline_text(struct fctx *c, const char *s, int n, const struct ictx *ic) {
    const struct style *st = ic->st;
    int ws = st->ws;
    bool collapse = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRELINE;
    bool wrap = ws == WS_NORMAL || ws == WS_PREWRAP || ws == WS_PRELINE;
    bool newlines = ws == WS_PRE || ws == WS_PREWRAP || ws == WS_PRELINE;
    char buf[512]; int k = 0, bw = 0;
    const struct font *f = st->mono ? 0 : web_font(st->fsize, st->bold, st->italic);
    bool wstart = c->space || !c->has;
    int avail = c->width;
    for (int i = 0; i <= n; i++) {
        char ch = i < n ? s[i] : 0;
        bool end = i == n;
        bool sp = !end && is_ws(ch);
        if (end || sp || k >= 500) {
            if (k) { add_word(c, ic, buf, k, bw, wrap); k = 0; bw = 0; }
            if (end) break;
            if (k >= 500 && !sp) { i--; continue; }
            if (newlines && ch == '\n') { flush_line(c, true); continue; }
            if (ch == '\r') continue;
            if (collapse) { c->space = true; wstart = true; continue; }
            /* preserved spaces: part of the text */
            int cnt = ch == '\t' ? (st->mono ? 8 - ((c->x - c->left) / MONO_W) % 8 : 4) : 1;
            for (int q = 0; q < cnt && k < 500; q++) { buf[k++] = ' '; bw += f ? glyph_w(f, ' ') : MONO_W; }
            if (wrap) { add_word(c, ic, buf, k, bw, false); k = 0; bw = 0; }
            continue;
        }
        int t = tchar(ch, st->tt, &wstart);
        int gw = f ? glyph_w(f, t) : MONO_W;
        if (wrap && bw + gw > avail && k) {          /* a word wider than the line: break it */
            add_word(c, ic, buf, k, bw, wrap); k = 0; bw = 0;
            c->space = false;
        }
        buf[k++] = (char)t; bw += gw;
    }
}

/* an atomic inline box: the caller laid out (or will draw) items in [i0, nit) relative to the box's top-left at (0,0)
 * with x already absolute; this moves them onto the line. */
static void place_atomic_items(struct fctx *c, int i0, int h, int base_off, int valign) {
    int a = base_off, d = h - base_off;
    if (valign == VA_MIDDLE) { int sa, sd, ga, gd; metrics(c->bst, &sa, &sd, &ga, &gd); a = h / 2 + ga / 3; d = h - a; }
    for (int i = i0; i < L->nit; i++) L->it[i].y -= a;
    line_metrics(c, a, d);
}

static bool fits(struct fctx *c, int w) {
    int sw = c->space && c->has ? 4 : 0;
    return !c->has || c->x + sw + w <= line_right(c);
}

__attribute__((noinline)) static void atomic_image(struct fctx *c, struct node *n, const struct ictx *ic) {
    const struct style *st = n->st;
    int w, h; img_size(n, c->width, &w, &h);
    int ml = rlen(st->m[3], c->width), mr = rlen(st->m[1], c->width), mt = rlen(st->m[0], c->width), mb = rlen(st->m[2], c->width);
    int bl = bwid(st, 3), br = bwid(st, 1), bt = bwid(st, 0), bb = bwid(st, 2);
    int pl = rlen(st->p[3], c->width), pr = rlen(st->p[1], c->width), pt = rlen(st->p[0], c->width), pb = rlen(st->p[2], c->width);
    int ow = ml + bl + pl + w + pr + br + mr, oh = mt + bt + pt + h + pb + bb + mb;
    if (!fits(c, ow)) flush_line(c, false);
    begin_line(c, ow);
    if (c->space && c->has) c->x += space_w(ic->st);
    int i0 = L->nit;
    int x = c->x + ml;
    if (bl || br || bt || bb) {
        int fr = push(DI_FRAME);
        if (fr >= 0) { struct ditem *it = IT(fr); it->x = x; it->y = mt; it->w = bl + pl + w + pr + br; it->h = bt + pt + h + pb + bb; it->col = st->bc[0]; it->asc = bt > 0 ? bt : 1; }
    }
    int i = push(DI_IMG);
    if (i >= 0) {
        struct ditem *it = IT(i);
        it->x = x + bl + pl; it->y = mt + bt + pt; it->w = w; it->h = h; it->n = n; it->link = ic->link;
        it->deco = ic->hidden || !st->vis ? 0x80 : 0;
    }
    place_atomic_items(c, i0, oh, oh - mb, st->valign);
    c->x += ow; c->has = true; c->space = false;
}

__attribute__((noinline)) static void atomic_control(struct fctx *c, struct node *n, const struct ictx *ic) {
    const struct style *st = n->st;
    int w, h, base; control_size(n, c->width, &w, &h, &base);
    if (!w) return;
    int ml = st->m[3].t == L_PX ? st->m[3].v : 2, mr = st->m[1].t == L_PX ? st->m[1].v : 2;
    if (n->fc && (n->fc->type == FC_CHECKBOX || n->fc->type == FC_RADIO)) { if (st->m[3].t != L_PX || !st->m[3].v) ml = 3; if (st->m[1].t != L_PX || !st->m[1].v) mr = 4; }
    if (ml < 0) ml = 0;
    if (mr < 0) mr = 0;
    if (ml > 40) ml = 40;
    if (mr > 40) mr = 40;
    int mt = st->m[0].t == L_PX && st->m[0].v > 0 && st->m[0].v < 40 ? st->m[0].v : 1;
    int mb = st->m[2].t == L_PX && st->m[2].v > 0 && st->m[2].v < 40 ? st->m[2].v : 1;
    if (!fits(c, ml + w + mr)) flush_line(c, false);
    begin_line(c, ml + w + mr);
    if (c->space && c->has) c->x += space_w(ic->st);
    int i0 = L->nit;
    int i = push(DI_CTRL);
    if (i >= 0) { struct ditem *it = IT(i); it->x = c->x + ml; it->y = mt; it->w = w; it->h = h; it->n = n; it->asc = base; it->col = st->color; it->deco = ic->hidden ? 0x80 : 0; }
    place_atomic_items(c, i0, mt + h + mb, mt + base, st->valign == VA_MIDDLE ? VA_MIDDLE : VA_BASELINE);
    c->x += ml + w + mr; c->has = true; c->space = false;
}

/* inline-block, inline-table, inline-flex */
__attribute__((noinline)) static void atomic_block(struct fctx *c, struct node *n, const struct ictx *ic) {
    const struct style *st = n->st;
    int mn, mx; intrinsic(n, c->width, &mn, &mx);
    int ow;
    if (st->w.t == L_PX || st->w.t == L_PCT) {
        int w = rlen(st->w, c->width);
        ow = w + hextra(st, c->width) + (st->bsizing ? 0 : rlen(st->p[1], c->width) + rlen(st->p[3], c->width) + bwid(st, 1) + bwid(st, 3));
    } else { ow = c->width < mx ? c->width : mx; if (ow < mn) ow = mn; }
    if (ow > c->width && c->width > 0) ow = c->width;
    if (!fits(c, ow)) flush_line(c, false);
    begin_line(c, ow);
    if (c->space && c->has) c->x += space_w(ic->st);
    int i0 = L->nit;
    struct fctx t; memset(&t, 0, sizeof t);
    t.left = c->x; t.width = ow; t.y = 0; t.x = t.left; t.line_first = L->nit; t.bst = c->bst;
    int fl = nflush, lb = 0;
    int ml = st->m[3].t == L_AUTO ? 0 : rlen(st->m[3], c->width), mr = st->m[1].t == L_AUTO ? 0 : rlen(st->m[1], c->width);
    int bw = ow - ml - mr;
    layout_block(&t, n, *ic, bw < 0 ? 0 : bw);
    int h = t.y + (t.pm > 0 ? t.pm : 0);
    if (nflush != fl && n->tag != T_TABLE && st->display != D_INLINE_TABLE) { lb = last_base; if (lb > h) lb = h; } else lb = h;
    place_atomic_items(c, i0, h, lb, st->valign);
    c->x += ow; c->has = true; c->space = false;
}

static void inline_element(struct fctx *c, struct node *n, struct ictx ic) {
    const struct style *st = n->st;
    struct ictx in = ic;
    in.st = st; in.deco = ic.deco | st->deco; if (n->link >= 0) in.link = n->link;
    if (!st->vis) in.hidden = true;
    if (st->bg) { in.bgst = st; c->bg_pad_left = rlen(st->p[3], 0) + bwid(st, 3); }
    int left = rlen(st->m[3], c->width) + rlen(st->p[3], c->width) + bwid(st, 3);
    int right = rlen(st->m[1], c->width) + rlen(st->p[1], c->width) + bwid(st, 1);
    if (left > 0 && left < 200) { if (c->space && c->has) { c->x += space_w(st); c->space = false; } c->x += left; }
    if (depth < MAX_DEPTH) { depth++; flow_children(c, n, in); depth--; }
    if (right > 0 && right < 200) {
        if (st->bg && c->last_bg == st && c->last_bg_rect >= 0) IT(c->last_bg_rect)->w += rlen(st->p[1], c->width);
        if (c->space && c->has) { c->x += space_w(st); c->space = false; }
        c->x += right;
    }
    c->bg_pad_left = 0;
}

static void block_level(struct fctx *c, struct node *n, struct ictx ic);
static void flow_node_(struct fctx *c, struct node *n, struct ictx ic);
static void flow_node(struct fctx *c, struct node *n, struct ictx ic) {
    struct node *saved = cur_src;
    if (n->type == N_ELEM) cur_src = n;
    flow_node_(c, n, ic);
    cur_src = saved;
}
static void flow_node_(struct fctx *c, struct node *n, struct ictx ic) {
    if (n->type == N_TEXT) { inline_text(c, n->text, n->tlen, &ic); return; }
    const struct style *st = n->st;
    if (!st || st->display == D_NONE) return;
    if (st->floated && c->fl) {
        if (c->has) {                                      /* after this line */
            if (c->pfloat) flush_line(c, false);
            if (c->has) { c->pfloat = n; c->pfic = ic; return; }
        }
        do_float(c, n, ic);
        return;
    }
    int d = st->display;
    struct ictx in = ic;
    if (d == D_INLINE) {
        in.st = st; in.deco = ic.deco | st->deco; if (n->link >= 0) in.link = n->link;
        if (!st->vis) in.hidden = true;
        if (n->tag == T_BR) {
            if (!c->has) { int a, dd; metrics(st, &a, &dd, 0, 0); line_metrics(c, a, dd); }
            flush_line(c, true);
            if (st->clear && c->fl) { int b = fl_clear(c->fl, st->clear); if (b > c->y) c->y = b; }
            return;
        }
        if (n->tag == T_IMG) { atomic_image(c, n, &in); return; }
        if (is_control(n)) { atomic_control(c, n, &in); return; }
        inline_element(c, n, ic);
        return;
    }
    if (d == D_INLINE_BLOCK || d == D_INLINE_TABLE || d == D_INLINE_FLEX) {
        in.st = st; in.deco = ic.deco | st->deco; if (n->link >= 0) in.link = n->link;
        if (n->tag == T_IMG) { atomic_image(c, n, &in); return; }
        if (is_control(n)) { atomic_control(c, n, &in); return; }
        atomic_block(c, n, &in);
        return;
    }
    block_level(c, n, ic);
}

static void flow_children(struct fctx *c, struct node *e, struct ictx ic) {
    for (struct node *ch = e->first; ch; ch = ch->next) flow_node(c, ch, ic);
}

/* ---------------- tables ---------------- */
#define MAXCOLS 64
#define MAXROWS 4000
struct trow { struct node *tr; struct node **cells; int nc; };
struct tinfo { struct trow *rows; int nr; struct node *caps[4]; int ncap; int ncols; struct node **pool; int used, anon_n; bool anon; };
static int colspan(struct node *cell) { int s = attr_px(cell, "colspan"); return s < 1 ? 1 : s > MAXCOLS ? MAXCOLS : s; }

static int count_cells(struct node *e, int d) {
    int n = 0;
    for (struct node *c = e->first; c && n < MAXCOLS; c = c->next) {
        if (c->type != N_ELEM || !c->st || c->st->display == D_NONE) continue;
        n += c->st->display == D_CELL || d >= 2 ? 1 : count_cells(c, d + 1);
    }
    return n < MAXCOLS ? n : MAXCOLS;
}
static void row_cells(struct trow *r, struct node *e, int d, int cap) {
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->st || c->st->display == D_NONE) continue;
        if (c->st->display == D_CELL || d >= 2) { if (r->nc < cap) r->cells[r->nc++] = c; }
        else row_cells(r, c, d + 1, cap);
    }
}
/* two passes: count rows and cells (rows == 0), then fill them in */
static void gather(struct tinfo *t, struct node *e, int d) {
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->st || c->st->display == D_NONE) continue;
        int dsp = c->st->display;
        if (dsp == D_ROW) {
            if (t->nr >= MAXROWS) continue;
            t->anon = false;
            int cap = count_cells(c, 0);
            if (t->rows) { struct trow *r = &t->rows[t->nr]; r->tr = c; r->cells = t->pool + t->used; r->nc = 0; row_cells(r, c, 0, cap); }
            t->nr++; t->used += cap;
        } else if (dsp == D_CELL) {
            if (!t->anon) {
                if (t->nr >= MAXROWS) continue;
                if (t->rows) { struct trow *r = &t->rows[t->nr]; r->tr = 0; r->nc = 0; r->cells = t->pool + t->used; }
                t->nr++; t->anon = true; t->anon_n = 0;
            }
            if (t->anon_n >= MAXCOLS) continue;
            if (t->rows) { struct trow *r = &t->rows[t->nr - 1]; r->cells[r->nc++] = c; }
            t->used++; t->anon_n++;
        } else if (dsp == D_CAPTION) { if (t->ncap < 4) t->caps[t->ncap++] = c; }
        else if (d < 3) gather(t, c, d + 1);
    }
}
static bool table_info(struct node *e, struct tinfo *t) {
    memset(t, 0, sizeof *t);
    gather(t, e, 0);
    int nr = t->nr, used = t->used;
    memset(t, 0, sizeof *t);
    t->rows = ar_alloc(&L->ar, (nr ? nr : 1) * sizeof(struct trow));
    t->pool = ar_alloc(&L->ar, (used ? used : 1) * sizeof(struct node *));
    if (!t->rows || !t->pool) return false;
    gather(t, e, 0);
    for (int i = 0; i < t->nr; i++) {
        int n = 0; for (int k = 0; k < t->rows[i].nc; k++) n += colspan(t->rows[i].cells[k]);
        if (n > t->ncols) t->ncols = n;
    }
    if (t->ncols > MAXCOLS) t->ncols = MAXCOLS;
    return true;
}
static void columns(struct tinfo *t, int cbw, int *cmin, int *cmax, int *cpct) {
    for (int i = 0; i < t->ncols; i++) cmin[i] = cmax[i] = cpct[i] = 0;
    for (int pass = 0; pass < 2; pass++)                       /* single-column cells first, then spanning ones */
        for (int r = 0; r < t->nr; r++) {
            int col = 0;
            for (int k = 0; k < t->rows[r].nc && col < t->ncols; k++) {
                struct node *cell = t->rows[r].cells[k];
                int span = colspan(cell); if (col + span > t->ncols) span = t->ncols - col;
                if ((span > 1) != pass) { col += span; continue; }
                int mn, mx; intrinsic(cell, cbw, &mn, &mx);
                const struct style *st = cell->st;
                if (st->w.t == L_PX) { int w = st->w.v + (st->bsizing ? 0 : hextra(st, cbw)); if (w > mx) mx = w; if (w > mn && span == 1) mn = w < mn ? mn : w; }
                if (span == 1) {
                    if (mn > cmin[col]) cmin[col] = mn;
                    if (mx > cmax[col]) cmax[col] = mx;
                    if (st->w.t == L_PCT && st->w.v / 100 > cpct[col]) cpct[col] = st->w.v / 100;
                } else {
                    int smin = 0, smax = 0;
                    for (int j = col; j < col + span; j++) { smin += cmin[j]; smax += cmax[j]; }
                    if (mn > smin) for (int j = col; j < col + span; j++) cmin[j] += (mn - smin) / span;
                    if (mx > smax) for (int j = col; j < col + span; j++) cmax[j] += (mx - smax) / span;
                }
                col += span;
            }
        }
    for (int i = 0; i < t->ncols; i++) if (cmax[i] < cmin[i]) cmax[i] = cmin[i];
}
__attribute__((noinline)) static void table_intrinsic(struct node *e, int cbw, int *mn, int *mx) {
    struct tinfo t;
    *mn = *mx = 0;
    if (depth >= MAX_DEPTH || table_depth >= 12 || !table_info(e, &t) || !t.ncols) return;
    int *cmin = ar_alloc(&L->ar, 3 * MAXCOLS * sizeof(int));
    if (!cmin) return;
    int *cmax = cmin + MAXCOLS, *cpct = cmax + MAXCOLS;
    depth++; table_depth++;
    columns(&t, cbw, cmin, cmax, cpct);
    depth--; table_depth--;
    int s = e->st->spacing;
    for (int i = 0; i < t.ncols; i++) { *mn += cmin[i]; *mx += cmax[i]; }
    *mn += s * (t.ncols + 1); *mx += s * (t.ncols + 1);
    int pad = rlen(e->st->p[1], 0) + rlen(e->st->p[3], 0) + bwid(e->st, 1) + bwid(e->st, 3);
    *mn += pad; *mx += pad;
    if (e->st->w.t == L_PX && e->st->w.v > *mn) *mn = *mx = e->st->w.v;
}

static void rect(int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    int i = push(DI_RECT);
    if (i >= 0) { struct ditem *it = IT(i); it->x = x; it->y = y; it->w = w; it->h = h; it->col = col & 0xFFFFFF; }
}
static void borders_m(const struct style *st, int x, int y, int w, int h, bool no_top, bool no_left) {
    int bt = no_top ? 0 : bwid(st, 0), br = bwid(st, 1), bb = bwid(st, 2), bl = no_left ? 0 : bwid(st, 3);
    if (!st->vis) return;
    if (bt) rect(x, y, w, bt, st->bc[0]);
    if (bb) rect(x, y + h - bb, w, bb, st->bc[2]);
    if (bl) rect(x, y, bl, h, st->bc[3]);
    if (br) rect(x + w - br, y, br, h, st->bc[1]);
}
static void borders(const struct style *st, int x, int y, int w, int h) { borders_m(st, x, y, w, h, false, false); }
static void shift_items(int i0, int i1, int dy) { for (int i = i0; i < i1; i++) L->it[i].y += dy; }

__attribute__((noinline)) static void layout_table(struct fctx *c, struct node *e, struct ictx ic) {
    const struct style *st = e->st;
    struct tinfo t;
    if (depth >= MAX_DEPTH || table_depth >= 12 || !table_info(e, &t)) return;
    struct tcell { int i0, i1, h, bg, x, w; struct node *n; };
    int *cmin = ar_alloc(&L->ar, 4 * MAXCOLS * sizeof(int));
    struct tcell *cell = ar_alloc(&L->ar, MAXCOLS * sizeof(struct tcell));
    if (!cmin || !cell) return;
    int *cmax = cmin + MAXCOLS, *cpct = cmax + MAXCOLS, *cw = cpct + MAXCOLS;
    depth++; table_depth++;
    int nc = t.ncols;
    columns(&t, c->width, cmin, cmax, cpct);
    int s = st->spacing;
    int cbw = c->width;
    int fleft = c->left;
    if (c->fl && c->fl->n) {                               /* a table goes beside floats, not under them */
        int ty = c->y + (c->pm > 0 ? c->pm : 0);
        fleft = fl_left(c, ty, 40); cbw = fl_right(c, ty, 40) - fleft;
        if (cbw < 100 && c->width >= 100) { int b = fl_clear(c->fl, 3); if (b > c->y) c->y = b; fleft = c->left; cbw = c->width; }
    }
    int ml = st->m[3].t == L_AUTO ? 0 : rlen(st->m[3], cbw), mr = st->m[1].t == L_AUTO ? 0 : rlen(st->m[1], cbw);
    int mt = rlen(st->m[0], cbw), mb = rlen(st->m[2], cbw);
    int pl = rlen(st->p[3], cbw), pr = rlen(st->p[1], cbw), pt = rlen(st->p[0], cbw), pb = rlen(st->p[2], cbw);
    int bl = bwid(st, 3), br = bwid(st, 1), bt = bwid(st, 0), bb = bwid(st, 2);
    int frame = pl + pr + bl + br;
    int avail = cbw - ml - mr - frame - s * (nc + 1);
    int summin = 0, summax = 0;
    for (int i = 0; i < nc; i++) { summin += cmin[i]; summax += cmax[i]; }
    int W;
    bool fixed = st->w.t == L_PX || st->w.t == L_PCT;
    if (fixed) { W = rlen(st->w, cbw) - (st->bsizing ? frame : 0) - s * (nc + 1); if (W > avail && st->w.t == L_PCT) W = avail; }
    else W = summax < avail ? summax : avail;
    if (st->maxw.t == L_PX || st->maxw.t == L_PCT) { int m = rlen(st->maxw, cbw) - s * (nc + 1); if (W > m) W = m; }
    if (W < summin) W = summin;
    /* percentage columns first */
    int used = 0, rmin = 0, rmax = 0;
    for (int i = 0; i < nc; i++) {
        if (cpct[i]) { cw[i] = W * cpct[i] / 100; if (cw[i] < cmin[i]) cw[i] = cmin[i]; used += cw[i]; }
        else { rmin += cmin[i]; rmax += cmax[i]; }
    }
    int rem = W - used; if (rem < rmin) rem = rmin;
    int nfree = 0; for (int i = 0; i < nc; i++) if (!cpct[i]) nfree++;
    for (int i = 0; i < nc; i++) {
        if (cpct[i]) continue;
        if (rmax <= rem) cw[i] = cmax[i] + (rmax ? (int)((long)(rem - rmax) * cmax[i] / rmax) : (rem - rmax) / (nfree ? nfree : 1));
        else if (rmax > rmin) cw[i] = cmin[i] + (int)((long)(rem - rmin) * (cmax[i] - cmin[i]) / (rmax - rmin));
        else cw[i] = cmin[i];
    }
    int tw = s * (nc + 1); for (int i = 0; i < nc; i++) tw += cw[i];
    int boxw = tw + frame;
    /* place the table */
    int y = c->y + (c->pm > mt ? c->pm : mt);
    if (st->clear && c->fl) { int b = fl_clear(c->fl, st->clear); if (b > y) y = b; }
    int x = fleft + ml;
    if (st->m[3].t == L_AUTO && st->m[1].t == L_AUTO) x = fleft + (cbw - boxw) / 2;
    else if (st->m[3].t == L_AUTO) x = fleft + cbw - mr - boxw;
    if (x < fleft) x = fleft;
    e->y = y;
    struct ictx in = ic; in.st = st; in.deco = ic.deco | st->deco; in.bgst = 0; if (e->link >= 0) in.link = e->link;
    /* captions above */
    for (int k = 0; k < t.ncap; k++) {
        struct fctx cc; memset(&cc, 0, sizeof cc);
        cc.left = x; cc.width = boxw; cc.y = y; cc.x = x; cc.line_first = L->nit; cc.bst = st;
        layout_block(&cc, t.caps[k], in, -1);
        y = cc.y + (cc.pm > 0 ? cc.pm : 0);
    }
    int bgi = st->bg && e != canvas_node ? push(DI_RECT) : -1;
    int top = y;
    int cy = y + bt + pt + s;
    int cx0 = x + bl + pl + s;
    for (int r = 0; r < t.nr; r++) {
        struct trow *row = &t.rows[r];
        const struct style *rs = row->tr ? row->tr->st : st;
        int rbg = row->tr && rs->bg ? push(DI_RECT) : -1;
        int ncell = 0, rowh = 0;
        if (row->tr && rs->h.t == L_PX) rowh = rs->h.v;
        int col = 0, xx = cx0;
        struct ictx rin = in; if (row->tr) { rin.st = rs; rin.deco = in.deco | rs->deco; if (row->tr->link >= 0) rin.link = row->tr->link; }
        for (int k = 0; k < row->nc && col < nc; k++) {
            struct node *cn = row->cells[k];
            const struct style *cs = cn->st;
            int span = colspan(cn); if (col + span > nc) span = nc - col;
            int w = (span - 1) * s; for (int j = col; j < col + span; j++) w += cw[j];
            int cbg = cs->bg ? push(DI_RECT) : -1;
            int cpl = rlen(cs->p[3], w), cpr = rlen(cs->p[1], w), cpt = rlen(cs->p[0], w), cpb = rlen(cs->p[2], w);
            int cbl = bwid(cs, 3), cbr = bwid(cs, 1), cbt = bwid(cs, 0), cbb = bwid(cs, 2);
            struct fctx cc; memset(&cc, 0, sizeof cc);
            cc.left = xx + cbl + cpl; cc.width = w - cbl - cpl - cpr - cbr; if (cc.width < 0) cc.width = 0;
            cc.y = cy + cbt + cpt; cc.x = cc.left; cc.line_first = L->nit; cc.bst = cs; cc.fl = new_flist();
            int i0 = L->nit;
            struct ictx cin = rin; cin.st = cs; cin.deco = rin.deco | cs->deco; if (cn->link >= 0) cin.link = cn->link; cin.bgst = 0;
            if (!cs->vis) cin.hidden = true;
            depth++;
            flow_children(&cc, cn, cin);
            flush_line(&cc, false);
            depth--;
            int fb = fl_clear(cc.fl, 3); if (fb > cc.y + (cc.pm > 0 ? cc.pm : 0)) { cc.y = fb; cc.pm = 0; }
            int h = cc.y + (cc.pm > 0 ? cc.pm : 0) - cy + cpb + cbb;
            if (cs->h.t == L_PX && cs->h.v > h) h = cs->h.v;
            const char *ha = node_attr(cn, "height"); if (ha && katoi(ha) > h && katoi(ha) < 2000) h = katoi(ha);
            if (h > rowh) rowh = h;
            if (ncell < MAXCOLS) { cell[ncell].i0 = i0; cell[ncell].i1 = L->nit; cell[ncell].h = h; cell[ncell].bg = cbg; cell[ncell].x = xx; cell[ncell].w = w; cell[ncell].n = cn; ncell++; }
            cn->y = cy;
            xx += w + s; col += span;
        }
        for (int k = 0; k < ncell; k++) {
            const struct style *cs = cell[k].n->st;
            int va = cs->valign_set ? cs->valign : rs->valign_set ? rs->valign : VA_MIDDLE;
            int dy = va == VA_MIDDLE ? (rowh - cell[k].h) / 2 : va == VA_BOTTOM ? rowh - cell[k].h : 0;
            if (dy > 0) shift_items(cell[k].i0, cell[k].i1, dy);
            if (cell[k].bg >= 0) { struct ditem *b = IT(cell[k].bg); b->x = cell[k].x; b->y = cy; b->w = cell[k].w; b->h = rowh; b->col = cs->bg & 0xFFFFFF; }
            /* collapsed borders: a cell leaves out the edges its neighbour above or to the left already drew */
            borders_m(cs, cell[k].x, cy, cell[k].w, rowh, st->bcollapse && r > 0, st->bcollapse && cell[k].x > cx0);
        }
        if (rbg >= 0) { struct ditem *b = IT(rbg); b->x = cx0 - s; b->y = cy; b->w = tw; b->h = rowh; b->col = rs->bg & 0xFFFFFF; }
        if (row->tr) row->tr->y = cy;
        cy += rowh + s;
        if (cy > 4000000) break;
    }
    int bottom = cy + pb + bb;
    if (!t.nr) bottom = y + bt + pt + pb + bb;
    if (bgi >= 0) { struct ditem *b = IT(bgi); b->x = x; b->y = top; b->w = boxw; b->h = bottom - top; b->col = st->bg & 0xFFFFFF; }
    borders(st, x, top, boxw, bottom - top);
    c->y = bottom; c->pm = mb;
    depth--; table_depth--;
}

/* ---------------- flex rows ---------------- */
__attribute__((noinline)) static bool layout_flex(struct fctx *cc, struct node *e, struct ictx ic) {
    const struct style *st = e->st;
    if (flex_depth >= 16) return false;
    struct node **items = ar_alloc(&L->ar, 64 * (sizeof(struct node *) + 3 * sizeof(int)));
    if (!items) return false;
    int *basis = (int *)(items + 64), *mins = basis + 64, *w = mins + 64;
    int n = 0;
    for (struct node *c = e->first; c; c = c->next) {
        if (c->type == N_TEXT) { for (int i = 0; i < c->tlen; i++) if (!is_ws(c->text[i])) return false; continue; }
        if (!c->st || c->st->display == D_NONE) continue;
        if (n >= 64) return false;
        items[n++] = c;
    }
    if (!n) return true;
    int W = cc->width, gap = st->gap;
    flex_depth++;
    for (int i = 0; i < n; i++) {
        const struct style *is = items[i]->st;
        int mn, mx; intrinsic(items[i], W, &mn, &mx);
        if (is->w.t == L_PX || is->w.t == L_PCT) mx = rlen(is->w, W) + hextra(is, W) + (is->bsizing ? 0 : rlen(is->p[1], W) + rlen(is->p[3], W) + bwid(is, 1) + bwid(is, 3));
        if (mx > W) mx = W;
        if (mn > mx) mn = mx;
        basis[i] = mx; mins[i] = mn;
    }
    int y = cc->y;
    int i = 0;
    while (i < n) {
        /* one line */
        int j = i, sum = 0;
        while (j < n) {
            int add = basis[j] + (j > i ? gap : 0);
            if (st->flex_wrap && j > i && sum + add > W) break;
            sum += add; j++;
        }
        for (int k = i; k < j; k++) w[k] = basis[k];
        if (sum > W) {                                    /* shrink towards the minimum widths */
            int over = sum - W, room = 0;
            for (int k = i; k < j; k++) room += basis[k] - mins[k];
            for (int k = i; k < j; k++) { int r = basis[k] - mins[k]; w[k] -= room ? (int)((long)over * r / room) : 0; if (w[k] < mins[k]) w[k] = mins[k]; }
            sum = 0; for (int k = i; k < j; k++) sum += w[k] + (k > i ? gap : 0);
        }
        int free = W - sum; if (free < 0) free = 0;
        int x = cc->left, between = gap;
        int autoidx = -1;
        for (int k = i; k < j; k++) if (items[k]->st->m[3].t == L_AUTO && k > i) { autoidx = k; break; }
        if (autoidx < 0) {
            if (st->justify == JC_CENTER) x += free / 2;
            else if (st->justify == JC_END) x += free;
            else if (st->justify == JC_BETWEEN && j - i > 1) between = gap + free / (j - i - 1);
            else if (st->justify == JC_AROUND) { int sp = free / (j - i); x += sp / 2; between = gap + sp; }
        }
        int lineh = 0;
        for (int k = i; k < j; k++) {
            if (k == autoidx) x += free;
            struct fctx t; memset(&t, 0, sizeof t);
            t.left = x; t.width = w[k]; t.y = y; t.x = x; t.line_first = L->nit; t.bst = items[k]->st;
            struct ictx in = ic; in.bgst = 0;
            const struct style *is = items[k]->st;
            int ml = is->m[3].t == L_AUTO ? 0 : rlen(is->m[3], W), mr = is->m[1].t == L_AUTO ? 0 : rlen(is->m[1], W);
            if (items[k]->tag == T_IMG || is_control(items[k])) {
                struct fctx lc = t; lc.bst = st;
                flow_node(&lc, items[k], in);
                flush_line(&lc, false);
                t.y = lc.y;
            } else {
                int bw = w[k] - ml - mr;
                layout_block(&t, items[k], in, bw < 0 ? 0 : bw);
            }
            int h = t.y + (t.pm > 0 ? t.pm : 0) - y;
            if (h > lineh) lineh = h;
            x += w[k] + between;
        }
        y += lineh + (j < n ? gap : 0);
        i = j;
    }
    flush_line(cc, false);
    cc->y = y; cc->pm = 0; cc->line_first = L->nit;
    flex_depth--;
    return true;
}

/* ---------------- floats ---------------- */
__attribute__((noinline)) static void do_float(struct fctx *c, struct node *n, struct ictx ic) {
    const struct style *st = n->st;
    if (!c->fl || depth >= MAX_DEPTH) return;
    int avail = c->width, ow;
    int ml = st->m[3].t == L_AUTO ? 0 : rlen(st->m[3], avail), mr = st->m[1].t == L_AUTO ? 0 : rlen(st->m[1], avail);
    if (n->tag == T_IMG) { int w, h; img_size(n, avail, &w, &h); ow = w + hextra(st, avail); }
    else if ((st->w.t == L_PX || st->w.t == L_PCT) && st->display != D_TABLE)
        ow = rlen(st->w, avail) + hextra(st, avail) - (st->bsizing ? rlen(st->p[1], avail) + rlen(st->p[3], avail) + bwid(st, 1) + bwid(st, 3) : 0);
    else { int mn, mx; intrinsic(n, avail, &mn, &mx); ow = mx < avail ? mx : avail; if (ow < mn) ow = mn; if (st->display == D_TABLE && (st->w.t == L_PX || st->w.t == L_PCT)) { int tw = rlen(st->w, avail) + ml + mr; if (tw > ow) ow = tw; } }
    if (ow > avail) ow = avail;
    if (ow < 1) ow = 1;
    if (c->fl->n >= MAXFLOATS) { int b = fl_clear(c->fl, 3); if (b > c->y) c->y = b; c->fl->n = 0; }
    /* the highest place where it fits */
    int y = c->y, x = c->left;
    for (int k = 0; k < MAXFLOATS + 1; k++) {
        int l = fl_left(c, y, 1), r = fl_right(c, y, 1);
        if (r - l >= ow || (l == c->left && r == c->left + c->width)) { x = st->floated == 2 ? r - ow : l; break; }
        int b = fl_next_bottom(c, y);
        if (b < 0) break;
        y = b;
    }
    struct fctx t; memset(&t, 0, sizeof t);
    t.left = x; t.width = ow; t.y = y; t.x = x; t.line_first = L->nit; t.bst = st;
    struct ictx in = ic; in.bgst = 0;
    depth++;
    if (st->display == D_TABLE) layout_table(&t, n, in);
    else layout_block(&t, n, in, ow - ml - mr < 0 ? 0 : ow - ml - mr);
    depth--;
    int bottom = t.y + (t.pm > 0 ? t.pm : 0);
    struct flist *f = c->fl;
    f->f[f->n].x0 = x; f->f[f->n].x1 = x + ow; f->f[f->n].top = y; f->f[f->n].bottom = bottom; f->f[f->n].side = st->floated == 2 ? 2 : 1;
    f->n++;
    c->line_first = L->nit;
    if (!c->has) { c->ll = fl_left(c, c->y, 16); c->lr = fl_right(c, c->y, 16); if (c->x < c->ll) c->x = c->ll; }
}

/* ---------------- blocks ---------------- */
/* a picture or form control with display: block; its margins, borders and padding were done by the block */
__attribute__((noinline)) static void replaced_block(struct fctx *cc, struct node *n, const struct ictx *in, int w) {
    struct ictx ri = { in->st, in->deco, in->link, 0, in->hidden };
    static struct style flat;
    struct style *keep = n->st;
    flat = *keep;
    for (int i = 0; i < 4; i++) { flat.m[i].t = L_PX; flat.m[i].v = 0; flat.p[i].t = L_PX; flat.p[i].v = 0; flat.bs[i] = 0; flat.bw[i] = 0; }
    flat.w.t = L_PX; flat.w.v = w; flat.maxw.t = L_NONE;
    n->st = &flat;
    if (n->tag == T_IMG) atomic_image(cc, n, &ri); else atomic_control(cc, n, &ri);
    n->st = keep;
}

static void layout_block_(struct fctx *c, struct node *n, struct ictx ic, int forced_w);
static void layout_block(struct fctx *c, struct node *n, struct ictx ic, int forced_w) {
    struct node *saved = cur_src;
    if (n->type == N_ELEM) cur_src = n;
    layout_block_(c, n, ic, forced_w);
    cur_src = saved;
}
static void layout_block_(struct fctx *c, struct node *n, struct ictx ic, int forced_w) {
    const struct style *st = n->st;
    int cbw = c->width;
    if (st->display == D_TABLE && forced_w < 0) { layout_table(c, n, ic); return; }
    if (depth >= MAX_DEPTH) return;
    int ml = rlen(st->m[3], cbw), mr = rlen(st->m[1], cbw), mt = rlen(st->m[0], cbw), mb = rlen(st->m[2], cbw);
    bool al = st->m[3].t == L_AUTO, ar = st->m[1].t == L_AUTO;
    int pl = rlen(st->p[3], cbw), pr = rlen(st->p[1], cbw), pt = rlen(st->p[0], cbw), pb = rlen(st->p[2], cbw);
    int bl = bwid(st, 3), br = bwid(st, 1), bt = bwid(st, 0), bb = bwid(st, 2);
    int frame = pl + pr + bl + br;
    int w; bool narrowed = false;
    if (forced_w >= 0) { w = forced_w - frame; al = ar = false; }
    else if (st->w.t == L_PX || st->w.t == L_PCT) { w = rlen(st->w, cbw) - (st->bsizing ? frame : 0); narrowed = true; }
    else w = cbw - ml - mr - frame;
    if (forced_w < 0 && (st->maxw.t == L_PX || st->maxw.t == L_PCT)) {
        int m = rlen(st->maxw, cbw) - (st->bsizing ? frame : 0);
        if (w > m) { w = m; narrowed = true; }
    }
    if (n->tag == T_IMG && forced_w < 0) {                  /* a block-level picture: as wide as the picture */
        int iw, ih; img_size(n, cbw - frame, &iw, &ih);
        w = iw; narrowed = true;
    }
    if (forced_w < 0 && (st->minw.t == L_PX || st->minw.t == L_PCT)) { int m = rlen(st->minw, cbw) - (st->bsizing ? frame : 0); if (w < m) w = m; }
    if (w < 0) w = 0;
    if (narrowed) {
        int room = cbw - (w + frame);
        if (al && ar) { ml = mr = room / 2; }
        else if (al) ml = room - mr;
        else if (st->align == TA_CENTER && n->tag == T_TABLE) ml = room / 2;
    }
    if (ml < -2000) ml = -2000;
    /* vertical position, collapsing with the margin above */
    int top;
    if (c->pm >= 0 && mt >= 0) top = c->y + (c->pm > mt ? c->pm : mt);
    else if (c->pm < 0 && mt < 0) top = c->y + (c->pm < mt ? c->pm : mt);
    else top = c->y + c->pm + mt;
    if (st->clear && c->fl) { int b = fl_clear(c->fl, st->clear); if (b > top) top = b; }
    int x0 = c->left + ml;
    n->y = top;
    int bgi = st->bg && n != canvas_node && st->vis ? push(DI_RECT) : -1;
    int bgimg = -1;
    if (st->bgimage && st->vis) {
        struct imgent *im = img_get(st->bgimage);
        if (im && (bgimg = push(DI_BGIMG)) >= 0) { IT(bgimg)->img = im; IT(bgimg)->mark = st->bgrep; IT(bgimg)->asc = st->bgcenter; }
    }
    struct fctx cc; memset(&cc, 0, sizeof cc);
    cc.left = x0 + bl + pl; cc.width = w; cc.y = top + bt + pt; cc.bst = st;
    cc.x = cc.left + (st->indent > 0 && st->indent < w / 2 ? st->indent : 0);
    cc.line_first = L->nit;
    cc.fl = st->bfc || forced_w >= 0 || !c->fl || n->tag == T_HTML ? new_flist() : c->fl;
    if (c->mark && cc.y == top) { cc.mark = c->mark; cc.mark_num = c->mark_num; cc.mark_st = c->mark_st; c->mark = 0; }
    if (st->display == D_LIST_ITEM && st->list != LS_NONE) {
        int num = ++c->counter;
        int v = attr_px(n, "value"); if (v >= 0) { num = v; c->counter = v; }
        cc.mark = st->list + 1; cc.mark_num = num; cc.mark_st = st;
    }
    if (n->tag == T_OL) { int s = attr_px(n, "start"); cc.counter = s >= 0 ? s - 1 : 0; }
    struct ictx in = ic; in.st = st; in.deco = ic.deco | st->deco; in.bgst = 0; if (n->link >= 0) in.link = n->link;
    if (!st->vis) in.hidden = true;
    depth++;
    if (n->tag == T_IMG || is_control(n)) replaced_block(&cc, n, &in, w);   /* a replaced element shown as a block */ else if (!((st->display == D_FLEX || st->display == D_INLINE_FLEX) && !st->flex_col && layout_flex(&cc, n, in)))
        flow_children(&cc, n, in);
    depth--;
    flush_line(&cc, false);
    if (cc.mark) { int a, d; metrics(st, &a, &d, 0, 0); draw_marker(&cc, cc.y + a); }
    int content = cc.y + (cc.pm > 0 ? cc.pm : 0) - (top + bt + pt);
    if (cc.fl != c->fl) { int fb = fl_clear(cc.fl, 3) - (top + bt + pt); if (fb > content) content = fb; }   /* its own floats are inside it */
    if (st->h.t == L_PX) { int h = st->h.v - (st->bsizing ? pt + pb + bt + bb : 0); if (h > content) content = h; }
    int bottom = top + bt + pt + content + pb + bb;
    if (bgi >= 0) { struct ditem *b = IT(bgi); b->x = x0; b->y = top; b->w = w + frame; b->h = bottom - top; b->col = st->bg & 0xFFFFFF; }
    if (bgimg >= 0) { struct ditem *b = IT(bgimg); b->x = x0; b->y = top; b->w = w + frame; b->h = bottom - top; }
    borders(st, x0, top, w + frame, bottom - top);
    c->y = bottom; c->pm = mb;
}

static void block_level(struct fctx *c, struct node *n, struct ictx ic) {
    flush_line(c, false);
    layout_block(c, n, ic, -1);
    c->line_first = L->nit; c->x = c->left; c->has = false; c->space = false;
}

void layout_free(struct layout *L_) {
    free(L_->it); free(L_->tp);
    ar_reset(&L_->ar);
    memset(L_, 0, sizeof *L_);
}

void layout_doc(struct doc *d, struct layout *Lo, int width) {
    L = Lo; DOC = d;
    L->nit = 0; L->tlen = 0;
    ar_reset(&L->ar);
    gen++; depth = 0; table_depth = 0; flex_depth = 0; cur_src = d->body ? d->body : d->root;
    L->canvas_bg = 0xFFFFFF;
    canvas_node = 0;
    if (d->root->st && d->root->st->bg) { L->canvas_bg = d->root->st->bg & 0xFFFFFF; canvas_node = d->root; }
    else if (d->body && d->body->st && d->body->st->bg) { L->canvas_bg = d->body->st->bg & 0xFFFFFF; canvas_node = d->body; }
    struct fctx c; memset(&c, 0, sizeof c);
    static struct style rootst; rootst.fsize = 16; rootst.vis = 1;
    c.left = 0; c.width = width; c.y = 0; c.x = 0; c.bst = &rootst; c.line_first = 0; c.fl = new_flist();
    struct ictx ic = { &rootst, 0, -1, 0, false };
    if (d->root->st && d->root->st->display != D_NONE) layout_block(&c, d->root, ic, -1);
    L->doc_h = c.y + (c.pm > 0 ? c.pm : 0);
    L->doc_w = width;
}
