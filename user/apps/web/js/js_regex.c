/* AXIOM JavaScript engine: backtracking regular expression engine and RegExp builtins. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

/* ---------------- program ---------------- */
enum {
    R_CHAR, R_CHARI, R_ANY, R_ANYALL, R_CLASS, R_NCLASS, R_BOL, R_EOL, R_WORDB, R_NWORDB, R_SPLIT, R_JMP, R_SAVE,
    R_RESET, R_BACKREF, R_BACKREFI, R_LOOK, R_MATCH, R_REP_START, R_REP_TEST, R_REP_POS, R_REP_END, R_GREEDY1, R_LAZY1,
    R_ASSERT_POS, R_NBACKREF
};
/* class item kinds */
enum { CI_RANGE, CI_PROP };
enum {
    PR_ANY, PR_ASCII, PR_L, PR_LU, PR_LL, PR_N, PR_ND, PR_P, PR_S, PR_WS, PR_LATIN, PR_GREEK, PR_CYRILLIC, PR_HAN,
    PR_HIRAGANA, PR_KATAKANA, PR_EMOJI, PR_ALPHA, PR_ARABIC, PR_HEBREW, PR_HANGUL, PR_THAI, PR_M, PR_C, PR_Z
};
typedef struct CItem {
    uint8_t kind;
    bool neg;
    uint32_t lo, hi;
} CItem;
typedef struct RClass {
    CItem *items;
    int n, cap;
} RClass;

struct Regex {
    int32_t *code;
    int ncode, ccap;
    int ncaps; /* including group 0 */
    int nregs;
    JSString **names;
    uint32_t flags;
    RClass *classes;
    int nclasses, clcap;
    int refs;
    /* set of characters a match can start with (fs_valid): bitmap for c < 256, fs_wide for c >= 256 */
    bool fs_valid, fs_wide;
    uint8_t fset[32];
};

int re_ncaps(Regex *re) { return re->ncaps; }
JSString *re_groupname(Regex *re, int i) { return re->names ? re->names[i] : 0; }

void re_free(Regex *re) {
    if (!re) return;
    if (--re->refs > 0) return;
    free(re->code);
    for (int i = 0; i < re->nclasses; i++) free(re->classes[i].items);
    free(re->classes);
    free(re->names);
    free(re);
}

int re_parse_flags(JSString *f, uint32_t *out) {
    uint32_t fl = 0;
    for (uint32_t i = 0; i < f->len; i++) {
        uint32_t c = str_at(f, i), b = 0;
        switch (c) {
        case 'g': b = RF_G; break;
        case 'i': b = RF_I; break;
        case 'm': b = RF_M; break;
        case 's': b = RF_S; break;
        case 'u': b = RF_U; break;
        case 'y': b = RF_Y; break;
        case 'd': b = RF_D; break;
        case 'v': b = RF_V; break;
        default: return -1;
        }
        if (fl & b) return -1;
        fl |= b;
    }
    if ((fl & RF_U) && (fl & RF_V)) return -1;
    *out = fl;
    return 0;
}

/* ---------------- character predicates ---------------- */
static bool is_word(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static bool is_lt(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
static bool is_letter(uint32_t c) {
    if (c < 128) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
    if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;
    if (c >= 0x250 && c <= 0x2C1) return true;
    if (c >= 0x370 && c <= 0x3FF) return c != 0x375 && c != 0x37E && c != 0x384 && c != 0x385 && c != 0x387;
    if (c >= 0x400 && c <= 0x52F) return !(c >= 0x482 && c <= 0x489);
    if (c >= 0x531 && c <= 0x587) return !(c >= 0x557 && c <= 0x560);
    if (c >= 0x5D0 && c <= 0x5EA) return true;
    if (c >= 0x620 && c <= 0x64A) return true;
    if (c >= 0xE01 && c <= 0xE30) return true;
    if (c >= 0x10A0 && c <= 0x10FF) return true;
    if (c >= 0x1E00 && c <= 0x1FFF) return true;
    if (c >= 0x3041 && c <= 0x3096) return true;
    if (c >= 0x30A1 && c <= 0x30FA) return true;
    if (c >= 0x3400 && c <= 0x4DBF) return true;
    if (c >= 0x4E00 && c <= 0x9FFF) return true;
    if (c >= 0xAC00 && c <= 0xD7A3) return true;
    if (c >= 0xF900 && c <= 0xFAFF) return true;
    if (c >= 0xFF21 && c <= 0xFF3A) return true;
    if (c >= 0xFF41 && c <= 0xFF5A) return true;
    if (c >= 0x20000 && c <= 0x2FA1F) return true;
    return false;
}
static bool prop_match(int p, uint32_t c) {
    switch (p) {
    case PR_ANY: return true;
    case PR_ASCII: return c < 128;
    case PR_L:
    case PR_ALPHA: return is_letter(c);
    case PR_LU: return is_letter(c) && uc_lower(c) != c;
    case PR_LL: return is_letter(c) && uc_upper(c) != c;
    case PR_N:
    case PR_ND:
        return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9) || (c >= 0x966 && c <= 0x96F) ||
               (c >= 0xFF10 && c <= 0xFF19) || (p == PR_N && (c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE) ||
                                                               (c >= 0x2150 && c <= 0x218B) || (c >= 0x2460 && c <= 0x249B)));
    case PR_P:
        if (c < 128) return (c >= '!' && c <= '#') || (c >= '%' && c <= '*') || (c >= ',' && c <= '/') || c == ':' || c == ';' ||
                            c == '?' || c == '@' || (c >= '[' && c <= ']') || c == '_' || c == '{' || c == '}';
        return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF || (c >= 0x2010 && c <= 0x2027) ||
               (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) || (c >= 0x3008 && c <= 0x3011);
    case PR_S:
        if (c < 128) return c == '$' || c == '+' || c == '<' || c == '=' || c == '>' || c == '^' || c == '`' || c == '|' || c == '~';
        return (c >= 0xA2 && c <= 0xA9 && c != 0xA7) || c == 0xAC || (c >= 0xAE && c <= 0xB1) || c == 0xB4 || c == 0xD7 || c == 0xF7 ||
               (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x20A0 && c <= 0x20C0) || (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF);
    case PR_WS: return uc_is_space(c) || is_lt(c);
    case PR_LATIN: return (c < 128 && is_letter(c)) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x1E00 && c <= 0x1EFF) || c == 0xAA || c == 0xBA;
    case PR_GREEK: return (c >= 0x370 && c <= 0x3FF) || (c >= 0x1F00 && c <= 0x1FFF);
    case PR_CYRILLIC: return c >= 0x400 && c <= 0x52F;
    case PR_HAN: return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2FA1F) || (c >= 0xF900 && c <= 0xFAFF) || c == 0x3005 || c == 0x3007;
    case PR_HIRAGANA: return c >= 0x3041 && c <= 0x309F;
    case PR_KATAKANA: return (c >= 0x30A0 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF);
    case PR_ARABIC: return c >= 0x600 && c <= 0x6FF;
    case PR_HEBREW: return c >= 0x591 && c <= 0x5F4;
    case PR_HANGUL: return (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
    case PR_THAI: return c >= 0xE01 && c <= 0xE5B;
    case PR_EMOJI: return (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x1F000 && c <= 0x1F2FF) || c == 0x2B50 || c == 0x2B55 || c == 0x231A || c == 0x231B || c == 0xA9 || c == 0xAE || c == 0x203C || c == 0x2049 || c == 0x2122;
    case PR_M: return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x591 && c <= 0x5BD);
    case PR_C: return c < 0x20 || (c >= 0x7F && c <= 0x9F);
    case PR_Z: return c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
    default: return false;
    }
}

static bool class_has(RClass *cl, uint32_t c) {
    for (int i = 0; i < cl->n; i++) {
        CItem *it = &cl->items[i];
        bool m;
        if (it->kind == CI_RANGE)
            m = c >= it->lo && c <= it->hi;
        else
            m = prop_match((int)it->lo, c);
        if (it->neg) m = !m;
        if (m) return true;
    }
    return false;
}

/* canonicalize for case-insensitive comparison */
static uint32_t canon(uint32_t c, bool unicode) {
    if (unicode) {
        if (c == 0x17F) return 's';
        if (c == 0x212A) return 'k';
        if (c == 0x2126) return 0x3C9;
        uint32_t l = uc_lower(c);
        if (l == c) l = uc_lower(uc_upper(c));
        return l;
    }
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    uint32_t u = uc_upper(c);
    if (u < 128) return c;
    if (c == 0xDF) return c;
    return u;
}

/* ---------------- compiler ---------------- */
typedef struct RC {
    js_rt *rt;
    JSString *s;
    uint32_t i, n;
    Regex *re;
    bool u, ic;
    const char *err;
    int depth;
    int ngroups; /* groups seen so far (excluding 0) */
    int total_groups;
    JSString **gnames;
    int gncap;
    bool has_named;
    bool unresolved_named;
} RC;

static void emit(RC *c, int32_t v) {
    Regex *re = c->re;
    if (re->ncode == re->ccap) {
        re->ccap = re->ccap ? re->ccap * 2 : 64;
        re->code = (int32_t *)realloc(re->code, (size_t)re->ccap * 4);
    }
    re->code[re->ncode++] = v;
}
static int new_class(RC *c) {
    Regex *re = c->re;
    if (re->nclasses == re->clcap) {
        re->clcap = re->clcap ? re->clcap * 2 : 8;
        re->classes = (RClass *)realloc(re->classes, (size_t)re->clcap * sizeof(RClass));
    }
    memset(&re->classes[re->nclasses], 0, sizeof(RClass));
    return re->nclasses++;
}
static void cl_add(RClass *cl, int kind, uint32_t lo, uint32_t hi, bool neg) {
    if (cl->n == cl->cap) {
        cl->cap = cl->cap ? cl->cap * 2 : 8;
        cl->items = (CItem *)realloc(cl->items, (size_t)cl->cap * sizeof(CItem));
    }
    CItem *it = &cl->items[cl->n++];
    it->kind = (uint8_t)kind;
    it->lo = lo;
    it->hi = hi;
    it->neg = neg;
}
static void cl_add_escape(RClass *cl, int e, bool unicode_ic) {
    /* e: 'd','D','w','W','s','S' */
    bool neg = e == 'D' || e == 'W' || e == 'S';
    int lc = e | 0x20;
    if (!neg) {
        if (lc == 'd') cl_add(cl, CI_RANGE, '0', '9', false);
        else if (lc == 'w') {
            cl_add(cl, CI_RANGE, '0', '9', false);
            cl_add(cl, CI_RANGE, 'A', 'Z', false);
            cl_add(cl, CI_RANGE, 'a', 'z', false);
            cl_add(cl, CI_RANGE, '_', '_', false);
            if (unicode_ic) {
                cl_add(cl, CI_RANGE, 0x17F, 0x17F, false);
                cl_add(cl, CI_RANGE, 0x212A, 0x212A, false);
            }
        } else
            cl_add(cl, CI_PROP, PR_WS, 0, false);
        return;
    }
    /* complements as ranges */
    if (lc == 'd') {
        cl_add(cl, CI_RANGE, 0, '0' - 1, false);
        cl_add(cl, CI_RANGE, '9' + 1, 0x10FFFF, false);
    } else if (lc == 'w') {
        cl_add(cl, CI_RANGE, 0, '0' - 1, false);
        cl_add(cl, CI_RANGE, '9' + 1, 'A' - 1, false);
        cl_add(cl, CI_RANGE, 'Z' + 1, '_' - 1, false);
        cl_add(cl, CI_RANGE, '_' + 1, 'a' - 1, false);
        cl_add(cl, CI_RANGE, 'z' + 1, 0x10FFFF, false);
    } else
        cl_add(cl, CI_PROP, PR_WS, 0, true);
}

static uint32_t peekc(RC *c) { return c->i < c->n ? str_at(c->s, c->i) : 0xFFFFFFFFu; }
static uint32_t nextc(RC *c) {
    uint32_t ch = str_at(c->s, c->i++);
    if (c->u && ch >= 0xD800 && ch <= 0xDBFF && c->i < c->n) {
        uint32_t d = str_at(c->s, c->i);
        if (d >= 0xDC00 && d <= 0xDFFF) {
            c->i++;
            return 0x10000 + ((ch - 0xD800) << 10) + (d - 0xDC00);
        }
    }
    return ch;
}
static int hexd(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (int)((c | 0x20) - 'a' + 10);
    return -1;
}

static int parse_prop_name(RC *c) {
    /* after \p{ ... } */
    uint32_t start = c->i;
    while (c->i < c->n && str_at(c->s, c->i) != '}') c->i++;
    if (c->i >= c->n) {
        c->err = "Invalid property name";
        return -1;
    }
    char buf[64];
    uint32_t len = c->i - start;
    if (len >= sizeof buf) len = sizeof buf - 1;
    for (uint32_t k = 0; k < len; k++) buf[k] = (char)str_at(c->s, start + k);
    buf[len] = 0;
    c->i++;
    const char *v = buf;
    for (uint32_t k = 0; k < len; k++)
        if (buf[k] == '=') v = buf + k + 1;
    static const struct {
        const char *n;
        int p;
    } t[] = {{"L", PR_L}, {"Letter", PR_L}, {"Lu", PR_LU}, {"Uppercase_Letter", PR_LU}, {"Ll", PR_LL}, {"Lowercase_Letter", PR_LL},
             {"Uppercase", PR_LU}, {"Lowercase", PR_LL},
             {"N", PR_N}, {"Number", PR_N}, {"Nd", PR_ND}, {"Decimal_Number", PR_ND}, {"digit", PR_ND}, {"P", PR_P},
             {"Punctuation", PR_P}, {"S", PR_S}, {"Symbol", PR_S}, {"White_Space", PR_WS}, {"space", PR_WS}, {"ASCII", PR_ASCII},
             {"Any", PR_ANY}, {"Latin", PR_LATIN}, {"Latn", PR_LATIN}, {"Greek", PR_GREEK}, {"Grek", PR_GREEK},
             {"Cyrillic", PR_CYRILLIC}, {"Cyrl", PR_CYRILLIC}, {"Han", PR_HAN}, {"Hani", PR_HAN}, {"Hiragana", PR_HIRAGANA},
             {"Katakana", PR_KATAKANA}, {"Emoji", PR_EMOJI}, {"Emoji_Presentation", PR_EMOJI}, {"Extended_Pictographic", PR_EMOJI},
             {"Alphabetic", PR_ALPHA}, {"Alpha", PR_ALPHA}, {"Arabic", PR_ARABIC}, {"Hebrew", PR_HEBREW}, {"Hangul", PR_HANGUL},
             {"Thai", PR_THAI}, {"M", PR_M}, {"Mark", PR_M}, {"Mn", PR_M}, {"C", PR_C}, {"Cc", PR_C}, {"Control", PR_C},
             {"Z", PR_Z}, {"Zs", PR_Z}, {"Separator", PR_Z}};
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++)
        if (strcmp(t[k].n, v) == 0) return t[k].p;
    c->err = "Invalid property name";
    return -1;
}

/* parses an escape (after '\'); in class: returns char code or -2 (added class escape) via *cl */
static int64_t parse_escape(RC *c, RClass *cl, bool in_class) {
    if (c->i >= c->n) {
        c->err = "\\ at end of pattern";
        return -1;
    }
    uint32_t ch = nextc(c);
    switch (ch) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        if (cl) cl_add_escape(cl, (int)ch, c->u && c->ic);
        return -2;
    case 'p': case 'P':
        if (c->u) {
            if (peekc(c) != '{') {
                c->err = "Invalid property name";
                return -1;
            }
            c->i++;
            int p = parse_prop_name(c);
            if (p < 0) return -1;
            if (cl) cl_add(cl, CI_PROP, (uint32_t)p, 0, ch == 'P');
            return -2;
        }
        return ch;
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case 'f': return '\f';
    case 'b':
        if (in_class) return '\b';
        return ch; /* handled by caller */
    case '0':
        if (!(peekc(c) >= '0' && peekc(c) <= '9')) return 0;
        if (c->u) {
            c->err = "Invalid decimal escape";
            return -1;
        }
        /* legacy octal */
        {
            uint32_t v = 0;
            c->i--;
            int k = 0;
            while (k < 3 && peekc(c) >= '0' && peekc(c) <= '7') {
                uint32_t nv = v * 8 + (peekc(c) - '0');
                if (nv > 255) break;
                v = nv;
                c->i++;
                k++;
            }
            return v;
        }
    case 'c': {
        uint32_t d = peekc(c);
        if ((d | 0x20) >= 'a' && (d | 0x20) <= 'z') {
            c->i++;
            return d & 31;
        }
        if (c->u) {
            c->err = "Invalid unicode escape";
            return -1;
        }
        c->i--;
        return '\\';
    }
    case 'x': {
        if (c->i + 2 <= c->n && hexd(str_at(c->s, c->i)) >= 0 && hexd(str_at(c->s, c->i + 1)) >= 0) {
            uint32_t v = (uint32_t)(hexd(str_at(c->s, c->i)) * 16 + hexd(str_at(c->s, c->i + 1)));
            c->i += 2;
            return v;
        }
        if (c->u) {
            c->err = "Invalid escape";
            return -1;
        }
        return 'x';
    }
    case 'u': {
        if (c->u && peekc(c) == '{') {
            uint32_t v = 0;
            c->i++;
            int nd = 0;
            while (c->i < c->n && hexd(str_at(c->s, c->i)) >= 0) {
                v = v * 16 + (uint32_t)hexd(str_at(c->s, c->i));
                c->i++;
                nd++;
                if (v > 0x10FFFF) {
                    c->err = "Invalid Unicode escape";
                    return -1;
                }
            }
            if (peekc(c) != '}' || !nd) {
                c->err = "Invalid Unicode escape";
                return -1;
            }
            c->i++;
            return v;
        }
        if (c->i + 4 <= c->n) {
            uint32_t v = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                int h = hexd(str_at(c->s, c->i + (uint32_t)k));
                if (h < 0) ok = false;
                v = v * 16 + (uint32_t)(h < 0 ? 0 : h);
            }
            if (ok) {
                c->i += 4;
                if (c->u && v >= 0xD800 && v <= 0xDBFF && c->i + 6 <= c->n && str_at(c->s, c->i) == '\\' && str_at(c->s, c->i + 1) == 'u') {
                    uint32_t w = 0;
                    bool ok2 = true;
                    for (int k = 0; k < 4; k++) {
                        int h = hexd(str_at(c->s, c->i + 2 + (uint32_t)k));
                        if (h < 0) ok2 = false;
                        w = w * 16 + (uint32_t)(h < 0 ? 0 : h);
                    }
                    if (ok2 && w >= 0xDC00 && w <= 0xDFFF) {
                        c->i += 6;
                        return 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
                    }
                }
                return v;
            }
        }
        if (c->u) {
            c->err = "Invalid Unicode escape";
            return -1;
        }
        return 'u';
    }
    default:
        if (ch >= '1' && ch <= '9' && in_class) {
            if (c->u) {
                c->err = "Invalid class escape";
                return -1;
            }
            if (ch <= '7') {
                uint32_t v = ch - '0';
                if (peekc(c) >= '0' && peekc(c) <= '7') v = v * 8 + (nextc(c) - '0');
                return v;
            }
            return ch;
        }
        if (c->u && !(ch == '^' || ch == '$' || ch == '\\' || ch == '.' || ch == '*' || ch == '+' || ch == '?' || ch == '(' ||
                      ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '|' || ch == '/' || (ch == '-' && in_class))) {
            c->err = "Invalid escape";
            return -1;
        }
        return ch;
    }
}

static void emit_char(RC *c, uint32_t ch) {
    if (c->ic) {
        uint32_t cc = canon(ch, c->u);
        /* chars with no case partner can match exactly */
        if (cc == ch && uc_lower(ch) == ch && uc_upper(ch) == ch) {
            emit(c, R_CHAR);
            emit(c, (int32_t)ch);
        } else {
            emit(c, R_CHARI);
            emit(c, (int32_t)cc);
        }
    } else {
        emit(c, R_CHAR);
        emit(c, (int32_t)ch);
    }
}

static int parse_class(RC *c) {
    /* after '[' */
    bool neg = false;
    if (peekc(c) == '^') {
        neg = true;
        c->i++;
    }
    int ci = new_class(c);
    for (;;) {
        if (c->i >= c->n) {
            c->err = "Unterminated character class";
            return -1;
        }
        uint32_t ch = peekc(c);
        if (ch == ']') {
            c->i++;
            break;
        }
        int64_t lo;
        c->i++;
        if (ch == '\\') {
            lo = parse_escape(c, &c->re->classes[ci], true);
            if (lo == -1) return -1;
        } else {
            c->i--;
            lo = nextc(c);
        }
        if (lo == -2) {
            if (peekc(c) == '-' && c->i + 1 < c->n && str_at(c->s, c->i + 1) != ']' && c->u) {
                c->err = "Invalid character class";
                return -1;
            }
            continue;
        }
        if (peekc(c) == '-' && c->i + 1 < c->n && str_at(c->s, c->i + 1) != ']') {
            c->i++;
            int64_t hi;
            uint32_t ch2 = peekc(c);
            c->i++;
            if (ch2 == '\\') {
                RClass tmp = {0, 0, 0};
                hi = parse_escape(c, &tmp, true);
                free(tmp.items);
                if (hi == -1) return -1;
                if (hi == -2) {
                    if (c->u) {
                        c->err = "Invalid character class";
                        return -1;
                    }
                    /* treat as literal '-' plus class escape: re-parse not supported, approximate */
                    cl_add(&c->re->classes[ci], CI_RANGE, (uint32_t)lo, (uint32_t)lo, false);
                    cl_add(&c->re->classes[ci], CI_RANGE, '-', '-', false);
                    continue;
                }
            } else {
                c->i--;
                hi = nextc(c);
            }
            if (hi < lo) {
                c->err = "Range out of order in character class";
                return -1;
            }
            cl_add(&c->re->classes[ci], CI_RANGE, (uint32_t)lo, (uint32_t)hi, false);
        } else
            cl_add(&c->re->classes[ci], CI_RANGE, (uint32_t)lo, (uint32_t)lo, false);
    }
    emit(c, neg ? R_NCLASS : R_CLASS);
    emit(c, ci);
    return 0;
}

static int parse_disj(RC *c);
static void relocate(int32_t *code, int from, int to, int delta);

/* returns start pc of the atom just emitted, or -1 if none / error */
static int parse_atom(RC *c, bool *quantifiable, int *gfirst, int *glast) {
    uint32_t ch = peekc(c);
    int start = c->re->ncode;
    *quantifiable = true;
    *gfirst = c->ngroups + 1;
    *glast = c->ngroups;
    switch (ch) {
    case '^':
        c->i++;
        emit(c, R_BOL);
        *quantifiable = false;
        return start;
    case '$':
        c->i++;
        emit(c, R_EOL);
        *quantifiable = false;
        return start;
    case '.':
        c->i++;
        emit(c, (c->re->flags & RF_S) ? R_ANYALL : R_ANY);
        emit(c, 0);
        return start;
    case '[':
        c->i++;
        if (parse_class(c) < 0) return -1;
        return start;
    case '(': {
        c->i++;
        if (peekc(c) == '?') {
            c->i++;
            uint32_t k = peekc(c);
            if (k == ':') {
                c->i++;
                if (parse_disj(c) < 0) return -1;
                if (peekc(c) != ')') {
                    c->err = "Unterminated group";
                    return -1;
                }
                c->i++;
                *glast = c->ngroups;
                return start;
            }
            bool behind = false;
            if (k == '<' && c->i + 1 < c->n && (str_at(c->s, c->i + 1) == '=' || str_at(c->s, c->i + 1) == '!')) {
                behind = true;
                c->i++;
                k = peekc(c);
            }
            if (k == '=' || k == '!') {
                c->i++;
                emit(c, R_LOOK);
                int at = c->re->ncode;
                emit(c, 0); /* flags */
                emit(c, 0); /* length of body */
                int body = c->re->ncode;
                if (parse_disj(c) < 0) return -1;
                if (peekc(c) != ')') {
                    c->err = "Unterminated group";
                    return -1;
                }
                c->i++;
                if (behind) {
                    emit(c, R_ASSERT_POS);
                }
                emit(c, R_MATCH);
                c->re->code[at] = (k == '!' ? 1 : 0) | (behind ? 2 : 0);
                c->re->code[at + 1] = c->re->ncode - body;
                *quantifiable = !c->u && !behind;
                *glast = c->ngroups;
                return start;
            }
            if (k == '<') {
                c->i++;
                uint32_t ns = c->i;
                while (c->i < c->n && str_at(c->s, c->i) != '>') c->i++;
                if (c->i >= c->n || c->i == ns) {
                    c->err = "Invalid capture group name";
                    return -1;
                }
                JSString *name = str_sub(c->rt, c->s, ns, c->i);
                c->i++;
                int g = ++c->ngroups;
                if (g >= c->gncap) {
                    int nc = c->gncap ? c->gncap * 2 : 16;
                    while (nc <= g) nc *= 2;
                    c->gnames = (JSString **)realloc(c->gnames, (size_t)nc * sizeof(JSString *));
                    for (int q = c->gncap; q < nc; q++) c->gnames[q] = 0;
                    c->gncap = nc;
                }
                for (int q = 1; q < g; q++)
                    if (c->gnames[q] && str_eq(c->rt, c->gnames[q], name)) {
                        c->err = "Duplicate capture group name";
                        return -1;
                    }
                c->gnames[g] = name;
                c->has_named = true;
                emit(c, R_SAVE);
                emit(c, g * 2);
                if (parse_disj(c) < 0) return -1;
                if (peekc(c) != ')') {
                    c->err = "Unterminated group";
                    return -1;
                }
                c->i++;
                emit(c, R_SAVE);
                emit(c, g * 2 + 1);
                *gfirst = g;
                *glast = c->ngroups;
                return start;
            }
            c->err = "Invalid group";
            return -1;
        }
        int g = ++c->ngroups;
        emit(c, R_SAVE);
        emit(c, g * 2);
        if (parse_disj(c) < 0) return -1;
        if (peekc(c) != ')') {
            c->err = "Unterminated group";
            return -1;
        }
        c->i++;
        emit(c, R_SAVE);
        emit(c, g * 2 + 1);
        *gfirst = g;
        *glast = c->ngroups;
        return start;
    }
    case ')':
    case '|': return -1;
    case '*':
    case '+':
    case '?':
        c->err = "Nothing to repeat";
        return -1;
    case '{':
        if (c->u) {
            c->err = "Nothing to repeat";
            return -1;
        }
        {
            /* a valid quantifier here is an error; otherwise literal */
            uint32_t j = c->i + 1;
            bool digits = false;
            while (j < c->n && str_at(c->s, j) >= '0' && str_at(c->s, j) <= '9') {
                j++;
                digits = true;
            }
            if (digits && j < c->n && (str_at(c->s, j) == '}' || str_at(c->s, j) == ',')) {
                c->err = "Nothing to repeat";
                return -1;
            }
        }
        c->i++;
        emit_char(c, '{');
        return start;
    case ']':
    case '}':
        if (c->u) {
            c->err = "Lone quantifier brackets";
            return -1;
        }
        c->i++;
        emit_char(c, ch);
        return start;
    case '\\': {
        c->i++;
        if (c->i >= c->n) {
            c->err = "\\ at end of pattern";
            return -1;
        }
        uint32_t e = peekc(c);
        if (e == 'b' || e == 'B') {
            c->i++;
            emit(c, e == 'b' ? R_WORDB : R_NWORDB);
            *quantifiable = false;
            return start;
        }
        if (e >= '1' && e <= '9') {
            uint32_t save = c->i;
            int v = 0;
            while (c->i < c->n && peekc(c) >= '0' && peekc(c) <= '9') {
                v = v * 10 + (int)(peekc(c) - '0');
                c->i++;
                if (v > 100000) break;
            }
            if (v <= c->total_groups) {
                emit(c, c->ic ? R_BACKREFI : R_BACKREF);
                emit(c, v);
                return start;
            }
            if (c->u) {
                c->err = "Invalid escape";
                return -1;
            }
            c->i = save;
            /* legacy octal / identity escape */
            if (e >= '8') {
                c->i++;
                emit_char(c, e);
                return start;
            }
            uint32_t ov = 0;
            int k = 0;
            while (k < 3 && peekc(c) >= '0' && peekc(c) <= '7') {
                uint32_t nv = ov * 8 + (peekc(c) - '0');
                if (nv > 255) break;
                ov = nv;
                c->i++;
                k++;
            }
            emit_char(c, ov);
            return start;
        }
        if (e == 'k' && (c->u || c->has_named || c->unresolved_named)) {
            c->i++;
            if (peekc(c) != '<') {
                c->err = "Invalid named reference";
                return -1;
            }
            c->i++;
            uint32_t ns = c->i;
            while (c->i < c->n && str_at(c->s, c->i) != '>') c->i++;
            if (c->i >= c->n) {
                c->err = "Invalid named reference";
                return -1;
            }
            JSString *name = str_sub(c->rt, c->s, ns, c->i);
            c->i++;
            /* resolved after parsing: emit placeholder with name index */
            emit(c, R_NBACKREF);
            emit(c, 0);
            /* store name temporarily by scanning the full pattern for the group */
            int found = 0, gi = 0;
            {
                /* scan pattern for (?<name> occurrences in order */
                for (uint32_t q = 0; q + 3 < c->n; q++) {
                    if (str_at(c->s, q) == '\\') {
                        q++;
                        continue;
                    }
                    if (str_at(c->s, q) == '(') {
                        if (q + 2 < c->n && str_at(c->s, q + 1) == '?') {
                            if (str_at(c->s, q + 2) == '<' && q + 3 < c->n && str_at(c->s, q + 3) != '=' && str_at(c->s, q + 3) != '!') {
                                gi++;
                                uint32_t e2 = q + 3;
                                while (e2 < c->n && str_at(c->s, e2) != '>') e2++;
                                if (e2 - (q + 3) == name->len) {
                                    bool same = true;
                                    for (uint32_t z = 0; z < name->len; z++)
                                        if (str_at(c->s, q + 3 + z) != str_at(name, z)) same = false;
                                    if (same) {
                                        found = gi;
                                        break;
                                    }
                                }
                            }
                        } else
                            gi++;
                    }
                }
            }
            if (!found) {
                c->err = "Invalid named capture referenced";
                return -1;
            }
            c->re->code[c->re->ncode - 2] = c->ic ? R_BACKREFI : R_BACKREF;
            c->re->code[c->re->ncode - 1] = found;
            return start;
        }
        RClass tmpc = {0, 0, 0};
        int64_t v = parse_escape(c, &tmpc, false);
        if (v == -1) {
            free(tmpc.items);
            return -1;
        }
        if (v == -2) {
            int ci = new_class(c);
            c->re->classes[ci] = tmpc;
            emit(c, R_CLASS);
            emit(c, ci);
            return start;
        }
        free(tmpc.items);
        emit_char(c, (uint32_t)v);
        return start;
    }
    default:
        emit_char(c, nextc(c));
        return start;
    }
}

static bool single_char_op(Regex *re, int pc, int end) {
    if (end - pc != 2) return false;
    int op = re->code[pc];
    return op == R_CHAR || op == R_CHARI || op == R_CLASS || op == R_NCLASS || op == R_ANY || op == R_ANYALL;
}

static int parse_term(RC *c) {
    bool q;
    int gf, gl;
    if (++c->depth > 200 || check_stack(c->rt)) {
        c->err = "Regular expression too large";
        return -1;
    }
    int start = parse_atom(c, &q, &gf, &gl);
    c->depth--;
    if (start < 0) return c->err ? -1 : 0;
    uint32_t ch = peekc(c);
    uint32_t min, max;
    if (ch == '*') {
        min = 0;
        max = 0xFFFFFFFFu;
        c->i++;
    } else if (ch == '+') {
        min = 1;
        max = 0xFFFFFFFFu;
        c->i++;
    } else if (ch == '?') {
        min = 0;
        max = 1;
        c->i++;
    } else if (ch == '{') {
        uint32_t j = c->i + 1;
        uint64_t a = 0, b;
        bool da = false;
        while (j < c->n && str_at(c->s, j) >= '0' && str_at(c->s, j) <= '9') {
            a = a * 10 + (str_at(c->s, j) - '0');
            if (a > 0xFFFFFFFFull) a = 0xFFFFFFFFull;
            j++;
            da = true;
        }
        if (!da) {
            if (c->u) {
                c->err = "Incomplete quantifier";
                return -1;
            }
            return 0;
        }
        b = a;
        if (j < c->n && str_at(c->s, j) == ',') {
            j++;
            if (j < c->n && str_at(c->s, j) == '}')
                b = 0xFFFFFFFFull;
            else {
                b = 0;
                bool db = false;
                while (j < c->n && str_at(c->s, j) >= '0' && str_at(c->s, j) <= '9') {
                    b = b * 10 + (str_at(c->s, j) - '0');
                    if (b > 0xFFFFFFFFull) b = 0xFFFFFFFFull;
                    j++;
                    db = true;
                }
                if (!db) {
                    if (c->u) {
                        c->err = "Incomplete quantifier";
                        return -1;
                    }
                    return 0;
                }
            }
        }
        if (j >= c->n || str_at(c->s, j) != '}') {
            if (c->u) {
                c->err = "Incomplete quantifier";
                return -1;
            }
            return 0;
        }
        if (b < a) {
            c->err = "numbers out of order in {} quantifier";
            return -1;
        }
        c->i = j + 1;
        min = (uint32_t)a;
        max = (uint32_t)b;
    } else
        return 0;
    if (!q) {
        c->err = "Nothing to repeat";
        return -1;
    }
    bool greedy = true;
    if (peekc(c) == '?') {
        greedy = false;
        c->i++;
    }
    Regex *re = c->re;
    int end = re->ncode;
    int len = end - start;
    int32_t *body = (int32_t *)malloc((size_t)(len ? len : 1) * 4);
    memcpy(body, re->code + start, (size_t)len * 4);
    re->ncode = start;
    if (single_char_op(re, start, end) && !(c->u && (body[0] == R_ANY || body[0] == R_ANYALL || body[0] == R_CLASS || body[0] == R_NCLASS))) {
        emit(c, greedy ? R_GREEDY1 : R_LAZY1);
        emit(c, (int32_t)min);
        emit(c, (int32_t)max);
        emit(c, body[0]);
        emit(c, body[1]);
        free(body);
        return 0;
    }
    if (max == 0) {
        free(body);
        return 0;
    }
    int r = re->nregs;
    re->nregs += 2; /* counter, position */
    emit(c, R_REP_START);
    emit(c, r);
    int test = re->ncode;
    emit(c, R_REP_TEST);
    emit(c, r);
    emit(c, (int32_t)min);
    emit(c, (int32_t)max);
    emit(c, greedy);
    int exit_slot = re->ncode;
    emit(c, 0);
    emit(c, R_REP_POS);
    emit(c, r);
    if (gl >= gf) {
        emit(c, R_RESET);
        emit(c, gf * 2);
        emit(c, gl * 2 + 1);
    }
    int bstart = re->ncode;
    for (int k = 0; k < len; k++) emit(c, body[k]);
    free(body);
    relocate(re->code, bstart, bstart + len, bstart - start);
    emit(c, R_REP_END);
    emit(c, r);
    emit(c, (int32_t)min);
    emit(c, test);
    re->code[exit_slot] = re->ncode;
    return 0;
}

static int parse_alt(RC *c) {
    while (c->i < c->n && peekc(c) != '|' && peekc(c) != ')') {
        if (parse_term(c) < 0) return -1;
        if (c->err) return -1;
    }
    return 0;
}

static int parse_disj(RC *c) {
    if (++c->depth > 200 || check_stack(c->rt)) {
        c->err = "Regular expression too large";
        return -1;
    }
    int start = c->re->ncode;
    if (parse_alt(c) < 0) return -1;
    if (peekc(c) == '|') {
        /* restructure: SPLIT a, b ; a: alt1 ; JMP end ; b: rest */
        int len = c->re->ncode - start;
        int32_t *alt = (int32_t *)malloc((size_t)(len ? len : 1) * 4);
        memcpy(alt, c->re->code + start, (size_t)len * 4);
        c->re->ncode = start;
        emit(c, R_SPLIT);
        int sa = c->re->ncode;
        emit(c, 0);
        emit(c, 0);
        int a = c->re->ncode;
        for (int k = 0; k < len; k++) emit(c, alt[k]);
        free(alt);
        relocate(c->re->code, a, a + len, a - start);
        emit(c, R_JMP);
        int jslot = c->re->ncode;
        emit(c, 0);
        int b = c->re->ncode;
        c->i++;
        if (parse_disj(c) < 0) return -1;
        c->re->code[sa] = a;
        c->re->code[sa + 1] = b;
        c->re->code[jslot] = c->re->ncode;
    }
    c->depth--;
    return 0;
}

/* jump targets are absolute; relocate code copied in quantifier bodies */
static void relocate(int32_t *code, int from, int to, int delta) {
    for (int pc = from; pc < to;) {
        int op = code[pc];
        switch (op) {
        case R_CHAR: case R_CHARI: case R_CLASS: case R_NCLASS: case R_SAVE: case R_BACKREF: case R_BACKREFI: case R_REP_START:
        case R_REP_POS: case R_NBACKREF: case R_ANY: case R_ANYALL: pc += 2; break;
        case R_BOL: case R_EOL: case R_WORDB: case R_NWORDB: case R_MATCH: case R_ASSERT_POS: pc += 1; break;
        case R_SPLIT: code[pc + 1] += delta; code[pc + 2] += delta; pc += 3; break;
        case R_JMP: code[pc + 1] += delta; pc += 2; break;
        case R_RESET: pc += 3; break;
        case R_LOOK: pc += 3; break;
        case R_REP_TEST: code[pc + 5] += delta; pc += 6; break;
        case R_REP_END: code[pc + 3] += delta; pc += 4; break;
        case R_GREEDY1: case R_LAZY1: pc += 5; break;
        default: pc++; break;
        }
    }
}

/* first-character analysis: adds the characters a match starting at pc can begin with to re->fset;
 * false when unknown (the match may be empty or start with an assertion) */
static void fs_add(Regex *re, uint32_t c) {
    if (c < 256)
        re->fset[c >> 3] |= (uint8_t)(1u << (c & 7));
    else
        re->fs_wide = true;
}
static bool fs_single(Regex *re, int op, int32_t arg) {
    bool ic = (re->flags & RF_I) != 0, u = (re->flags & (RF_U | RF_V)) != 0;
    switch (op) {
    case R_CHAR: fs_add(re, (uint32_t)arg); return true;
    case R_CHARI:
        for (uint32_t c = 0; c < 256; c++)
            if (canon(c, u) == (uint32_t)arg) fs_add(re, c);
        re->fs_wide = true;
        return true;
    case R_ANY:
        for (uint32_t c = 0; c < 256; c++)
            if (c != '\n' && c != '\r') fs_add(re, c);
        re->fs_wide = true;
        return true;
    case R_CLASS:
    case R_NCLASS: {
        RClass *cl = &re->classes[arg];
        for (uint32_t c = 0; c < 256; c++) {
            bool in = class_has(cl, c);
            if (!in && ic) {
                uint32_t l = uc_lower(c), up = uc_upper(c);
                in = (l != c && class_has(cl, l)) || (up != c && class_has(cl, up));
                if (!in && u) {
                    uint32_t cc = canon(c, true);
                    in = cc != c && class_has(cl, cc);
                }
            }
            if (op == R_NCLASS) in = !in;
            if (in) fs_add(re, c);
        }
        re->fs_wide = true;
        return true;
    }
    default: return false;
    }
}
static bool first_set(Regex *re, int pc, int depth) {
    int32_t *code = re->code;
    if (depth > 40) return false;
    for (;;) {
        switch (code[pc]) {
        case R_CHAR: case R_CHARI: case R_ANY: case R_CLASS: case R_NCLASS: return fs_single(re, code[pc], code[pc + 1]);
        case R_SAVE: pc += 2; continue;
        case R_RESET: pc += 3; continue;
        case R_REP_START: case R_REP_POS: pc += 2; continue;
        case R_JMP: pc = code[pc + 1]; continue;
        case R_SPLIT: return first_set(re, code[pc + 1], depth + 1) && first_set(re, code[pc + 2], depth + 1);
        case R_REP_TEST:
            if (code[pc + 2] >= 1) {
                pc += 6;
                continue;
            }
            return first_set(re, pc + 6, depth + 1) && first_set(re, code[pc + 5], depth + 1);
        case R_GREEDY1: case R_LAZY1:
            if (!fs_single(re, code[pc + 3], code[pc + 4])) return false;
            if (code[pc + 1] >= 1) return true;
            pc += 5;
            continue;
        default: return false;
        }
    }
}

Regex *re_compile(js_rt *rt, JSString *src, uint32_t flags, JSString **err) {
    str_flat(rt, src);
    RC c;
    memset(&c, 0, sizeof c);
    c.rt = rt;
    c.s = src;
    c.n = src->len;
    c.u = (flags & (RF_U | RF_V)) != 0;
    c.ic = (flags & RF_I) != 0;
    Regex *re = (Regex *)calloc(1, sizeof(Regex));
    re->flags = flags;
    re->refs = 1;
    c.re = re;
    /* count groups first (for backreference decisions) */
    {
        bool in_cls = false;
        for (uint32_t i = 0; i < c.n; i++) {
            uint16_t ch = str_at(src, i);
            if (ch == '\\') {
                i++;
                continue;
            }
            if (in_cls) {
                if (ch == ']') in_cls = false;
                continue;
            }
            if (ch == '[') in_cls = true;
            else if (ch == '(') {
                if (i + 1 < c.n && str_at(src, i + 1) == '?') {
                    if (i + 2 < c.n && str_at(src, i + 2) == '<' && i + 3 < c.n && str_at(src, i + 3) != '=' && str_at(src, i + 3) != '!') {
                        c.total_groups++;
                        c.unresolved_named = true;
                    }
                } else
                    c.total_groups++;
            }
        }
    }
    emit(&c, R_SAVE);
    emit(&c, 0);
    int rc = parse_disj(&c);
    if (rc == 0 && c.i < c.n) {
        c.err = peekc(&c) == ')' ? "Unmatched ')'" : "Unexpected character";
        rc = -1;
    }
    if (rc < 0 || c.err) {
        if (err) *err = str_cstr(rt, c.err ? c.err : "Invalid regular expression");
        free(c.gnames);
        re_free(re);
        return 0;
    }
    emit(&c, R_SAVE);
    emit(&c, 1);
    emit(&c, R_MATCH);
    re->fs_valid = first_set(re, 0, 0);
    re->ncaps = c.ngroups + 1;
    if (c.has_named) {
        re->names = (JSString **)calloc((size_t)re->ncaps, sizeof(JSString *));
        for (int i = 1; i < re->ncaps && i < c.gncap; i++) {
            /* compiled regexes are shared through a cache that the GC does not see: group names are
             * atomized and kept for the lifetime of the runtime */
            JSString *nm = c.gnames[i];
            if (nm) {
                nm = vstr(atomize(rt, nm));
                nm->h.flags |= STR_PERM;
            }
            re->names[i] = nm;
        }
    }
    free(c.gnames);
    return re;
}

/* ---------------- matcher ---------------- */
enum { BT_BRANCH, BT_CAP, BT_REG, BT_GREEDY, BT_LAZY, BT_BARRIER };
typedef struct BT {
    int32_t type, pc;
    int32_t a, b;
} BT;

typedef struct RM {
    js_rt *rt;
    Regex *re;
    JSString *s;
    uint32_t n;
    int32_t *caps;
    int32_t *regs;
    BT *st;
    int sp, cap;
    bool u, ic, multi;
    uint64_t steps;
    int error;
    int depth;
} RM;

static bool bt_push(RM *m, int type, int pc, int32_t a, int32_t b) {
    if (m->sp == m->cap) {
        int nc = m->cap ? m->cap * 2 : 256;
        if (nc > (1 << 24)) {
            m->error = 1;
            return false;
        }
        BT *n = (BT *)realloc(m->st, (size_t)nc * sizeof(BT));
        if (!n) {
            m->error = 1;
            return false;
        }
        m->st = n;
        m->cap = nc;
    }
    BT *b2 = &m->st[m->sp++];
    b2->type = type;
    b2->pc = pc;
    b2->a = a;
    b2->b = b;
    return true;
}

static inline uint32_t char_at(RM *m, uint32_t pos, uint32_t *w) {
    uint32_t c = str_at(m->s, pos);
    *w = 1;
    if (m->u && c >= 0xD800 && c <= 0xDBFF && pos + 1 < m->n) {
        uint32_t d = str_at(m->s, pos + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) {
            *w = 2;
            return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        }
    }
    return c;
}
static inline uint32_t char_before(RM *m, uint32_t pos, uint32_t *w) {
    uint32_t c = str_at(m->s, pos - 1);
    *w = 1;
    if (m->u && c >= 0xDC00 && c <= 0xDFFF && pos >= 2) {
        uint32_t h = str_at(m->s, pos - 2);
        if (h >= 0xD800 && h <= 0xDBFF) {
            *w = 2;
            return 0x10000 + ((h - 0xD800) << 10) + (c - 0xDC00);
        }
    }
    return c;
}

/* match one single-char op at pos; returns width or 0 */
static uint32_t match1(RM *m, int op, int32_t arg, uint32_t pos) {
    if (pos >= m->n) return 0;
    uint32_t w;
    uint32_t c = char_at(m, pos, &w);
    switch (op) {
    case R_CHAR: return c == (uint32_t)arg ? w : 0;
    case R_CHARI: return canon(c, m->u) == (uint32_t)arg ? w : 0;
    case R_ANY: return is_lt(c) ? 0 : w;
    case R_ANYALL: return w;
    case R_CLASS:
    case R_NCLASS: {
        RClass *cl = &m->re->classes[arg];
        bool in = class_has(cl, c);
        if (!in && m->ic) {
            uint32_t l = uc_lower(c), u2 = uc_upper(c);
            in = (l != c && class_has(cl, l)) || (u2 != c && class_has(cl, u2));
            if (!in && m->u) {
                uint32_t cc = canon(c, true);
                in = cc != c && class_has(cl, cc);
            }
        }
        if (op == R_NCLASS) in = !in;
        return in ? w : 0;
    }
    default: return 0;
    }
}

static bool word_at(RM *m, int64_t pos) {
    if (pos < 0 || pos >= (int64_t)m->n) return false;
    uint32_t c = str_at(m->s, (uint32_t)pos);
    if (is_word(c)) return true;
    if (m->u && m->ic && (c == 0x17F || c == 0x212A)) return true;
    return false;
}

static int run_re(RM *m, int pc, uint32_t pos, uint32_t *endpos, int32_t target);

static int do_look(RM *m, int pc, uint32_t pos) {
    int32_t fl = m->re->code[pc + 1];
    int body = pc + 3;
    bool neg = fl & 1, behind = (fl & 2) != 0;
    if (++m->depth > 100) {
        m->error = 1;
        m->depth--;
        return -1;
    }
    int base = m->sp;
    bool ok = false;
    if (!behind) {
        uint32_t e;
        int r = run_re(m, body, pos, &e, -1);
        if (r < 0) {
            m->depth--;
            return -1;
        }
        ok = r == 1;
    } else {
        for (int64_t s = pos; s >= 0; s--) {
            uint32_t e;
            int r = run_re(m, body, (uint32_t)s, &e, (int32_t)pos);
            if (r < 0) {
                m->depth--;
                return -1;
            }
            if (r == 1) {
                ok = true;
                break;
            }
            m->sp = base;
        }
    }
    m->depth--;
    if (ok && !neg) {
        /* keep capture undo records, drop branch points of the sub-match */
        int k = base;
        for (int i = base; i < m->sp; i++)
            if (m->st[i].type == BT_CAP || m->st[i].type == BT_REG) m->st[k++] = m->st[i];
        m->sp = k;
        return 1;
    }
    /* restore captures changed inside a failed or negative lookaround */
    while (m->sp > base) {
        BT *b = &m->st[--m->sp];
        if (b->type == BT_CAP) m->caps[b->a] = b->b;
        else if (b->type == BT_REG) m->regs[b->a] = b->b;
    }
    return ok == !neg ? 1 : 0;
}

/* run from pc at pos; returns 1 match (endpos set), 0 fail, -1 error. Backtrack entries above
 * the entry stack pointer belong to this run. target >= 0: ASSERT_POS requires pos == target */
static int run_re(RM *m, int pc, uint32_t pos, uint32_t *endpos, int32_t target) {
    int32_t *code = m->re->code;
    int base = m->sp;
    for (;;) {
        if (UNLIKELY((++m->steps & 0xFFFF) == 0)) {
            js_rt *rt = m->rt;
            if (rt->host.interrupt && rt->host.interrupt(rt->host.ctx)) {
                m->error = 2;
                return -1;
            }
        }
        int op = code[pc];
        switch (op) {
        case R_CHAR:
            if (pos < m->n) {
                uint32_t c = str_at(m->s, pos);
                if (c == (uint32_t)code[pc + 1]) {
                    pos++;
                    pc += 2;
                    continue;
                }
                if (m->u && code[pc + 1] > 0xFFFF) {
                    uint32_t w;
                    if (char_at(m, pos, &w) == (uint32_t)code[pc + 1]) {
                        pos += w;
                        pc += 2;
                        continue;
                    }
                }
            }
            goto fail;
        case R_CHARI:
        case R_ANY:
        case R_ANYALL:
        case R_CLASS:
        case R_NCLASS: {
            uint32_t w = match1(m, op, code[pc + 1], pos);
            if (!w) goto fail;
            pos += w;
            pc += 2;
            continue;
        }
        case R_BOL:
            if (pos == 0 || (m->multi && is_lt(str_at(m->s, pos - 1)))) {
                pc++;
                continue;
            }
            goto fail;
        case R_EOL:
            if (pos == m->n || (m->multi && is_lt(str_at(m->s, pos)))) {
                pc++;
                continue;
            }
            goto fail;
        case R_WORDB:
        case R_NWORDB: {
            bool a = word_at(m, (int64_t)pos - 1), b = word_at(m, pos);
            if ((a != b) == (op == R_WORDB)) {
                pc++;
                continue;
            }
            goto fail;
        }
        case R_SPLIT:
            if (!bt_push(m, BT_BRANCH, code[pc + 2], (int32_t)pos, 0)) return -1;
            pc = code[pc + 1];
            continue;
        case R_JMP: pc = code[pc + 1]; continue;
        case R_SAVE: {
            int i = code[pc + 1];
            if (!bt_push(m, BT_CAP, 0, i, m->caps[i])) return -1;
            m->caps[i] = (int32_t)pos;
            pc += 2;
            continue;
        }
        case R_RESET:
            for (int i = code[pc + 1]; i <= code[pc + 2]; i++) {
                if (m->caps[i] != -1) {
                    if (!bt_push(m, BT_CAP, 0, i, m->caps[i])) return -1;
                    m->caps[i] = -1;
                }
            }
            pc += 3;
            continue;
        case R_BACKREF:
        case R_BACKREFI: {
            int g = code[pc + 1];
            int32_t s = m->caps[g * 2], e = m->caps[g * 2 + 1];
            if (s < 0 || e < 0) {
                pc += 2;
                continue;
            }
            uint32_t len = (uint32_t)(e - s);
            if (pos + len > m->n) goto fail;
            bool ok = true;
            for (uint32_t k = 0; k < len; k++) {
                uint32_t a = str_at(m->s, (uint32_t)s + k), b = str_at(m->s, pos + k);
                if (a != b && !(op == R_BACKREFI && canon(a, m->u) == canon(b, m->u))) {
                    ok = false;
                    break;
                }
            }
            if (!ok) goto fail;
            pos += len;
            pc += 2;
            continue;
        }
        case R_LOOK: {
            int r = do_look(m, pc, pos);
            if (r < 0) return -1;
            if (!r) goto fail;
            pc += 3 + code[pc + 2];
            continue;
        }
        case R_ASSERT_POS:
            if (target >= 0 && pos != (uint32_t)target) goto fail;
            pc++;
            continue;
        case R_MATCH:
            *endpos = pos;
            return 1;
        case R_REP_START: {
            int r = code[pc + 1];
            if (!bt_push(m, BT_REG, 0, r, m->regs[r])) return -1;
            m->regs[r] = 0;
            pc += 2;
            continue;
        }
        case R_REP_TEST: {
            int r = code[pc + 1];
            uint32_t cnt = (uint32_t)m->regs[r];
            uint32_t mn = (uint32_t)code[pc + 2], mx = (uint32_t)code[pc + 3];
            bool greedy = code[pc + 4] != 0;
            int exitpc = code[pc + 5];
            if (cnt < mn) {
                pc += 6;
                continue;
            }
            if (cnt >= mx) {
                pc = exitpc;
                continue;
            }
            if (greedy) {
                if (!bt_push(m, BT_BRANCH, exitpc, (int32_t)pos, 0)) return -1;
                pc += 6;
            } else {
                if (!bt_push(m, BT_BRANCH, pc + 6, (int32_t)pos, 0)) return -1;
                pc = exitpc;
            }
            continue;
        }
        case R_REP_POS: {
            int r = code[pc + 1] + 1;
            if (!bt_push(m, BT_REG, 0, r, m->regs[r])) return -1;
            m->regs[r] = (int32_t)pos;
            pc += 2;
            continue;
        }
        case R_REP_END: {
            int r = code[pc + 1];
            uint32_t mn = (uint32_t)code[pc + 2];
            if ((uint32_t)m->regs[r + 1] == pos && (uint32_t)m->regs[r] >= mn) goto fail;
            if (!bt_push(m, BT_REG, 0, r, m->regs[r])) return -1;
            m->regs[r]++;
            pc = code[pc + 3];
            continue;
        }
        case R_GREEDY1: {
            uint32_t mn = (uint32_t)code[pc + 1], mx = (uint32_t)code[pc + 2];
            int sop = code[pc + 3];
            int32_t arg = code[pc + 4];
            uint32_t cnt = 0, p = pos;
            while (cnt < mx) {
                uint32_t w = match1(m, sop, arg, p);
                if (!w) break;
                p += w;
                cnt++;
            }
            if (cnt < mn) goto fail;
            if (cnt > mn && !bt_push(m, BT_GREEDY, pc + 5, (int32_t)(pos + mn), (int32_t)p - 1)) return -1;
            pos = p;
            pc += 5;
            continue;
        }
        case R_LAZY1: {
            uint32_t mn = (uint32_t)code[pc + 1], mx = (uint32_t)code[pc + 2];
            int sop = code[pc + 3];
            int32_t arg = code[pc + 4];
            uint32_t p = pos;
            for (uint32_t k = 0; k < mn; k++) {
                uint32_t w = match1(m, sop, arg, p);
                if (!w) goto fail;
                p += w;
            }
            if (mx > mn && !bt_push(m, BT_LAZY, pc, (int32_t)p, (int32_t)mn)) return -1;
            pos = p;
            pc += 5;
            continue;
        }
        default: return -1;
        }
    fail:
        for (;;) {
            if (m->sp <= base) return 0;
            BT *b = &m->st[--m->sp];
            switch (b->type) {
            case BT_CAP: m->caps[b->a] = b->b; continue;
            case BT_REG: m->regs[b->a] = b->b; continue;
            case BT_BRANCH:
                pc = b->pc;
                pos = (uint32_t)b->a;
                break;
            case BT_GREEDY:
                /* give back one char: positions from b->b down to b->a */
                pos = (uint32_t)b->b;
                pc = b->pc;
                if ((uint32_t)b->b > (uint32_t)b->a) {
                    b->b--;
                    m->sp++;
                }
                break;
            case BT_LAZY: {
                /* take one more */
                int lpc = b->pc;
                uint32_t mx = (uint32_t)code[lpc + 2];
                uint32_t cnt = (uint32_t)b->b;
                uint32_t p = (uint32_t)b->a;
                uint32_t w = match1(m, code[lpc + 3], code[lpc + 4], p);
                if (!w) continue;
                p += w;
                cnt++;
                if (cnt < mx) {
                    b->a = (int32_t)p;
                    b->b = (int32_t)cnt;
                    m->sp++;
                }
                pos = p;
                pc = lpc + 5;
                break;
            }
            default: continue;
            }
            break;
        }
    }
}

int re_exec(js_rt *rt, Regex *re, JSString *s, uint32_t start, int32_t *caps) {
    str_flat(rt, s);
    RM m;
    memset(&m, 0, sizeof m);
    m.rt = rt;
    m.re = re;
    m.s = s;
    m.n = s->len;
    m.u = (re->flags & (RF_U | RF_V)) != 0;
    m.ic = (re->flags & RF_I) != 0;
    m.multi = (re->flags & RF_M) != 0;
    int32_t regbuf[64];
    m.regs = re->nregs <= 64 ? regbuf : (int32_t *)calloc((size_t)re->nregs, 4);
    m.caps = caps;
    bool sticky = (re->flags & RF_Y) != 0;
    int result = 0;
    /* quick first-char filter */
    bool filt = re->fs_valid && !sticky;
    for (uint32_t p = start; p <= m.n; p++) {
        if (filt) {
            if (s->h.flags & STR_WIDE) {
                const uint16_t *d = s->s16;
                while (p < m.n) {
                    uint32_t ch = d[p];
                    if (ch < 256 ? (re->fset[ch >> 3] >> (ch & 7)) & 1 : re->fs_wide) break;
                    p++;
                }
            } else {
                const uint8_t *d = s->s8;
                while (p < m.n && !((re->fset[d[p] >> 3] >> (d[p] & 7)) & 1)) p++;
            }
            if (p >= m.n) break;
        }
        for (int i = 0; i < re->ncaps * 2; i++) caps[i] = -1;
        m.sp = 0;
        uint32_t e;
        int r = run_re(&m, 0, p, &e, -1);
        if (r < 0) {
            result = -1;
            break;
        }
        if (r == 1) {
            result = 1;
            break;
        }
        if (sticky) break;
        if (m.u && p < m.n) {
            uint32_t c = str_at(s, p);
            if (c >= 0xD800 && c <= 0xDBFF && p + 1 < m.n && str_at(s, p + 1) >= 0xDC00 && str_at(s, p + 1) <= 0xDFFF) p++;
        }
    }
    free(m.st);
    if (m.regs != regbuf) free(m.regs);
    if (result < 0) {
        if (m.error == 2) {
            rt->terminating = true;
            throw_val(rt, mkobj(make_error(rt, E_INTERNAL, str_cstr(rt, "Script execution was interrupted"))));
        } else
            throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    return result;
}

/* ---------------- RegExp objects ---------------- */
typedef struct RCacheEnt {
    uint16_t *src;
    uint32_t len, flags;
    Regex *re;
} RCacheEnt;
#define RCACHE_N 64
typedef struct RCache {
    RCacheEnt e[RCACHE_N];
    int next;
} RCache;

static Regex *cached_compile(js_rt *rt, JSString *src, uint32_t flags, JSString **err) {
    RCache *c = (RCache *)rt->regex_cache;
    if (!c) {
        c = (RCache *)calloc(1, sizeof(RCache));
        rt->regex_cache = c;
    }
    str_flat(rt, src);
    for (int i = 0; i < RCACHE_N; i++) {
        RCacheEnt *e = &c->e[i];
        if (!e->re || e->flags != flags || e->len != src->len) continue;
        bool same = true;
        for (uint32_t k = 0; k < src->len; k++)
            if (e->src[k] != str_at(src, k)) {
                same = false;
                break;
            }
        if (same) {
            e->re->refs++;
            return e->re;
        }
    }
    Regex *re = re_compile(rt, src, flags, err);
    if (!re) return 0;
    RCacheEnt *e = &c->e[c->next];
    c->next = (c->next + 1) % RCACHE_N;
    if (e->re) {
        re_free(e->re);
        free(e->src);
    }
    e->src = (uint16_t *)malloc((size_t)(src->len ? src->len : 1) * 2);
    for (uint32_t k = 0; k < src->len; k++) e->src[k] = str_at(src, k);
    e->len = src->len;
    e->flags = flags;
    e->re = re;
    re->refs++;
    return re;
}
void regex_cache_free(js_rt *rt) {
    RCache *c = (RCache *)rt->regex_cache;
    if (!c) return;
    for (int i = 0; i < RCACHE_N; i++)
        if (c->e[i].re) {
            re_free(c->e[i].re);
            free(c->e[i].src);
        }
    free(c);
    rt->regex_cache = 0;
}

static Regex *rx(jsval v) { return (Regex *)vobj(v)->u.data.p; }

static int regexp_init(js_rt *rt, JSObject *o, jsval pattern, jsval flags) {
    JSString *ps = is_undef(pattern) ? vstr(ATOM(empty)) : to_string(rt, pattern);
    if (!ps) return -1;
    jsval *root = push_tmp(rt, mkstr(ps));
    JSString *fs = is_undef(flags) ? vstr(ATOM(empty)) : to_string(rt, flags);
    rt->sp = root;
    if (!fs) return -1;
    uint32_t fl;
    if (re_parse_flags(fs, &fl) < 0) {
        throw_error(rt, E_SYNTAX, "Invalid flags supplied to RegExp constructor '%S'", fs);
        return -1;
    }
    JSString *err = 0;
    Regex *re = cached_compile(rt, ps, fl, &err);
    if (!re) {
        throw_error(rt, E_SYNTAX, "Invalid regular expression: /%S/%S: %S", ps, fs, err);
        return -1;
    }
    if (o->u.data.p) re_free((Regex *)o->u.data.p);
    o->u.data.p = re;
    o->u.data.v = mkstr(ps);
    Prop *li = find_own(rt, o, ATOM(lastIndex));
    if (li)
        li->val = mknum(0);
    else
        add_prop(rt, o, ATOM(lastIndex), mknum(0), P_W);
    return 0;
}

jsval regexp_create(js_rt *rt, jsval pattern, jsval flags, JSObject *proto) {
    JSObject *o = new_object(rt, proto, CLS_REGEXP);
    o->u.data.v = ATOM(empty);
    jsval *root = push_tmp(rt, mkobj(o));
    int r = regexp_init(rt, o, pattern, flags);
    rt->sp = root;
    return r < 0 ? JS_EXC : mkobj(o);
}

bool regexp_is_pristine(js_rt *rt, jsval re) {
    if (!is_cls(re, CLS_REGEXP)) return false;
    JSObject *o = vobj(re);
    if (o->proto != rt->intr[I_RegExpProto]) return false;
    if (find_own(rt, o, ATOM(exec))) return false;
    Prop *p = find_own(rt, rt->intr[I_RegExpProto], ATOM(exec));
    return p && !(p->flags & P_ACC) && p->val == mkobj(rt->intr[I_RegExpExec]);
}

static int get_last_index(js_rt *rt, jsval re, uint64_t *out) {
    JSObject *o = vobj(re);
    Prop *p = find_own(rt, o, ATOM(lastIndex));
    if (p && !(p->flags & P_ACC) && is_num(p->val)) {
        double d = vnum(p->val);
        if (d >= 0 && d < 4294967296.0 && d == (uint32_t)d) {
            *out = (uint64_t)d;
            return 0;
        }
    }
    jsval v = get_p(rt, re, ATOM(lastIndex));
    if (v == JS_EXC) return -1;
    return to_length(rt, v, out);
}
static int set_last_index(js_rt *rt, jsval re, double v) {
    JSObject *o = vobj(re);
    Prop *p = find_own(rt, o, ATOM(lastIndex));
    if (p && (p->flags & (P_W | P_ACC)) == P_W) {
        p->val = mknum(v);
        return 0;
    }
    return set_p(rt, re, ATOM(lastIndex), mknum(v), true) < 0 ? -1 : 0;
}

/* RegExpBuiltinExec: returns match array, null, or JS_EXC */
static jsval builtin_exec(js_rt *rt, jsval re, JSString *s) {
    Regex *r = rx(re);
    jsval *root = push_tmp(rt, re);
    push_tmp(rt, mkstr(s));
    uint64_t li = 0;
    bool gs = (r->flags & (RF_G | RF_Y)) != 0;
    if (get_last_index(rt, re, &li) < 0) goto exc;
    if (!gs) li = 0;
    if (li > s->len) {
        if (gs && set_last_index(rt, re, 0) < 0) goto exc;
        rt->sp = root;
        return JS_NULL;
    }
    {
        r->refs++;
        int nc = r->ncaps;
        int32_t cbuf[64];
        int32_t *caps = nc * 2 <= 64 ? cbuf : (int32_t *)malloc((size_t)nc * 2 * 4);
        int m = re_exec(rt, r, s, (uint32_t)li, caps);
        if (m <= 0) {
            if (caps != cbuf) free(caps);
            re_free(r);
            if (m < 0) goto exc;
            if (gs && set_last_index(rt, re, 0) < 0) goto exc;
            rt->sp = root;
            return JS_NULL;
        }
        uint32_t e = (uint32_t)caps[1];
        if (gs && set_last_index(rt, re, e) < 0) {
            if (caps != cbuf) free(caps);
            re_free(r);
            goto exc;
        }
        JSObject *a = new_array(rt, (uint32_t)nc);
        push_tmp(rt, mkobj(a));
        for (int i = 0; i < nc; i++) {
            int32_t cs = caps[i * 2], ce = caps[i * 2 + 1];
            a->u.arr.elems[i] = (cs >= 0 && ce >= 0) ? mkstr(str_sub(rt, s, (uint32_t)cs, (uint32_t)ce)) : JS_UNDEF;
        }
        add_prop(rt, a, ATOM(index), mknum(caps[0]), P_DEFAULT);
        add_prop(rt, a, ATOM(input), mkstr(s), P_DEFAULT);
        jsval groups = JS_UNDEF;
        if (r->names) {
            JSObject *g = new_object(rt, 0, CLS_OBJECT);
            for (int i = 1; i < nc; i++)
                if (r->names[i]) add_prop(rt, g, atomize(rt, r->names[i]), a->u.arr.elems[i], P_DEFAULT);
            groups = mkobj(g);
        }
        add_prop(rt, a, ATOM(groups), groups, P_DEFAULT);
        if (r->flags & RF_D) {
            JSObject *ind = new_array(rt, (uint32_t)nc);
            push_tmp(rt, mkobj(ind));
            JSObject *gi = r->names ? new_object(rt, 0, CLS_OBJECT) : 0;
            for (int i = 0; i < nc; i++) {
                int32_t cs = caps[i * 2], ce = caps[i * 2 + 1];
                jsval pv = JS_UNDEF;
                if (cs >= 0 && ce >= 0) {
                    jsval pr[2] = {mknum(cs), mknum(ce)};
                    pv = mkobj(new_array_from(rt, pr, 2));
                }
                ind->u.arr.elems[i] = pv;
                if (gi && r->names[i]) add_prop(rt, gi, atomize(rt, r->names[i]), pv, P_DEFAULT);
            }
            add_prop(rt, ind, ATOM(groups), gi ? mkobj(gi) : JS_UNDEF, P_DEFAULT);
            add_prop(rt, a, ATOM(indices), mkobj(ind), P_DEFAULT);
        }
        if (caps != cbuf) free(caps);
        re_free(r);
        rt->sp = root;
        return mkobj(a);
    }
exc:
    rt->sp = root;
    return JS_EXC;
}

jsval regexp_exec(js_rt *rt, jsval re, JSString *s) {
    if (regexp_is_pristine(rt, re)) return builtin_exec(rt, re, s);
    jsval *root = push_tmp(rt, re);
    push_tmp(rt, mkstr(s));
    jsval ex = get_p(rt, re, ATOM(exec));
    if (ex == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    if (is_callable(ex)) {
        jsval sv = mkstr(s);
        jsval r = call_fn(rt, ex, re, 1, &sv);
        rt->sp = root;
        if (r == JS_EXC) return JS_EXC;
        if (!is_obj(r) && !is_null(r)) return throw_error(rt, E_TYPE, "object null is not a function");
        return r;
    }
    rt->sp = root;
    if (!is_cls(re, CLS_REGEXP)) return throw_error(rt, E_TYPE, "RegExp.prototype.exec called on incompatible receiver");
    return builtin_exec(rt, re, s);
}

NATIVE(regexp_ctor) {
    jsval pattern = ARG(0), flags = ARG(1);
    jsval nt = rt->new_target;
    bool pattern_is_re = is_cls(pattern, CLS_REGEXP);
    if (is_undef(nt)) {
        nt = mkobj(rt->intr[I_RegExp]);
        if (pattern_is_re && is_undef(flags)) {
            jsval c = get_p(rt, pattern, ATOM(constructor));
            TRY(c);
            if (c == nt) return pattern;
        }
    }
    jsval P = pattern, F = flags;
    if (pattern_is_re) {
        P = vobj(pattern)->u.data.v;
        if (is_undef(flags)) {
            Regex *r = rx(pattern);
            char fb[10];
            int k = 0;
            if (r->flags & RF_D) fb[k++] = 'd';
            if (r->flags & RF_G) fb[k++] = 'g';
            if (r->flags & RF_I) fb[k++] = 'i';
            if (r->flags & RF_M) fb[k++] = 'm';
            if (r->flags & RF_S) fb[k++] = 's';
            if (r->flags & RF_U) fb[k++] = 'u';
            if (r->flags & RF_V) fb[k++] = 'v';
            if (r->flags & RF_Y) fb[k++] = 'y';
            fb[k] = 0;
            F = mkstr(str_cstr(rt, fb));
        }
    }
    push_tmp(rt, F);
    JSObject *proto = proto_from_ctor(rt, nt, I_RegExpProto);
    if (!proto) return JS_EXC;
    return regexp_create(rt, P, F, proto);
}

static Regex *this_re(js_rt *rt, jsval t, const char *fn) {
    if (!is_cls(t, CLS_REGEXP)) {
        throw_error(rt, E_TYPE, "RegExp.prototype.%s called on incompatible receiver %S", fn, val_brief(rt, t));
        return 0;
    }
    return rx(t);
}

NATIVE(regexp_exec_n) {
    if (!this_re(rt, this_, "exec")) return JS_EXC;
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    return builtin_exec(rt, this_, s);
}
NATIVE(regexp_test) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype.test called on incompatible receiver");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    if (regexp_is_pristine(rt, this_)) {
        Regex *r = rx(this_);
        if (!(r->flags & (RF_G | RF_Y))) {
            int32_t cbuf[64];
            int nc = r->ncaps;
            int32_t *caps = nc * 2 <= 64 ? cbuf : (int32_t *)malloc((size_t)nc * 8);
            r->refs++;
            int m = re_exec(rt, r, s, 0, caps);
            re_free(r);
            if (caps != cbuf) free(caps);
            if (m < 0) return JS_EXC;
            return mkbool(m == 1);
        }
    }
    jsval m = regexp_exec(rt, this_, s);
    TRY(m);
    return mkbool(!is_null(m));
}
NATIVE(regexp_to_string) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype.toString called on incompatible receiver");
    jsval src = get_p(rt, this_, ATOM(source));
    TRY(src);
    JSString *ss = to_string(rt, src);
    if (!ss) return JS_EXC;
    push_tmp(rt, mkstr(ss));
    jsval fl = get_p(rt, this_, ATOM(flags));
    TRY(fl);
    JSString *fs = to_string(rt, fl);
    if (!fs) return JS_EXC;
    return mkstr(jsfmt(rt, "/%S/%S", ss, fs));
}
NATIVE(regexp_source) {
    if (this_ == mkobj(rt->intr[I_RegExpProto])) return mkstr(str_cstr(rt, "(?:)"));
    if (!this_re(rt, this_, "source")) return JS_EXC;
    JSString *s = str_flat(rt, vstr(vobj(this_)->u.data.v));
    if (s->len == 0) return mkstr(str_cstr(rt, "(?:)"));
    SB b;
    sb_init(&b, rt);
    bool in_class = false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        if (c == '\\') {
            sb_putc(&b, c);
            if (i + 1 < s->len) sb_putc(&b, str_at(s, ++i));
            continue;
        }
        if (c == '[') in_class = true;
        if (c == ']') in_class = false;
        if (c == '/' && !in_class)
            sb_puts(&b, "\\/");
        else if (c == '\n')
            sb_puts(&b, "\\n");
        else if (c == '\r')
            sb_puts(&b, "\\r");
        else if (c == 0x2028)
            sb_puts(&b, "\\u2028");
        else if (c == 0x2029)
            sb_puts(&b, "\\u2029");
        else
            sb_putc(&b, c);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}
NATIVE(regexp_flag) {
    if (this_ == mkobj(rt->intr[I_RegExpProto])) return JS_UNDEF;
    Regex *r = this_re(rt, this_, "flags");
    if (!r) return JS_EXC;
    return mkbool((r->flags & (uint32_t)MAGIC) != 0);
}
NATIVE(regexp_flags) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype.flags getter called on non-object");
    static const struct {
        const char *name;
        char ch;
    } f[] = {{"hasIndices", 'd'}, {"global", 'g'}, {"ignoreCase", 'i'}, {"multiline", 'm'}, {"dotAll", 's'}, {"unicode", 'u'}, {"unicodeSets", 'v'}, {"sticky", 'y'}};
    char buf[10];
    int k = 0;
    for (int i = 0; i < 8; i++) {
        jsval v = get_c(rt, this_, f[i].name);
        TRY(v);
        if (truthy(v)) buf[k++] = f[i].ch;
    }
    buf[k] = 0;
    return mkstr(str_cstr(rt, buf));
}
NATIVE(regexp_compile) {
    if (!this_re(rt, this_, "compile")) return JS_EXC;
    jsval p = ARG(0), f = ARG(1);
    if (is_cls(p, CLS_REGEXP)) {
        if (!is_undef(f)) THROW_TYPE("Invalid flags");
        jsval fl = get_p(rt, p, ATOM(flags));
        TRY(fl);
        f = fl;
        p = vobj(p)->u.data.v;
    }
    if (regexp_init(rt, vobj(this_), p, f) < 0) return JS_EXC;
    return this_;
}

static uint32_t advance(JSString *s, uint32_t i, bool unicode) {
    if (!unicode || i + 1 >= s->len) return i + 1;
    uint32_t c = str_at(s, i);
    if (c >= 0xD800 && c <= 0xDBFF) {
        uint32_t d = str_at(s, i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) return i + 2;
    }
    return i + 1;
}
static bool re_flag(js_rt *rt, jsval re, const char *name, int *err) {
    if (regexp_is_pristine(rt, re)) {
        Regex *r = rx(re);
        if (!strcmp(name, "global")) return (r->flags & RF_G) != 0;
        if (!strcmp(name, "unicode")) return (r->flags & (RF_U | RF_V)) != 0;
    }
    jsval v = get_c(rt, re, name);
    if (v == JS_EXC) {
        *err = 1;
        return false;
    }
    return truthy(v);
}

NATIVE(regexp_sym_match) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype[Symbol.match] called on non-object");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    int err = 0;
    bool g = re_flag(rt, this_, "global", &err);
    if (err) return JS_EXC;
    if (!g) return regexp_exec(rt, this_, s);
    bool u = re_flag(rt, this_, "unicode", &err);
    if (err) return JS_EXC;
    if (set_last_index(rt, this_, 0) < 0) return JS_EXC;
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    for (;;) {
        jsval m = regexp_exec(rt, this_, s);
        TRY(m);
        if (is_null(m)) return a->u.arr.len ? mkobj(a) : JS_NULL;
        jsval *r2 = push_tmp(rt, m);
        jsval ms = get_idx(rt, m, 0);
        TRY(ms);
        JSString *mss = to_string(rt, ms);
        if (!mss) return JS_EXC;
        array_push(rt, a, mkstr(mss));
        if (mss->len == 0) {
            uint64_t li;
            if (get_last_index(rt, this_, &li) < 0) return JS_EXC;
            if (set_last_index(rt, this_, (double)advance(s, (uint32_t)li, u)) < 0) return JS_EXC;
        }
        rt->sp = r2;
    }
}
NATIVE(regexp_sym_search) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype[Symbol.search] called on non-object");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    jsval prev = get_p(rt, this_, ATOM(lastIndex));
    TRY(prev);
    push_tmp(rt, prev);
    if (!same_value(rt, prev, mknum(0)) && set_p(rt, this_, ATOM(lastIndex), mknum(0), true) < 0) return JS_EXC;
    jsval m = regexp_exec(rt, this_, s);
    TRY(m);
    push_tmp(rt, m);
    jsval cur = get_p(rt, this_, ATOM(lastIndex));
    TRY(cur);
    if (!same_value(rt, cur, prev) && set_p(rt, this_, ATOM(lastIndex), prev, true) < 0) return JS_EXC;
    if (is_null(m)) return mknum(-1);
    return get_p(rt, m, ATOM(index));
}

JSString *get_substitution(js_rt *rt, JSString *matched, JSString *str, uint32_t pos, jsval *caps, int ncaps, jsval groups, JSString *rep);

NATIVE(regexp_sym_replace) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype[Symbol.replace] called on non-object");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    push_tmp(rt, mkstr(s));
    jsval repl = ARG(1);
    bool fn = is_callable(repl);
    JSString *rs = 0;
    if (!fn) {
        rs = to_string(rt, repl);
        if (!rs) return JS_EXC;
        push_tmp(rt, mkstr(rs));
    }
    int err = 0;
    bool g = re_flag(rt, this_, "global", &err);
    if (err) return JS_EXC;
    bool u = false;
    if (g) {
        u = re_flag(rt, this_, "unicode", &err);
        if (err) return JS_EXC;
        if (set_last_index(rt, this_, 0) < 0) return JS_EXC;
    }
    JSObject *results = new_array(rt, 0);
    push_tmp(rt, mkobj(results));
    for (;;) {
        jsval m = regexp_exec(rt, this_, s);
        TRY(m);
        if (is_null(m)) break;
        array_push(rt, results, m);
        if (!g) break;
        jsval ms = get_idx(rt, m, 0);
        TRY(ms);
        JSString *mss = to_string(rt, ms);
        if (!mss) return JS_EXC;
        if (mss->len == 0) {
            uint64_t li;
            if (get_last_index(rt, this_, &li) < 0) return JS_EXC;
            if (set_last_index(rt, this_, (double)advance(s, (uint32_t)li, u)) < 0) return JS_EXC;
        }
    }
    SB b;
    sb_init(&b, rt);
    uint32_t next = 0;
    for (uint32_t ri = 0; ri < results->u.arr.len; ri++) {
        jsval m = results->u.arr.elems[ri];
        jsval *r2 = rt->sp;
        uint64_t nc;
        if (len_of(rt, m, &nc) < 0) goto fail;
        jsval mv = get_idx(rt, m, 0);
        if (mv == JS_EXC) goto fail;
        JSString *matched = to_string(rt, mv);
        if (!matched) goto fail;
        push_tmp(rt, mkstr(matched));
        jsval pv = get_p(rt, m, ATOM(index));
        if (pv == JS_EXC) goto fail;
        double pd;
        if (to_int(rt, pv, &pd) < 0) goto fail;
        uint32_t pos = pd < 0 ? 0 : pd > s->len ? s->len : (uint32_t)pd;
        int ncap = nc > 0 ? (int)nc - 1 : 0;
        JSObject *capa = new_array(rt, (uint32_t)ncap);
        push_tmp(rt, mkobj(capa));
        for (int k = 1; k <= ncap; k++) {
            jsval cv = get_idx(rt, m, (uint32_t)k);
            if (cv == JS_EXC) goto fail;
            if (!is_undef(cv)) {
                JSString *cs = to_string(rt, cv);
                if (!cs) goto fail;
                cv = mkstr(cs);
            }
            capa->u.arr.elems[k - 1] = cv;
        }
        jsval groups = get_p(rt, m, ATOM(groups));
        if (groups == JS_EXC) goto fail;
        push_tmp(rt, groups);
        JSString *replacement;
        if (fn) {
            uint32_t na = (uint32_t)ncap + 3 + (is_undef(groups) ? 0 : 1);
            jsval *args = (jsval *)malloc(na * sizeof(jsval));
            uint32_t k = 0;
            args[k++] = mkstr(matched);
            for (int q = 0; q < ncap; q++) args[k++] = capa->u.arr.elems[q];
            args[k++] = mknum(pos);
            args[k++] = mkstr(s);
            if (!is_undef(groups)) args[k++] = groups;
            jsval r = call_fn(rt, repl, JS_UNDEF, (int)k, args);
            free(args);
            if (r == JS_EXC) goto fail;
            replacement = to_string(rt, r);
        } else {
            if (!is_undef(groups)) {
                groups = to_object(rt, groups);
                if (groups == JS_EXC) goto fail;
                push_tmp(rt, groups);
            }
            replacement = get_substitution(rt, matched, s, pos, capa->u.arr.elems, ncap, groups, rs);
        }
        if (!replacement) goto fail;
        if (pos >= next) {
            sb_putsub(&b, s, next, pos);
            sb_putstr(&b, replacement);
            next = pos + matched->len;
        }
        rt->sp = r2;
    }
    if (next < s->len) sb_putsub(&b, s, next, s->len);
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
fail:
    sb_free(&b);
    return JS_EXC;
}

NATIVE(regexp_sym_split) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype[Symbol.split] called on non-object");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    push_tmp(rt, mkstr(s));
    uint32_t lim = 0xFFFFFFFFu;
    if (!is_undef(ARG(1))) {
        double d;
        if (to_number(rt, argv[1], &d) < 0) return JS_EXC;
        lim = to_uint32(d);
    }
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    if (lim == 0) return mkobj(a);
    /* build a sticky clone */
    jsval fl = get_p(rt, this_, ATOM(flags));
    TRY(fl);
    JSString *fs = to_string(rt, fl);
    if (!fs) return JS_EXC;
    str_flat(rt, fs);
    bool u = false, has_y = false;
    for (uint32_t i = 0; i < fs->len; i++) {
        if (str_at(fs, i) == 'u' || str_at(fs, i) == 'v') u = true;
        if (str_at(fs, i) == 'y') has_y = true;
    }
    JSString *nf = has_y ? fs : str_concat(rt, fs, str_cstr(rt, "y"));
    jsval ctor;
    if (!species_ctor(rt, vobj(this_), I_RegExp, &ctor)) return JS_EXC;
    jsval cargs[2] = {this_, mkstr(nf)};
    jsval splitter = construct_fn(rt, ctor, 2, cargs, ctor);
    TRY(splitter);
    push_tmp(rt, splitter);
    if (s->len == 0) {
        jsval m = regexp_exec(rt, splitter, s);
        TRY(m);
        if (is_null(m)) array_push(rt, a, mkstr(s));
        return mkobj(a);
    }
    uint32_t p = 0, q = 0;
    while (q < s->len) {
        if (set_last_index(rt, splitter, q) < 0) return JS_EXC;
        jsval m = regexp_exec(rt, splitter, s);
        TRY(m);
        if (is_null(m)) {
            q = advance(s, q, u);
            continue;
        }
        jsval *r2 = push_tmp(rt, m);
        uint64_t e;
        if (get_last_index(rt, splitter, &e) < 0) return JS_EXC;
        if (e > s->len) e = s->len;
        if (e == p) {
            q = advance(s, q, u);
            rt->sp = r2;
            continue;
        }
        array_push(rt, a, mkstr(str_sub(rt, s, p, q)));
        if (a->u.arr.len >= lim) return mkobj(a);
        p = (uint32_t)e;
        uint64_t nc;
        if (len_of(rt, m, &nc) < 0) return JS_EXC;
        for (uint64_t i = 1; i < nc; i++) {
            jsval cv = get_idx(rt, m, (uint32_t)i);
            TRY(cv);
            array_push(rt, a, cv);
            if (a->u.arr.len >= lim) return mkobj(a);
        }
        q = p;
        rt->sp = r2;
    }
    array_push(rt, a, mkstr(str_sub(rt, s, p, s->len)));
    return mkobj(a);
}

/* matchAll iterator: it.a = regexp, it.b = string, i: flags (1 global 2 unicode 4 done) */
NATIVE(regexp_iter_next) {
    if (!is_cls(this_, CLS_REGEXP_ITER)) THROW_TYPE("next method called on incompatible receiver");
    JSObject *it = vobj(this_);
    if (it->u.it.i & 4) return iter_result(rt, JS_UNDEF, true);
    JSString *s = vstr(it->u.it.b);
    jsval m = regexp_exec(rt, it->u.it.a, s);
    TRY(m);
    if (is_null(m)) {
        it->u.it.i |= 4;
        return iter_result(rt, JS_UNDEF, true);
    }
    if (!(it->u.it.i & 1)) {
        it->u.it.i |= 4;
        return iter_result(rt, m, false);
    }
    push_tmp(rt, m);
    jsval ms = get_idx(rt, m, 0);
    TRY(ms);
    JSString *mss = to_string(rt, ms);
    if (!mss) return JS_EXC;
    if (mss->len == 0) {
        uint64_t li;
        if (get_last_index(rt, it->u.it.a, &li) < 0) return JS_EXC;
        if (set_last_index(rt, it->u.it.a, (double)advance(s, (uint32_t)li, (it->u.it.i & 2) != 0)) < 0) return JS_EXC;
    }
    return iter_result(rt, m, false);
}
NATIVE(regexp_sym_match_all) {
    if (!is_obj(this_)) THROW_TYPE("RegExp.prototype[Symbol.matchAll] called on non-object");
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    jsval ctor;
    if (!species_ctor(rt, vobj(this_), I_RegExp, &ctor)) return JS_EXC;
    jsval fl = get_p(rt, this_, ATOM(flags));
    TRY(fl);
    JSString *fs = to_string(rt, fl);
    if (!fs) return JS_EXC;
    push_tmp(rt, mkstr(fs));
    jsval cargs[2] = {this_, mkstr(fs)};
    jsval matcher = construct_fn(rt, ctor, 2, cargs, ctor);
    TRY(matcher);
    push_tmp(rt, matcher);
    uint64_t li;
    if (get_last_index(rt, this_, &li) < 0) return JS_EXC;
    if (set_last_index(rt, matcher, (double)li) < 0) return JS_EXC;
    str_flat(rt, fs);
    uint32_t f = 0;
    for (uint32_t i = 0; i < fs->len; i++) {
        if (str_at(fs, i) == 'g') f |= 1;
        if (str_at(fs, i) == 'u' || str_at(fs, i) == 'v') f |= 2;
    }
    JSObject *it = new_object(rt, rt->intr[I_RegExpIterProto], CLS_REGEXP_ITER);
    it->u.it.a = matcher;
    it->u.it.b = mkstr(s);
    it->u.it.i = f;
    return mkobj(it);
}
NATIVE(regexp_species) { return this_; }

void init_regexp(js_rt *rt) {
    JSObject *rp = new_object(rt, rt->intr[I_ObjectProto], CLS_OBJECT);
    rt->intr[I_RegExpProto] = rp;
    JSObject *c = new_ctor(rt, "RegExp", regexp_ctor, 2, rp);
    rt->intr[I_RegExp] = c;
    def_getter_k(rt, c, SYM(species), "get [Symbol.species]", regexp_species, 0);
    rt->intr[I_RegExpExec] = def_method(rt, rp, "exec", regexp_exec_n, 1, 0);
    def_method(rt, rp, "test", regexp_test, 1, 0);
    def_method(rt, rp, "toString", regexp_to_string, 0, 0);
    def_method(rt, rp, "compile", regexp_compile, 2, 0);
    def_getter(rt, rp, "source", regexp_source, 0);
    def_getter(rt, rp, "flags", regexp_flags, 0);
    def_getter(rt, rp, "global", regexp_flag, RF_G);
    def_getter(rt, rp, "ignoreCase", regexp_flag, RF_I);
    def_getter(rt, rp, "multiline", regexp_flag, RF_M);
    def_getter(rt, rp, "dotAll", regexp_flag, RF_S);
    def_getter(rt, rp, "unicode", regexp_flag, RF_U);
    def_getter(rt, rp, "unicodeSets", regexp_flag, RF_V);
    def_getter(rt, rp, "sticky", regexp_flag, RF_Y);
    def_getter(rt, rp, "hasIndices", regexp_flag, RF_D);
    def_method_k(rt, rp, SYM(match), "[Symbol.match]", regexp_sym_match, 1, 0);
    def_method_k(rt, rp, SYM(matchAll), "[Symbol.matchAll]", regexp_sym_match_all, 1, 0);
    def_method_k(rt, rp, SYM(replace), "[Symbol.replace]", regexp_sym_replace, 2, 0);
    def_method_k(rt, rp, SYM(search), "[Symbol.search]", regexp_sym_search, 1, 0);
    def_method_k(rt, rp, SYM(split), "[Symbol.split]", regexp_sym_split, 2, 0);
    JSObject *rip = new_object(rt, rt->intr[I_IteratorProto], CLS_OBJECT);
    rt->intr[I_RegExpIterProto] = rip;
    def_method(rt, rip, "next", regexp_iter_next, 0, 0);
    def_prop(rt, rip, SYM(toStringTag), mkstr(str_cstr(rt, "RegExp String Iterator")), P_C);
}

void regexp_trace(js_rt *rt, JSObject *o) { gc_mark_val(rt, o->u.data.v); }
void regexp_finalize(JSObject *o) {
    re_free((Regex *)o->u.data.p);
    o->u.data.p = 0;
}
