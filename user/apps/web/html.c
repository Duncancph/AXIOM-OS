/* Web: HTML tokenizer and tree builder (a forgiving subset of the HTML5 rules). */
#include "web.h"

/* ---------------- arenas ---------------- */
struct achunk { struct achunk *next; long size, used; long pad; };
void *ar_alloc(struct arena *a, long n) {
    n = (n + 15) & ~15l;
    struct achunk *c = a->head;
    if (!c || c->used + n > c->size) {
        long sz = n > (1 << 20) - 64 ? n + 64 : (1 << 20);
        c = malloc(sizeof *c + sz);
        if (!c) return 0;
        c->size = sz; c->used = 0; c->next = a->head; a->head = c;
        a->total += sz;
    }
    void *p = (char *)(c + 1) + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}
char *ar_strndup(struct arena *a, const char *s, long n) {
    char *p = ar_alloc(a, n + 1);
    if (!p) return 0;
    memcpy(p, s, n); p[n] = 0;
    return p;
}
void ar_reset(struct arena *a) {
    for (struct achunk *c = a->head, *nx; c; c = nx) { nx = c->next; free(c); }
    a->head = 0; a->total = 0;
}

/* ---------------- tags ---------------- */
static const char *const tag_names[T__COUNT] = {
    "", "html", "head", "body", "title", "meta", "link", "style", "script", "noscript", "base", "template",
    "div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "menu", "dir",
    "pre", "listing", "xmp", "plaintext", "blockquote", "hr", "br", "wbr", "a", "img", "table", "caption", "colgroup", "col",
    "thead", "tbody", "tfoot", "tr", "td", "th", "form", "input", "textarea", "select", "option", "optgroup", "button",
    "label", "fieldset", "legend", "span", "b", "strong", "i", "em", "u", "s", "strike", "del", "ins", "code", "tt", "kbd",
    "samp", "var", "small", "big", "font", "center", "sup", "sub", "mark", "abbr", "cite", "q", "dfn", "nobr",
    "section", "article", "aside", "nav", "header", "footer", "main", "figure", "figcaption", "address", "details",
    "summary", "hgroup", "iframe", "svg", "math", "canvas", "video", "audio", "object", "embed", "param", "source",
    "track", "picture", "area", "map", "frame", "frameset", "noframes", "dialog", "time", "data", "output", "progress",
    "meter", "keygen", "search",
};
int tag_lookup(const char *name, int len) {
    for (int i = 1; i < T__COUNT; i++) if ((int)strlen(tag_names[i]) == len && !memcmp(tag_names[i], name, len)) return i;
    return T_UNKNOWN;
}
static bool is_void(int t) {
    switch (t) { case T_META: case T_LINK: case T_BASE: case T_HR: case T_BR: case T_WBR: case T_IMG: case T_COL: case T_INPUT:
    case T_PARAM: case T_SOURCE: case T_TRACK: case T_AREA: case T_EMBED: case T_FRAME: case T_KEYGEN: return true; }
    return false;
}
/* start tags that close an open <p> */
static bool closes_p(int t) {
    switch (t) { case T_ADDRESS: case T_ARTICLE: case T_ASIDE: case T_BLOCKQUOTE: case T_DETAILS: case T_DIV: case T_DL: case T_FIELDSET:
    case T_FIGCAPTION: case T_FIGURE: case T_FOOTER: case T_FORM: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
    case T_HEADER: case T_HGROUP: case T_HR: case T_MAIN: case T_MENU: case T_NAV: case T_OL: case T_P: case T_PRE: case T_SECTION:
    case T_TABLE: case T_UL: case T_DD: case T_DT: case T_LI: case T_LISTING: case T_XMP: case T_PLAINTEXT: case T_CENTER: case T_DIR:
    case T_SUMMARY: case T_DIALOG: case T_SEARCH: return true; }
    return false;
}
static bool is_scope(int t) {
    return t == T_TABLE || t == T_TD || t == T_TH || t == T_CAPTION || t == T_HTML || t == T_BODY || t == T_TEMPLATE ||
           t == T_BUTTON || t == T_OBJECT || t == T_SVG || t == T_MATH || t == T_SELECT;
}

/* ---------------- entities ---------------- */
static const struct { const char *n; uint16_t u; } ents[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 160}, {"copy", 169}, {"reg", 174},
    {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
    {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"middot", 183}, {"raquo", 187}, {"laquo", 171},
    {"bull", 0x2022}, {"euro", 0x20AC}, {"pound", 163}, {"yen", 165}, {"cent", 162}, {"curren", 164}, {"sect", 167}, {"para", 182},
    {"deg", 176}, {"plusmn", 177}, {"times", 215}, {"divide", 247}, {"frac12", 189}, {"frac14", 188}, {"frac34", 190},
    {"sup1", 185}, {"sup2", 178}, {"sup3", 179}, {"micro", 181}, {"iexcl", 161}, {"iquest", 191}, {"acute", 180}, {"cedil", 184},
    {"uml", 168}, {"macr", 175}, {"ordf", 170}, {"ordm", 186}, {"not", 172}, {"shy", 173}, {"brvbar", 166},
    {"dagger", 0x2020}, {"Dagger", 0x2021}, {"permil", 0x2030}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"OElig", 0x152},
    {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"Zcaron", 0x17D}, {"zcaron", 0x17E},
    {"fnof", 0x192}, {"circ", 0x2C6}, {"tilde", 0x2DC}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003},
    {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F}, {"minus", 0x2212}, {"prime", 0x2032},
    {"Agrave", 192}, {"Aacute", 193}, {"Acirc", 194}, {"Atilde", 195}, {"Auml", 196}, {"Aring", 197}, {"AElig", 198},
    {"Ccedil", 199}, {"Egrave", 200}, {"Eacute", 201}, {"Ecirc", 202}, {"Euml", 203}, {"Igrave", 204}, {"Iacute", 205},
    {"Icirc", 206}, {"Iuml", 207}, {"ETH", 208}, {"Ntilde", 209}, {"Ograve", 210}, {"Oacute", 211}, {"Ocirc", 212},
    {"Otilde", 213}, {"Ouml", 214}, {"Oslash", 216}, {"Ugrave", 217}, {"Uacute", 218}, {"Ucirc", 219}, {"Uuml", 220},
    {"Yacute", 221}, {"THORN", 222}, {"szlig", 223}, {"agrave", 224}, {"aacute", 225}, {"acirc", 226}, {"atilde", 227},
    {"auml", 228}, {"aring", 229}, {"aelig", 230}, {"ccedil", 231}, {"egrave", 232}, {"eacute", 233}, {"ecirc", 234},
    {"euml", 235}, {"igrave", 236}, {"iacute", 237}, {"icirc", 238}, {"iuml", 239}, {"eth", 240}, {"ntilde", 241},
    {"ograve", 242}, {"oacute", 243}, {"ocirc", 244}, {"otilde", 245}, {"ouml", 246}, {"oslash", 248}, {"ugrave", 249},
    {"uacute", 250}, {"ucirc", 251}, {"uuml", 252}, {"yacute", 253}, {"thorn", 254}, {"yuml", 255},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"hearts", 0x2665},
};
/* a few Unicode characters Windows-1252 lacks that read well as ASCII */
static int fallback(unsigned u) {
    switch (u) {
    case 0x2190: return '<'; case 0x2192: return '>'; case 0x2191: return '^'; case 0x2193: return 'v'; case 0x2194: return '-';
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: case 0xAD: return 0;   /* invisible */
    case 0x2715: case 0x2716: case 0x2717: return 'x'; case 0x2713: case 0x2714: return 'v';
    case 0x25B2: return '^'; case 0x25BC: return 'v'; case 0x25B6: case 0x25BA: return '>'; case 0x25C0: case 0x25C4: return '<';
    case 0x2018: case 0x2019: return '\'';
    }
    return '?';
}
static int uni_to_cp(unsigned u) {
    if (u == 0xAD) return 0;
    int b = cp1252_from_unicode(u);
    if (b >= 0 && b != 0x81 && b != 0x8D && b != 0x8F && b != 0x90 && b != 0x9D) return b;
    return fallback(u);
}
int html_entity(const char *s, long n, char *out) {
    if (n < 2) return 0;
    if (s[1] == '#') {
        long v = 0; int i = 2; bool hx = n > 2 && (s[2] == 'x' || s[2] == 'X'); if (hx) i++;
        int digits = 0;
        for (; i < n && digits < 8; i++, digits++) {
            char c = s[i] | 32;
            if (c >= '0' && c <= '9') v = v * (hx ? 16 : 10) + c - '0';
            else if (hx && c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
            else break;
        }
        if (!digits) return 0;
        if (i < n && s[i] == ';') i++;
        int b = v >= 0x80 && v < 0xA0 ? (int)v : uni_to_cp((unsigned)v);
        if (v == 9 || v == 10) b = ' ';
        else if (v < 32) b = '?';
        *out = (char)b; return i;
    }
    int l = 0; while (l + 1 < n && l < 10 && ((s[l + 1] | 32) >= 'a' && (s[l + 1] | 32) <= 'z' ? 1 : (s[l + 1] >= '0' && s[l + 1] <= '9'))) l++;
    for (unsigned k = 0; k < sizeof ents / sizeof ents[0]; k++) {
        int el = strlen(ents[k].n);
        if (el > l || memcmp(s + 1, ents[k].n, el)) continue;
        bool semi = el + 1 < n && s[el + 1] == ';';
        if (!semi && (el != l || ents[k].u > 255)) continue;   /* old pages: "&amp" / "&copy" without ';' */
        *out = (char)uni_to_cp(ents[k].u);
        return el + 1 + semi;
    }
    return 0;
}

long utf8_lenient_to_cp1252(char *s, long n) {
    const unsigned char *p = (const unsigned char *)s;
    long o = 0;
    for (long i = 0; i < n;) {
        unsigned c = p[i];
        if (c < 0x80) { s[o++] = (char)c; i++; continue; }
        int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len && i + len <= n;
        for (int k = 1; ok && k < len; k++) if ((p[i + k] & 0xC0) != 0x80) ok = false;
        if (!ok) { s[o++] = '?'; i++; continue; }
        unsigned u = len == 2 ? (c & 0x1F) << 6 | (p[i + 1] & 0x3F)
                   : len == 3 ? (c & 0x0F) << 12 | (p[i + 1] & 0x3F) << 6 | (p[i + 2] & 0x3F)
                   : (c & 0x07) << 18 | (p[i + 1] & 0x3F) << 12 | (p[i + 2] & 0x3F) << 6 | (p[i + 3] & 0x3F);
        i += len;
        int b = uni_to_cp(u);
        if (b) s[o++] = (char)b;
    }
    return o;
}

/* ---------------- tree building ---------------- */
#define MAX_DEPTH 120
#define MAX_NODES 80000
static struct doc *D;
static struct node *stack[MAX_DEPTH + 2]; static int sp;
static struct node *cur_form;
static bool in_body;

static struct node *new_node(int type) {
    if (D->nnodes >= MAX_NODES) return 0;
    struct node *n = ar_alloc(&D->ar, sizeof *n);
    if (!n) return 0;
    D->nnodes++;
    n->type = type; n->link = -1;
    return n;
}
static void append(struct node *p, struct node *c) {
    c->parent = p; c->depth = p->depth < 250 ? p->depth + 1 : 250;
    c->prev = p->last;
    if (p->last) p->last->next = c; else p->first = c;
    p->last = c;
}
const char *node_attr(const struct node *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return 0;
}

static struct node *top(void) { return stack[sp - 1]; }
static void ensure_body(void) {
    if (in_body) return;
    in_body = true;
    if (!D->body) { D->body = new_node(N_ELEM); if (!D->body) { D->body = D->root; return; } D->body->name = "body"; D->body->tag = T_BODY; append(D->root, D->body); }
    sp = 1; stack[sp++] = D->body;
}
static void pop_to(int i) { if (i >= 1) sp = i; }       /* pop stack[i] and everything above it */
static int find_open(int tag, const char *name, bool scoped) {
    for (int i = sp - 1; i >= 1; i--) {
        struct node *n = stack[i];
        if (tag ? n->tag == tag : (n->tag == T_UNKNOWN && !strcmp(n->name, name))) return i;
        if (scoped && is_scope(n->tag)) return -1;
    }
    return -1;
}
static int find_open_until(int tag, int stop1, int stop2) {
    for (int i = sp - 1; i >= 1; i--) {
        if (stack[i]->tag == tag) return i;
        if (stack[i]->tag == stop1 || stack[i]->tag == stop2 || is_scope(stack[i]->tag)) return -1;
    }
    return -1;
}

static void add_text(const char *s, long n) {
    if (!n) return;
    if (!in_body) {
        bool ws = true; for (long i = 0; i < n; i++) if (s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t' && s[i] != '\f') ws = false;
        if (ws) return;
        ensure_body();
    }
    struct node *p = top(), *l = p->last;
    if (l && l->type == N_TEXT) {                        /* join with the previous text node */
        char *t = ar_alloc(&D->ar, l->tlen + n + 1);
        if (!t) return;
        memcpy(t, l->text, l->tlen); memcpy(t + l->tlen, s, n); l->tlen += n; t[l->tlen] = 0; l->text = t;
        return;
    }
    struct node *t = new_node(N_TEXT);
    if (!t) return;
    t->text = ar_strndup(&D->ar, s, n); t->tlen = n;
    append(p, t);
}

/* text with entities decoded */
static void add_text_decoded(const char *s, long n) {
    char *b = ar_alloc(&D->ar, n + 1);
    if (!b) return;
    long k = 0;
    for (long i = 0; i < n; i++) {
        if (s[i] == '&') { char c; int u = html_entity(s + i, n - i, &c); if (u) { if (c) b[k++] = c; i += u - 1; continue; } }
        if (s[i] == '\r') { if (i + 1 < n && s[i + 1] == '\n') continue; b[k++] = '\n'; continue; }
        if (s[i] == 0) continue;
        b[k++] = s[i];
    }
    add_text(b, k);
}

static char *decode_attr(const char *s, long n) {
    char *b = ar_alloc(&D->ar, n + 1);
    if (!b) return 0;
    long k = 0;
    for (long i = 0; i < n; i++) {
        if (s[i] == '&') {
            char c; int u = html_entity(s + i, n - i, &c);
            if (u) { if (c) b[k++] = c; i += u - 1; continue; }
        }
        b[k++] = s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i];
    }
    b[k] = 0;
    return b;
}

static void insert_element(struct node *n) {
    int t = n->tag;
    if (t == T_HTML) { if (!D->root->nattr) { D->root->attrs = n->attrs; D->root->nattr = n->nattr; } return; }
    if (t == T_HEAD) return;
    if (t == T_BODY) {
        if (!D->body || !D->body->nattr) {
            if (D->body) { D->body->attrs = n->attrs; D->body->nattr = n->nattr; D->body->id = n->id; D->body->cls = n->cls; D->body->ncls = n->ncls; D->body->style_attr = n->style_attr; }
            else { D->body = n; append(D->root, n); in_body = true; sp = 1; stack[sp++] = n; return; }
        }
        ensure_body();
        return;
    }
    bool headish = t == T_TITLE || t == T_META || t == T_LINK || t == T_STYLE || t == T_SCRIPT || t == T_BASE || t == T_TEMPLATE;
    if (!in_body && headish) { append(D->head, n); return; }
    ensure_body();
    /* implied end tags */
    if (closes_p(t)) { int i = find_open(T_P, 0, true); if (i > 0) pop_to(i); }
    if (t == T_LI) { int i = find_open_until(T_LI, T_UL, T_OL); if (i > 0) pop_to(i); }
    if (t == T_DT || t == T_DD) { int i = find_open_until(T_DD, T_DL, 0), j = find_open_until(T_DT, T_DL, 0); if (j > i) i = j; if (i > 0) pop_to(i); }
    if (t == T_OPTION || t == T_OPTGROUP) { if (top()->tag == T_OPTION) sp--; if (t == T_OPTGROUP && top()->tag == T_OPTGROUP) sp--; }
    if (t >= T_H1 && t <= T_H6 && top()->tag >= T_H1 && top()->tag <= T_H6) sp--;
    if (t == T_TR) { int i = find_open_until(T_TR, T_TABLE, 0); if (i > 0) pop_to(i); }
    if (t == T_TD || t == T_TH) { int i = find_open_until(T_TD, T_TR, T_TABLE), j = find_open_until(T_TH, T_TR, T_TABLE);
        if (i < 0 && j < 0) { /* a cell's own scope stops the search: look past it */
            for (int k = sp - 1; k >= 1; k--) { int g = stack[k]->tag; if (g == T_TD || g == T_TH) { i = k; break; } if (g == T_TR || g == T_TABLE) break; }
        }
        if (j > i) i = j;
        if (i > 0) pop_to(i); }
    if (t == T_THEAD || t == T_TBODY || t == T_TFOOT) {
        for (int k = sp - 1; k >= 1; k--) { int g = stack[k]->tag; if (g == T_THEAD || g == T_TBODY || g == T_TFOOT) { pop_to(k); break; } if (g == T_TABLE) break; }
    }
    if (t == T_A) { int i = find_open(T_A, 0, true); if (i > 0) pop_to(i); }
    if ((t == T_SELECT || t == T_BUTTON) && find_open(t, 0, false) > 0) pop_to(find_open(t, 0, false));
    if (t == T_FORM) { if (cur_form) return; cur_form = n; }
    if (t == T_INPUT || t == T_TEXTAREA || t == T_SELECT || t == T_BUTTON) n->form = cur_form;
    append(top(), n);
    if (!is_void(t) && sp <= MAX_DEPTH) stack[sp++] = n;
}

static void end_tag(int t, const char *name) {
    if (t == T_BODY || t == T_HTML || t == T_HEAD) return;
    if (t == T_BR) { struct node *n = new_node(N_ELEM); if (n) { n->name = "br"; n->tag = T_BR; ensure_body(); append(top(), n); } return; }
    if (t == T_FORM) { cur_form = 0; int i = find_open(T_FORM, 0, true); if (i > 0) pop_to(i); return; }
    if (!in_body) return;
    int i = -1;
    if (t == T_TABLE || t == T_TD || t == T_TH || t == T_CAPTION || t == T_SELECT || t == T_BUTTON) {
        for (int k = sp - 1; k >= 1; k--) if (stack[k]->tag == t) { i = k; break; } else if (stack[k]->tag == T_TABLE && t != T_TABLE) break;
    } else if (t == T_TR || t == T_TBODY || t == T_THEAD || t == T_TFOOT) {
        for (int k = sp - 1; k >= 1; k--) if (stack[k]->tag == t) { i = k; break; } else if (stack[k]->tag == T_TABLE) break;
    } else i = find_open(t, name, true);
    if (i > 0) pop_to(i);
}

static bool is_name_char(char c) { return c && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '/' && c != '>'; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* index just past "</name" ... ">" (case-insensitive), or n */
static long find_end(const char *s, long i, long n, const char *name, long *tag_start) {
    int l = strlen(name);
    for (long k = i; k + 1 + l < n; k++) {
        if (s[k] != '<' || s[k + 1] != '/') continue;
        bool m = true;
        for (int j = 0; j < l; j++) if (lc(s[k + 2 + j]) != name[j]) { m = false; break; }
        if (!m || is_name_char(s[k + 2 + l]) ) continue;
        *tag_start = k;
        long e = k + 2 + l; while (e < n && s[e] != '>') e++;
        return e < n ? e + 1 : n;
    }
    *tag_start = n;
    return n;
}

void html_parse(struct doc *d, const char *s, long n) {
    D = d; sp = 0; cur_form = 0; in_body = false;
    d->nnodes = 0; d->standards = false;
    d->root = new_node(N_ELEM); d->root->name = "html"; d->root->tag = T_HTML;
    d->head = new_node(N_ELEM); d->head->name = "head"; d->head->tag = T_HEAD; append(d->root, d->head);
    d->body = 0;
    stack[sp++] = d->root;
    struct attr at[64];
    bool skip_nl = false;
    long i = 0;
    while (i < n && d->nnodes < MAX_NODES) {
        if (s[i] != '<') {
            long j = i; while (j < n && s[j] != '<') j++;
            const char *t = s + i; long tl = j - i;
            if (skip_nl && tl && (*t == '\n' || *t == '\r')) { if (*t == '\r' && tl > 1 && t[1] == '\n') { t++; tl--; } t++; tl--; }
            skip_nl = false;
            add_text_decoded(t, tl);
            i = j; continue;
        }
        /* comments, doctype, processing instructions */
        if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            long j = i + 4; while (j + 2 < n && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
            i = j + 2 < n ? j + 3 : n; continue;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            if (i + 9 < n && (s[i + 2] | 32) == 'd' && (s[i + 3] | 32) == 'o' && (s[i + 4] | 32) == 'c') d->standards = true;
            long j = i; while (j < n && s[j] != '>') j++;
            i = j + 1; continue;
        }
        bool close = i + 1 < n && s[i + 1] == '/';
        long p = i + 1 + close;
        if (p >= n || !((lc(s[p]) >= 'a' && lc(s[p]) <= 'z'))) {   /* a lone '<' is text */
            if (close && p < n && s[p] == '>') { i = p + 1; continue; }
            add_text("<", 1); i++; continue;
        }
        long ns = p; while (p < n && is_name_char(s[p])) p++;
        char name[128]; int nl = p - ns < 127 ? p - ns : 127;   /* custom element names can be long */
        for (int k = 0; k < nl; k++) name[k] = lc(s[ns + k]);
        name[nl] = 0;
        /* attributes */
        int na = 0; bool selfclose = false;
        while (p < n && s[p] != '>') {
            if (is_space(s[p])) { p++; continue; }
            if (s[p] == '/') { selfclose = true; p++; continue; }
            selfclose = false;
            long as = p; while (p < n && s[p] != '=' && s[p] != '>' && !is_space(s[p]) && !(s[p] == '/' && p + 1 < n && s[p + 1] == '>')) p++;
            long ae = p == as ? ++p : p;
            while (p < n && is_space(s[p])) p++;
            long vs = 0, ve = 0; bool hasv = false;
            if (p < n && s[p] == '=') {
                p++; while (p < n && is_space(s[p])) p++;
                if (p < n && (s[p] == '"' || s[p] == '\'')) { char q = s[p++]; vs = p; while (p < n && s[p] != q) p++; ve = p; if (p < n) p++; }
                else { vs = p; while (p < n && !is_space(s[p]) && s[p] != '>') p++; ve = p; }
                hasv = true;
            }
            if (na < 64 && !close && ae > as && ae - as < 64) {
                char *an = ar_alloc(&d->ar, ae - as + 1);
                if (!an) break;
                for (long k = 0; k < ae - as; k++) an[k] = lc(s[as + k]);
                bool dup = false; for (int k = 0; k < na; k++) if (!strcmp(at[k].name, an)) dup = true;
                if (!dup) { at[na].name = an; at[na].value = hasv ? decode_attr(s + vs, ve - vs) : ""; if (at[na].value) na++; }
            }
        }
        i = p < n ? p + 1 : n;
        int t = tag_lookup(name, nl);
        if (close) { end_tag(t, name); continue; }
        struct node *e = new_node(N_ELEM);
        if (!e) break;
        e->tag = t; e->name = t ? tag_names[t] : ar_strndup(&d->ar, name, nl);
        if (na) { e->attrs = ar_alloc(&d->ar, na * sizeof(struct attr)); if (e->attrs) { memcpy(e->attrs, at, na * sizeof(struct attr)); e->nattr = na; } }
        e->id = node_attr(e, "id");
        e->style_attr = node_attr(e, "style");
        const char *cl = node_attr(e, "class");
        if (cl) {
            int nc = 0; for (const char *q = cl; *q;) { while (*q == ' ') q++; if (!*q) break; nc++; while (*q && *q != ' ') q++; }
            if (nc > 32) nc = 32;
            e->cls = nc ? ar_alloc(&d->ar, nc * sizeof(char *)) : 0;
            for (const char *q = cl; *q && e->cls && e->ncls < nc;) {
                while (*q == ' ') q++;
                if (!*q) break;
                const char *b = q; while (*q && *q != ' ') q++;
                e->cls[e->ncls++] = ar_strndup(&d->ar, b, q - b);
            }
        }
        insert_element(e);
        if (selfclose && (e->tag == T_UNKNOWN || e->tag == T_SVG) && sp > 1 && top() == e) sp--;     /* <foo/> in XHTML-ish pages */
        /* raw text elements */
        if (t == T_SCRIPT || t == T_STYLE || t == T_TITLE || t == T_TEXTAREA || t == T_XMP || t == T_NOFRAMES || t == T_IFRAME ||
            t == T_PLAINTEXT || t == T_MATH) {
            long ts, e2 = t == T_PLAINTEXT ? (ts = n) : find_end(s, i, n, name, &ts);
            if (selfclose && (t == T_SCRIPT || t == T_SVG || t == T_MATH || t == T_IFRAME)) { if (sp > 1 && top() == e) sp--; continue; }
            if (t == T_STYLE || t == T_SCRIPT) { e->text = ar_strndup(&d->ar, s + i, ts - i); e->tlen = ts - i; }
            else if (t == T_TITLE) {
                long k = 0; char tmp[160];
                for (long q = i; q < ts && k < 159; q++) {
                    char c = s[q];
                    if (c == '&') { char ch; int u = html_entity(s + q, ts - q, &ch); if (u) { if (ch) tmp[k++] = ch; q += u - 1; continue; } }
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (c == ' ' && (!k || tmp[k - 1] == ' ')) continue;
                    tmp[k++] = c;
                }
                while (k && tmp[k - 1] == ' ') k--;
                tmp[k] = 0;
                if (!d->title[0]) strncpy0(d->title, tmp, sizeof d->title);
            } else if (t == T_TEXTAREA || t == T_XMP || t == T_PLAINTEXT) {
                long a0 = i; if (t == T_TEXTAREA && a0 < ts && s[a0] == '\n') a0++;
                if (t == T_TEXTAREA) add_text_decoded(s + a0, ts - a0); else add_text(s + a0, ts - a0);
            }
            /* script, style, iframe, math, noframes: no text */
            if (sp > 1 && top() == e) sp--;
            i = e2; continue;
        }
        if (t == T_PRE || t == T_LISTING) skip_nl = true;
    }
    if (!d->body) { ensure_body(); }
}
