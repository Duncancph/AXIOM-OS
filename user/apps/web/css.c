/* Web: CSS - parsing style sheets, matching selectors and computing each element's style. */
#include "web.h"

/* ---------------- small helpers ---------------- */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool eq_ci(const char *s, int n, const char *w) {      /* s[0..n) equals w, ignoring case */
    int l = strlen(w); if (l != n) return false;
    for (int i = 0; i < n; i++) if (lc(s[i]) != w[i]) return false;
    return true;
}
static bool pre_ci(const char *s, int n, const char *w) {
    int l = strlen(w); if (l > n) return false;
    for (int i = 0; i < l; i++) if (lc(s[i]) != w[i]) return false;
    return true;
}
static bool has_ci(const char *s, int n, const char *w) {
    int l = strlen(w);
    for (int i = 0; i + l <= n; i++) if (pre_ci(s + i, n - i, w)) return true;
    return false;
}

/* ---------------- colours ---------------- */
static const struct { const char *n; uint32_t c; } named[] = {
    {"black", 0x000000}, {"silver", 0xC0C0C0}, {"gray", 0x808080}, {"grey", 0x808080}, {"white", 0xFFFFFF}, {"maroon", 0x800000},
    {"red", 0xFF0000}, {"purple", 0x800080}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"green", 0x008000}, {"lime", 0x00FF00},
    {"olive", 0x808000}, {"yellow", 0xFFFF00}, {"navy", 0x000080}, {"blue", 0x0000FF}, {"teal", 0x008080}, {"aqua", 0x00FFFF},
    {"cyan", 0x00FFFF}, {"orange", 0xFFA500}, {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"blanchedalmond", 0xFFEBCD}, {"blueviolet", 0x8A2BE2},
    {"brown", 0xA52A2A}, {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"darkblue", 0x00008B},
    {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"darkgreen", 0x006400},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"greenyellow", 0xADFF2F},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0},
    {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD},
    {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"lightgreen", 0x90EE90}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
    {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"oldlace", 0xFDF5E6}, {"olivedrab", 0x6B8E23}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"rebeccapurple", 0x663399}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090},
    {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"whitesmoke", 0xF5F5F5},
    {"yellowgreen", 0x9ACD32},
};
static int hexv(char c) { c = lc(c); return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* a number with up to 3 decimals, as value * 1000; *end: where it stopped */
static bool num1000(const char *s, int n, long *out, int *used) {
    int i = 0; bool neg = false;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    long v = 0; int dig = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { if (v < 100000000) v = v * 10 + (s[i] - '0'); i++; dig++; }
    long frac = 0; int fd = 0;
    if (i < n && s[i] == '.') { i++; while (i < n && s[i] >= '0' && s[i] <= '9') { if (fd < 3) { frac = frac * 10 + (s[i] - '0'); fd++; } i++; dig++; } }
    if (!dig) return false;
    while (fd < 3) { frac *= 10; fd++; }
    if (i < n && (s[i] == 'e' || s[i] == 'E') && i + 1 < n && (s[i + 1] == '-' || (s[i + 1] >= '0' && s[i + 1] <= '9'))) {   /* 1e-5: about 0 */
        i++; if (s[i] == '-') { i++; v = 0; frac = 0; } while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    *out = (v * 1000 + frac) * (neg ? -1 : 1); *used = i;
    return true;
}

/* comma/space separated numeric arguments inside (...) ; pct[i] set if given in % */
static int func_args(const char *s, int n, long *v, bool *pct, int max) {
    int k = 0, i = 0;
    while (i < n && k < max) {
        while (i < n && (sp(s[i]) || s[i] == ',' || s[i] == '/')) i++;
        if (i >= n) break;
        int u;
        if (!num1000(s + i, n - i, &v[k], &u)) { /* "none" or an angle unit: skip the token */
            while (i < n && !sp(s[i]) && s[i] != ',' && s[i] != '/') i++;
            continue;
        }
        i += u; pct[k] = i < n && s[i] == '%';
        while (i < n && !sp(s[i]) && s[i] != ',' && s[i] != '/') i++;   /* units like deg */
        k++;
    }
    return k;
}
static int clamp255(long v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }
static int hue2rgb(long p, long q, long t) {      /* all * 1000, t in 0..1000 (of 1 turn) */
    if (t < 0) t += 1000;
    if (t > 1000) t -= 1000;
    long r;
    if (t * 6 < 1000) r = p + (q - p) * 6 * t / 1000;
    else if (t * 2 < 1000) r = q;
    else if (t * 3 < 2000) r = p + (q - p) * (666 - t) * 6 / 1000;
    else r = p;
    return clamp255((r * 255 + 500) / 1000);
}

/* returns false if not a colour. *out = alpha << 24 | rgb (alpha 255: opaque); nearly invisible colours (alpha
 * under 5%) are 0, transparent */
bool css_parse_color(const char *s, int n, uint32_t *out) {
    while (n && sp(*s)) { s++; n--; }
    while (n && sp(s[n - 1])) n--;
    if (!n) return false;
    if (s[0] == '#') {
        int d[8], k = n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return false;
        for (int i = 0; i < k; i++) if ((d[i] = hexv(s[1 + i])) < 0) return false;
        uint32_t r, g, b, a = 255;
        if (k <= 4) { r = d[0] * 17; g = d[1] * 17; b = d[2] * 17; if (k == 4) a = d[3] * 17; }
        else { r = d[0] * 16 + d[1]; g = d[2] * 16 + d[3]; b = d[4] * 16 + d[5]; if (k == 8) a = d[6] * 16 + d[7]; }
        *out = a < 13 ? 0 : a << 24 | r << 16 | g << 8 | b;
        return true;
    }
    if (pre_ci(s, n, "rgb") || pre_ci(s, n, "hsl")) {
        const char *o = s; while (o < s + n && *o != '(') o++;
        const char *c = o; while (c < s + n && *c != ')') c++;
        if (o >= s + n) return false;
        long v[4]; bool pct[4];
        int k = func_args(o + 1, c - o - 1, v, pct, 4);
        if (k < 3) return false;
        long a = k > 3 ? (pct[3] ? v[3] / 100 : v[3]) : 1000;   /* alpha * 1000 */
        if (a < 50) { *out = 0; return true; }
        uint32_t a8 = a >= 1000 ? 255 : (uint32_t)(a * 255 / 1000);
        int r, g, b;
        if (lc(s[0]) == 'r') {
            r = clamp255(pct[0] ? v[0] * 255 / 100000 : v[0] / 1000); g = clamp255(pct[1] ? v[1] * 255 / 100000 : v[1] / 1000);
            b = clamp255(pct[2] ? v[2] * 255 / 100000 : v[2] / 1000);
        } else {
            long h = (v[0] / 360) % 1000; if (h < 0) h += 1000;          /* turns * 1000 */
            long sa = v[1] / 100, l = v[2] / 100;                       /* * 1000 */
            if (sa > 1000) sa = 1000;
            if (l > 1000) l = 1000;
            if (sa < 0) sa = 0;
            if (l < 0) l = 0;
            long q = l < 500 ? l * (1000 + sa) / 1000 : l + sa - l * sa / 1000, p = 2 * l - q;
            r = hue2rgb(p, q, h + 333); g = hue2rgb(p, q, h); b = hue2rgb(p, q, h - 333);
        }
        *out = a8 << 24 | r << 16 | g << 8 | b;
        return true;
    }
    if (eq_ci(s, n, "transparent")) { *out = 0; return true; }
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) if (eq_ci(s, n, named[i].n)) { *out = 0xFF000000 | named[i].c; return true; }
    return false;
}

/* ---------------- style sheets ---------------- */
struct csel {                         /* one compound selector; left: the one before it in the chain */
    const char *tag;                  /* 0: any */
    const char *id;
    const char *cls[6]; uint8_t ncls;
    uint8_t comb;                     /* how it relates to the next one to the right: ' ', '>', '+', '~' */
    uint8_t pseudo;                   /* PS_* bits */
    int8_t nth_a, nth_b;              /* :nth-child(an+b) */
    struct cattr { const char *name, *val; char op; } at[3]; uint8_t nat;
    struct csel *not_;                /* :not(simple) */
    struct csel *left;
};
#define PS_LINK 1
#define PS_FIRST 2
#define PS_LAST 4
#define PS_ROOT 8
#define PS_NTH 16
#define PS_EMPTY 32
#define PS_ONLY 64
#define PS_CHECKED 128

struct decl { const char *prop, *val; int plen, vlen; uint8_t imp; };
struct media { const char *cond; int len; struct media *parent; int gen; bool result; };
struct rule {
    struct csel *sel; int spec; int order; uint8_t origin;
    struct decl *d; int nd;
    struct media *mq;
    struct rule *next;                /* bucket chain */
};
#define NBUCKET 1024
struct ruleset { struct rule *bucket[NBUCKET]; struct rule *universal; int nrules; struct arena *ar; };
static struct arena ua_ar;
static struct ruleset ua_set, au_set;
static int order_counter;
static bool media_dep;
static int cur_origin;
static struct ruleset *cur_set;
static int viewport_w, media_gen;
#define MAX_RULES 60000

static uint32_t hash_s(const char *s, char kind) { uint32_t h = 2166136261u ^ (uint8_t)kind; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
static uint32_t hash_ci(const char *s, int n, char kind) { uint32_t h = 2166136261u ^ (uint8_t)kind; for (int i = 0; i < n; i++) h = (h ^ (uint8_t)lc(s[i])) * 16777619u; return h; }

static char *dup(const char *s, int n) { return ar_strndup(cur_set->ar, s, n); }
static char *dup_lc(const char *s, int n) { char *p = dup(s, n); if (p) for (int i = 0; i < n; i++) p[i] = lc(p[i]); return p; }

static bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\'; }

/* parse one compound selector at s[*i]; false if unsupported */
static bool parse_compound(const char *s, int n, int *ip, struct csel *c, int *spec, bool inner) {
    int i = *ip; bool any = false;
    if (i < n && (s[i] == '*' )) { i++; any = true; }
    else if (i < n && ident_char(s[i])) {
        int b = i; while (i < n && ident_char(s[i])) i++;
        c->tag = dup_lc(s + b, i - b); *spec += 1; any = true;
    }
    while (i < n) {
        char ch = s[i];
        if (ch == '#' || ch == '.') {
            i++; int b = i; while (i < n && ident_char(s[i])) i++;
            if (i == b) return false;
            if (ch == '#') { if (c->id) return false; c->id = dup(s + b, i - b); *spec += 10000; }
            else { if (c->ncls >= 6) return false; c->cls[c->ncls++] = dup(s + b, i - b); *spec += 100; }
            any = true;
        } else if (ch == '[') {
            int e = i; while (e < n && s[e] != ']') e++;
            if (e >= n || c->nat >= 3) return false;
            int b = i + 1; while (b < e && sp(s[b])) b++;
            int ne = b; while (ne < e && ident_char(s[ne])) ne++;
            struct cattr *a = &c->at[c->nat++];
            a->name = dup_lc(s + b, ne - b); a->op = 0; a->val = 0;
            int q = ne; while (q < e && sp(s[q])) q++;
            if (q < e) {
                char op = s[q];
                if (op == '=') { a->op = '='; q++; }
                else if (q + 1 < e && s[q + 1] == '=' && (op == '~' || op == '^' || op == '$' || op == '*' || op == '|')) { a->op = op; q += 2; }
                else return false;
                while (q < e && sp(s[q])) q++;
                int ve = e; while (ve > q && sp(s[ve - 1])) ve--;
                if (ve > q + 1 && ve - 1 > q && (s[ve - 1] == 'i' || s[ve - 1] == 's') && sp(s[ve - 2])) { ve -= 2; while (ve > q && sp(s[ve - 1])) ve--; }
                if (ve - q >= 2 && (s[q] == '"' || s[q] == '\'')) { q++; ve--; }
                a->val = dup(s + q, ve - q);
            }
            *spec += 100; i = e + 1; any = true;
        } else if (ch == ':') {
            i++;
            if (i < n && s[i] == ':') return false;                      /* pseudo-elements */
            int b = i; while (i < n && ident_char(s[i])) i++;
            const char *p = s + b; int pl = i - b;
            const char *arg = 0; int al = 0;
            if (i < n && s[i] == '(') {
                int depth = 1, e = i + 1;
                while (e < n && depth) { if (s[e] == '(') depth++; else if (s[e] == ')') depth--; e++; }
                arg = s + i + 1; al = e - i - 2; i = e;
                if (al < 0) return false;
            }
            if (eq_ci(p, pl, "link") || eq_ci(p, pl, "any-link")) c->pseudo |= PS_LINK;
            else if (eq_ci(p, pl, "visited")) return false;                /* no history of visits: never visited */
            else if (eq_ci(p, pl, "first-child")) c->pseudo |= PS_FIRST;
            else if (eq_ci(p, pl, "last-child")) c->pseudo |= PS_LAST;
            else if (eq_ci(p, pl, "only-child")) c->pseudo |= PS_ONLY;
            else if (eq_ci(p, pl, "root")) c->pseudo |= PS_ROOT;
            else if (eq_ci(p, pl, "empty")) c->pseudo |= PS_EMPTY;
            else if (eq_ci(p, pl, "checked")) c->pseudo |= PS_CHECKED;
            else if (eq_ci(p, pl, "first-of-type") || eq_ci(p, pl, "last-of-type")) return false;
            else if (eq_ci(p, pl, "before") || eq_ci(p, pl, "after") || eq_ci(p, pl, "first-line") || eq_ci(p, pl, "first-letter")) return false;
            else if (eq_ci(p, pl, "nth-child") && arg) {
                while (al && sp(*arg)) { arg++; al--; }
                while (al && sp(arg[al - 1])) al--;
                int A = 0, B = 0;
                if (eq_ci(arg, al, "odd")) { A = 2; B = 1; }
                else if (eq_ci(arg, al, "even")) { A = 2; B = 0; }
                else {
                    int k = 0; bool neg = false; int v = 0; bool hv = false;
                    if (k < al && (arg[k] == '-' || arg[k] == '+')) { neg = arg[k] == '-'; k++; }
                    while (k < al && arg[k] >= '0' && arg[k] <= '9') { v = v * 10 + arg[k] - '0'; k++; hv = true; if (v > 100) return false; }
                    if (k < al && lc(arg[k]) == 'n') {
                        A = hv ? v : 1; if (neg) A = -A; k++;
                        while (k < al && sp(arg[k])) k++;
                        if (k < al && (arg[k] == '+' || arg[k] == '-')) {
                            bool bn = arg[k] == '-'; k++; while (k < al && sp(arg[k])) k++;
                            while (k < al && arg[k] >= '0' && arg[k] <= '9') { B = B * 10 + arg[k] - '0'; k++; if (B > 100) return false; }
                            if (bn) B = -B;
                        }
                    } else { if (!hv) return false; B = neg ? -v : v; }
                    if (k != al) return false;
                }
                c->pseudo |= PS_NTH; c->nth_a = A; c->nth_b = B;
            } else if (eq_ci(p, pl, "not") && arg && !inner) {
                struct csel *nc = ar_alloc(cur_set->ar, sizeof *nc);
                if (!nc) return false;
                int j = 0, dummy = 0;
                while (j < al && sp(arg[j])) j++;
                if (!parse_compound(arg, al, &j, nc, &dummy, true)) return false;
                while (j < al && sp(arg[j])) j++;
                if (j != al || c->not_) return false;
                c->not_ = nc; *spec += dummy;
            } else return false;                                         /* :hover, :focus, ... never apply */
            if (!(eq_ci(p, pl, "not"))) *spec += 100;
            any = true;
        } else break;
    }
    *ip = i;
    return any;
}

/* a whole selector (no commas); returns the rightmost compound or 0 */
static struct csel *parse_selector(const char *s, int n, int *spec) {
    struct csel *right = 0;
    int i = 0; *spec = 0;
    char comb = 0;
    while (i < n && sp(s[i])) i++;
    while (i < n) {
        struct csel *c = ar_alloc(cur_set->ar, sizeof *c);
        if (!c) return 0;
        if (!parse_compound(s, n, &i, c, spec, false)) return 0;
        c->left = right; c->comb = comb; right = c;
        bool ws = false;
        while (i < n && sp(s[i])) { i++; ws = true; }
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') { comb = s[i++]; while (i < n && sp(s[i])) i++; }
        else if (ws) comb = ' ';
        else return 0;
    }
    return right;          /* the rightmost compound; left links go leftwards, comb relates each one to its left */
}

static void add_rule(struct csel *sel, int spec, struct decl *d, int nd, struct media *mq) {
    if (cur_set->nrules >= MAX_RULES) return;
    struct rule *r = ar_alloc(cur_set->ar, sizeof *r);
    if (!r) return;
    r->sel = sel; r->spec = spec; r->order = ++order_counter; r->origin = cur_origin; r->d = d; r->nd = nd; r->mq = mq;
    cur_set->nrules++;
    uint32_t h; struct rule **b;
    if (sel->id) h = hash_s(sel->id, '#');
    else if (sel->ncls) h = hash_s(sel->cls[0], '.');
    else if (sel->tag) h = hash_s(sel->tag, 't');
    else { r->next = cur_set->universal; cur_set->universal = r; return; }
    b = &cur_set->bucket[h % NBUCKET];
    r->next = *b; *b = r;
}

/* declarations "a: b; c: d !important" */
static int parse_decls(const char *s, int n, struct decl *out, int max) {
    int k = 0, i = 0;
    while (i < n && k < max) {
        while (i < n && (sp(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;
        int ps = i; while (i < n && s[i] != ':' && s[i] != ';') i++;
        if (i >= n || s[i] != ':') { while (i < n && s[i] != ';') i++; continue; }
        int pe = i; while (pe > ps && sp(s[pe - 1])) pe--;
        i++;
        int vs = i, depth = 0; char q = 0; bool block = false;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == q) q = 0; else if (c == '\\') i++; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '{') { depth++; if (c == '{') block = true; }
            else if (c == ')' || c == '}') { if (depth) depth--; }
            else if (c == ';' && !depth) break;
            i++;
        }
        int ve = i < n ? i : n;
        while (ve > vs && sp(s[ve - 1])) ve--;
        while (vs < ve && sp(s[vs])) vs++;
        bool imp = false;
        if (ve - vs >= 10) {
            int b = ve - 1; while (b > vs && s[b] != '!') b--;
            if (s[b] == '!' && has_ci(s + b, ve - b, "important")) { imp = true; ve = b; while (ve > vs && sp(s[ve - 1])) ve--; }
        }
        if (pe > ps && ve > vs && !block) { out[k].prop = s + ps; out[k].plen = pe - ps; out[k].val = s + vs; out[k].vlen = ve - vs; out[k].imp = imp; k++; }
    }
    return k;
}

static struct decl *copy_decls(struct decl *d, int nd) {
    struct decl *o = ar_alloc(cur_set->ar, nd * sizeof *o);
    if (!o) return 0;
    for (int i = 0; i < nd; i++) {
        o[i] = d[i];
        o[i].prop = dup_lc(d[i].prop, d[i].plen);
        o[i].val = dup(d[i].val, d[i].vlen);
        if (!o[i].prop || !o[i].val) return 0;
        if (o[i].plen > 2 && d[i].prop[0] == '-' && d[i].prop[1] == '-') o[i].prop = dup(d[i].prop, d[i].plen);   /* custom properties keep their case */
    }
    return o;
}

/* end of a {...} block starting at s[i] == '{' (index after the matching '}') */
static int block_end(const char *s, int n, int i) {
    int depth = 0; char q = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (q) { if (c == '\\') i++; else if (c == q || c == '\n') q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '{') depth++;
        else if (c == '}') { if (--depth <= 0) return i + 1; }
    }
    return n;
}

static struct decl tmpd[256];
static char *strip_comments(const char *s, long n, long *out_len);
static void parse_rules(const char *s, int n, struct media *mq, const char *base, int import_depth, int nest) {
    int i = 0;
    while (i < n) {
        while (i < n && (sp(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;
        if (s[i] == '<' && pre_ci(s + i, n - i, "<!--")) { i += 4; continue; }
        if (s[i] == '-' && pre_ci(s + i, n - i, "-->")) { i += 3; continue; }
        if (s[i] == '}') { i++; continue; }
        if (s[i] == '@') {
            int b = i + 1; while (b < n && ident_char(s[b])) b++;
            const char *kw = s + i + 1; int kl = b - i - 1;
            int e = b; char q = 0;
            while (e < n && (q || (s[e] != '{' && s[e] != ';'))) { if (q) { if (s[e] == q) q = 0; } else if (s[e] == '"' || s[e] == '\'') q = s[e]; e++; }
            if (e >= n) return;
            if (s[e] == ';') {                                              /* @import, @charset, @namespace */
                if (eq_ci(kw, kl, "import") && import_depth < 3) {
                    const char *a = s + b, *ae = s + e;
                    while (a < ae && sp(*a)) a++;
                    if (ae - a > 4 && pre_ci(a, ae - a, "url(")) a += 4;
                    while (a < ae && sp(*a)) a++;
                    char qq = *a == '"' || *a == '\'' ? *a++ : 0;
                    const char *ue = a; while (ue < ae && (qq ? *ue != qq : (*ue != ')' && !sp(*ue)))) ue++;
                    const char *rest = ue; while (rest < ae && (*rest == ')' || *rest == '"' || *rest == '\'' || sp(*rest))) rest++;
                    bool ok = true;
                    if (rest < ae) { if (has_ci(rest, ae - rest, "print") && !has_ci(rest, ae - rest, "screen") && !has_ci(rest, ae - rest, "all")) ok = false; }
                    if (ok && ue > a && ue - a < 500) {
                        char rel[512], full[600]; memcpy(rel, a, ue - a); rel[ue - a] = 0;
                        resolve_url(base, rel, full, sizeof full);
                        long len = 0; char *t = web_fetch_text(full, &len, 512 << 10);
                        if (t) {
                            struct ruleset *keep = cur_set;
                            long cl;
                            char *copy = strip_comments(t, len, &cl);
                            free(t);
                            if (copy) { parse_rules(copy, cl, mq, full, import_depth + 1, nest); }
                            cur_set = keep;
                        }
                    }
                }
                i = e + 1; continue;
            }
            int be = block_end(s, n, e);
            if (eq_ci(kw, kl, "media") && nest < 4) {
                struct media *m = ar_alloc(cur_set->ar, sizeof *m);
                if (m) {
                    m->cond = dup(s + b, e - b); m->len = e - b; m->parent = mq; m->gen = -1;
                    if (has_ci(s + b, e - b, "width")) media_dep = true;
                    parse_rules(s + e + 1, be - e - 2 > 0 ? be - e - 2 : 0, m, base, import_depth, nest + 1);
                }
            } else if (eq_ci(kw, kl, "supports") && nest < 4) {
                /* assume support unless it asks for something we certainly don't do */
                if (!has_ci(s + b, e - b, "not ") && !has_ci(s + b, e - b, "grid") && !has_ci(s + b, e - b, "selector"))
                    parse_rules(s + e + 1, be - e - 2 > 0 ? be - e - 2 : 0, mq, base, import_depth, nest + 1);
            } else if ((eq_ci(kw, kl, "layer") || eq_ci(kw, kl, "document") || eq_ci(kw, kl, "-moz-document")) && nest < 4) {
                parse_rules(s + e + 1, be - e - 2 > 0 ? be - e - 2 : 0, mq, base, import_depth, nest + 1);
            }
            /* @font-face, @keyframes, @page, ...: skipped */
            i = be; continue;
        }
        /* a qualified rule: selectors { declarations } */
        int b = i; char q = 0;
        while (i < n && (q || s[i] != '{')) { if (q) { if (s[i] == q) q = 0; } else if (s[i] == '"' || s[i] == '\'') q = s[i]; else if (s[i] == '}') break; i++; }
        if (i >= n) return;
        if (s[i] == '}') { i++; continue; }
        int be = block_end(s, n, i);
        int nd = parse_decls(s + i + 1, be - i - 2 > 0 ? be - i - 2 : 0, tmpd, 256);
        if (nd) {
            struct decl *d = copy_decls(tmpd, nd);
            if (!d) return;
            /* the selector list */
            int k = b, depth = 0;
            for (int j = b; j <= i; j++) {
                if (j < i && s[j] == '(') depth++;
                if (j < i && s[j] == ')' && depth) depth--;
                if (j == i || (s[j] == ',' && !depth)) {
                    int spec;
                    struct csel *sel = j > k ? parse_selector(s + k, j - k, &spec) : 0;
                    if (sel) add_rule(sel, spec, d, nd, mq);
                    k = j + 1;
                }
            }
        }
        i = be;
    }
}

/* a copy without comments */
static char *strip_comments(const char *s, long n, long *out_len) {
    char *o = ar_alloc(cur_set->ar, n + 1);
    if (!o) return 0;
    long k = 0; char q = 0;
    for (long i = 0; i < n; i++) {
        char c = s[i];
        if (q) { if (c == q || c == '\n') q = 0; else if (c == '\\' && i + 1 < n) { o[k++] = c; c = s[++i]; } o[k++] = c; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i++; o[k++] = ' '; continue;
        }
        if (c == '"' || c == '\'') q = c;
        o[k++] = c;
    }
    o[k] = 0; *out_len = k;
    return o;
}

void css_add_sheet(const char *text, long len, const char *base_url, int import_depth) {
    if (len > (8l << 20)) len = 8l << 20;
    long n;
    char *s = strip_comments(text, len, &n);
    if (s) parse_rules(s, (int)n, 0, base_url ? base_url : "", import_depth, 0);
}

static const char UA_CSS[] =
    "html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, dl, dt, dd, pre, blockquote, form, fieldset, address, section, article, aside,"
    " nav, header, footer, main, figure, figcaption, hr, center, details, summary, hgroup, menu, dir, legend, listing, xmp, plaintext,"
    " search, dialog, optgroup { display: block }"
    " noscript { display: none }"
    " svg { display: inline-block }"
    " iframe { display: inline-block; border: 2px solid #c8c8c8 }"
    "head, title, meta, link, style, script, base, template, area, map, param, source, track, datalist, noframes, math,"
    " audio, object, embed, colgroup, col, option, frame, frameset, input[type=hidden], [hidden], dialog { display: none }"
    " video { display: inline } audio[controls] { display: inline }"
    "dialog[open] { display: block }"
    "li { display: list-item }"
    "table { display: table; border-spacing: 2px }"
    "caption { display: table-caption; text-align: center }"
    "thead, tbody, tfoot { display: table-row-group }"
    "tr { display: table-row }"
    "td, th { display: table-cell; padding: 1px }"
    "th { font-weight: bold; text-align: center }"
    "body { margin: 8px }"
    "p, dl, ul, ol, menu, dir, pre, listing, xmp, plaintext { margin: 1em 0 }"
    "blockquote, figure { margin: 1em 40px }"
    "h1 { font-size: 2em; margin: .67em 0; font-weight: bold }"
    "h2 { font-size: 1.5em; margin: .83em 0; font-weight: bold }"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold }"
    "h4 { margin: 1.33em 0; font-weight: bold }"
    "h5 { font-size: .83em; margin: 1.67em 0; font-weight: bold }"
    "h6 { font-size: .67em; margin: 2.33em 0; font-weight: bold }"
    "ul, ol, menu, dir { padding-left: 40px }"
    "ol { list-style-type: decimal }"
    "ul ul, ol ul, menu ul, ul menu { list-style-type: circle }"
    "ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square }"
    "ul ul, ul ol, ol ol, ol ul { margin-top: 0; margin-bottom: 0 }"
    "dd { margin-left: 40px }"
    "b, strong, dt { font-weight: bold }"
    "i, em, cite, var, dfn, address { font-style: italic }"
    "u, ins { text-decoration: underline }"
    "s, strike, del { text-decoration: line-through }"
    "pre, code, tt, kbd, samp, listing, xmp, plaintext { font-family: monospace }"
    "pre, listing, xmp, plaintext { white-space: pre }"
    "small { font-size: smaller } big { font-size: larger }"
    "sub { font-size: smaller; vertical-align: sub } sup { font-size: smaller; vertical-align: super }"
    "a:link { color: #0B57C9; text-decoration: underline }"
    "center { text-align: center }"
    "hr { border-top: 1px solid #A0A0A0; margin: .5em 0 }"
    "mark { background-color: yellow; color: black }"
    "nobr { white-space: nowrap }"
    "fieldset { border: 1px solid #A0A0A0; padding: .35em .75em .625em; margin: 0 2px }"
    "legend { padding: 0 2px; font-weight: bold }"
    "summary { font-weight: bold }"
    "input, textarea, select, button { display: inline-block }"
    "textarea { white-space: pre }"
    "img { display: inline }"
    "abbr[title] { text-decoration: underline }"
    "q { font-style: italic }";

void css_init(void) {
    ua_set.ar = &ua_ar;
    cur_set = &ua_set; cur_origin = 0;
    css_add_sheet(UA_CSS, sizeof UA_CSS - 1, "", 0);
    cur_set = &au_set; cur_origin = 2;
}

void css_reset(struct arena *page) {
    memset(&au_set, 0, sizeof au_set);
    au_set.ar = page;
    cur_set = &au_set; cur_origin = 2;
    media_dep = false;
}
bool css_media_dependent(void) { return media_dep; }

/* keep the author style sheets of the page on screen while another page's are read (and put them back) */
struct css_saved { struct ruleset set; bool media_dep; };
void *css_save(void) {
    struct css_saved *s = malloc(sizeof *s);
    if (s) { s->set = au_set; s->media_dep = media_dep; }
    return s;
}
void css_restore(void *p) {
    struct css_saved *s = p;
    if (!s) return;
    au_set = s->set; media_dep = s->media_dep;
    free(s);
}

/* ---------------- media queries ---------------- */
static int media_len_px(const char *s, int n) {
    long v; int u;
    if (!num1000(s, n, &v, &u)) return -1;
    const char *un = s + u; int ul = n - u;
    if (pre_ci(un, ul, "em") || pre_ci(un, ul, "rem")) return (int)(v * 16 / 1000);
    return (int)(v / 1000);
}
/* one query of a comma list: "screen and (max-width: 600px)" */
static bool media_one(const char *s, int n) {
    while (n && sp(*s)) { s++; n--; }
    while (n && sp(s[n - 1])) n--;
    bool neg = false;
    if (pre_ci(s, n, "not ")) { neg = true; s += 4; n -= 4; }
    else if (pre_ci(s, n, "only ")) { s += 5; n -= 5; }
    bool ok = true;
    for (int i = 0; i < n;) {
        while (i < n && sp(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '(') {
            int e = i; while (e < n && s[e] != ')') e++;
            const char *f = s + i + 1; int fl = e - i - 1;
            const char *c = f; while (c < f + fl && *c != ':') c++;
            int name_l = c - f; while (name_l && sp(f[name_l - 1])) name_l--;
            const char *v = c + 1; int vl = f + fl - v; while (vl > 0 && sp(*v)) { v++; vl--; }
            if (c >= f + fl) {                                          /* a feature with no value */
                if (eq_ci(f, name_l, "color") || eq_ci(f, name_l, "hover") || eq_ci(f, name_l, "pointer")) ; else ok = false;
            } else if (eq_ci(f, name_l, "max-width")) { int px = media_len_px(v, vl); if (px < 0 || viewport_w > px) ok = false; }
            else if (eq_ci(f, name_l, "min-width")) { int px = media_len_px(v, vl); if (px < 0 || viewport_w < px) ok = false; }
            else if (eq_ci(f, name_l, "max-device-width")) { int px = media_len_px(v, vl); if (px < 0 || 1280 > px) ok = false; }
            else if (eq_ci(f, name_l, "min-device-width")) { int px = media_len_px(v, vl); if (px < 0 || 1280 < px) ok = false; }
            else if (eq_ci(f, name_l, "prefers-color-scheme")) { if (!pre_ci(v, vl, "light")) ok = false; }
            else if (eq_ci(f, name_l, "prefers-reduced-motion")) { if (!pre_ci(v, vl, "reduce")) ok = false; }
            else if (eq_ci(f, name_l, "orientation")) { if (!pre_ci(v, vl, "landscape")) ok = false; }
            else if (eq_ci(f, name_l, "hover") || eq_ci(f, name_l, "any-hover")) { if (!pre_ci(v, vl, "hover")) ok = false; }
            else if (eq_ci(f, name_l, "pointer") || eq_ci(f, name_l, "any-pointer")) { if (!pre_ci(v, vl, "fine")) ok = false; }
            else if (eq_ci(f, name_l, "min-resolution") || eq_ci(f, name_l, "-webkit-min-device-pixel-ratio")) { if (media_len_px(v, vl) > 1) ok = false; }
            else ok = false;
            i = e + 1; continue;
        }
        int b = i; while (i < n && !sp(s[i]) && s[i] != '(') i++;
        const char *w = s + b; int wl = i - b;
        if (eq_ci(w, wl, "and")) continue;
        if (eq_ci(w, wl, "print") || eq_ci(w, wl, "speech") || eq_ci(w, wl, "tv") || eq_ci(w, wl, "handheld") || eq_ci(w, wl, "aural") || eq_ci(w, wl, "braille")) ok = false;
        else if (eq_ci(w, wl, "screen") || eq_ci(w, wl, "all")) ;
        else ok = false;
    }
    return neg ? !ok : ok;
}
static bool media_eval(const char *s, int n) {
    while (n && sp(*s)) { s++; n--; }
    if (!n) return true;
    int b = 0;
    for (int i = 0; i <= n; i++) if (i == n || s[i] == ',') { if (media_one(s + b, i - b)) return true; b = i + 1; }
    return false;
}
bool css_media_matches(const char *s, int n, int width) { viewport_w = width; return media_eval(s, n); }
static bool media_ok(struct media *m) {
    for (; m; m = m->parent) {
        if (m->gen != media_gen) { m->gen = media_gen; m->result = media_eval(m->cond, m->len); }
        if (!m->result) return false;
    }
    return true;
}

/* ---------------- matching ---------------- */
static long match_budget;
static bool attr_match(const struct node *e, const struct cattr *a) {
    const char *v = node_attr(e, a->name);
    if (!v) return false;
    if (!a->op) return true;
    int vl = strlen(v), al = strlen(a->val);
    switch (a->op) {
    case '=': return !strcmp(v, a->val);
    case '^': return al && vl >= al && !memcmp(v, a->val, al);
    case '$': return al && vl >= al && !memcmp(v + vl - al, a->val, al);
    case '*': for (int i = 0; al && i + al <= vl; i++) if (!memcmp(v + i, a->val, al)) return true; return false;
    case '|': return !strcmp(v, a->val) || (vl > al && !memcmp(v, a->val, al) && v[al] == '-');
    case '~': for (int i = 0; i + al <= vl; i++) if ((i == 0 || v[i - 1] == ' ') && !memcmp(v + i, a->val, al) && (v[i + al] == 0 || v[i + al] == ' ')) return true; return false;
    }
    return false;
}
static struct node *prev_elem(const struct node *e) { for (struct node *p = e->prev; p; p = p->prev) if (p->type == N_ELEM) return p; return 0; }
static struct node *next_elem(const struct node *e) { for (struct node *p = e->next; p; p = p->next) if (p->type == N_ELEM) return p; return 0; }

static bool compound_match(const struct csel *c, const struct node *e) {
    if (c->tag && strcmp(c->tag, e->name)) return false;
    if (c->id && (!e->id || strcmp(c->id, e->id))) return false;
    for (int i = 0; i < c->ncls; i++) {
        bool f = false;
        for (int j = 0; j < e->ncls; j++) if (!strcmp(e->cls[j], c->cls[i])) { f = true; break; }
        if (!f) return false;
    }
    for (int i = 0; i < c->nat; i++) if (!attr_match(e, &c->at[i])) return false;
    if (c->pseudo) {
        if ((c->pseudo & PS_LINK) && !(e->tag == T_A && node_attr(e, "href"))) return false;
        if ((c->pseudo & (PS_FIRST | PS_ONLY)) && prev_elem(e)) return false;
        if ((c->pseudo & (PS_LAST | PS_ONLY)) && next_elem(e)) return false;
        if ((c->pseudo & PS_ROOT) && e->tag != T_HTML) return false;
        if ((c->pseudo & PS_EMPTY) && e->first) return false;
        if ((c->pseudo & PS_CHECKED) && !(e->fc && e->fc->checked)) return false;
        if (c->pseudo & PS_NTH) {
            int idx = 1; for (struct node *p = prev_elem(e); p && idx < 100000; p = prev_elem(p)) idx++;
            int k = idx - c->nth_b;
            if (c->nth_a == 0) { if (k) return false; }
            else if (k % c->nth_a || k / c->nth_a < 0) return false;
        }
    }
    if (c->not_ && compound_match(c->not_, e)) return false;
    return true;
}
static bool sel_match(const struct csel *c, const struct node *e) {
    if (--match_budget < 0) return false;
    if (!compound_match(c, e)) return false;
    if (!c->left) return true;
    switch (c->comb) {
    case '>': return e->parent && e->parent->type == N_ELEM && sel_match(c->left, e->parent);
    case '+': { struct node *p = prev_elem(e); return p && sel_match(c->left, p); }
    case '~': { int k = 0; for (struct node *p = prev_elem(e); p && k < 200; p = prev_elem(p), k++) if (sel_match(c->left, p)) return true; return false; }
    default:
        for (struct node *p = e->parent; p && p->type == N_ELEM; p = p->parent) {
            if (match_budget < 0) return false;
            if (sel_match(c->left, p)) return true;
        }
        return false;
    }
}

/* ---------------- computing styles ---------------- */
#define MAXM 600
static const struct rule *matched[MAXM]; static int nm;
static struct doc *DOC;
static int root_fs = 16;
static struct arena *sar;               /* where styles go (the page arena) */

static void collect_bucket(const struct rule *r, const struct node *e, uint32_t h, char kind, const char *key) {
    for (; r && nm < MAXM; r = r->next) {
        const struct csel *s = r->sel;
        const char *k = kind == '#' ? s->id : kind == '.' ? (s->id ? 0 : s->ncls ? s->cls[0] : 0) : (s->id || s->ncls ? 0 : s->tag);
        if (!k || strcmp(k, key)) continue;
        if (r->mq && !media_ok(r->mq)) continue;
        if (sel_match(s, e)) matched[nm++] = r;
    }
    (void)h;
}
static void collect(const struct ruleset *rs, const struct node *e) {
    if (e->id) { uint32_t h = hash_s(e->id, '#'); collect_bucket(rs->bucket[h % NBUCKET], e, h, '#', e->id); }
    for (int i = 0; i < e->ncls; i++) {
        bool seen = false; for (int j = 0; j < i; j++) if (!strcmp(e->cls[j], e->cls[i])) seen = true;
        if (seen) continue;
        uint32_t h = hash_s(e->cls[i], '.'); collect_bucket(rs->bucket[h % NBUCKET], e, h, '.', e->cls[i]);
    }
    { uint32_t h = hash_s(e->name, 't'); collect_bucket(rs->bucket[h % NBUCKET], e, h, 't', e->name); }
    for (const struct rule *r = rs->universal; r && nm < MAXM; r = r->next) {
        if (r->mq && !media_ok(r->mq)) continue;
        if (sel_match(r->sel, e)) matched[nm++] = r;
    }
}
static bool rule_before(const struct rule *a, const struct rule *b) {
    if (a->origin != b->origin) return a->origin < b->origin;
    if (a->spec != b->spec) return a->spec < b->spec;
    return a->order < b->order;
}

/* lengths */
struct lenctx { int fs; int pfs; };
static bool parse_len(const char *s, int n, int fs, struct len *out, bool allow_pct) {
    while (n && sp(*s)) { s++; n--; }
    while (n && sp(s[n - 1])) n--;
    if (eq_ci(s, n, "auto")) { out->t = L_AUTO; out->v = 0; return true; }
    if (eq_ci(s, n, "none")) { out->t = L_NONE; out->v = 0; return true; }
    long v; int u;
    if (!num1000(s, n, &v, &u)) return false;
    const char *un = s + u; int ul = n - u;
    long px;
    if (ul == 0) { if (v != 0) return false; px = 0; }
    else if (eq_ci(un, ul, "px")) px = v / 1000;
    else if (eq_ci(un, ul, "em")) px = v * fs / 1000;
    else if (eq_ci(un, ul, "rem")) px = v * root_fs / 1000;
    else if (eq_ci(un, ul, "ex") || eq_ci(un, ul, "ch")) px = v * fs / 2000;
    else if (eq_ci(un, ul, "pt")) px = v * 4 / 3000;
    else if (eq_ci(un, ul, "pc")) px = v * 16 / 1000;
    else if (eq_ci(un, ul, "in")) px = v * 96 / 1000;
    else if (eq_ci(un, ul, "cm")) px = v * 378 / 10000;
    else if (eq_ci(un, ul, "mm")) px = v * 378 / 100000;
    else if (eq_ci(un, ul, "vw")) px = v * viewport_w / 100000;
    else if (eq_ci(un, ul, "vh")) px = v * 600 / 100000;
    else if (un[0] == '%' && ul == 1) { if (!allow_pct) return false; out->t = L_PCT; out->v = (int32_t)(v / 10); return true; }
    else return false;
    if (px > 100000) px = 100000;
    if (px < -100000) px = -100000;
    out->t = L_PX; out->v = (int32_t)px;
    return true;
}

/* split a value into space-separated tokens (keeping (...) together) */
static int tokens(const char *s, int n, const char **t, int *tl, int max) {
    int k = 0, i = 0;
    while (i < n && k < max) {
        while (i < n && (sp(s[i]) || s[i] == ',')) i++;
        if (i >= n) break;
        int b = i, depth = 0;
        while (i < n && (depth || (!sp(s[i]) && s[i] != ','))) { if (s[i] == '(') depth++; else if (s[i] == ')' && depth) depth--; i++; }
        t[k] = s + b; tl[k] = i - b; k++;
    }
    return k;
}

static int border_width_kw(const char *s, int n, int fs, int *out) {
    if (eq_ci(s, n, "thin")) { *out = 1; return 1; }
    if (eq_ci(s, n, "medium")) { *out = 3; return 1; }
    if (eq_ci(s, n, "thick")) { *out = 5; return 1; }
    struct len l; if (parse_len(s, n, fs, &l, false) && l.t == L_PX) { *out = l.v < 0 ? 0 : l.v > 40 ? 40 : l.v; if (l.v > 0 && !*out) *out = 1; return 1; }
    return 0;
}
static int border_style_kw(const char *s, int n) {
    if (eq_ci(s, n, "none") || eq_ci(s, n, "hidden")) return 0;
    if (eq_ci(s, n, "solid")) return 1;
    if (eq_ci(s, n, "dashed") || eq_ci(s, n, "dotted") || eq_ci(s, n, "double") || eq_ci(s, n, "groove") || eq_ci(s, n, "ridge") ||
        eq_ci(s, n, "inset") || eq_ci(s, n, "outset")) return 2;
    return -1;
}
#define CURRENT 0x01000000u                 /* "currentColor", resolved when the element's colour is known */
static bool color_val(const char *s, int n, uint32_t *out) {
    while (n && sp(*s)) { s++; n--; }
    while (n && sp(s[n - 1])) n--;
    if (eq_ci(s, n, "currentcolor")) { *out = CURRENT; return true; }
    return css_parse_color(s, n, out);
}

/* a plain number * 100 ("1", "0.5", ".25") */
static int num100(const char *s, int n) {
    long v = 0, frac = 0, div = 1; int i = 0;
    while (i < n && s[i] == ' ') i++;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    if (i < n && s[i] == '.') for (i++; i < n && s[i] >= '0' && s[i] <= '9' && div < 1000; i++) { frac = frac * 10 + (s[i] - '0'); div *= 10; }
    v = v * 100 + frac * 100 / div;
    return v > 30000 ? 30000 : (int)v;
}
/* grid-template-columns: "200px 1fr", "repeat(3, 1fr)", "repeat(auto-fill, minmax(240px, 1fr))", "minmax(0, 2fr) auto" */
static bool parse_len(const char *s, int n, int fs, struct len *out, bool allow_pct);
static void grid_track(struct style *st, const char *s, int n) {
    while (n && s[0] == ' ') { s++; n--; }
    while (n && s[n - 1] == ' ') n--;
    if (!n || st->ngrid >= 12) return;
    if (pre_ci(s, n, "minmax(")) {                       /* the larger end decides: fr, else the minimum */
        const char *c = s + 7, *e = s + n - 1, *comma = c; int d = 0;
        while (comma < e && (d || *comma != ',')) { if (*comma == '(') d++; else if (*comma == ')') d--; comma++; }
        const char *b = comma + 1; int bl = (int)(e - b);
        while (bl && *b == ' ') { b++; bl--; }
        bool fr = bl > 2 && b[bl - 2] == 'f' && b[bl - 1] == 'r';
        if (fr || eq_ci(b, bl, "auto") || has_ci(b, bl, "content")) { grid_track(st, b, bl); return; }
        grid_track(st, b, bl); return;
    }
    if (pre_ci(s, n, "fit-content(")) { st->grid[st->ngrid].t = GT_AUTO; st->grid[st->ngrid++].v = 0; return; }
    if (n > 2 && s[n - 2] == 'f' && s[n - 1] == 'r') { st->grid[st->ngrid].t = GT_FR; st->grid[st->ngrid++].v = num100(s, n - 2); return; }
    struct len l;
    if (parse_len(s, n, st->fsize, &l, true) && (l.t == L_PX || l.t == L_PCT)) { st->grid[st->ngrid].t = l.t == L_PX ? GT_PX : GT_PCT; st->grid[st->ngrid++].v = l.v; return; }
    st->grid[st->ngrid].t = GT_AUTO; st->grid[st->ngrid++].v = 0;      /* auto, min-content, max-content */
}
static void grid_list(struct style *st, const char *v, int vl, int depth) {
    int i = 0;
    while (i < vl) {
        while (i < vl && v[i] == ' ') i++;
        int b = i, d = 0;
        while (i < vl && (d || v[i] != ' ')) { if (v[i] == '(') d++; else if (v[i] == ')') d--; i++; }
        if (i == b) break;
        if (v[b] == '[') continue;                         /* line names */
        if (pre_ci(v + b, i - b, "repeat(") && depth < 2) {
            const char *c = v + b + 7, *e = v + i - 1, *comma = c;
            while (comma < e && *comma != ',') comma++;
            const char *body = comma + 1; int bl = (int)(e - body);
            if (has_ci(c, (int)(comma - c), "auto-fi")) {  /* auto-fill / auto-fit: as many as fit */
                const char *m = body; int ml = bl; while (ml && *m == ' ') { m++; ml--; }
                struct len l; int minw = 0;
                if (pre_ci(m, ml, "minmax(")) { const char *q = m + 7, *qe = q; while (qe < m + ml && *qe != ',') qe++; if (parse_len(q, (int)(qe - q), st->fsize, &l, false) && l.t == L_PX) minw = l.v; }
                else if (parse_len(m, ml, st->fsize, &l, false) && l.t == L_PX) minw = l.v;
                st->grid_auto_min = minw > 0 ? minw : 200; st->ngrid = 0;
                return;
            }
            int cnt = 0; for (const char *q = c; q < comma && *q >= '0' && *q <= '9'; q++) cnt = cnt * 10 + (*q - '0');
            for (int k = 0; k < cnt && st->ngrid < 12; k++) grid_list(st, body, bl, depth + 1);
            continue;
        }
        grid_track(st, v + b, i - b);
    }
}
static void grid_tracks(struct style *st, const char *v, int vl) {
    st->ngrid = 0; st->grid_auto_min = 0;
    if (eq_ci(v, vl, "none") || eq_ci(v, vl, "subgrid") || eq_ci(v, vl, "initial")) return;
    grid_list(st, v, vl, 0);
}

/* the 4 values of margin/padding/border-width shorthands */
static int four(const char *s, int n, const char **t, int *tl) {
    int k = tokens(s, n, t, tl, 4);
    if (k == 1) { t[1] = t[2] = t[3] = t[0]; tl[1] = tl[2] = tl[3] = tl[0]; }
    else if (k == 2) { t[2] = t[0]; tl[2] = tl[0]; t[3] = t[1]; tl[3] = tl[1]; }
    else if (k == 3) { t[3] = t[1]; tl[3] = tl[1]; }
    return k;
}

static void apply_border_side(struct style *st, int side, const char *v, int vl) {
    const char *t[6]; int tl[6]; int k = tokens(v, vl, t, tl, 6);
    int w = 3, sty = 0; uint32_t c = CURRENT; bool hw = false;
    for (int i = 0; i < k; i++) {
        int bs = border_style_kw(t[i], tl[i]);
        if (bs >= 0) { sty = bs; continue; }
        int bw; if (border_width_kw(t[i], tl[i], st->fsize, &bw)) { w = bw; hw = true; continue; }
        uint32_t cc; if (color_val(t[i], tl[i], &cc)) c = cc;
    }
    (void)hw;
    st->bs[side] = sty; st->bw[side] = w; st->bc[side] = c;
}

static int fs_keyword(const char *s, int n, int pfs) {
    static const struct { const char *k; int px; } kw[] = { {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
        {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48} };
    for (unsigned i = 0; i < sizeof kw / sizeof kw[0]; i++) if (eq_ci(s, n, kw[i].k)) return kw[i].px;
    if (eq_ci(s, n, "smaller")) return pfs * 5 / 6;
    if (eq_ci(s, n, "larger")) return pfs * 6 / 5;
    return 0;
}
static int font_size_val(const char *s, int n, int pfs) {
    while (n && sp(*s)) { s++; n--; }
    while (n && sp(s[n - 1])) n--;
    int k = fs_keyword(s, n, pfs);
    if (k) return k;
    struct len l;
    if (!parse_len(s, n, pfs, &l, true)) return 0;
    if (l.t == L_PCT) return (int)((long)pfs * l.v / 10000);
    if (l.t == L_PX) return l.v;
    return 0;
}
static bool is_mono_family(const char *s, int n) {
    return has_ci(s, n, "mono") || has_ci(s, n, "courier") || has_ci(s, n, "consolas") || has_ci(s, n, "menlo") || has_ci(s, n, "monaco") ||
           has_ci(s, n, "fixed") || has_ci(s, n, "terminal") || has_ci(s, n, "lucida console") || has_ci(s, n, "source code");
}
static int weight_val(const char *s, int n, int parent) {
    if (eq_ci(s, n, "bold") || eq_ci(s, n, "bolder")) return 1;
    if (eq_ci(s, n, "normal") || eq_ci(s, n, "lighter")) return 0;
    long v; int u;
    if (num1000(s, n, &v, &u) && u == n) return v >= 600000;
    return parent;
}
static void line_height_val(struct style *st, const char *s, int n) {
    if (eq_ci(s, n, "normal")) { st->lh = 0; st->lh_num = 0; return; }
    long v; int u;
    if (num1000(s, n, &v, &u) && u == n) { st->lh_num = (int)(v / 10); if (st->lh_num > 400) st->lh_num = 400; st->lh = st->fsize * st->lh_num / 100; return; }
    struct len l;
    if (parse_len(s, n, st->fsize, &l, true)) {
        st->lh_num = 0;
        st->lh = l.t == L_PCT ? st->fsize * l.v / 10000 : l.t == L_PX ? l.v : 0;
        if (st->lh > st->fsize * 4) st->lh = st->fsize * 4;
    }
}

/* var(--x, fallback) substitution, into an arena string */
static const char *lookup_var(struct style *st, const char *name, int nl) {
    for (struct cvar *v = st->vars; v; v = v->next) if ((int)strlen(v->name) == nl && !memcmp(v->name, name, nl)) return v->value;
    return 0;
}
static const char *subst_vars(struct style *st, const char *v, int *vl, int depth) {
    if (depth > 6 || *vl > 4000) return 0;              /* depth 0..6 use bufs[0..6] */
    bool any = false;
    for (int i = 0; i + 4 <= *vl; i++) if (pre_ci(v + i, *vl - i, "var(")) { any = true; break; }
    if (!any) return v;
    static char bufs[8][4096];                        /* one per nesting level; the caller copies what it keeps */
    char *out = bufs[depth];
    int o = 0;
    for (int i = 0; i < *vl;) {
        if (i + 4 <= *vl && pre_ci(v + i, *vl - i, "var(")) {
            int depth2 = 1, e = i + 4;
            while (e < *vl && depth2) { if (v[e] == '(') depth2++; else if (v[e] == ')') depth2--; e++; }
            const char *a = v + i + 4; int al = e - i - 5; if (al < 0) return 0;
            while (al && sp(*a)) { a++; al--; }
            int nl = 0; while (nl < al && a[nl] != ',' && !sp(a[nl]) && a[nl] != ')') nl++;
            const char *val = lookup_var(st, a, nl);
            int len;
            if (val) len = strlen(val);
            else {
                const char *c = a + nl; while (c < a + al && *c != ',') c++;
                if (c >= a + al) return 0;                              /* undefined, no fallback: the declaration is invalid */
                val = c + 1; len = a + al - val;
            }
            const char *sv = subst_vars(st, val, &len, depth + 1);
            if (!sv) return 0;
            if (o + len >= 4095) return 0;
            memcpy(out + o, sv, len); o += len;
            i = e; continue;
        }
        if (o >= 4095) return 0;
        out[o++] = v[i++];
    }
    out[o] = 0; *vl = o;
    return out;
}

static void apply(struct style *st, const struct style *ps, const char *prop, const char *v, int vl) {
    int pl = strlen(prop);
    if (!vl) return;
    v = subst_vars(st, v, &vl, 0);
    if (!v) return;
    while (vl && sp(*v)) { v++; vl--; }
    while (vl && sp(v[vl - 1])) vl--;
    if (!vl) return;
    bool inherit = eq_ci(v, vl, "inherit");
    bool initial = eq_ci(v, vl, "initial") || eq_ci(v, vl, "unset") || eq_ci(v, vl, "revert");
    const char *t[8]; int tl[8];
#define P(x) (pl == (int)sizeof(x) - 1 && !memcmp(prop, x, pl))
    if (P("display")) {
        if (inherit) { st->display = ps->display; return; }
        static const struct { const char *k; int d; } dk[] = { {"none", D_NONE}, {"block", D_BLOCK}, {"inline", D_INLINE},
            {"inline-block", D_INLINE_BLOCK}, {"list-item", D_LIST_ITEM}, {"table", D_TABLE}, {"inline-table", D_INLINE_TABLE},
            {"table-row-group", D_ROW_GROUP}, {"table-header-group", D_ROW_GROUP}, {"table-footer-group", D_ROW_GROUP},
            {"table-row", D_ROW}, {"table-cell", D_CELL}, {"table-caption", D_CAPTION}, {"table-column", D_NONE},
            {"table-column-group", D_NONE}, {"flex", D_FLEX}, {"inline-flex", D_INLINE_FLEX}, {"grid", D_GRID},
            {"inline-grid", D_INLINE_GRID}, {"flow-root", D_BLOCK}, {"contents", D_INLINE}, {"run-in", D_BLOCK},
            {"-webkit-box", D_BLOCK}, {"-ms-flexbox", D_FLEX}, {"-webkit-flex", D_FLEX}, {"block flex", D_FLEX}, {"inline flex", D_INLINE_FLEX},
            {"block flow", D_BLOCK}, {"inline flow-root", D_INLINE_BLOCK} };
        if (eq_ci(v, vl, "flow-root")) st->bfc = 1;
        for (unsigned i = 0; i < sizeof dk / sizeof dk[0]; i++) if (eq_ci(v, vl, dk[i].k)) { st->display = dk[i].d; return; }
        return;
    }
    if (P("color")) {
        uint32_t c;
        if (inherit) st->color = ps->color;
        else if (initial) st->color = 0;
        else if (color_val(v, vl, &c) && c) st->color = c == CURRENT ? ps->color : c & 0xFFFFFF;
        return;
    }
    if (P("background-color")) { uint32_t c; if (inherit) st->bg = ps->bg; else if (initial) st->bg = 0; else if (color_val(v, vl, &c)) st->bg = c == CURRENT ? 0xFF000000 | st->color : c; return; }
    if (P("background") || P("background-image")) {
        if (inherit) { st->bg = ps->bg; st->bgimage = ps->bgimage; return; }
        bool img_only = P("background-image");
        st->grad = 0;                                    /* a new background replaces an earlier gradient */
        if (!img_only) { st->bg = 0; st->bgrep = 0; st->bgcenter = 0; }
        st->bgimage = 0;
        int k = tokens(v, vl, t, tl, 8);
        for (int i = 0; i < k; i++) {
            uint32_t c;
            if (pre_ci(t[i], tl[i], "url(")) {
                const char *a = t[i] + 4; int al = tl[i] - 5;
                while (al > 0 && sp(*a)) { a++; al--; }
                while (al > 0 && sp(a[al - 1])) al--;
                if (al >= 2 && (*a == '"' || *a == '\'')) { a++; al -= 2; }
                if (al > 0 && al < 500 && !pre_ci(a, al, "data:")) {
                    char rel[512], full[600]; memcpy(rel, a, al); rel[al] = 0;
                    resolve_url(DOC->base, rel, full, sizeof full);
                    st->bgimage = ar_strndup(sar, full, strlen(full));
                }
            } else if (pre_ci(t[i], tl[i], "linear-gradient(") || pre_ci(t[i], tl[i], "radial-gradient(")) {
                /* a flat colour: the average of the colour stops (see-through ones count as such) */
                const char *a = t[i]; int al = tl[i];
                const char *o = a; while (o < a + al && *o != '(') o++;
                const char *q = o + 1;
                long sr = 0, sg = 0, sb = 0, sa = 0; int ns = 0;
                uint32_t gc0 = 0, gc1 = 0; bool grad_first = false;
                bool radial = pre_ci(t[i], tl[i], "radial-gradient("), clear_stop = false;
                while (q < a + al) {
                    const char *b = q; int depth = 0;
                    while (q < a + al && (depth || (*q != ',' && *q != ')'))) { if (*q == '(') depth++; else if (*q == ')') depth--; q++; }
                    const char *tt[3]; int ttl[3]; int kk = tokens(b, q - b, tt, ttl, 3);
                    if (kk && (color_val(tt[0], ttl[0], &c) || eq_ci(tt[0], ttl[0], "transparent"))) {
                        if (eq_ci(tt[0], ttl[0], "transparent")) c = 0;
                        if (!ns) gc0 = c == CURRENT ? 0xFF000000 | st->color : c;
                        gc1 = c == CURRENT ? 0xFF000000 | st->color : c;
                        if (!(c >> 24)) clear_stop = true;
                        if (c == CURRENT) c = 0xFF000000 | st->color;
                        long ca = c >> 24; sa += ca; sr += ca * (c >> 16 & 255); sg += ca * (c >> 8 & 255); sb += ca * (c & 255); ns++;
                    }
                    q++;
                }
                if (radial && clear_stop) ns = 0;          /* a glow that fades out: nothing is closer than a flat patch */
                if (!radial && ns >= 2 && !img_only) {     /* a linear gradient: from its first colour to its last */
                    const char *g0 = o + 1; while (*g0 == ' ') g0++;
                    int gl = (int)(a + al - g0);
                    st->grad = pre_ci(g0, gl, "to right") || pre_ci(g0, gl, "90deg") ? 2 : pre_ci(g0, gl, "to left") || pre_ci(g0, gl, "270deg") || pre_ci(g0, gl, "-90deg") ? 4 :
                               pre_ci(g0, gl, "to top") || pre_ci(g0, gl, "0deg") || pre_ci(g0, gl, "360deg") ? 3 : 1;
                    grad_first = true;
                }
                if (ns && !img_only) {
                    long ma = sa / ns;
                    st->bg = ma < 13 ? 0 : (uint32_t)(ma > 255 ? 255 : ma) << 24 | (uint32_t)(sr / sa) << 16 | (uint32_t)(sg / sa) << 8 | (uint32_t)(sb / sa);
                    if (grad_first && (gc0 >> 24 || gc1 >> 24)) { st->bg = gc0 ? gc0 : (gc1 & 0xFFFFFF) | 0x01000000; st->bg2 = gc1 ? gc1 : (gc0 & 0xFFFFFF); }
                    else st->grad = 0;
                }
            } else if (!img_only && color_val(t[i], tl[i], &c)) st->bg = c == CURRENT ? 0xFF000000 | st->color : c;
            else if (eq_ci(t[i], tl[i], "no-repeat")) st->bgrep = 1;
            else if (eq_ci(t[i], tl[i], "repeat-x")) st->bgrep = 2;
            else if (eq_ci(t[i], tl[i], "repeat-y")) st->bgrep = 3;
            else if (eq_ci(t[i], tl[i], "center")) st->bgcenter = 1;
        }
        return;
    }
    if (P("background-repeat")) { st->bgrep = has_ci(v, vl, "no-repeat") ? 1 : eq_ci(v, vl, "repeat-x") ? 2 : eq_ci(v, vl, "repeat-y") ? 3 : 0; return; }
    if (P("background-position")) { st->bgcenter = has_ci(v, vl, "center") || has_ci(v, vl, "50%"); return; }
    if (P("font-size")) {
        if (inherit) { st->fsize = ps->fsize; return; }
        if (initial) { st->fsize = 16; return; }
        int px = font_size_val(v, vl, ps->fsize);
        if (px > 0) st->fsize = px > 200 ? 200 : px < 6 ? 6 : px;
        return;
    }
    if (P("font-weight")) { st->bold = inherit ? ps->bold : weight_val(v, vl, ps->bold); return; }
    if (P("font-style")) { st->italic = inherit ? ps->italic : (pre_ci(v, vl, "italic") || pre_ci(v, vl, "oblique")); return; }
    if (P("font-family")) { st->mono = inherit ? ps->mono : is_mono_family(v, vl); return; }
    if (P("font")) {
        if (inherit) { st->fsize = ps->fsize; st->bold = ps->bold; st->italic = ps->italic; st->mono = ps->mono; return; }
        int k = tokens(v, vl, t, tl, 8);
        st->bold = 0; st->italic = 0;
        for (int i = 0; i < k; i++) {
            const char *s = t[i]; int n = tl[i];
            if (eq_ci(s, n, "bold") || eq_ci(s, n, "bolder")) st->bold = 1;
            else if (eq_ci(s, n, "italic") || eq_ci(s, n, "oblique")) st->italic = 1;
            else if (n >= 3 && s[0] >= '1' && s[0] <= '9' && s[n - 1] >= '0' && s[n - 1] <= '9' && n == 3) st->bold = s[0] >= '6';
            else {
                const char *sl = s; while (sl < s + n && *sl != '/') sl++;
                int px = font_size_val(s, sl - s, ps->fsize);
                if (px > 0) {
                    st->fsize = px > 200 ? 200 : px < 6 ? 6 : px;
                    if (sl < s + n) line_height_val(st, sl + 1, s + n - sl - 1);
                    else { st->lh = 0; st->lh_num = 0; }
                    const char *fam = s + n; st->mono = is_mono_family(fam, v + vl - fam);
                    break;
                }
            }
        }
        return;
    }
    if (P("text-align")) {
        if (inherit) { st->align = ps->align; return; }
        if (pre_ci(v, vl, "center") || pre_ci(v, vl, "-webkit-center") || pre_ci(v, vl, "-moz-center")) st->align = TA_CENTER;
        else if (pre_ci(v, vl, "right") || pre_ci(v, vl, "end") || pre_ci(v, vl, "-webkit-right")) st->align = TA_RIGHT;
        else if (pre_ci(v, vl, "justify")) st->align = TA_JUSTIFY;
        else st->align = TA_LEFT;
        return;
    }
    if (P("text-decoration") || P("text-decoration-line")) {
        if (inherit) { st->deco = ps->deco; return; }
        st->deco = 0;
        if (has_ci(v, vl, "underline")) st->deco |= DECO_UNDER;
        if (has_ci(v, vl, "line-through")) st->deco |= DECO_STRIKE;
        if (has_ci(v, vl, "overline")) st->deco |= DECO_OVER;
        return;
    }
    if (P("text-transform")) {
        st->tt = inherit ? ps->tt : eq_ci(v, vl, "uppercase") ? TT_UPPER : eq_ci(v, vl, "lowercase") ? TT_LOWER : eq_ci(v, vl, "capitalize") ? TT_CAP : TT_NONE;
        return;
    }
    if (P("white-space") || P("white-space-collapse") || P("text-wrap")) {
        if (inherit) { st->ws = ps->ws; return; }
        if (eq_ci(v, vl, "pre") || eq_ci(v, vl, "preserve nowrap")) st->ws = WS_PRE;
        else if (eq_ci(v, vl, "nowrap")) { if (P("white-space") || st->ws == WS_NORMAL) st->ws = WS_NOWRAP; }
        else if (eq_ci(v, vl, "pre-wrap") || eq_ci(v, vl, "break-spaces") || eq_ci(v, vl, "preserve")) st->ws = WS_PREWRAP;
        else if (eq_ci(v, vl, "pre-line")) st->ws = WS_PRELINE;
        else if (eq_ci(v, vl, "normal") || eq_ci(v, vl, "wrap")) st->ws = WS_NORMAL;
        return;
    }
    if (P("list-style-type") || P("list-style")) {
        if (inherit) { st->list = ps->list; st->list_inside = ps->list_inside; return; }
        int k = tokens(v, vl, t, tl, 4);
        for (int i = 0; i < k; i++) {
            const char *s = t[i]; int n = tl[i];
            if (eq_ci(s, n, "none")) st->list = LS_NONE;
            else if (eq_ci(s, n, "disc")) st->list = LS_DISC;
            else if (eq_ci(s, n, "circle")) st->list = LS_CIRCLE;
            else if (eq_ci(s, n, "square")) st->list = LS_SQUARE;
            else if (eq_ci(s, n, "decimal") || eq_ci(s, n, "decimal-leading-zero")) st->list = LS_DECIMAL;
            else if (eq_ci(s, n, "lower-alpha") || eq_ci(s, n, "lower-latin")) st->list = LS_LALPHA;
            else if (eq_ci(s, n, "upper-alpha") || eq_ci(s, n, "upper-latin")) st->list = LS_UALPHA;
            else if (eq_ci(s, n, "lower-roman")) st->list = LS_LROMAN;
            else if (eq_ci(s, n, "upper-roman")) st->list = LS_UROMAN;
            else if (eq_ci(s, n, "inside")) st->list_inside = 1;
            else if (eq_ci(s, n, "outside")) st->list_inside = 0;
        }
        return;
    }
    if (P("list-style-position")) { st->list_inside = inherit ? ps->list_inside : eq_ci(v, vl, "inside"); return; }
    if (P("opacity")) {                                  /* only fully transparent matters here: it hides the box */
        bool zero = vl > 0 && (v[0] == '0' || v[0] == '.');
        for (int k = 0; k < vl && zero; k++) if (v[k] != '0' && v[k] != '.' && v[k] != '%' && v[k] != ' ') zero = false;
        if (zero) st->vis = 0;
        return;
    }
    if (P("visibility")) { st->vis = inherit ? ps->vis : !(eq_ci(v, vl, "hidden") || eq_ci(v, vl, "collapse")); return; }
    if (P("line-height")) { if (inherit) { st->lh = ps->lh; st->lh_num = ps->lh_num; } else line_height_val(st, v, vl); return; }
    if (P("vertical-align")) {
        st->valign_set = 1;
        st->valign = eq_ci(v, vl, "top") || eq_ci(v, vl, "text-top") ? VA_TOP : eq_ci(v, vl, "middle") ? VA_MIDDLE :
                     eq_ci(v, vl, "bottom") || eq_ci(v, vl, "text-bottom") ? VA_BOTTOM : eq_ci(v, vl, "super") ? VA_SUPER :
                     eq_ci(v, vl, "sub") ? VA_SUB : VA_BASELINE;
        return;
    }
    if (P("text-indent")) { struct len l; if (parse_len(v, vl, st->fsize, &l, false) && l.t == L_PX) st->indent = l.v; else if (inherit) st->indent = ps->indent; return; }
    if (P("clear")) { st->clear = eq_ci(v, vl, "left") ? 1 : eq_ci(v, vl, "right") ? 2 : eq_ci(v, vl, "both") ? 3 : 0; return; }
    if (P("overflow") || P("overflow-x") || P("overflow-y")) { if (eq_ci(v, vl, "hidden") || eq_ci(v, vl, "auto") || eq_ci(v, vl, "scroll") || pre_ci(v, vl, "hidden ")) st->bfc = 1; return; }
    if (P("float")) { st->floated = eq_ci(v, vl, "left") ? 1 : eq_ci(v, vl, "right") ? 2 : 0; return; }
    /* pos: 1 absolute, 2 fixed, 3 relative (sticky is treated as relative) */
    if (P("position")) { st->pos = eq_ci(v, vl, "absolute") ? 1 : eq_ci(v, vl, "fixed") ? 2 : eq_ci(v, vl, "relative") || eq_ci(v, vl, "sticky") || eq_ci(v, vl, "-webkit-sticky") ? 3 : 0; return; }
    if (P("z-index")) { int z = 0, neg = 0, k = 0; if (k < vl && v[k] == '-') { neg = 1; k++; } for (; k < vl && v[k] >= '0' && v[k] <= '9'; k++) z = z * 10 + (v[k] - '0'); st->z = neg ? -z : z; return; }
    if (P("inset")) {
        int k = tokens(v, vl, t, tl, 4); struct len l[4];
        for (int i = 0; i < k; i++) if (!parse_len(t[i], tl[i], st->fsize, &l[i], true)) l[i].t = L_AUTO;
        if (k == 1) l[1] = l[2] = l[3] = l[0]; else if (k == 2) { l[2] = l[0]; l[3] = l[1]; } else if (k == 3) l[3] = l[1];
        if (k) for (int i = 0; i < 4; i++) st->inset[i] = l[i];
        return;
    }
    /* boxes moved far off the screen (top/left -1000px and the like) are a way of hiding them */
    {
        static const char *const isides[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) if (pl == (int)strlen(isides[i]) && !memcmp(prop, isides[i], pl)) {
            struct len l; if (!parse_len(v, vl, st->fsize, &l, true)) l.t = L_AUTO;
            st->inset[i] = l;
            if (i == 0) st->off_top = l.t == L_PX && l.v < -999;
            if (i == 3) st->off_left = l.t == L_PX && l.v < -999;
            return;
        }
    }
    if (P("box-sizing")) { st->bsizing = eq_ci(v, vl, "border-box"); return; }
    if (P("border-spacing")) { struct len l; int k = tokens(v, vl, t, tl, 2); if (k && parse_len(t[0], tl[0], st->fsize, &l, false) && l.t == L_PX) st->spacing = l.v < 0 ? 0 : l.v > 50 ? 50 : l.v; return; }
    if (P("border-collapse")) { st->bcollapse = eq_ci(v, vl, "collapse"); if (st->bcollapse) st->spacing = 0; return; }
    if (P("flex-direction")) { st->flex_col = pre_ci(v, vl, "column"); return; }
    if (P("flex-flow")) { st->flex_col = has_ci(v, vl, "column"); st->flex_wrap = has_ci(v, vl, "wrap") && !has_ci(v, vl, "nowrap"); return; }
    if (P("flex-wrap")) { st->flex_wrap = pre_ci(v, vl, "wrap"); return; }
    if (P("justify-content")) {
        st->justify = has_ci(v, vl, "center") ? JC_CENTER : has_ci(v, vl, "space-between") ? JC_BETWEEN :
                      has_ci(v, vl, "space-around") || has_ci(v, vl, "space-evenly") ? JC_AROUND : has_ci(v, vl, "end") || has_ci(v, vl, "right") ? JC_END : JC_START;
        return;
    }
    if (P("gap") || P("column-gap") || P("grid-gap") || P("grid-column-gap")) {
        struct len l; int k = tokens(v, vl, t, tl, 2);
        if (k && parse_len(t[k - 1], tl[k - 1], st->fsize, &l, false) && l.t == L_PX) st->gap = l.v < 0 ? 0 : l.v > 200 ? 200 : l.v;
        if (k && (P("gap") || P("grid-gap")) && parse_len(t[0], tl[0], st->fsize, &l, false) && l.t == L_PX) st->rgap = l.v < 0 ? 0 : l.v > 200 ? 200 : l.v;
        return;
    }
    if (P("row-gap") || P("grid-row-gap")) { struct len l; if (parse_len(v, vl, st->fsize, &l, false) && l.t == L_PX) st->rgap = l.v < 0 ? 0 : l.v > 200 ? 200 : l.v; return; }
    if (P("border-radius") || P("border-top-left-radius")) {
        int k = tokens(v, vl, t, tl, 2); struct len l;
        if (k && tl[0] && t[0][tl[0] - 1] == '%') st->radius = num100(t[0], tl[0] - 1) >= 4000 ? -1 : 0;
        else if (k && parse_len(t[0], tl[0], st->fsize, &l, false) && l.t == L_PX) st->radius = l.v < 0 ? 0 : l.v > 4000 ? -1 : l.v;
        return;
    }
    if (P("transform") || P("-webkit-transform")) {
        st->tx.t = st->ty.t = L_PX; st->tx.v = st->ty.v = 0;
        for (int i = 0; i + 9 < vl; i++) if (pre_ci(v + i, vl - i, "translate")) {
            const char *q = v + i + 9; char ax = 0;
            if (*q == 'X' || *q == 'x' || *q == 'Y' || *q == 'y') ax = *q++ | 32;
            if (*q == '3') { q += 2; }                       /* translate3d(x, y, z) */
            if (*q != '(') continue;
            const char *e = q + 1; while (e < v + vl && *e != ')') e++;
            const char *c1 = q + 1, *cm = c1; while (cm < e && *cm != ',') cm++;
            struct len l;
            if (parse_len(c1, (int)(cm - c1), st->fsize, &l, true)) { if (ax == 'y') st->ty = l; else st->tx = l; }
            if (!ax && cm < e) { const char *c2 = cm + 1, *ce = c2; while (ce < e && *ce != ',') ce++; if (parse_len(c2, (int)(ce - c2), st->fsize, &l, true)) st->ty = l; }
            i = (int)(e - v);
        }
        return;
    }
    if (P("flex-grow")) { st->fgrow = (int16_t)num100(v, vl); return; }
    if (P("flex-shrink")) { st->fshrink = (int16_t)num100(v, vl); return; }
    if (P("flex-basis")) { struct len l; if (eq_ci(v, vl, "auto") || eq_ci(v, vl, "content")) st->fbasis.t = L_AUTO; else if (parse_len(v, vl, st->fsize, &l, true)) st->fbasis = l; return; }
    if (P("flex")) {
        if (eq_ci(v, vl, "none")) { st->fgrow = 0; st->fshrink = 0; st->fbasis.t = L_AUTO; return; }
        if (eq_ci(v, vl, "auto")) { st->fgrow = 100; st->fshrink = 100; st->fbasis.t = L_AUTO; return; }
        if (eq_ci(v, vl, "initial")) { st->fgrow = 0; st->fshrink = 100; st->fbasis.t = L_AUTO; return; }
        int k = tokens(v, vl, t, tl, 3), nn = 0; bool basis = false;
        st->fgrow = 0; st->fshrink = 100; st->fbasis.t = L_AUTO;
        for (int i = 0; i < k; i++) {
            bool unitless = true; for (int j = 0; j < tl[i]; j++) if (!((t[i][j] >= '0' && t[i][j] <= '9') || t[i][j] == '.')) unitless = false;
            struct len l;
            if (unitless && tl[i] && nn < 2) { if (nn++ == 0) st->fgrow = (int16_t)num100(t[i], tl[i]); else st->fshrink = (int16_t)num100(t[i], tl[i]); }
            else if (eq_ci(t[i], tl[i], "auto")) { st->fbasis.t = L_AUTO; basis = true; }
            else if (parse_len(t[i], tl[i], st->fsize, &l, true)) { st->fbasis = l; basis = true; }
        }
        if (!basis && nn) { st->fbasis.t = L_PX; st->fbasis.v = 0; }       /* flex: 1 -> 1 1 0 */
        return;
    }
    if (P("align-items") || P("align-self")) {
        int a = has_ci(v, vl, "center") ? 2 : has_ci(v, vl, "end") ? 3 : has_ci(v, vl, "start") || has_ci(v, vl, "baseline") ? 1 : 0;
        if (P("align-items")) st->align_items = (uint8_t)a; else st->align_self = (uint8_t)(eq_ci(v, vl, "auto") ? 0 : a ? a : 4);
        return;
    }
    if (P("place-items")) { st->align_items = has_ci(v, vl, "center") ? 2 : has_ci(v, vl, "end") ? 3 : has_ci(v, vl, "start") ? 1 : 0; return; }
    if (P("grid-template-columns")) { grid_tracks(st, v, vl); return; }
    if (P("grid-column") || P("grid-column-end") || P("grid-area")) {
        const char *sp_ = 0; for (int i = 0; i + 4 <= vl; i++) if (pre_ci(v + i, vl - i, "span")) { sp_ = v + i + 4; break; }
        if (sp_) { int n_ = 0; while (sp_ < v + vl && *sp_ == ' ') sp_++; while (sp_ < v + vl && *sp_ >= '0' && *sp_ <= '9') n_ = n_ * 10 + (*sp_++ - '0'); st->col_span = (uint8_t)(n_ < 1 ? 1 : n_ > 12 ? 12 : n_); }
        else if (P("grid-column") && has_ci(v, vl, "-1")) st->col_span = 255;
        return;
    }
    /* box model */
    static const char *const sides[4] = { "top", "right", "bottom", "left" };
    if (pl >= 6 && (!memcmp(prop, "margin", 6) || (pl >= 7 && !memcmp(prop, "padding", 7)))) {
        bool m = prop[0] == 'm';
        struct len *dst = m ? st->m : st->p; const struct len *src = m ? ps->m : ps->p;
        const char *rest = prop + (m ? 6 : 7);
        if (!*rest) {
            if (inherit) { for (int i = 0; i < 4; i++) dst[i] = src[i]; return; }
            int k = four(v, vl, t, tl);
            if (!k) return;
            struct len l[4];
            for (int i = 0; i < 4; i++) if (!parse_len(t[i], tl[i], st->fsize, &l[i], true) || (!m && (l[i].t == L_AUTO || l[i].v < 0))) return;
            for (int i = 0; i < 4; i++) dst[i] = l[i];
            return;
        }
        if (*rest == '-') {
            rest++;
            int idx = -1;
            for (int i = 0; i < 4; i++) if (!strcmp(rest, sides[i])) idx = i;
            if (!strcmp(rest, "inline-start")) idx = 3; else if (!strcmp(rest, "inline-end")) idx = 1;
            else if (!strcmp(rest, "block-start")) idx = 0; else if (!strcmp(rest, "block-end")) idx = 2;
            if (!strcmp(rest, "inline") || !strcmp(rest, "block")) {
                int k = tokens(v, vl, t, tl, 2); if (!k) return; if (k == 1) { t[1] = t[0]; tl[1] = tl[0]; }
                struct len a, b;
                if (!parse_len(t[0], tl[0], st->fsize, &a, true) || !parse_len(t[1], tl[1], st->fsize, &b, true)) return;
                if (rest[0] == 'i') { dst[3] = a; dst[1] = b; } else { dst[0] = a; dst[2] = b; }
                return;
            }
            if (idx < 0) return;
            if (inherit) { dst[idx] = src[idx]; return; }
            struct len l;
            if (parse_len(v, vl, st->fsize, &l, true) && !(!m && (l.t == L_AUTO || l.v < 0))) dst[idx] = l;
            return;
        }
        return;
    }
    if (pl >= 6 && !memcmp(prop, "border", 6)) {
        const char *rest = prop + 6;
        if (!*rest) {
            if (initial || eq_ci(v, vl, "none") || eq_ci(v, vl, "0")) { for (int i = 0; i < 4; i++) { st->bs[i] = 0; st->bw[i] = 0; } return; }
            for (int i = 0; i < 4; i++) apply_border_side(st, i, v, vl);
            return;
        }
        if (!strcmp(rest, "-width") || !strcmp(rest, "-style") || !strcmp(rest, "-color")) {
            int k = four(v, vl, t, tl);
            if (!k) return;
            for (int i = 0; i < 4; i++) {
                if (rest[1] == 'w') { int w; if (border_width_kw(t[i], tl[i], st->fsize, &w)) st->bw[i] = w; }
                else if (rest[1] == 's') { int s = border_style_kw(t[i], tl[i]); if (s >= 0) st->bs[i] = s; }
                else { uint32_t c; if (color_val(t[i], tl[i], &c)) st->bc[i] = c; }
            }
            return;
        }
        if (!strcmp(rest, "-radius") || pre_ci(rest, strlen(rest), "-image")) return;
        if (*rest == '-') {
            rest++;
            int idx = -1, sl = 0;
            for (int i = 0; i < 4; i++) { int l = strlen(sides[i]); if (!strncmp(rest, sides[i], l)) { idx = i; sl = l; } }
            if (idx < 0) {
                if (!strncmp(rest, "inline-start", 12)) { idx = 3; sl = 12; } else if (!strncmp(rest, "inline-end", 10)) { idx = 1; sl = 10; }
                else if (!strncmp(rest, "block-start", 11)) { idx = 0; sl = 11; } else if (!strncmp(rest, "block-end", 9)) { idx = 2; sl = 9; }
                else return;
            }
            const char *what = rest + sl;
            if (!*what) { apply_border_side(st, idx, v, vl); return; }
            if (!strcmp(what, "-width")) { int w; if (border_width_kw(v, vl, st->fsize, &w)) st->bw[idx] = w; return; }
            if (!strcmp(what, "-style")) { int s = border_style_kw(v, vl); if (s >= 0) st->bs[idx] = s; return; }
            if (!strcmp(what, "-color")) { uint32_t c; if (color_val(v, vl, &c)) st->bc[idx] = c; return; }
        }
        return;
    }
    if (P("width") || P("min-width") || P("max-width") || P("height") || P("min-height")) {
        struct len l;
        if (!parse_len(v, vl, st->fsize, &l, true)) {
            if (eq_ci(v, vl, "fit-content") || eq_ci(v, vl, "max-content") || eq_ci(v, vl, "min-content")) { l.t = L_AUTO; l.v = 0; }
            else return;
        }
        if (l.t == L_PX && l.v < 0) return;
        if (P("width")) st->w = l; else if (P("min-width")) st->minw = l; else if (P("max-width")) st->maxw = l;
        else if (P("height")) st->h = l;
        else if (l.t == L_PX && (st->h.t != L_PX || st->h.v < l.v)) st->h = l;    /* min-height: as a height */
        return;
    }
#undef P
}

static struct decl inl[128];

/* presentational attributes (bgcolor, width, align, ...) as low-priority declarations */
static int hints(const struct node *e, char *buf, int max) {
    int n = 0; const char *v;
    buf[0] = 0;
#define ADD(...) do { if (n < max - 100) n += ksnprintf(buf + n, max - n, __VA_ARGS__); } while (0)
    if ((v = node_attr(e, "bgcolor"))) ADD("background-color:%s%s;", v[0] != '#' && strlen(v) == 6 && hexv(v[0]) >= 0 && hexv(v[5]) >= 0 ? "#" : "", v);
    if (e->tag == T_FONT) {
        if ((v = node_attr(e, "color"))) ADD("color:%s%s;", v[0] != '#' && strlen(v) == 6 && hexv(v[0]) >= 0 && hexv(v[5]) >= 0 ? "#" : "", v);
        if ((v = node_attr(e, "face"))) ADD("font-family:%s;", v);
        if ((v = node_attr(e, "size"))) {
            int s = katoi(v[0] == '+' || v[0] == '-' ? v + 1 : v);
            if (v[0] == '+') s = 3 + s; else if (v[0] == '-') s = 3 - s;
            static const char *const fs[] = { "x-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large" };
            if (s < 1) s = 1;
            if (s > 7) s = 7;
            ADD("font-size:%s;", fs[s]);
        }
    }
    /* <center> and align=center centre the blocks inside them, too */
    if (e->parent && e->parent->type == N_ELEM && e->tag != T_TD && e->tag != T_TH && e->tag != T_TR) {
        const char *pa = node_attr(e->parent, "align");
        if (e->parent->tag == T_CENTER || (pa && !strcmp(pa, "center") && e->parent->tag != T_TABLE)) ADD("margin-left:auto;margin-right:auto;");
    }
    if (e->tag == T_TABLE && !DOC->standards) ADD("text-align:left;");       /* quirks mode: tables don't inherit it */
    if (e->tag == T_BODY) {
        if ((v = node_attr(e, "text"))) ADD("color:%s;", v);
    }
    if ((v = node_attr(e, "align"))) {
        if (e->tag == T_TABLE) { if (!strcmp(v, "center")) ADD("margin-left:auto;margin-right:auto;"); else if (!strcmp(v, "right") || !strcmp(v, "left")) ADD("float:%s;", v); }
        else if (e->tag == T_IMG) { if (!strcmp(v, "middle") || !strcmp(v, "absmiddle")) ADD("vertical-align:middle;"); }
        else ADD("text-align:%s;", v);
    }
    if ((v = node_attr(e, "valign"))) ADD("vertical-align:%s;", v);
    if (e->tag == T_BR && (v = node_attr(e, "clear"))) ADD("clear:%s;", !strcmp(v, "all") ? "both" : v);
    if (e->tag == T_IMG && (v = node_attr(e, "align")) && (!strcmp(v, "left") || !strcmp(v, "right"))) ADD("float:%s;", v);
    if (e->tag == T_TABLE || e->tag == T_TD || e->tag == T_TH || e->tag == T_IMG || e->tag == T_COL || e->tag == T_HR || e->tag == T_IFRAME || e->tag == T_PRE) {
        if ((v = node_attr(e, "width")) && v[0] >= '0' && v[0] <= '9' && e->tag != T_PRE) { int k = katoi(v); bool pct = v[strlen(v) - 1] == '%'; if (k) ADD("width:%d%s;", k, pct ? "%" : "px"); }
        if ((v = node_attr(e, "height")) && v[0] >= '0' && v[0] <= '9' && e->tag != T_TD && e->tag != T_TH && e->tag != T_TABLE) { int k = katoi(v); bool pct = v[strlen(v) - 1] == '%'; if (!pct) ADD("height:%dpx;", k); }
    }
    if (e->tag == T_TD || e->tag == T_TH) {
        if (node_attr(e, "nowrap")) ADD("white-space:nowrap;");
        /* cellpadding and table borders come from the table */
        const struct node *t = e->parent;
        for (int k = 0; t && k < 3 && t->tag != T_TABLE; k++) t = t->parent;
        if (t && t->tag == T_TABLE) {
            if ((v = node_attr(t, "cellpadding"))) ADD("padding:%dpx;", katoi(v));
            if ((v = node_attr(t, "border")) && katoi(v) > 0) ADD("border:1px solid #808080;");
        }
    }
    if (e->tag == T_TABLE) {
        if ((v = node_attr(e, "cellspacing"))) ADD("border-spacing:%dpx;", katoi(v));
        if ((v = node_attr(e, "border"))) { int k = katoi(v); if (k > 0) ADD("border:%dpx solid #808080;", k > 10 ? 10 : k); }
    }
    if (e->tag == T_HR) {
        if (node_attr(e, "noshade") || node_attr(e, "color")) ADD("border-top:2px solid %s;", node_attr(e, "color") ? node_attr(e, "color") : "#808080");
        if ((v = node_attr(e, "size")) && katoi(v) > 1) ADD("border-top-width:%dpx;", katoi(v) > 20 ? 20 : katoi(v));
        if ((v = node_attr(e, "align"))) { if (!strcmp(v, "center")) ADD("margin-left:auto;margin-right:auto;"); }
        else if (node_attr(e, "width")) ADD("margin-left:auto;margin-right:auto;");
    }
    if (e->tag == T_IMG || e->tag == T_TABLE) {
        if ((v = node_attr(e, "hspace"))) ADD("margin-left:%dpx;margin-right:%dpx;", katoi(v), katoi(v));
        if ((v = node_attr(e, "vspace"))) ADD("margin-top:%dpx;margin-bottom:%dpx;", katoi(v), katoi(v));
        if (e->tag == T_IMG && (v = node_attr(e, "border")) && katoi(v) > 0) ADD("border:%dpx solid;", katoi(v));
    }
    if ((e->tag == T_OL || e->tag == T_UL || e->tag == T_LI) && (v = node_attr(e, "type"))) {
        const char *ls = !strcmp(v, "1") ? "decimal" : !strcmp(v, "a") ? "lower-alpha" : !strcmp(v, "A") ? "upper-alpha" :
                         !strcmp(v, "i") ? "lower-roman" : !strcmp(v, "I") ? "upper-roman" : v;
        ADD("list-style-type:%s;", ls);
    }
#undef ADD
    return n;
}

static int cascade_depth;
static void style_elem(struct node *e, const struct style *ps) {
    struct style *st = e->st_mem;                    /* reused when the styles are worked out again (window resized) */
    if (!st) st = e->st_mem = ar_alloc(sar, sizeof *st);
    if (!st) return;
    memset(st, 0, sizeof *st);
    /* inherited */
    st->color = ps->color; st->fsize = ps->fsize; st->bold = ps->bold; st->italic = ps->italic; st->mono = ps->mono;
    st->align = ps->align; st->ws = ps->ws; st->list = ps->list; st->list_inside = ps->list_inside; st->vis = ps->vis;
    st->lh_num = ps->lh_num; st->lh = ps->lh_num ? 0 : ps->lh; st->tt = ps->tt; st->indent = ps->indent; st->vars = ps->vars;
    st->spacing = ps->spacing; st->bcollapse = ps->bcollapse;    /* inherited, as in CSS */
    /* not inherited: initial values */
    st->display = D_INLINE; st->bg = 0; st->deco = 0; st->valign = VA_BASELINE;
    for (int i = 0; i < 4; i++) { st->m[i].t = L_PX; st->p[i].t = L_PX; st->bc[i] = CURRENT; st->bw[i] = 3; }
    st->fshrink = 100;
    st->w.t = L_AUTO; st->h.t = L_AUTO; st->minw.t = L_AUTO; st->maxw.t = L_NONE;
    e->st = st;

    nm = 0;
    collect(&ua_set, e);
    if (match_budget > 0) collect(&au_set, e);
    /* sort the matches */
    for (int i = 1; i < nm; i++) { const struct rule *r = matched[i]; int j = i; while (j > 0 && rule_before(r, matched[j - 1])) { matched[j] = matched[j - 1]; j--; } matched[j] = r; }
    /* presentational hints, then the style attribute */
    static char hb[1024];
    int hl = hints(e, hb, sizeof hb);
    int nh = 0; struct decl hd[24];
    if (hl) nh = parse_decls(hb, hl, hd, 24);
    int ni = 0;
    if (e->style_attr) ni = parse_decls(e->style_attr, strlen(e->style_attr), inl, 128);
    /* custom properties first */
    for (int pass = 0; pass < 4; pass++) {             /* 0: custom normal, 1: custom important, 2: normal, 3: important */
        bool imp = pass & 1;
        bool custom = pass < 2;
        if (!custom) {                                 /* font-size first: other lengths depend on it */
            for (int sub = 0; sub < 2; sub++) {
                bool fsz = sub == 0;
#define APPLY(dd) do { const struct decl *D_ = (dd); bool isfs = !strcmp(D_->prop, "font-size") || !strcmp(D_->prop, "font"); \
                       if (D_->imp == imp && isfs == fsz && D_->prop[0] != '-') apply(st, ps, D_->prop, D_->val, D_->vlen); } while (0)
                for (int i = 0; i < nm; i++) { if (matched[i]->origin >= 2) break; for (int k = 0; k < matched[i]->nd; k++) APPLY(&matched[i]->d[k]); }
                for (int k = 0; k < nh; k++) {
                    struct decl d = hd[k]; char pn[40]; int l = d.plen < 39 ? d.plen : 39; for (int q = 0; q < l; q++) pn[q] = lc(d.prop[q]); pn[l] = 0;
                    char *vv = ar_strndup(sar, d.val, d.vlen); if (!vv) break;
                    d.prop = pn; d.val = vv; APPLY(&d);
                }
                for (int i = 0; i < nm; i++) { if (matched[i]->origin < 2) continue; for (int k = 0; k < matched[i]->nd; k++) APPLY(&matched[i]->d[k]); }
                for (int k = 0; k < ni; k++) {
                    struct decl d = inl[k]; char pn[40]; int l = d.plen < 39 ? d.plen : 39; for (int q = 0; q < l; q++) pn[q] = lc(d.prop[q]); pn[l] = 0;
                    if (d.plen > 39) continue;
                    d.prop = pn; APPLY(&d);
                }
#undef APPLY
            }
        } else {
            for (int i = 0; i <= nm; i++) {
                const struct decl *dl = i < nm ? matched[i]->d : inl; int nd = i < nm ? matched[i]->nd : ni;
                for (int k = 0; k < nd; k++) {
                    const struct decl *d = &dl[k];
                    if (d->imp != imp || d->plen < 3 || d->prop[0] != '-' || d->prop[1] != '-') continue;
                    struct cvar *cv = ar_alloc(sar, sizeof *cv);
                    if (!cv) break;
                    cv->name = i < nm ? d->prop : ar_strndup(sar, d->prop, d->plen);
                    int vl = d->vlen; const char *vv = subst_vars(st, d->val, &vl, 0);
                    cv->value = vv ? ar_strndup(sar, vv, vl) : "";
                    cv->next = st->vars; st->vars = cv;
                }
            }
        }
    }
    /* resolve currentColor and line heights */
    for (int i = 0; i < 4; i++) {
        if (st->bc[i] == CURRENT) st->bc[i] = st->color;
        else st->bc[i] &= 0xFFFFFF;
        if (!st->bs[i]) st->bw[i] = 0;
    }
    if (st->lh_num) st->lh = st->fsize * st->lh_num / 100;
    if (e->tag == T_HTML) root_fs = st->fsize;
    /* elements hidden by moving them away or clipping them to nothing */
    bool outflow = st->pos == 1 || st->pos == 2;
    if (outflow && ((st->w.t == L_PX && st->w.v <= 1) || (st->h.t == L_PX && st->h.v <= 1))) st->display = D_NONE;
    if (outflow && (st->m[3].t == L_PX && st->m[3].v < -1000)) st->display = D_NONE;
    if (outflow && (st->off_top || st->off_left)) st->display = D_NONE;
    if (st->floated && (st->display == D_INLINE || st->display == D_INLINE_BLOCK || st->display == D_LIST_ITEM || st->display == D_INLINE_FLEX))
        st->display = st->display == D_INLINE_FLEX ? D_FLEX : D_BLOCK;
    if (st->floated && (st->display == D_ROW || st->display == D_CELL || st->display == D_ROW_GROUP)) st->floated = 0;
    if (outflow) st->floated = 0;                    /* absolute/fixed boxes are placed by layout.c, outside the flow */
    /* table internals outside tables, and the other way round, are fixed up by layout */
}

/* ---------------- shadow DOM: the flattened tree ---------------- */
static uint32_t fgen_now;
static struct node **ftail;
struct node *css_tree_host(const struct node *n) {
    for (const struct node *p = n ? n->parent : 0; p; p = p->parent) if (p->host && p->host->shadow == p) return p->host;
    return 0;
}
static bool is_slot(const struct node *c) { return c->type == N_ELEM && c->name && !strcmp(c->name, "slot") && css_tree_host(c); }
static void fadd(struct node *c) {
    if (c->fgen == fgen_now) return;                   /* already placed (two slots with the same name) */
    c->fgen = fgen_now; c->fnext = 0; *ftail = c; ftail = &c->fnext;
}
static void flat_add(struct node *c, int depth) {
    if (depth < 16 && is_slot(c)) {
        struct node *h = css_tree_host(c);
        const char *nm = node_attr(c, "name"); if (!nm) nm = "";
        bool any = false;
        for (struct node *k = h->first; k; k = k->next) {
            const char *ks = k->type == N_ELEM ? node_attr(k, "slot") : 0;
            if (nm[0] ? (ks && !strcmp(ks, nm)) : (!ks || !ks[0])) {
                if (k->type == N_TEXT && !nm[0]) { bool blank = true; for (int i = 0; i < k->tlen; i++) if ((unsigned char)k->text[i] > ' ') blank = false; if (blank && !any) continue; }
                any = true;
                if (is_slot(k)) flat_add(k, depth + 1); else fadd(k);
            }
        }
        if (!any) for (struct node *k = c->first; k; k = k->next) flat_add(k, depth + 1);   /* fallback content */
        return;
    }
    fadd(c);
}
static void compose_rec(struct node *e, int depth) {
    if (depth > 400) return;
    struct node *src = e->shadow ? e->shadow : e;
    e->ffirst = 0; ftail = &e->ffirst;
    for (struct node *c = src->first; c; c = c->next) flat_add(c, 0);
    for (struct node *c = e->ffirst; c; c = c->fnext) if (c->type == N_ELEM) compose_rec(c, depth + 1);
}
void css_compose(struct doc *d) { if (!d || !d->root) return; fgen_now++; if (!fgen_now) fgen_now = 1; d->root->fgen = fgen_now; compose_rec(d->root, 0); }

/* :host -> the host's tag, ::slotted(x) -> x inside the host, everything else -> inside the host */
static void sb_put(char **o, long *n, long *cap, const char *s, long k) {
    if (*n + k + 1 > *cap) { long nc = (*cap + k + 64) * 2; char *t = realloc(*o, nc); if (!t) return; *o = t; *cap = nc; }
    memcpy(*o + *n, s, k); *n += k; (*o)[*n] = 0;
}
static long paren_end(const char *s, long i, long n) {      /* s[i] == '(' -> index after its ')' */
    int d = 0;
    for (; i < n; i++) { if (s[i] == '(') d++; else if (s[i] == ')' && --d == 0) return i + 1; }
    return n;
}
static void scope_sel(char **o, long *on, long *cap, const char *s, long n, const char *host) {
    while (n && (unsigned char)s[0] <= ' ') { s++; n--; }
    while (n && (unsigned char)s[n - 1] <= ' ') n--;
    if (!n) return;
    long hl = strlen(host);
    bool hosted = false;
    for (long i = 0; i + 5 <= n; i++) if (!memcmp(s + i, ":host", 5)) hosted = true;
    if (!hosted) { sb_put(o, on, cap, host, hl); sb_put(o, on, cap, " ", 1); }
    for (long i = 0; i < n;) {
        if (i + 13 <= n && !memcmp(s + i, ":host-context", 13) && s[i + 13] == '(') {
            long e = paren_end(s, i + 13, n);
            sb_put(o, on, cap, s + i + 14, e - i - 15); sb_put(o, on, cap, " ", 1); sb_put(o, on, cap, host, hl);
            i = e; continue;
        }
        if (i + 5 <= n && !memcmp(s + i, ":host", 5)) {
            sb_put(o, on, cap, host, hl);
            if (i + 5 < n && s[i + 5] == '(') { long e = paren_end(s, i + 5, n); sb_put(o, on, cap, s + i + 6, e - i - 7); i = e; }
            else i += 5;
            continue;
        }
        if (i + 9 <= n && !memcmp(s + i, "::slotted", 9) && i + 9 < n && s[i + 9] == '(') {
            long e = paren_end(s, i + 9, n);
            if (*on && (*o)[*on - 1] != ' ') sb_put(o, on, cap, " ", 1);
            sb_put(o, on, cap, s + i + 10, e - i - 11); i = e; continue;
        }
        sb_put(o, on, cap, s + i, 1); i++;
    }
}
char *css_scope_shadow(const char *s, long n, const char *host, long *outn) {
    long cap = n + 256, on = 0; char *o = malloc(cap); if (!o) return 0; o[0] = 0;
    int ctx[64], dep = 0; ctx[0] = 0;                      /* 0 rules, 1 declarations, 2 copied as is */
    long i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') { long e = i + 2; while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/')) e++; i = e + 2 > n ? n : e + 2; continue; }
        if (ctx[dep] != 0) {                               /* inside declarations or a copied block */
            long st = i; char q = 0;
            while (i < n) {
                char c = s[i];
                if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
                else if (c == '"' || c == '\'') q = c;
                else if (c == '{') { if (dep < 63) { dep++; ctx[dep] = 2; } i++; break; }
                else if (c == '}') { dep = dep > 0 ? dep - 1 : 0; i++; break; }
                i++;
            }
            sb_put(&o, &on, &cap, s + st, i - st);
            continue;
        }
        if (ch == '}') { dep = dep > 0 ? dep - 1 : 0; sb_put(&o, &on, &cap, "}", 1); i++; continue; }
        if ((unsigned char)ch <= ' ') { sb_put(&o, &on, &cap, s + i, 1); i++; continue; }
        long st = i; char q = 0; int pd = 0;               /* a prelude: up to '{' or ';' */
        while (i < n) {
            char c = s[i];
            if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') pd++;
            else if (c == ')' || c == ']') pd--;
            else if (pd <= 0 && (c == '{' || c == ';' || c == '}')) break;
            i++;
        }
        if (i >= n || s[i] != '{') { sb_put(&o, &on, &cap, s + st, i - st + (i < n && s[i] == ';')); if (i < n && s[i] == ';') i++; continue; }
        if (s[st] == '@') {
            sb_put(&o, &on, &cap, s + st, i - st + 1);
            bool nested = !strncmp(s + st, "@media", 6) || !strncmp(s + st, "@supports", 9) || !strncmp(s + st, "@container", 10) || !strncmp(s + st, "@layer", 6) || !strncmp(s + st, "@document", 9);
            if (dep < 63) { dep++; ctx[dep] = nested ? 0 : 2; }
            i++; continue;
        }
        long a = st; pd = 0; q = 0; bool first = true;     /* the selector list, one at a time */
        for (long k = st; k <= i; k++) {
            char c = k < i ? s[k] : ',';
            if (q) { if (c == q) q = 0; continue; }
            if (c == '"' || c == '\'') { q = c; continue; }
            if (c == '(' || c == '[') pd++; else if (c == ')' || c == ']') pd--;
            else if (c == ',' && pd <= 0) { if (!first) sb_put(&o, &on, &cap, ", ", 2); scope_sel(&o, &on, &cap, s + a, k - a, host); first = false; a = k + 1; }
        }
        sb_put(&o, &on, &cap, " {", 2);
        if (dep < 63) { dep++; ctx[dep] = 1; }
        i++;
    }
    *outn = on;
    return o;
}

static void cascade_rec(struct node *e, const struct style *ps) {
    if (cascade_depth > 200) { e->st = 0; return; }
    style_elem(e, ps);
    if (!e->st) return;
    if (e->st->display == D_NONE) return;
    cascade_depth++;
    for (struct node *c = e->ffirst; c; c = c->fnext) if (c->type == N_ELEM) cascade_rec(c, e->st); else c->st = 0;
    cascade_depth--;
}

static void clear_styles(struct node *e) {
    e->st = 0;
    for (struct node *c = e->first; c; c = c->next) clear_styles(c);
    if (e->shadow) clear_styles(e->shadow);
}

/* attributes like <a href> become :link; <base href> was read already */
void css_cascade(struct doc *d, int vw) {
    DOC = d; sar = &d->ar;
    viewport_w = vw; media_gen++;
    match_budget = 40000000;
    root_fs = 16;
    css_compose(d);
    clear_styles(d->root);
    struct style init; memset(&init, 0, sizeof init);
    init.color = 0x000000; init.fsize = 16; init.vis = 1; init.display = D_BLOCK; init.spacing = 0;
    for (int i = 0; i < 4; i++) { init.m[i].t = L_PX; init.p[i].t = L_PX; }
    init.w.t = L_AUTO; init.h.t = L_AUTO; init.maxw.t = L_NONE;
    cascade_depth = 0;
    cascade_rec(d->root, &init);
}

/* For scripts (querySelector, matches): does element e match the selector list s? Parsed selectors are cached. */
bool css_query_match(const char *s, int n, const struct node *e) {
    static struct arena qa; static struct ruleset qset;
    static char last[256]; static int last_n = -1;
    static struct csel *sels[16]; static int nsel;
    if (n < 0 || n >= (int)sizeof last) return false;
    if (n != last_n || memcmp(last, s, n)) {
        ar_reset(&qa); memset(&qset, 0, sizeof qset); qset.ar = &qa;
        struct ruleset *saved = cur_set; cur_set = &qset;
        nsel = 0;
        int st = 0, depth = 0;
        for (int i = 0; i <= n && nsel < 16; i++) {
            if (i < n && (s[i] == '(' || s[i] == '[')) depth++;
            else if (i < n && (s[i] == ')' || s[i] == ']')) depth--;
            if (i == n || (s[i] == ',' && depth == 0)) {
                int spec; struct csel *c = parse_selector(s + st, i - st, &spec);
                if (!c) { nsel = -1; break; }
                sels[nsel++] = c; st = i + 1;
            }
        }
        cur_set = saved;
        memcpy(last, s, n); last_n = n;
    }
    if (nsel < 0) return false;
    for (int i = 0; i < nsel; i++) { match_budget = 100000; if (sel_match(sels[i], e)) return true; }
    return false;
}
