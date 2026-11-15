/* Web 0.3 - shared definitions for the browser's parts:
 *   web.c          window, navigation, loading, images, forms, drawing
 *   web/html.c     HTML tokenizer and document tree
 *   web/css.c      style sheets, selector matching and the cascade
 *   web/layout.c   block, inline and table layout into a display list */
#pragma once
#include "../../lib/dunc.h"
#include "../../lib/img/img.h"

/* ---------------- memory: arenas freed all at once ---------------- */
struct achunk;
struct arena { struct achunk *head; long total; };
void *ar_alloc(struct arena *a, long n);          /* zeroed */
char *ar_strndup(struct arena *a, const char *s, long n);
void ar_reset(struct arena *a);

/* ---------------- document tree ---------------- */
enum {
    T_UNKNOWN, T_HTML, T_HEAD, T_BODY, T_TITLE, T_META, T_LINK, T_STYLE, T_SCRIPT, T_NOSCRIPT, T_BASE, T_TEMPLATE,
    T_DIV, T_P, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_MENU, T_DIR,
    T_PRE, T_LISTING, T_XMP, T_PLAINTEXT, T_BLOCKQUOTE, T_HR, T_BR, T_WBR, T_A, T_IMG, T_TABLE, T_CAPTION, T_COLGROUP, T_COL,
    T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH, T_FORM, T_INPUT, T_TEXTAREA, T_SELECT, T_OPTION, T_OPTGROUP, T_BUTTON,
    T_LABEL, T_FIELDSET, T_LEGEND, T_SPAN, T_B, T_STRONG, T_I, T_EM, T_U, T_S, T_STRIKE, T_DEL, T_INS, T_CODE, T_TT, T_KBD,
    T_SAMP, T_VAR, T_SMALL, T_BIG, T_FONT, T_CENTER, T_SUP, T_SUB, T_MARK, T_ABBR, T_CITE, T_Q, T_DFN, T_NOBR,
    T_SECTION, T_ARTICLE, T_ASIDE, T_NAV, T_HEADER, T_FOOTER, T_MAIN, T_FIGURE, T_FIGCAPTION, T_ADDRESS, T_DETAILS,
    T_SUMMARY, T_HGROUP, T_IFRAME, T_SVG, T_MATH, T_CANVAS, T_VIDEO, T_AUDIO, T_OBJECT, T_EMBED, T_PARAM, T_SOURCE,
    T_TRACK, T_PICTURE, T_AREA, T_MAP, T_FRAME, T_FRAMESET, T_NOFRAMES, T_DIALOG, T_TIME, T_DATA, T_OUTPUT, T_PROGRESS,
    T_METER, T_KEYGEN, T_SEARCH, T__COUNT
};
enum { N_ELEM, N_TEXT };

struct attr { const char *name, *value; };
struct style;
struct fctl;
struct imgent;

struct node {
    struct node *parent, *first, *last, *next, *prev;
    const char *name;                 /* lower-case tag name */
    uint8_t type, depth; uint16_t tag;
    int nattr; struct attr *attrs;
    const char *id; const char **cls; int ncls;
    char *text; int tlen;             /* text nodes: entity-decoded Windows-1252 text */
    const char *style_attr;
    struct style *st;                 /* computed style (null: not rendered) */
    struct style *st_mem;             /* its memory, kept for the next cascade */
    struct fctl *fc;                  /* form control state */
    struct imgent *img;               /* <img> */
    int link;                         /* index into the page's links, or -1 */
    int y;                            /* layout position (for #fragment scrolling) */
    int bx, by, bw, bh, bgen;         /* its border box from the last layout (bgen == layout_pass), for scripts */
    int minw, maxw, iw_gen;           /* intrinsic widths, cached per layout */
    struct node *form;                /* the form a control belongs to */
    int jsw;                          /* scripts: index+1 of this node's JavaScript object (0: none yet) */
    /* shadow DOM: a host's shadow root (a "#shadow-root" node, not in the host's children; its parent is the
     * host, its host field too). What is drawn is the flattened tree (ffirst/fnext): an element's shadow
     * content instead of its children, with each <slot> replaced by the host children assigned to it. */
    struct node *shadow, *host, *ffirst, *fnext;
    uint32_t fgen;
    uint8_t css_done;                 /* <style>/<link>: its sheet is loaded */
};

struct doc {
    struct arena ar;
    struct node *root, *body, *head;
    char title[160];
    char base[600];                   /* base URL */
    int nnodes;
    char charset[32];
    bool standards;                   /* had a <!doctype html> (else quirks mode) */
};
/* parse HTML (already Windows-1252) into a new tree in d->ar */
void html_parse(struct doc *d, const char *s, long n);
const char *node_attr(const struct node *n, const char *name);
int tag_lookup(const char *name, int len);
/* decode an entity at s (s[0] == '&'): writes one Windows-1252 byte, returns how many bytes were used (0: not an entity) */
int html_entity(const char *s, long n, char *out);
/* Windows-1252 text from a UTF-8 page (invalid bytes become '?'); returns the new length */
long utf8_lenient_to_cp1252(char *s, long n);

/* ---------------- styles ---------------- */
enum { D_NONE, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW_GROUP, D_ROW, D_CELL, D_CAPTION, D_INLINE_TABLE, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };
enum { JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND };
enum { GT_PX, GT_PCT, GT_FR, GT_AUTO };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PREWRAP, WS_PRELINE };
enum { TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LALPHA, LS_UALPHA, LS_LROMAN, LS_UROMAN, LS_NONE };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUPER, VA_SUB };
#define DECO_UNDER 1
#define DECO_STRIKE 2
#define DECO_OVER 4
enum { L_AUTO, L_PX, L_PCT, L_NONE };       /* L_PCT: v is percent * 100 */
struct len { int16_t t; int32_t v; };
struct cvar { const char *name, *value; struct cvar *next; };

struct style {
    uint8_t display, ws, align, deco, list, vis, bold, italic, mono, valign, valign_set, bsizing, floated, list_inside, tt, pos, flex_col, flex_wrap, justify, bcollapse, clear, bfc, off_top, off_left;
    int fsize;                        /* px */
    int lh;                           /* line height in px, 0: normal */
    int lh_num;                       /* line height as a factor * 100 (inherited as a factor), 0: not */
    uint32_t color, bg;               /* bg: 0 = transparent, else 0xFF000000 | rgb */
    struct len m[4], p[4];            /* top right bottom left */
    int bw[4]; uint32_t bc[4]; uint8_t bs[4];   /* border width, colour, style (0 none, 1 solid, 2 other visible) */
    struct len w, minw, maxw, h;
    struct len inset[4];              /* top right bottom left (positioned boxes) */
    int z;                            /* z-index */
    int indent;                       /* text-indent px */
    int spacing;                      /* table border-spacing px */
    int gap;                          /* flex/grid gap between columns, px */
    int rgap;                         /* gap between rows, px */
    int radius;                       /* border-radius px (-1: half the box, a pill or circle) */
    struct len tx, ty;                /* transform: translate(x, y) (percentages are of the box's own size) */
    uint32_t bg2; uint8_t grad;       /* linear-gradient: its last colour, and direction (1 down, 2 right, 3 up, 4 left) */
    int16_t fgrow, fshrink;           /* flex-grow, flex-shrink, * 100 */
    struct len fbasis;                /* flex-basis (L_AUTO: the width) */
    uint8_t align_items, align_self;  /* 0 stretch (self: 0 auto), 1 start, 2 center, 3 end */
    /* grid-template-columns: tracks (type GT_PX: px, GT_PCT: percent * 100, GT_FR: fr * 100, GT_AUTO), or
     * as many columns of at least grid_auto_min px as fit (repeat(auto-fill, minmax(Npx, 1fr))) */
    uint8_t ngrid; struct { uint8_t t; int32_t v; } grid[12];
    int grid_auto_min;
    uint8_t col_span;                 /* grid-column: span N (0: 1), 255: the whole row (1 / -1) */
    const char *bgimage;              /* background-image url (resolved) */
    uint8_t bgrep, bgcenter;          /* background-repeat: 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y; position centred */
    struct cvar *vars;
};

void css_init(void);                                  /* parse the built-in style sheet */
void css_reset(struct arena *page);                   /* forget the author style sheets */
void css_add_sheet(const char *text, long len, const char *base_url, int import_depth);
void css_cascade(struct doc *d, int viewport_w);      /* compute node->st for every element (and the flattened tree) */
void css_compose(struct doc *d);                      /* the flattened tree (ffirst/fnext) for shadow DOM */
struct node *css_tree_host(const struct node *n);     /* the host of the shadow tree n is in (0: the document) */
/* a shadow root's style sheet, rewritten so its rules apply inside its host only (:host, ::slotted) */
char *css_scope_shadow(const char *css, long n, const char *host_tag, long *out_len);
bool css_parse_color(const char *s, int n, uint32_t *out);
bool css_media_dependent(void);
void *css_save(void);                                 /* the current author rules, to put back with css_restore */
void css_restore(void *saved);                        /* (frees saved) */
bool css_media_matches(const char *s, int n, int width);   /* a media attribute / query list */                       /* some rules depend on the window width */
/* fetch a style sheet for @import (implemented in web.c); returns malloc'd text or 0 */
char *web_fetch_text(const char *url, long *len, long cap);
void resolve_url(const char *base, const char *href, char *out, int n);

/* ---------------- fonts ---------------- */
const struct font *web_font(int px, bool bold, bool italic);
#define MONO_W 8
#define MONO_H 16

/* ---------------- display list ---------------- */
enum { DI_RECT, DI_TEXT, DI_MONO, DI_IMG, DI_CTRL, DI_MARK, DI_FRAME, DI_BGIMG, DI_RFRAME };   /* DI_RFRAME: a rounded border */
struct ditem {
    uint8_t kind, deco, mark; int16_t link_hi;
    int16_t layer;                    /* painting order: positioned boxes and z-index (items are sorted by it) */
    int x, y, w, h;                   /* DI_TEXT/DI_MONO: y = top of the text box, h its height; base = y + asc */
    int asc;
    uint32_t col;
    uint32_t off; uint32_t len;        /* text in the layout's text pool */
    const struct font *f;
    struct node *n;                   /* DI_IMG, DI_CTRL: the element */
    struct imgent *img;               /* DI_BGIMG: the picture */
    int link;                         /* link index, -1 */
    struct node *src;                 /* the element this item was drawn for (for scripts' events) */
};
struct layout {
    struct arena ar;
    struct ditem *it; int nit, cap;
    char *tp; long tlen, tcap;        /* text pool */
    int doc_h, doc_w;
    uint32_t canvas_bg;
};
void layout_doc(struct doc *d, struct layout *L, int width);
extern int layout_viewport_h;
extern int layout_pass;                                /* counts layouts: a node's box is current if bgen == layout_pass */                          /* the visible height: fixed boxes are placed against it */
void layout_free(struct layout *L);

/* ---------------- links, images, forms (owned by web.c) ---------------- */
struct link { const char *href; struct node *n; };
extern struct link *links; extern int nlinks, links_cap;
int add_link(const char *href, struct node *n);

enum { IS_PENDING, IS_LOADING, IS_OK, IS_FAILED };
struct imgent {
    char *url; int state; int w, h;   /* w/h: intrinsic size once known */
    uint32_t *px; int pw, ph;         /* decoded pixels (maybe reduced to pw x ph) */
    uint32_t *spx; int sw, sh;        /* a scaled copy for the size it's shown at */
    bool opaque; bool used;
    struct node *svg; uint32_t svg_gen; /* an <svg> element drawn into px at the size it's shown (svg.c) */
    struct imgent *next;
};
struct imgent *img_get(const char *url);
void svg_size(const struct node *svg, int *w, int *h);                 /* the size an <svg> asks for */
uint32_t *svg_render(struct node *svg, int w, int h, uint32_t cur);    /* a new w x h picture of it (malloc) */          /* find or add a cache entry */

enum { FC_TEXT, FC_PASSWORD, FC_HIDDEN, FC_SUBMIT, FC_RESET, FC_BUTTON, FC_CHECKBOX, FC_RADIO, FC_NUMBER, FC_TEXTAREA, FC_SELECT, FC_IMAGE, FC_FILE, FC_MEDIA };
struct fctl {
    int type;
    char *val; int len, cap, cur;     /* text */
    bool checked, disabled, readonly;
    int sel, nopt;                    /* select */
    struct node **opts;
    int size, rows, cols;             /* size: characters (text), rows/cols: textarea */
    int scroll;                       /* textarea: first line shown */
    const char *label;                /* buttons */
    void *media;                      /* FC_MEDIA: <video>/<audio> state (web/media.c) */
};
void form_init_control(struct doc *d, struct node *n);
void form_option_text(struct node *opt, char *out, int max);

/* ---------------- scripts (web/dom.c) <-> the browser (web.c) ---------------- */
bool css_query_match(const char *sel, int n, const struct node *e);   /* querySelector, matches() */
/* web.c, for dom.c */
extern struct doc *web_doc;
void web_dom_changed(struct node *added);       /* the tree or styles changed (added: a new subtree to set up, or 0) */
void web_navigate(const char *url);
void web_text_changed(struct node *n);          /* text inside n changed (button labels follow it) */
void web_page_url(char *out, int n);
void web_set_title(const char *t);
void web_viewport(int *w, int *h, int *scroll, int *doc_h);
void web_scroll_to(int y);
void web_message(const char *title, const char *msg);   /* alert() */
bool web_event_for_bar(const struct dunc_event *e);      /* a click or key meant for Arc's bar, not the page */
void web_still_alive(void);                              /* a script is making progress: restart the stuck alarm */
void web_busy_status(const char *msg);                  /* a note in the status bar while a script runs (drawn at once) */
bool web_node_rect(struct node *n, int *x, int *y, int *w, int *h);   /* where it is drawn (page coordinates) */
long web_http_script(const char *url, char **out, int *status, char *ctype, int ctlen, char *final, int fulen);   /* up to 24 MB */
long web_http(const char *method, const char *url, const char *body, long blen, const char *headers,
              char **out, int *status, char *ctype, int ctlen, char *final, int fulen);
/* dom.c, for web.c */
void js_page_start(struct doc *d);              /* run the page's scripts (after it's shown) */
void js_page_end(void);                         /* the page goes away */
bool js_page_active(void);
bool js_event(struct node *target, const char *type, int x, int y, int key);   /* returns true if a script cancelled the default */
long js_timer_wait(void);                       /* ms until the next timer is due (-1: none) */
void js_run_timers(void);
void js_eval_url(const char *code);             /* a javascript: link */
void js_console_lines(char *out, long max);     /* for about:console */
bool js_scripts_pending(void);
void js_abandon(uint64_t where);   /* Web got stuck in a page's script: drop its scripts without running anything of them */
void web_show_status(const char *s);
extern long web_fetch_timeout;
extern uint64_t web_fetch_deadline;   /* if set: a download gives up at this time (sys_time) */   /* ms; 0 = the default (15 s) */
void dom_check_soon(void);
void js_stop_scripts(void);
bool js_page_step(char *status, int n);
extern int web_time_scale;              /* 1; 10 on a slow test machine (setting web.test_slow) */
extern int web_script_waiting;          /* set while a running script waits for the network: events are held back */
void web_requeue_event(const struct dunc_event *e);   /* an event seen while a script ran, handled afterwards */

/* ---------------- <video> and <audio> (web/media.c) ---------------- */
struct canvas;
void media_attach(struct doc *d, struct node *n);  /* give a <video>/<audio> its player (as an FC_MEDIA control) */
void media_page_end(void);                         /* stop and forget them all */
void media_box(struct node *n, int *w, int *h);
/* Media Source Extensions (media.c) */
int media_mse_new(void);
int media_mse_supported(const char *mime);
int media_mse_add(int id, const char *mime);
int media_mse_append(int id, int bi, const uint8_t *p, long n);
void media_mse_buffered(int id, int bi, double *start, double *end);
void media_mse_duration(int id, double d);
void media_mse_end(int id);
void media_mse_attach(struct node *n, int id);    /* its size before styles */
void media_draw(struct canvas *c, struct node *n, int x, int y, int w, int h);
void media_click(struct node *n, int x, int y, int w, int h);   /* x, y inside the box */
void media_toggle(struct node *n);                 /* play / pause */
long media_tick(void);                             /* main loop: download, decode, show; ms it may wait (-1: idle) */
double media_js(struct node *n, const char *op, const char *name, double v);
const char *media_error_text(struct node *n);
const char *media_src(struct node *n);
/* web.c, for media.c */
void web_redraw_node(struct node *n);              /* draw just this element's box and show it */
void web_media_resized(void);                      /* a video's size became known: lay out again */
/* download a whole file (growing buffer up to cap) while the window keeps answering; progress text goes to
 * msg (and the element is redrawn). Returns the length, -2 when over cap, other negatives on errors. */
long web_download(const char *url, long cap, char **out, int *status, char *ctype, int ctlen, char *msg, int msglen, struct node *n);
