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
    int minw, maxw, iw_gen;           /* intrinsic widths, cached per layout */
    struct node *form;                /* the form a control belongs to */
};

struct doc {
    struct arena ar;
    struct node *root, *body, *head;
    char title[160];
    char base[600];                   /* base URL */
    int nnodes;
    char charset[32];
    bool standards;                   /* had a <!doctype html> (else quirks mode) */
    struct node *forms;               /* unused (forms are found through node->form) */
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
enum { D_NONE, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW_GROUP, D_ROW, D_CELL, D_CAPTION, D_INLINE_TABLE, D_FLEX, D_INLINE_FLEX };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };
enum { JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND };
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
    uint8_t display, ws, align, deco, list, vis, bold, italic, mono, valign, valign_set, bsizing, floated, list_inside, tt, pos, flex_col, flex_wrap, justify, bcollapse, clear, bfc;
    int fsize;                        /* px */
    int lh;                           /* line height in px, 0: normal */
    int lh_num;                       /* line height as a factor * 100 (inherited as a factor), 0: not */
    uint32_t color, bg;               /* bg: 0 = transparent, else 0xFF000000 | rgb */
    struct len m[4], p[4];            /* top right bottom left */
    int bw[4]; uint32_t bc[4]; uint8_t bs[4];   /* border width, colour, style (0 none, 1 solid, 2 other visible) */
    struct len w, minw, maxw, h;
    int indent;                       /* text-indent px */
    int spacing;                      /* table border-spacing px */
    int gap;                          /* flex gap px */
    const char *bgimage;              /* background-image url (resolved) */
    uint8_t bgrep, bgcenter;          /* background-repeat: 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y; position centred */
    struct cvar *vars;
};

void css_init(void);                                  /* parse the built-in style sheet */
void css_reset(struct arena *page);                   /* forget the author style sheets */
void css_add_sheet(const char *text, long len, const char *base_url, int import_depth);
void css_cascade(struct doc *d, int viewport_w);      /* compute node->st for every element */
bool css_parse_color(const char *s, int n, uint32_t *out);
bool css_media_dependent(void);
bool css_media_matches(const char *s, int n, int width);   /* a media attribute / query list */                       /* some rules depend on the window width */
/* fetch a style sheet for @import (implemented in web.c); returns malloc'd text or 0 */
char *web_fetch_text(const char *url, long *len, long cap);
void resolve_url(const char *base, const char *href, char *out, int n);

/* ---------------- fonts ---------------- */
const struct font *web_font(int px, bool bold, bool italic);
#define MONO_W 8
#define MONO_H 16

/* ---------------- display list ---------------- */
enum { DI_RECT, DI_TEXT, DI_MONO, DI_IMG, DI_CTRL, DI_MARK, DI_FRAME, DI_BGIMG };
struct ditem {
    uint8_t kind, deco, mark; int16_t link_hi;
    int x, y, w, h;                   /* DI_TEXT/DI_MONO: y = top of the text box, h its height; base = y + asc */
    int asc;
    uint32_t col;
    uint32_t off; uint32_t len;        /* text in the layout's text pool */
    const struct font *f;
    struct node *n;                   /* DI_IMG, DI_CTRL: the element */
    struct imgent *img;               /* DI_BGIMG: the picture */
    int link;                         /* link index, -1 */
};
struct layout {
    struct arena ar;
    struct ditem *it; int nit, cap;
    char *tp; long tlen, tcap;        /* text pool */
    int doc_h, doc_w;
    uint32_t canvas_bg;
};
void layout_doc(struct doc *d, struct layout *L, int width);
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
    struct imgent *next;
};
struct imgent *img_get(const char *url);          /* find or add a cache entry */

enum { FC_TEXT, FC_PASSWORD, FC_HIDDEN, FC_SUBMIT, FC_RESET, FC_BUTTON, FC_CHECKBOX, FC_RADIO, FC_NUMBER, FC_TEXTAREA, FC_SELECT, FC_IMAGE, FC_FILE };
struct fctl {
    int type;
    char *val; int len, cap, cur;     /* text */
    bool checked, disabled, readonly;
    int sel, nopt;                    /* select */
    struct node **opts;
    int size, rows, cols;             /* size: characters (text), rows/cols: textarea */
    int scroll;                       /* textarea: first line shown */
    const char *label;                /* buttons */
};
void form_init_control(struct doc *d, struct node *n);
void form_option_text(struct node *opt, char *out, int max);
