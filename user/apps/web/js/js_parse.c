/* AXIOM JavaScript engine: lexer and parser (source -> AST with scopes). */
#include "js_parse.h"

struct ArenaBlk {
    struct ArenaBlk *next;
    size_t used, cap;
    char data[];
};
void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->blk || a->blk->used + n > a->blk->cap) {
        size_t cap = n > 65536 - 64 ? n : 65536 - 64;
        struct ArenaBlk *b = (struct ArenaBlk *)malloc(sizeof(struct ArenaBlk) + cap);
        if (!b) return 0;
        b->next = a->blk;
        b->used = 0;
        b->cap = cap;
        a->blk = b;
    }
    void *p = a->blk->data + a->blk->used;
    a->blk->used += n;
    memset(p, 0, n);
    return p;
}
void arena_free(Arena *a) {
    while (a->blk) {
        struct ArenaBlk *n = a->blk->next;
        free(a->blk);
        a->blk = n;
    }
}

enum {
    T_EOF, T_IDENT, T_NUM, T_BIGINT, T_STR, T_TEMPLATE, T_REGEXP, T_PRIVATE,
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_SEMI, T_COMMA, T_DOT, T_ELLIPSIS, T_QDOT,
    T_QUESTION, T_COLON, T_ARROW, T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE, T_SEQ, T_SNE, T_PLUS, T_MINUS, T_STAR,
    T_SLASH, T_PERCENT, T_POW, T_INC, T_DEC, T_SHL, T_SAR, T_SHR, T_AMP, T_PIPE, T_CARET, T_BANG, T_TILDE, T_AND,
    T_OR, T_NULLISH, T_ASSIGN, T_ADD_A, T_SUB_A, T_MUL_A, T_DIV_A, T_MOD_A, T_POW_A, T_SHL_A, T_SAR_A, T_SHR_A,
    T_AND_A, T_OR_A, T_XOR_A, T_LAND_A, T_LOR_A, T_NULLISH_A, T_HASH
};
static const char *const tok_text[] = {
    "", "", "", "", "", "", "", "", "{", "}", "(", ")", "[", "]", ";", ",", ".", "...", "?.", "?", ":", "=>", "<", ">",
    "<=", ">=", "==", "!=", "===", "!==", "+", "-", "*", "/", "%", "**", "++", "--", "<<", ">>", ">>>", "&", "|", "^",
    "!", "~", "&&", "||", "??", "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=",
    "&&=", "||=", "?" "?=", "#"};

typedef struct Tok {
    int t;
    int line;
    uint32_t pos, end;
    bool nl, escaped, tail, octal;
    double num;
    jsval s;   /* identifier/string atom, template cooked (JS_UNDEF if invalid), regexp body */
    jsval raw; /* template raw, regexp flags */
} Tok;

typedef struct P {
    js_rt *rt;
    const uint8_t *s;
    uint32_t n, pos;
    int line;
    Tok t;
    uint32_t prev_end;
    Arena *ar;
    FuncNode *fn;
    Scope *sc;
    Node **refs;
    int nrefs, crefs;
    Scope **scopes;
    int nscopes, cscopes;
    FuncNode **funcs;
    int nfuncs, cfuncs;
    js_jmp_buf jb;
    char *errmsg;
    int *errline;
    int depth;
    int hidden;
    bool in_class_body;
    FuncNode *script;
    FuncNode *pending_arrow;
} P;

#define CHK_DEPTH(p) do { if (++(p)->depth > 1000 || check_stack((p)->rt)) p_error(p, "Maximum call stack size exceeded (code too deeply nested)"); } while (0)
#define END_DEPTH(p) ((p)->depth--)

static __attribute__((noreturn)) void p_error_line(P *p, int line, const char *fmt, ...);
static __attribute__((noreturn)) void p_error(P *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SB b;
    sb_init(&b, p->rt);
    fmt_into(p->rt, &b, fmt, ap);
    va_end(ap);
    JSString *m = sb_finish(&b);
    size_t ml;
    char *u = m ? str_to_utf8(p->rt, m, &ml) : 0;
    if (u) {
        size_t k = ml < 255 ? ml : 255;
        memcpy(p->errmsg, u, k);
        p->errmsg[k] = 0;
        free(u);
    }
    *p->errline = p->t.line;
    JS_LONGJMP(p->jb);
}
static __attribute__((noreturn)) void p_error_line(P *p, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SB b;
    sb_init(&b, p->rt);
    fmt_into(p->rt, &b, fmt, ap);
    va_end(ap);
    JSString *m = sb_finish(&b);
    size_t ml;
    char *u = m ? str_to_utf8(p->rt, m, &ml) : 0;
    if (u) {
        size_t k = ml < 255 ? ml : 255;
        memcpy(p->errmsg, u, k);
        p->errmsg[k] = 0;
        free(u);
    }
    *p->errline = line;
    JS_LONGJMP(p->jb);
}

static void *pal(P *p, size_t n) {
    void *r = arena_alloc(p->ar, n);
    if (!r) p_error(p, "out of memory");
    return r;
}
static void grow(P *p, void ***arr, int *cap, int n) {
    if (n < *cap) return;
    int nc = *cap ? *cap * 2 : 256;
    void **a = (void **)realloc(*arr, (size_t)nc * sizeof(void *));
    if (!a) p_error(p, "out of memory");
    *arr = a;
    *cap = nc;
}

/* ---------------- lexer ---------------- */
static bool at_line_start(P *p, uint32_t pos) {
    while (pos > 0) {
        uint8_t c = p->s[pos - 1];
        if (c == '\n' || c == '\r') return true;
        if (c != ' ' && c != '\t') return false;
        pos--;
    }
    return true;
}

static void skip_ws(P *p, bool *nl) {
    const uint8_t *s = p->s;
    uint32_t n = p->n;
    for (;;) {
        if (p->pos >= n) return;
        uint8_t c = s[p->pos];
        if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C) {
            p->pos++;
        } else if (c == '\n') {
            p->pos++;
            p->line++;
            *nl = true;
        } else if (c == '\r') {
            p->pos++;
            if (p->pos < n && s[p->pos] == '\n') p->pos++;
            p->line++;
            *nl = true;
        } else if (c == '/' && p->pos + 1 < n && s[p->pos + 1] == '/') {
            while (p->pos < n && s[p->pos] != '\n' && s[p->pos] != '\r') {
                if (s[p->pos] == 0xE2 && p->pos + 2 < n && s[p->pos + 1] == 0x80 && (s[p->pos + 2] == 0xA8 || s[p->pos + 2] == 0xA9)) break;
                p->pos++;
            }
        } else if (c == '/' && p->pos + 1 < n && s[p->pos + 1] == '*') {
            uint32_t start = p->pos;
            p->pos += 2;
            for (;;) {
                if (p->pos + 1 >= n) {
                    p->pos = start;
                    p_error(p, "Invalid or unexpected token");
                }
                if (s[p->pos] == '*' && s[p->pos + 1] == '/') {
                    p->pos += 2;
                    break;
                }
                if (s[p->pos] == '\n') {
                    p->line++;
                    *nl = true;
                } else if (s[p->pos] == '\r') {
                    if (p->pos + 1 < n && s[p->pos + 1] != '\n') p->line++;
                    *nl = true;
                }
                p->pos++;
            }
        } else if (c == '<' && p->pos + 3 < n && s[p->pos + 1] == '!' && s[p->pos + 2] == '-' && s[p->pos + 3] == '-') {
            while (p->pos < n && s[p->pos] != '\n' && s[p->pos] != '\r') p->pos++;
        } else if (c == '-' && p->pos + 2 < n && s[p->pos + 1] == '-' && s[p->pos + 2] == '>' && (*nl || at_line_start(p, p->pos))) {
            while (p->pos < n && s[p->pos] != '\n' && s[p->pos] != '\r') p->pos++;
        } else if (c >= 0x80) {
            uint32_t cp;
            int l = utf8_decode(s + p->pos, n - p->pos, &cp);
            if (cp == 0x2028 || cp == 0x2029) {
                p->line++;
                *nl = true;
                p->pos += (uint32_t)l;
            } else if (uc_is_space(cp)) {
                p->pos += (uint32_t)l;
            } else
                return;
        } else
            return;
    }
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* \u escape at p->pos (pointing after 'u'); returns code point or -1 */
static int32_t read_uescape(P *p) {
    const uint8_t *s = p->s;
    if (p->pos < p->n && s[p->pos] == '{') {
        uint32_t v = 0;
        p->pos++;
        int nd = 0;
        while (p->pos < p->n && s[p->pos] != '}') {
            int h = hexv(s[p->pos]);
            if (h < 0) return -1;
            v = v * 16 + (uint32_t)h;
            if (v > 0x10FFFF) return -1;
            p->pos++;
            nd++;
        }
        if (p->pos >= p->n || nd == 0) return -1;
        p->pos++;
        return (int32_t)v;
    }
    if (p->pos + 4 > p->n) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(s[p->pos + i]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    p->pos += 4;
    return v;
}

static void lex_ident(P *p, Tok *t) {
    const uint8_t *s = p->s;
    uint32_t start = p->pos;
    bool simple = true;
    while (p->pos < p->n) {
        uint8_t c = s[p->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')
            p->pos++;
        else if (c == '\\' || c >= 0x80) {
            simple = false;
            break;
        } else
            break;
    }
    if (simple) {
        t->s = atom_n(p->rt, s + start, p->pos - start);
        return;
    }
    SB b;
    sb_init(&b, p->rt);
    sb_putn(&b, (const char *)s + start, p->pos - start);
    while (p->pos < p->n) {
        uint8_t c = s[p->pos];
        uint32_t cp;
        if (c == '\\') {
            if (p->pos + 1 >= p->n || s[p->pos + 1] != 'u') {
                sb_free(&b);
                p_error(p, "Invalid or unexpected token");
            }
            p->pos += 2;
            int32_t v = read_uescape(p);
            if (v < 0 || !(b.len == 0 ? uc_is_id_start((uint32_t)v) : uc_is_id_part((uint32_t)v))) {
                sb_free(&b);
                p_error(p, "Invalid Unicode escape sequence");
            }
            sb_putcp(&b, (uint32_t)v);
            t->escaped = true;
            continue;
        }
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$') {
                sb_putc(&b, c);
                p->pos++;
                continue;
            }
            break;
        }
        int l = utf8_decode(s + p->pos, p->n - p->pos, &cp);
        if (!uc_is_id_part(cp)) break;
        sb_putcp(&b, cp);
        p->pos += (uint32_t)l;
    }
    t->s = atomize(p->rt, sb_finish(&b));
}

static void lex_number(P *p, Tok *t) {
    const uint8_t *s = p->s;
    uint32_t start = p->pos;
    char buf[400];
    int bn = 0;
    t->t = T_NUM;
    if (s[p->pos] == '0' && p->pos + 1 < p->n) {
        uint8_t c = s[p->pos + 1] | 0x20;
        int radix = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
        if (radix) {
            p->pos += 2;
            char *big = 0;
            size_t cap = 0, k = 0;
            while (p->pos < p->n) {
                int d = s[p->pos];
                if (d == '_') {
                    p->pos++;
                    continue;
                }
                int v = hexv(d);
                if (v < 0 || v >= radix) break;
                if (k + 2 > cap) {
                    cap = cap ? cap * 2 : 64;
                    big = (char *)realloc(big, cap);
                }
                big[k++] = (char)d;
                p->pos++;
            }
            if (k == 0) {
                free(big);
                p_error(p, "Invalid or unexpected token");
            }
            if (p->pos < p->n && s[p->pos] == 'n') {
                p->pos++;
                t->t = T_BIGINT;
                t->s = mkstr(str_new8(p->rt, s + start, p->pos - 1 - start));
            } else {
                size_t used;
                t->num = parse_int_digits(big, k, radix, &used);
            }
            free(big);
            goto check_end;
        }
        if (s[p->pos + 1] >= '0' && s[p->pos + 1] <= '9') {
            /* legacy octal (or decimal with leading zero) */
            uint32_t q = p->pos + 1;
            bool octal = true;
            while (q < p->n && s[q] >= '0' && s[q] <= '9') {
                if (s[q] >= '8') octal = false;
                q++;
            }
            if (octal) {
                double v = 0;
                for (uint32_t i = p->pos + 1; i < q; i++) v = v * 8 + (s[i] - '0');
                p->pos = q;
                t->num = v;
                t->octal = true;
                goto check_end;
            }
        }
    }
    while (p->pos < p->n) {
        uint8_t c = s[p->pos];
        if (c >= '0' && c <= '9') {
            if (bn < 390) buf[bn++] = (char)c;
            p->pos++;
        } else if (c == '_')
            p->pos++;
        else
            break;
    }
    if (p->pos < p->n && s[p->pos] == 'n') {
        p->pos++;
        t->t = T_BIGINT;
        t->s = mkstr(str_new8(p->rt, (const uint8_t *)buf, (uint32_t)bn));
        goto check_end;
    }
    if (p->pos < p->n && s[p->pos] == '.') {
        if (bn < 390) buf[bn++] = '.';
        p->pos++;
        while (p->pos < p->n) {
            uint8_t c = s[p->pos];
            if (c >= '0' && c <= '9') {
                if (bn < 390) buf[bn++] = (char)c;
                p->pos++;
            } else if (c == '_')
                p->pos++;
            else
                break;
        }
    }
    if (p->pos < p->n && (s[p->pos] | 0x20) == 'e') {
        uint32_t q = p->pos + 1;
        if (q < p->n && (s[q] == '+' || s[q] == '-')) q++;
        if (q < p->n && s[q] >= '0' && s[q] <= '9') {
            if (bn < 390) buf[bn++] = 'e';
            for (uint32_t i = p->pos + 1; i < q; i++)
                if (bn < 390) buf[bn++] = (char)s[i];
            p->pos = q;
            while (p->pos < p->n && ((s[p->pos] >= '0' && s[p->pos] <= '9') || s[p->pos] == '_')) {
                if (s[p->pos] != '_' && bn < 390) buf[bn++] = (char)s[p->pos];
                p->pos++;
            }
        } else
            p_error(p, "Invalid or unexpected token");
    }
    {
        size_t used;
        if (bn >= 390) {
            /* very long literal: re-collect fully */
            size_t cap = p->pos - start + 1, k = 0;
            char *big = (char *)malloc(cap);
            for (uint32_t i = start; i < p->pos; i++)
                if (s[i] != '_') big[k++] = (char)s[i];
            t->num = js_strtod(big, k, &used);
            free(big);
        } else
            t->num = js_strtod(buf, (size_t)bn, &used);
    }
check_end:
    if (p->pos < p->n) {
        uint8_t c = s[p->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (c >= '0' && c <= '9') || c == '\\')
            p_error(p, "Invalid or unexpected token");
    }
}

/* reads an escape after '\' (p->pos at the char after '\'); appends to b.
 * returns false for invalid escapes (templates: cooked = undefined) */
static bool lex_escape(P *p, SB *b, bool in_template, bool *octal) {
    const uint8_t *s = p->s;
    if (p->pos >= p->n) return false;
    uint8_t c = s[p->pos++];
    switch (c) {
    case 'n': sb_putc(b, '\n'); return true;
    case 't': sb_putc(b, '\t'); return true;
    case 'r': sb_putc(b, '\r'); return true;
    case 'b': sb_putc(b, '\b'); return true;
    case 'f': sb_putc(b, '\f'); return true;
    case 'v': sb_putc(b, '\v'); return true;
    case '\r':
        if (p->pos < p->n && s[p->pos] == '\n') p->pos++;
        p->line++;
        return true;
    case '\n': p->line++; return true;
    case 'x': {
        if (p->pos + 2 > p->n) return false;
        int h1 = hexv(s[p->pos]), h2 = hexv(s[p->pos + 1]);
        if (h1 < 0 || h2 < 0) return false;
        p->pos += 2;
        sb_putc(b, (uint32_t)(h1 * 16 + h2));
        return true;
    }
    case 'u': {
        int32_t v = read_uescape(p);
        if (v < 0) return false;
        sb_putcp(b, (uint32_t)v);
        return true;
    }
    default:
        if (c >= '0' && c <= '7') {
            if (c == '0' && !(p->pos < p->n && s[p->pos] >= '0' && s[p->pos] <= '9')) {
                sb_putc(b, 0);
                return true;
            }
            if (in_template) return false;
            /* legacy octal */
            int v = c - '0';
            if (p->pos < p->n && s[p->pos] >= '0' && s[p->pos] <= '7') {
                v = v * 8 + (s[p->pos++] - '0');
                if (c <= '3' && p->pos < p->n && s[p->pos] >= '0' && s[p->pos] <= '7') v = v * 8 + (s[p->pos++] - '0');
            }
            *octal = true;
            sb_putc(b, (uint32_t)v);
            return true;
        }
        if (c == '8' || c == '9') {
            if (in_template) return false;
            *octal = true;
            sb_putc(b, c);
            return true;
        }
        if (c >= 0x80) {
            uint32_t cp;
            p->pos--;
            int l = utf8_decode(s + p->pos, p->n - p->pos, &cp);
            p->pos += (uint32_t)l;
            if (cp == 0x2028 || cp == 0x2029) {
                p->line++;
                return true;
            }
            sb_putcp(b, cp);
            return true;
        }
        sb_putc(b, c);
        return true;
    }
}

static void lex_string(P *p, Tok *t) {
    const uint8_t *s = p->s;
    uint8_t q = s[p->pos++];
    uint32_t start = p->pos;
    /* fast path: no escapes, ASCII */
    uint32_t i = start;
    while (i < p->n && s[i] != q && s[i] != '\\' && s[i] != '\n' && s[i] != '\r' && s[i] < 0x80) i++;
    if (i < p->n && s[i] == q) {
        t->s = atom_n(p->rt, s + start, i - start);
        p->pos = i + 1;
        t->t = T_STR;
        return;
    }
    SB b;
    sb_init(&b, p->rt);
    for (;;) {
        if (p->pos >= p->n) {
            sb_free(&b);
            p_error(p, "Invalid or unexpected token");
        }
        uint8_t c = s[p->pos];
        if (c == q) {
            p->pos++;
            break;
        }
        if (c == '\n' || c == '\r') {
            sb_free(&b);
            p_error(p, "Invalid or unexpected token");
        }
        if (c == '\\') {
            p->pos++;
            if (!lex_escape(p, &b, false, &t->octal)) {
                sb_free(&b);
                p_error(p, "Invalid hexadecimal escape sequence");
            }
            continue;
        }
        if (c >= 0x80) {
            uint32_t cp;
            int l = utf8_decode(s + p->pos, p->n - p->pos, &cp);
            sb_putcp(&b, cp);
            p->pos += (uint32_t)l;
            continue;
        }
        sb_putc(&b, c);
        p->pos++;
    }
    JSString *str = sb_finish(&b);
    t->s = atomize(p->rt, str);
    t->t = T_STR;
}

/* template characters starting at p->pos, up to ` or ${ */
static void lex_template(P *p, Tok *t) {
    const uint8_t *s = p->s;
    SB cooked, raw;
    sb_init(&cooked, p->rt);
    sb_init(&raw, p->rt);
    bool valid = true;
    for (;;) {
        if (p->pos >= p->n) {
            sb_free(&cooked);
            sb_free(&raw);
            p_error(p, "Unterminated template literal");
        }
        uint8_t c = s[p->pos];
        if (c == '`') {
            p->pos++;
            t->tail = true;
            break;
        }
        if (c == '$' && p->pos + 1 < p->n && s[p->pos + 1] == '{') {
            p->pos += 2;
            t->tail = false;
            break;
        }
        if (c == '\\') {
            uint32_t es = p->pos;
            p->pos++;
            bool oct = false;
            if (!lex_escape(p, &cooked, true, &oct)) {
                valid = false;
                /* skip the bad escape's character */
                if (p->pos <= es + 1) p->pos = es + 2;
            }
            for (uint32_t k = es; k < p->pos; k++) {
                if (s[k] == '\r') {
                    sb_putc(&raw, '\n');
                    if (k + 1 < p->pos && s[k + 1] == '\n') k++;
                } else if (s[k] >= 0x80) {
                    uint32_t cp;
                    int l = utf8_decode(s + k, p->pos - k, &cp);
                    sb_putcp(&raw, cp);
                    k += (uint32_t)l - 1;
                } else
                    sb_putc(&raw, s[k]);
            }
            continue;
        }
        if (c == '\r') {
            p->pos++;
            if (p->pos < p->n && s[p->pos] == '\n') p->pos++;
            p->line++;
            sb_putc(&cooked, '\n');
            sb_putc(&raw, '\n');
            continue;
        }
        if (c == '\n') p->line++;
        if (c >= 0x80) {
            uint32_t cp;
            int l = utf8_decode(s + p->pos, p->n - p->pos, &cp);
            sb_putcp(&cooked, cp);
            sb_putcp(&raw, cp);
            p->pos += (uint32_t)l;
            continue;
        }
        sb_putc(&cooked, c);
        sb_putc(&raw, c);
        p->pos++;
    }
    JSString *cs = sb_finish(&cooked);
    t->s = valid ? mkstr(cs) : JS_UNDEF;
    t->raw = mkstr(sb_finish(&raw));
    t->t = T_TEMPLATE;
}

static void lex_punct(P *p, Tok *t) {
    const uint8_t *s = p->s;
    uint8_t c = s[p->pos];
    uint8_t c1 = p->pos + 1 < p->n ? s[p->pos + 1] : 0;
    uint8_t c2 = p->pos + 2 < p->n ? s[p->pos + 2] : 0;
    uint8_t c3 = p->pos + 3 < p->n ? s[p->pos + 3] : 0;
    int tt = -1, len = 1;
    switch (c) {
    case '{': tt = T_LBRACE; break;
    case '}': tt = T_RBRACE; break;
    case '(': tt = T_LPAREN; break;
    case ')': tt = T_RPAREN; break;
    case '[': tt = T_LBRACK; break;
    case ']': tt = T_RBRACK; break;
    case ';': tt = T_SEMI; break;
    case ',': tt = T_COMMA; break;
    case ':': tt = T_COLON; break;
    case '~': tt = T_TILDE; break;
    case '.':
        if (c1 == '.' && c2 == '.') tt = T_ELLIPSIS, len = 3;
        else tt = T_DOT;
        break;
    case '?':
        if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) tt = T_QDOT, len = 2;
        else if (c1 == '?' && c2 == '=') tt = T_NULLISH_A, len = 3;
        else if (c1 == '?') tt = T_NULLISH, len = 2;
        else tt = T_QUESTION;
        break;
    case '=':
        if (c1 == '>') tt = T_ARROW, len = 2;
        else if (c1 == '=' && c2 == '=') tt = T_SEQ, len = 3;
        else if (c1 == '=') tt = T_EQ, len = 2;
        else tt = T_ASSIGN;
        break;
    case '!':
        if (c1 == '=' && c2 == '=') tt = T_SNE, len = 3;
        else if (c1 == '=') tt = T_NE, len = 2;
        else tt = T_BANG;
        break;
    case '<':
        if (c1 == '<' && c2 == '=') tt = T_SHL_A, len = 3;
        else if (c1 == '<') tt = T_SHL, len = 2;
        else if (c1 == '=') tt = T_LE, len = 2;
        else tt = T_LT;
        break;
    case '>':
        if (c1 == '>' && c2 == '>' && c3 == '=') tt = T_SHR_A, len = 4;
        else if (c1 == '>' && c2 == '>') tt = T_SHR, len = 3;
        else if (c1 == '>' && c2 == '=') tt = T_SAR_A, len = 3;
        else if (c1 == '>') tt = T_SAR, len = 2;
        else if (c1 == '=') tt = T_GE, len = 2;
        else tt = T_GT;
        break;
    case '+':
        if (c1 == '+') tt = T_INC, len = 2;
        else if (c1 == '=') tt = T_ADD_A, len = 2;
        else tt = T_PLUS;
        break;
    case '-':
        if (c1 == '-') tt = T_DEC, len = 2;
        else if (c1 == '=') tt = T_SUB_A, len = 2;
        else tt = T_MINUS;
        break;
    case '*':
        if (c1 == '*' && c2 == '=') tt = T_POW_A, len = 3;
        else if (c1 == '*') tt = T_POW, len = 2;
        else if (c1 == '=') tt = T_MUL_A, len = 2;
        else tt = T_STAR;
        break;
    case '/':
        if (c1 == '=') tt = T_DIV_A, len = 2;
        else tt = T_SLASH;
        break;
    case '%':
        if (c1 == '=') tt = T_MOD_A, len = 2;
        else tt = T_PERCENT;
        break;
    case '&':
        if (c1 == '&' && c2 == '=') tt = T_LAND_A, len = 3;
        else if (c1 == '&') tt = T_AND, len = 2;
        else if (c1 == '=') tt = T_AND_A, len = 2;
        else tt = T_AMP;
        break;
    case '|':
        if (c1 == '|' && c2 == '=') tt = T_LOR_A, len = 3;
        else if (c1 == '|') tt = T_OR, len = 2;
        else if (c1 == '=') tt = T_OR_A, len = 2;
        else tt = T_PIPE;
        break;
    case '^':
        if (c1 == '=') tt = T_XOR_A, len = 2;
        else tt = T_CARET;
        break;
    default: break;
    }
    if (tt < 0) p_error(p, "Invalid or unexpected token");
    t->t = tt;
    p->pos += (uint32_t)len;
}

static void next(P *p) {
    Tok *t = &p->t;
    p->prev_end = t->end;
    bool nl = false;
    skip_ws(p, &nl);
    memset(t, 0, sizeof *t);
    t->nl = nl;
    t->pos = p->pos;
    t->line = p->line;
    if (p->pos >= p->n) {
        t->t = T_EOF;
        t->end = p->pos;
        return;
    }
    uint8_t c = p->s[p->pos];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c == '\\') {
        t->t = T_IDENT;
        lex_ident(p, t);
    } else if (c >= '0' && c <= '9') {
        lex_number(p, t);
    } else if (c == '.' && p->pos + 1 < p->n && p->s[p->pos + 1] >= '0' && p->s[p->pos + 1] <= '9') {
        lex_number(p, t);
    } else if (c == '"' || c == '\'') {
        lex_string(p, t);
    } else if (c == '`') {
        p->pos++;
        lex_template(p, t);
    } else if (c == '#') {
        p->pos++;
        if (p->pos < p->n && (uc_is_id_start(p->s[p->pos]) || p->s[p->pos] == '\\' || p->s[p->pos] >= 0x80)) {
            Tok tmp;
            memset(&tmp, 0, sizeof tmp);
            lex_ident(p, &tmp);
            JSString *h = jsfmt(p->rt, "#%S", vstr(tmp.s));
            t->s = atomize(p->rt, h);
            t->t = T_PRIVATE;
        } else if (p->pos == 1 && p->pos < p->n && p->s[p->pos] == '!') {
            /* hashbang */
            while (p->pos < p->n && p->s[p->pos] != '\n') p->pos++;
            next(p);
            return;
        } else
            p_error(p, "Invalid or unexpected token");
    } else if (c >= 0x80) {
        uint32_t cp;
        utf8_decode(p->s + p->pos, p->n - p->pos, &cp);
        if (uc_is_id_start(cp)) {
            t->t = T_IDENT;
            lex_ident(p, t);
        } else
            p_error(p, "Invalid or unexpected token");
    } else
        lex_punct(p, t);
    t->end = p->pos;
}

static void rescan_regex(P *p) {
    Tok *t = &p->t;
    p->pos = t->pos + 1;
    const uint8_t *s = p->s;
    bool in_class = false;
    uint32_t start = p->pos;
    for (;;) {
        if (p->pos >= p->n || s[p->pos] == '\n' || s[p->pos] == '\r') p_error(p, "Invalid regular expression: missing /");
        uint8_t c = s[p->pos];
        if (c == '\\') {
            p->pos += 2;
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) break;
        p->pos++;
    }
    uint32_t end = p->pos;
    p->pos++;
    uint32_t fs = p->pos;
    while (p->pos < p->n && ((s[p->pos] >= 'a' && s[p->pos] <= 'z') || (s[p->pos] >= 'A' && s[p->pos] <= 'Z'))) p->pos++;
    t->t = T_REGEXP;
    t->s = mkstr(str_utf8(p->rt, (const char *)s + start, end - start));
    t->raw = mkstr(str_new8(p->rt, s + fs, p->pos - fs));
    t->end = p->pos;
}

static void rescan_template(P *p) {
    Tok *t = &p->t;
    p->pos = t->pos + 1;
    int line = t->line;
    memset(t, 0, sizeof *t);
    t->line = line;
    t->pos = p->pos;
    lex_template(p, t);
    t->end = p->pos;
}

typedef struct LexState {
    uint32_t pos;
    int line;
    Tok t;
    uint32_t prev_end;
} LexState;
static void save(P *p, LexState *s) {
    s->pos = p->pos;
    s->line = p->line;
    s->t = p->t;
    s->prev_end = p->prev_end;
}
static void restore(P *p, LexState *s) {
    p->pos = s->pos;
    p->line = s->line;
    p->t = s->t;
    p->prev_end = s->prev_end;
}
static Tok peek(P *p) {
    LexState st;
    save(p, &st);
    next(p);
    Tok r = p->t;
    restore(p, &st);
    return r;
}

/* ---------------- helpers ---------------- */
#define RT (p->rt)
#define A(id) (p->rt->atoms[A_##id])
static bool is(P *p, int t) { return p->t.t == t; }
static bool is_id(P *p, jsval a) { return p->t.t == T_IDENT && p->t.s == a && !p->t.escaped; }
static bool strict(P *p) { return (p->fn->flags & FN_STRICT) != 0; }

static __attribute__((noreturn)) void unexpected(P *p) {
    Tok *t = &p->t;
    switch (t->t) {
    case T_EOF: p_error(p, "Unexpected end of input");
    case T_IDENT: {
        jsval a = t->s;
        if (a == A(break) || a == A(case) || a == A(catch) || a == A(class) || a == A(const) || a == A(continue) ||
            a == A(debugger) || a == A(default) || a == A(delete) || a == A(do) || a == A(else) || a == A(export) ||
            a == A(extends) || a == A(finally) || a == A(for) || a == A(function) || a == A(if) || a == A(import) ||
            a == A(in) || a == A(instanceof) || a == A(new) || a == A(return) || a == A(super) || a == A(switch) ||
            a == A(this) || a == A(throw) || a == A(try) || a == A(typeof) || a == A(var) || a == A(void) ||
            a == A(while) || a == A(with) || a == A(null) || a == A(true) || a == A(false) || a == A(enum))
            p_error(p, "Unexpected token '%S'", vstr(a));
        if (strict(p) && (a == A(let) || a == A(static) || a == A(yield) || a == A(implements) || a == A(interface) ||
                          a == A(package) || a == A(private) || a == A(protected) || a == A(public)))
            p_error(p, "Unexpected strict mode reserved word");
        p_error(p, "Unexpected identifier '%S'", vstr(a));
    }
    case T_NUM:
    case T_BIGINT: p_error(p, "Unexpected number");
    case T_STR: p_error(p, "Unexpected string");
    case T_TEMPLATE: p_error(p, "Unexpected template string");
    case T_PRIVATE: p_error(p, "Unexpected identifier '%S'", vstr(t->s));
    case T_REGEXP: p_error(p, "Unexpected regular expression");
    default: p_error(p, "Unexpected token '%s'", tok_text[t->t]);
    }
}
static void expect(P *p, int t) {
    if (p->t.t != t) unexpected(p);
    next(p);
}
static void semi(P *p) {
    if (is(p, T_SEMI)) {
        next(p);
        return;
    }
    if (is(p, T_RBRACE) || is(p, T_EOF) || p->t.nl) return;
    unexpected(p);
}

static bool is_reserved(P *p, jsval a) {
    if (a == A(break) || a == A(case) || a == A(catch) || a == A(class) || a == A(const) || a == A(continue) ||
        a == A(debugger) || a == A(default) || a == A(delete) || a == A(do) || a == A(else) || a == A(export) ||
        a == A(extends) || a == A(finally) || a == A(for) || a == A(function) || a == A(if) || a == A(import) ||
        a == A(in) || a == A(instanceof) || a == A(new) || a == A(return) || a == A(super) || a == A(switch) ||
        a == A(this) || a == A(throw) || a == A(try) || a == A(typeof) || a == A(var) || a == A(void) || a == A(while) ||
        a == A(with) || a == A(null) || a == A(true) || a == A(false) || a == A(enum))
        return true;
    if (strict(p) && (a == A(let) || a == A(static) || a == A(yield) || a == A(implements) || a == A(interface) ||
                      a == A(package) || a == A(private) || a == A(protected) || a == A(public)))
        return true;
    if (a == A(yield) && (p->fn->flags & FN_GEN)) return true;
    if (a == A(await) && (p->fn->flags & FN_ASYNC)) return true;
    return false;
}

static Node *mk(P *p, int kind) {
    Node *n = (Node *)pal(p, sizeof(Node));
    n->kind = (uint16_t)kind;
    n->line = p->t.line;
    n->pos = p->t.pos;
    return n;
}
static Node *mk_ref(P *p, jsval name) {
    Node *n = mk(p, N_IDENT);
    n->u.atom = name;
    n->scope = p->sc;
    grow(p, (void ***)&p->refs, &p->crefs, p->nrefs);
    p->refs[p->nrefs++] = n;
    return n;
}

static Scope *new_scope(P *p, int kind) {
    Scope *s = (Scope *)pal(p, sizeof(Scope));
    s->kind = (uint8_t)kind;
    s->parent = p->sc;
    s->func = p->fn;
    grow(p, (void ***)&p->scopes, &p->cscopes, p->nscopes);
    p->scopes[p->nscopes++] = s;
    return s;
}

static Binding *find_in(Scope *s, jsval name) {
    for (Binding *b = s->first; b; b = b->next)
        if (b->name == name) return b;
    return 0;
}
static Binding *add_binding(P *p, Scope *s, jsval name, int kind) {
    Binding *b = (Binding *)pal(p, sizeof(Binding));
    b->name = name;
    b->kind = (uint8_t)kind;
    b->scope = s;
    b->slot = -1;
    b->env_idx = -1;
    b->param_index = -1;
    b->tdz = kind == B_LET || kind == B_CONST || kind == B_CLASS;
    b->is_const = kind == B_CONST;
    if (s->last)
        s->last->next = b;
    else
        s->first = b;
    s->last = b;
    return b;
}
static void redeclared(P *p, jsval name) { p_error(p, "Identifier '%S' has already been declared", vstr(name)); }

static Node *list_find_atom(Node *l, jsval a) {
    for (; l; l = l->next)
        if (l->u.atom == a) return l;
    return 0;
}
static void list_add(Node **head, Node *n) {
    n->next = 0;
    if (!*head) {
        *head = n;
        return;
    }
    Node *t = *head;
    while (t->next) t = t->next;
    t->next = n;
}

static Binding *declare_var(P *p, jsval name) {
    Scope *s = p->sc;
    while (s->kind != SC_FUNC) {
        Binding *b = find_in(s, name);
        if (b && b->kind != B_VAR && !(b->kind == B_CATCH) && !(b->kind == B_FUNC && !strict(p))) redeclared(p, name);
        s = s->parent;
    }
    if (s->is_script) {
        if (list_find_atom(p->script->glex, name)) redeclared(p, name);
        if (!list_find_atom(p->script->gvars, name)) {
            Node *n = mk(p, N_IDENT);
            n->u.atom = name;
            list_add(&p->script->gvars, n);
        }
        return 0;
    }
    Binding *b = find_in(s, name);
    if (b) {
        if (b->kind == B_LET || b->kind == B_CONST || b->kind == B_CLASS) redeclared(p, name);
        return b;
    }
    return add_binding(p, s, name, B_VAR);
}
static Binding *declare_lex(P *p, jsval name, int kind) {
    Scope *s = p->sc;
    if (name == A(let) && kind != B_FUNC) p_error(p, "let is disallowed as a lexically bound name");
    if (s->is_script) {
        if (list_find_atom(p->script->glex, name) || list_find_atom(p->script->gvars, name)) redeclared(p, name);
        Node *g;
        for (g = p->script->gfuncs; g; g = g->next)
            if (g->u.fn->name == name) redeclared(p, name);
        Node *n = mk(p, N_IDENT);
        n->u.atom = name;
        n->flags = kind == B_CONST ? 1 : 0;
        list_add(&p->script->glex, n);
        return 0;
    }
    Binding *b = find_in(s, name);
    if (b) {
        if (kind == B_FUNC && b->kind == B_FUNC && !strict(p)) return b;
        if (s->kind == SC_FUNC && b->kind == B_FUNCNAME) {
            b->kind = (uint8_t)kind;
            b->tdz = kind == B_LET || kind == B_CONST || kind == B_CLASS;
            b->is_const = kind == B_CONST;
            return b;
        }
        redeclared(p, name);
    }
    /* catch parameter conflicts with let in catch block's first scope are checked by the block */
    return add_binding(p, s, name, kind);
}

/* declare all identifiers of a binding pattern */
static void declare_pattern(P *p, Node *n, int kind) {
    if (!n) return;
    switch (n->kind) {
    case N_IDENT:
        if (kind == B_VAR)
            declare_var(p, n->u.atom);
        else if (kind == B_PARAM) {
            Binding *b = find_in(p->sc, n->u.atom);
            if (b && b->kind == B_PARAM) {
                if (strict(p) || !p->fn->simple_params) p_error(p, "Duplicate parameter name not allowed in this context");
            } else if (!b)
                add_binding(p, p->sc, n->u.atom, B_PARAM);
        } else if (kind == B_CATCH) {
            if (find_in(p->sc, n->u.atom)) redeclared(p, n->u.atom);
            add_binding(p, p->sc, n->u.atom, B_CATCH);
        } else
            declare_lex(p, n->u.atom, kind);
        break;
    case N_ASSIGN_PAT: declare_pattern(p, n->a, kind); break;
    case N_REST: declare_pattern(p, n->a, kind); break;
    case N_ARRAY_PAT:
        for (Node *e = n->a; e; e = e->next) declare_pattern(p, e, kind);
        break;
    case N_OBJECT_PAT:
        for (Node *e = n->a; e; e = e->next) declare_pattern(p, e->kind == N_PROP_PAT ? e->b : e, kind);
        break;
    default: break;
    }
}

/* ---------------- expressions ---------------- */
static Node *parse_assign(P *p, bool noin);
static Node *parse_expr(P *p, bool noin);
static Node *parse_stmt(P *p);
static Node *parse_unary(P *p);
static FuncNode *parse_function_rest(P *p, uint32_t flags, jsval name, uint32_t start, int line, Node *cover_params);
static Node *parse_class(P *p, bool is_expr);
static Node *parse_binding_target(P *p);
static Node *parse_template(P *p, bool tagged);

static int binprec(P *p, bool noin, int *op) {
    switch (p->t.t) {
    case T_NULLISH: *op = OPR_NULLISH; return 1;
    case T_OR: *op = OPR_OR; return 2;
    case T_AND: *op = OPR_AND; return 3;
    case T_PIPE: *op = OPR_BOR; return 4;
    case T_CARET: *op = OPR_BXOR; return 5;
    case T_AMP: *op = OPR_BAND; return 6;
    case T_EQ: *op = OPR_EQ; return 7;
    case T_NE: *op = OPR_NE; return 7;
    case T_SEQ: *op = OPR_SEQ; return 7;
    case T_SNE: *op = OPR_SNE; return 7;
    case T_LT: *op = OPR_LT; return 8;
    case T_GT: *op = OPR_GT; return 8;
    case T_LE: *op = OPR_LE; return 8;
    case T_GE: *op = OPR_GE; return 8;
    case T_SHL: *op = OPR_SHL; return 9;
    case T_SAR: *op = OPR_SAR; return 9;
    case T_SHR: *op = OPR_SHR; return 9;
    case T_PLUS: *op = OPR_ADD; return 10;
    case T_MINUS: *op = OPR_SUB; return 10;
    case T_STAR: *op = OPR_MUL; return 11;
    case T_SLASH: *op = OPR_DIV; return 11;
    case T_PERCENT: *op = OPR_MOD; return 11;
    case T_POW: *op = OPR_POW; return 12;
    case T_IDENT:
        if (p->t.escaped) return 0;
        if (p->t.s == A(instanceof)) {
            *op = OPR_INSTANCEOF;
            return 8;
        }
        if (p->t.s == A(in) && !noin) {
            *op = OPR_IN;
            return 8;
        }
        return 0;
    default: return 0;
    }
}

static Node *parse_binary(P *p, int minprec, bool noin) {
    Node *left;
    if (is(p, T_PRIVATE)) {
        /* #x in obj */
        Node *r = mk_ref(p, p->t.s);
        next(p);
        if (!is_id(p, A(in))) unexpected(p);
        next(p);
        Node *n = mk(p, N_PRIV_IN);
        n->a = r;
        n->b = parse_binary(p, 8, noin);
        left = n;
    } else
        left = parse_unary(p);
    for (;;) {
        int op;
        int prec = binprec(p, noin, &op);
        if (!prec || prec <= minprec) break;
        if (op == OPR_POW && left->kind == N_UNARY && !(left->flags & NF_PAREN))
            p_error(p, "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
        Node *n = mk(p, (op == OPR_AND || op == OPR_OR || op == OPR_NULLISH) ? N_LOGICAL : N_BINARY);
        n->line = p->t.line;
        next(p);
        n->u.op = op;
        n->a = left;
        CHK_DEPTH(p);
        n->b = parse_binary(p, op == OPR_POW ? prec - 1 : prec, noin);
        END_DEPTH(p);
        left = n;
    }
    return left;
}

static Node *parse_args(P *p) {
    /* current token is '(' */
    next(p);
    Node *head = 0, *tail = 0;
    while (!is(p, T_RPAREN)) {
        Node *a;
        if (is(p, T_ELLIPSIS)) {
            a = mk(p, N_SPREAD);
            next(p);
            a->a = parse_assign(p, false);
        } else
            a = parse_assign(p, false);
        if (tail)
            tail->next = a;
        else
            head = a;
        tail = a;
        if (!is(p, T_RPAREN)) {
            if (!is(p, T_COMMA)) p_error(p, "missing ) after argument list");
            next(p);
        }
    }
    next(p);
    return head;
}

static jsval prop_name_atom(P *p, Tok *t) {
    if (t->t == T_IDENT || t->t == T_STR) return t->s;
    if (t->t == T_NUM) return atomize(p->rt, num_to_str(p->rt, t->num));
    if (t->t == T_BIGINT) return atomize(p->rt, vstr(t->s));
    return 0;
}

/* member name after '.' or '?.' */
static jsval member_name(P *p) {
    if (is(p, T_IDENT)) {
        jsval a = p->t.s;
        next(p);
        return a;
    }
    unexpected(p);
}

static Node *parse_call_tail(P *p, Node *e, bool no_call) {
    bool chain = false;
    for (;;) {
        if (is(p, T_DOT)) {
            next(p);
            if (is(p, T_PRIVATE)) {
                Node *n = mk(p, N_PRIV_MEMBER);
                n->a = e;
                n->b = mk_ref(p, p->t.s);
                next(p);
                e = n;
                continue;
            }
            Node *n = mk(p, N_MEMBER);
            n->a = e;
            n->u.atom = member_name(p);
            e = n;
        } else if (is(p, T_QDOT)) {
            if (no_call) p_error(p, "Invalid optional chain from new expression");
            next(p);
            chain = true;
            if (is(p, T_LPAREN)) {
                Node *n = mk(p, N_CALL);
                n->a = e;
                n->flags |= NF_OPTIONAL;
                n->b = parse_args(p);
                e = n;
            } else if (is(p, T_LBRACK)) {
                next(p);
                Node *n = mk(p, N_INDEX);
                n->a = e;
                n->flags |= NF_OPTIONAL;
                n->b = parse_expr(p, false);
                expect(p, T_RBRACK);
                e = n;
            } else if (is(p, T_PRIVATE)) {
                Node *n = mk(p, N_PRIV_MEMBER);
                n->a = e;
                n->flags |= NF_OPTIONAL;
                n->b = mk_ref(p, p->t.s);
                next(p);
                e = n;
            } else {
                Node *n = mk(p, N_MEMBER);
                n->a = e;
                n->flags |= NF_OPTIONAL;
                n->u.atom = member_name(p);
                e = n;
            }
        } else if (is(p, T_LBRACK)) {
            next(p);
            Node *n = mk(p, N_INDEX);
            n->a = e;
            n->b = parse_expr(p, false);
            expect(p, T_RBRACK);
            e = n;
        } else if (is(p, T_LPAREN) && !no_call) {
            Node *n = mk(p, N_CALL);
            n->a = e;
            n->b = parse_args(p);
            e = n;
        } else if (is(p, T_TEMPLATE)) {
            if (chain) p_error(p, "Invalid tagged template on optional chain");
            Node *n = mk(p, N_TAGGED);
            n->a = e;
            n->b = parse_template(p, true);
            e = n;
        } else
            break;
    }
    if (chain) {
        Node *c = mk(p, N_OPTCHAIN);
        c->a = e;
        c->line = e->line;
        return c;
    }
    return e;
}

static Node *parse_template(P *p, bool tagged) {
    Node *n = mk(p, N_TEMPLATE);
    Node *qt = 0, *et = 0;
    for (;;) {
        if (!is(p, T_TEMPLATE)) unexpected(p);
        if (is_undef(p->t.s) && !tagged) p_error(p, "Invalid escape sequence in template");
        Node *q = mk(p, N_STR);
        q->u.atom = p->t.s;
        q->str2 = p->t.raw;
        if (qt)
            qt->next = q;
        else
            n->a = q;
        qt = q;
        if (p->t.tail) {
            next(p);
            break;
        }
        next(p);
        Node *e = parse_expr(p, false);
        if (et)
            et->next = e;
        else
            n->b = e;
        et = e;
        if (!is(p, T_RBRACE)) unexpected(p);
        rescan_template(p);
    }
    return n;
}

/* convert an expression (cover grammar) to an assignment/binding pattern */
static Node *to_pattern(P *p, Node *e, bool binding) {
    switch (e->kind) {
    case N_IDENT:
        if (strict(p) && (e->u.atom == A(eval) || e->u.atom == A(arguments)))
            p_error(p, "Unexpected eval or arguments in strict mode");
        return e;
    case N_MEMBER:
    case N_INDEX:
    case N_PRIV_MEMBER:
    case N_SUPER_MEMBER:
        if (binding) p_error_line(p, e->line, "Invalid destructuring assignment target");
        return e;
    case N_ASSIGN:
        if (e->u.op != 0) break;
        {
            Node *n = mk(p, N_ASSIGN_PAT);
            n->line = e->line;
            n->a = to_pattern(p, e->a, binding);
            n->b = e->b;
            return n;
        }
    case N_ASSIGN_PAT: return e;
    case N_ARRAY: {
        Node *n = mk(p, N_ARRAY_PAT);
        n->line = e->line;
        Node *tail = 0;
        for (Node *el = e->a; el;) {
            Node *nx = el->next;
            Node *c;
            if (el->kind == N_HOLE)
                c = el;
            else if (el->kind == N_SPREAD) {
                c = mk(p, N_REST);
                c->a = to_pattern(p, el->a, binding);
                if (nx) p_error_line(p, el->line, "Rest element must be last element");
            } else
                c = to_pattern(p, el, binding);
            c->next = 0;
            if (tail)
                tail->next = c;
            else
                n->a = c;
            tail = c;
            el = nx;
        }
        return n;
    }
    case N_OBJECT: {
        Node *n = mk(p, N_OBJECT_PAT);
        n->line = e->line;
        Node *tail = 0;
        for (Node *pr = e->a; pr;) {
            Node *nx = pr->next;
            Node *c;
            if (pr->u.op == PK_SPREAD) {
                c = mk(p, N_REST);
                c->a = to_pattern(p, pr->a, binding);
            } else if (pr->u.op == PK_INIT || pr->u.op == PK_PROTO) {
                c = mk(p, N_PROP_PAT);
                c->flags = pr->flags & NF_COMPUTED;
                c->a = pr->a;
                c->b = to_pattern(p, pr->b, binding);
            } else if (pr->u.op == PK_SHORTHAND) {
                c = mk(p, N_PROP_PAT);
                c->a = pr->a;
                c->b = to_pattern(p, pr->b, binding);
            } else if (pr->u.op == PK_SHORTHAND_INIT) {
                c = mk(p, N_PROP_PAT);
                c->a = pr->a;
                Node *ap = mk(p, N_ASSIGN_PAT);
                ap->a = to_pattern(p, pr->b, binding);
                ap->b = pr->c;
                c->b = ap;
            } else
                p_error_line(p, pr->line, "Invalid destructuring assignment target");
            c->line = pr->line;
            c->next = 0;
            if (tail)
                tail->next = c;
            else
                n->a = c;
            tail = c;
            pr = nx;
        }
        return n;
    }
    default: break;
    }
    p_error_line(p, e->line, binding ? "Invalid destructuring assignment target" : "Invalid left-hand side in assignment");
}

/* check that an object literal used as expression has no CoverInitializedName */
static void check_no_cover_init(P *p, Node *e) {
    if (!e) return;
    if (e->kind == N_OBJECT) {
        for (Node *pr = e->a; pr; pr = pr->next) {
            if (pr->u.op == PK_SHORTHAND_INIT) p_error_line(p, pr->line, "Invalid shorthand property initializer");
            if (pr->u.op == PK_INIT || pr->u.op == PK_PROTO) check_no_cover_init(p, pr->b);
        }
    } else if (e->kind == N_ARRAY) {
        for (Node *el = e->a; el; el = el->next) check_no_cover_init(p, el->kind == N_SPREAD ? el->a : el);
    }
}

typedef struct Mark {
    int refs, scopes, funcs;
} Mark;
static Mark mark(P *p) {
    Mark m = {p->nrefs, p->nscopes, p->nfuncs};
    return m;
}

/* build an arrow function whose parameters are the (already parsed) cover expressions */
static Node *finish_arrow(P *p, Node *params, Mark m, uint32_t start, int line, bool is_async) {
    if (p->t.nl) unexpected(p);
    expect(p, T_ARROW);
    /* create the function first, then re-home refs/scopes parsed inside the parameter list */
    FuncNode *fn = (FuncNode *)pal(p, sizeof(FuncNode));
    fn->flags = FN_ARROW | (is_async ? FN_ASYNC : 0) | (p->fn->flags & FN_STRICT);
    fn->parent = p->fn;
    fn->name = JS_UNDEF;
    fn->line = line;
    fn->src_start = start;
    Scope *outer = p->sc;
    FuncNode *ofn = p->fn;
    p->fn = fn;
    Scope *fs = new_scope(p, SC_FUNC);
    fs->func = fn;
    fn->scope = fs;
    grow(p, (void ***)&p->funcs, &p->cfuncs, p->nfuncs);
    p->funcs[p->nfuncs++] = fn;
    for (int i = m.refs; i < p->nrefs; i++)
        if (p->refs[i]->scope == outer) p->refs[i]->scope = fs;
    for (int i = m.scopes; i < p->nscopes; i++)
        if (p->scopes[i] != fs && p->scopes[i]->parent == outer) p->scopes[i]->parent = fs;
    for (int i = m.funcs; i < p->nfuncs; i++)
        if (p->funcs[i] != fn && p->funcs[i]->parent == ofn) p->funcs[i]->parent = fn;
    for (int i = m.scopes; i < p->nscopes; i++)
        if (p->scopes[i]->func == ofn && p->scopes[i] != fs) p->scopes[i]->func = fn;
    p->fn = ofn;
    p->sc = outer;
    p->pending_arrow = fn;
    FuncNode *r = parse_function_rest(p, fn->flags, JS_UNDEF, start, line, params ? params : (Node *)1);
    Node *n = mk(p, N_FUNC);
    n->u.fn = r;
    n->line = line;
    return n;
}

static Node *parse_paren(P *p, bool is_async_call, uint32_t start, int line) {
    /* current token '(' */
    Mark m = mark(p);
    next(p);
    if (is(p, T_RPAREN)) {
        next(p);
        if (!is(p, T_ARROW)) unexpected(p);
        return finish_arrow(p, 0, m, start, line, is_async_call);
    }
    Node *head = 0, *tail = 0;
    bool has_rest = false, trailing_comma = false;
    for (;;) {
        Node *e;
        if (is(p, T_ELLIPSIS)) {
            Node *s = mk(p, N_SPREAD);
            next(p);
            s->a = parse_binding_target(p);
            e = s;
            has_rest = true;
        } else
            e = parse_assign(p, false);
        if (tail)
            tail->next = e;
        else
            head = e;
        tail = e;
        if (is(p, T_COMMA)) {
            next(p);
            if (is(p, T_RPAREN)) {
                trailing_comma = true;
                break;
            }
            if (has_rest) p_error(p, "Rest parameter must be last formal parameter");
            continue;
        }
        break;
    }
    expect(p, T_RPAREN);
    if (is(p, T_ARROW) && !p->t.nl) return finish_arrow(p, head, m, start, line, is_async_call);
    if (has_rest || trailing_comma) unexpected(p);
    for (Node *e = head; e; e = e->next) check_no_cover_init(p, e);
    Node *e;
    if (head->next) {
        e = mk(p, N_SEQ);
        e->a = head;
        e->line = head->line;
    } else
        e = head;
    e->flags |= NF_PAREN;
    return e;
}

static Node *parse_object_lit(P *p) {
    Node *n = mk(p, N_OBJECT);
    next(p);
    Node *tail = 0;
    bool has_proto = false;
    while (!is(p, T_RBRACE)) {
        Node *pr = mk(p, N_PROP);
        if (is(p, T_ELLIPSIS)) {
            next(p);
            pr->u.op = PK_SPREAD;
            pr->a = parse_assign(p, false);
            goto add;
        }
        {
            uint32_t fflags = 0;
            int kind = PK_INIT;
            uint32_t mstart = p->t.pos;
            int mline = p->t.line;
            if (is_id(p, A(async))) {
                Tok nt = peek(p);
                if (!nt.nl && nt.t != T_LPAREN && nt.t != T_COLON && nt.t != T_COMMA && nt.t != T_RBRACE && nt.t != T_ASSIGN) {
                    next(p);
                    fflags |= FN_ASYNC;
                    kind = PK_METHOD;
                }
            }
            if (is(p, T_STAR)) {
                next(p);
                fflags |= FN_GEN;
                kind = PK_METHOD;
            }
            if (kind == PK_INIT && (is_id(p, A(get)) || is_id(p, A(set)))) {
                Tok nt = peek(p);
                if (nt.t != T_LPAREN && nt.t != T_COLON && nt.t != T_COMMA && nt.t != T_RBRACE && nt.t != T_ASSIGN) {
                    kind = is_id(p, A(get)) ? PK_GET : PK_SET;
                    next(p);
                }
            }
            /* key */
            Tok kt = p->t;
            if (is(p, T_LBRACK)) {
                next(p);
                pr->flags |= NF_COMPUTED;
                pr->a = parse_assign(p, false);
                expect(p, T_RBRACK);
            } else if (is(p, T_IDENT) || is(p, T_STR) || is(p, T_NUM) || is(p, T_BIGINT)) {
                Node *k = mk(p, N_STR);
                k->u.atom = prop_name_atom(p, &p->t);
                pr->a = k;
                next(p);
            } else
                unexpected(p);
            if (kind == PK_GET || kind == PK_SET || kind == PK_METHOD || is(p, T_LPAREN)) {
                if (!is(p, T_LPAREN)) unexpected(p);
                uint32_t f = FN_METHOD | fflags;
                if (kind == PK_GET) f |= FN_GETTER;
                if (kind == PK_SET) f |= FN_SETTER;
                jsval nm = (pr->flags & NF_COMPUTED) ? JS_UNDEF : pr->a->u.atom;
                FuncNode *fn = parse_function_rest(p, f, nm, mstart, mline, 0);
                Node *fnode = mk(p, N_FUNC);
                fnode->u.fn = fn;
                pr->b = fnode;
                pr->u.op = kind == PK_INIT ? PK_METHOD : kind;
                goto add;
            }
            if (is(p, T_COLON)) {
                next(p);
                pr->b = parse_assign(p, false);
                if (!(pr->flags & NF_COMPUTED) && pr->a->u.atom == A(__proto__) && kt.t != T_NUM) {
                    if (has_proto) p_error(p, "Duplicate __proto__ fields are not allowed in object literals");
                    has_proto = true;
                    pr->u.op = PK_PROTO;
                } else
                    pr->u.op = PK_INIT;
                goto add;
            }
            if (kt.t != T_IDENT || (pr->flags & NF_COMPUTED)) unexpected(p);
            if (is_reserved(p, kt.s) && kt.s != A(yield) && kt.s != A(await)) {
                p->t = kt;
                unexpected(p);
            }
            {
                Node *r = mk_ref(p, kt.s);
                r->line = kt.line;
                pr->b = r;
            }
            if (is(p, T_ASSIGN)) {
                next(p);
                pr->u.op = PK_SHORTHAND_INIT;
                pr->c = parse_assign(p, false);
            } else
                pr->u.op = PK_SHORTHAND;
        }
    add:
        if (tail)
            tail->next = pr;
        else
            n->a = pr;
        tail = pr;
        if (!is(p, T_RBRACE)) expect(p, T_COMMA);
    }
    next(p);
    return n;
}

static Node *parse_array_lit(P *p) {
    Node *n = mk(p, N_ARRAY);
    next(p);
    Node *tail = 0;
    while (!is(p, T_RBRACK)) {
        Node *e;
        if (is(p, T_COMMA)) {
            e = mk(p, N_HOLE);
            next(p);
        } else {
            if (is(p, T_ELLIPSIS)) {
                e = mk(p, N_SPREAD);
                next(p);
                e->a = parse_assign(p, false);
            } else
                e = parse_assign(p, false);
            if (!is(p, T_RBRACK)) expect(p, T_COMMA);
        }
        if (tail)
            tail->next = e;
        else
            n->a = e;
        tail = e;
    }
    next(p);
    return n;
}

static Node *parse_primary(P *p) {
    Tok t = p->t;
    switch (t.t) {
    case T_NUM: {
        if (t.octal && strict(p)) p_error(p, "Octal literals are not allowed in strict mode.");
        Node *n = mk(p, N_NUM);
        n->u.num = t.num;
        next(p);
        return n;
    }
    case T_BIGINT: {
        Node *n = mk(p, N_BIGINT);
        n->u.atom = t.s;
        next(p);
        return n;
    }
    case T_STR: {
        if (t.octal && strict(p)) p_error(p, "Octal escape sequences are not allowed in strict mode.");
        Node *n = mk(p, N_STR);
        n->u.atom = t.s;
        next(p);
        return n;
    }
    case T_TEMPLATE: return parse_template(p, false);
    case T_SLASH:
    case T_DIV_A: {
        rescan_regex(p);
        Node *n = mk(p, N_REGEXP);
        n->u.atom = p->t.s;
        n->str2 = p->t.raw;
        /* validate early */
        uint32_t fl;
        if (re_parse_flags(vstr(p->t.raw), &fl) < 0) p_error(p, "Invalid regular expression flags");
        JSString *err = 0;
        Regex *re = re_compile(p->rt, vstr(p->t.s), fl, &err);
        if (!re) p_error(p, "Invalid regular expression: /%S/%S: %S", vstr(p->t.s), vstr(p->t.raw), err);
        re_free(re);
        next(p);
        return n;
    }
    case T_LPAREN: return parse_paren(p, false, t.pos, t.line);
    case T_LBRACK: return parse_array_lit(p);
    case T_LBRACE: return parse_object_lit(p);
    case T_IDENT: {
        jsval a = t.s;
        if (!t.escaped) {
            if (a == A(function)) {
                next(p);
                uint32_t f = 0;
                if (is(p, T_STAR)) {
                    f |= FN_GEN;
                    next(p);
                }
                jsval nm = JS_UNDEF;
                if (is(p, T_IDENT)) {
                    nm = p->t.s;
                    next(p);
                }
                FuncNode *fn = parse_function_rest(p, f | FN_EXPR, nm, t.pos, t.line, 0);
                Node *n = mk(p, N_FUNC);
                n->u.fn = fn;
                n->line = t.line;
                return n;
            }
            if (a == A(async)) {
                Tok nt = peek(p);
                if (!nt.nl && nt.t == T_IDENT && nt.s == A(function) && !nt.escaped) {
                    next(p);
                    next(p);
                    uint32_t f = FN_ASYNC;
                    if (is(p, T_STAR)) {
                        f |= FN_GEN;
                        next(p);
                    }
                    jsval nm = JS_UNDEF;
                    if (is(p, T_IDENT)) {
                        nm = p->t.s;
                        next(p);
                    }
                    FuncNode *fn = parse_function_rest(p, f | FN_EXPR, nm, t.pos, t.line, 0);
                    Node *n = mk(p, N_FUNC);
                    n->u.fn = fn;
                    n->line = t.line;
                    return n;
                }
                if (!nt.nl && nt.t == T_IDENT && !is_reserved(p, nt.s)) {
                    /* async x => ... */
                    next(p);
                    Mark m = mark(p);
                    Node *param = mk_ref(p, p->t.s);
                    next(p);
                    if (!is(p, T_ARROW)) unexpected(p);
                    return finish_arrow(p, param, m, t.pos, t.line, true);
                }
                if (!nt.nl && nt.t == T_LPAREN) {
                    next(p);
                    Mark m = mark(p);
                    /* parse as call args, possibly an arrow */
                    Node *args = parse_args(p);
                    if (is(p, T_ARROW) && !p->t.nl) {
                        /* convert args */
                        p->nrefs = p->nrefs; /* keep refs; finish_arrow re-homes */
                        (void)0;
                        return finish_arrow(p, args ? args : 0, m, t.pos, t.line, true);
                    }
                    Node *callee = mk_ref(p, A(async));
                    callee->line = t.line;
                    Node *c = mk(p, N_CALL);
                    c->a = callee;
                    c->b = args;
                    c->line = t.line;
                    return c;
                }
            }
            if (a == A(class)) return parse_class(p, true);
            if (a == A(this)) {
                next(p);
                Node *n = mk_ref(p, A(this));
                n->kind = N_THIS;
                n->line = t.line;
                n->pos = t.pos;
                return n;
            }
            if (a == A(null)) {
                next(p);
                return mk(p, N_NULL);
            }
            if (a == A(true)) {
                next(p);
                return mk(p, N_TRUE);
            }
            if (a == A(false)) {
                next(p);
                return mk(p, N_FALSE);
            }
            if (a == A(super)) {
                next(p);
                if (is(p, T_LPAREN)) {
                    if (!(p->fn->flags & FN_DERIVED) && !(p->fn->flags & FN_ARROW)) p_error(p, "'super' keyword unexpected here");
                    Node *n = mk(p, N_SUPER_CALL);
                    n->line = t.line;
                    n->b = parse_args(p);
                    /* references needed by codegen */
                    n->a = mk_ref(p, A(pfunc));
                    n->c = mk_ref(p, A(new_target));
                    n->d = mk_ref(p, A(this));
                    return n;
                }
                Node *n = mk(p, N_SUPER_MEMBER);
                n->line = t.line;
                if (is(p, T_DOT)) {
                    next(p);
                    n->u.atom = member_name(p);
                } else if (is(p, T_LBRACK)) {
                    next(p);
                    n->flags |= NF_COMPUTED;
                    n->b = parse_expr(p, false);
                    expect(p, T_RBRACK);
                } else
                    p_error(p, "'super' keyword unexpected here");
                n->a = mk_ref(p, A(pfunc));
                n->c = mk_ref(p, A(this));
                return n;
            }
            if (a == A(new)) unexpected(p);
            if (is_reserved(p, a) && !(a == A(yield) && !(p->fn->flags & FN_GEN)) && !(a == A(await) && !(p->fn->flags & FN_ASYNC)))
                unexpected(p);
        }
        /* identifier, maybe arrow */
        Mark m = mark(p);
        Node *r = mk_ref(p, a);
        next(p);
        if (is(p, T_ARROW) && !p->t.nl) return finish_arrow(p, r, m, t.pos, t.line, false);
        return r;
    }
    case T_PRIVATE: unexpected(p);
    default: unexpected(p);
    }
}

static Node *parse_new(P *p) {
    Tok t = p->t;
    next(p);
    if (is(p, T_DOT)) {
        next(p);
        if (!is_id(p, A(target))) unexpected(p);
        next(p);
        Node *n = mk_ref(p, A(new_target));
        n->kind = N_NEW_TARGET;
        n->line = t.line;
        return parse_call_tail(p, n, false);
    }
    Node *callee;
    CHK_DEPTH(p);
    if (is_id(p, A(new)))
        callee = parse_new(p);
    else
        callee = parse_primary(p);
    END_DEPTH(p);
    callee = parse_call_tail(p, callee, true);
    Node *n = mk(p, N_NEW);
    n->line = t.line;
    n->a = callee;
    if (is(p, T_LPAREN)) n->b = parse_args(p);
    return n;
}

static Node *parse_lhs(P *p) {
    Node *e;
    if (is_id(p, A(new)))
        e = parse_new(p);
    else
        e = parse_primary(p);
    return parse_call_tail(p, e, false);
}

static Node *parse_unary(P *p) {
    CHK_DEPTH(p);
    Node *r;
    int op = 0;
    switch (p->t.t) {
    case T_MINUS: op = OPR_NEG; break;
    case T_PLUS: op = OPR_PLUS; break;
    case T_BANG: op = OPR_NOT; break;
    case T_TILDE: op = OPR_BITNOT; break;
    case T_IDENT:
        if (p->t.escaped) break;
        if (p->t.s == A(typeof)) op = OPR_TYPEOF;
        else if (p->t.s == A(void)) op = OPR_VOID;
        else if (p->t.s == A(delete)) op = OPR_DELETE;
        else if (p->t.s == A(await) && (p->fn->flags & FN_ASYNC)) {
            Node *n = mk(p, N_AWAIT);
            next(p);
            n->a = parse_unary(p);
            END_DEPTH(p);
            return n;
        }
        break;
    case T_INC:
    case T_DEC: {
        Node *n = mk(p, N_UPDATE);
        n->u.op = is(p, T_INC) ? OPR_INC : OPR_DEC;
        n->flags |= NF_PREFIX;
        next(p);
        n->a = parse_unary(p);
        if (n->a->kind != N_IDENT && n->a->kind != N_MEMBER && n->a->kind != N_INDEX && n->a->kind != N_PRIV_MEMBER &&
            n->a->kind != N_SUPER_MEMBER)
            p_error_line(p, n->line, "Invalid left-hand side expression in prefix operation");
        END_DEPTH(p);
        return n;
    }
    default: break;
    }
    if (op) {
        Node *n = mk(p, N_UNARY);
        n->u.op = op;
        next(p);
        n->a = parse_unary(p);
        if (op == OPR_DELETE && strict(p) && n->a->kind == N_IDENT)
            p_error(p, "Delete of an unqualified identifier in strict mode.");
        END_DEPTH(p);
        return n;
    }
    Node *e = parse_lhs(p);
    if ((is(p, T_INC) || is(p, T_DEC)) && !p->t.nl) {
        if (e->kind != N_IDENT && e->kind != N_MEMBER && e->kind != N_INDEX && e->kind != N_PRIV_MEMBER && e->kind != N_SUPER_MEMBER)
            p_error(p, "Invalid left-hand side expression in postfix operation");
        Node *n = mk(p, N_UPDATE);
        n->u.op = is(p, T_INC) ? OPR_INC : OPR_DEC;
        n->a = e;
        n->line = e->line;
        next(p);
        e = n;
    }
    r = e;
    END_DEPTH(p);
    return r;
}

static Node *parse_cond(P *p, bool noin) {
    Node *c = parse_binary(p, 0, noin);
    if (!is(p, T_QUESTION)) return c;
    Node *n = mk(p, N_COND);
    next(p);
    n->a = c;
    n->line = c->line;
    n->b = parse_assign(p, false);
    expect(p, T_COLON);
    n->c = parse_assign(p, noin);
    return n;
}

static Node *parse_assign(P *p, bool noin) {
    CHK_DEPTH(p);
    if (is_id(p, A(yield)) && (p->fn->flags & FN_GEN) && !(p->fn->flags & FN_ARROW)) {
        Node *n = mk(p, N_YIELD);
        next(p);
        if (is(p, T_STAR)) {
            next(p);
            n->flags |= NF_DELEGATE;
            n->a = parse_assign(p, noin);
        } else if (!p->t.nl && !is(p, T_RPAREN) && !is(p, T_RBRACK) && !is(p, T_RBRACE) && !is(p, T_COMMA) &&
                   !is(p, T_SEMI) && !is(p, T_COLON) && !is(p, T_EOF) && !is_id(p, A(in)) && !is(p, T_QUESTION))
            n->a = parse_assign(p, noin);
        END_DEPTH(p);
        return n;
    }
    Node *left = parse_cond(p, noin);
    int op = -1;
    switch (p->t.t) {
    case T_ASSIGN: op = 0; break;
    case T_ADD_A: op = OPR_ADD; break;
    case T_SUB_A: op = OPR_SUB; break;
    case T_MUL_A: op = OPR_MUL; break;
    case T_DIV_A: op = OPR_DIV; break;
    case T_MOD_A: op = OPR_MOD; break;
    case T_POW_A: op = OPR_POW; break;
    case T_SHL_A: op = OPR_SHL; break;
    case T_SAR_A: op = OPR_SAR; break;
    case T_SHR_A: op = OPR_SHR; break;
    case T_AND_A: op = OPR_BAND; break;
    case T_OR_A: op = OPR_BOR; break;
    case T_XOR_A: op = OPR_BXOR; break;
    case T_LAND_A: op = OPR_AND; break;
    case T_LOR_A: op = OPR_OR; break;
    case T_NULLISH_A: op = OPR_NULLISH; break;
    default: break;
    }
    if (op < 0) {
        END_DEPTH(p);
        return left;
    }
    Node *n = mk(p, N_ASSIGN);
    n->line = left->line;
    n->u.op = op;
    if (op == 0 && (left->kind == N_OBJECT || left->kind == N_ARRAY) && !(left->flags & NF_PAREN))
        left = to_pattern(p, left, false);
    else if (left->kind == N_IDENT) {
        if (strict(p) && (left->u.atom == A(eval) || left->u.atom == A(arguments)))
            p_error(p, "Unexpected eval or arguments in strict mode");
    } else if (left->kind != N_MEMBER && left->kind != N_INDEX && left->kind != N_PRIV_MEMBER && left->kind != N_SUPER_MEMBER)
        p_error_line(p, left->line, "Invalid left-hand side in assignment");
    n->a = left;
    next(p);
    n->b = parse_assign(p, noin);
    END_DEPTH(p);
    return n;
}

static Node *parse_expr(P *p, bool noin) {
    Node *e = parse_assign(p, noin);
    if (!is(p, T_COMMA)) return e;
    Node *s = mk(p, N_SEQ);
    s->line = e->line;
    s->a = e;
    Node *tail = e;
    while (is(p, T_COMMA)) {
        next(p);
        Node *x = parse_assign(p, noin);
        tail->next = x;
        tail = x;
    }
    return s;
}

/* ---------------- binding patterns ---------------- */
static Node *parse_binding_elem(P *p) {
    Node *t = parse_binding_target(p);
    if (is(p, T_ASSIGN)) {
        next(p);
        Node *n = mk(p, N_ASSIGN_PAT);
        n->line = t->line;
        n->a = t;
        n->b = parse_assign(p, false);
        return n;
    }
    return t;
}
static Node *parse_binding_target(P *p) {
    if (is(p, T_LBRACK)) {
        Node *n = mk(p, N_ARRAY_PAT);
        next(p);
        Node *tail = 0;
        while (!is(p, T_RBRACK)) {
            Node *e;
            if (is(p, T_COMMA)) {
                e = mk(p, N_HOLE);
                next(p);
            } else {
                if (is(p, T_ELLIPSIS)) {
                    e = mk(p, N_REST);
                    next(p);
                    e->a = parse_binding_target(p);
                    if (!is(p, T_RBRACK)) p_error(p, "Rest element must be last element");
                } else
                    e = parse_binding_elem(p);
                if (!is(p, T_RBRACK)) expect(p, T_COMMA);
            }
            if (tail)
                tail->next = e;
            else
                n->a = e;
            tail = e;
        }
        next(p);
        return n;
    }
    if (is(p, T_LBRACE)) {
        Node *n = mk(p, N_OBJECT_PAT);
        next(p);
        Node *tail = 0;
        while (!is(p, T_RBRACE)) {
            Node *e;
            if (is(p, T_ELLIPSIS)) {
                e = mk(p, N_REST);
                next(p);
                if (!is(p, T_IDENT)) unexpected(p);
                e->a = mk_ref(p, p->t.s);
                next(p);
            } else {
                e = mk(p, N_PROP_PAT);
                Tok kt = p->t;
                if (is(p, T_LBRACK)) {
                    next(p);
                    e->flags |= NF_COMPUTED;
                    e->a = parse_assign(p, false);
                    expect(p, T_RBRACK);
                } else if (is(p, T_IDENT) || is(p, T_STR) || is(p, T_NUM) || is(p, T_BIGINT)) {
                    Node *k = mk(p, N_STR);
                    k->u.atom = prop_name_atom(p, &p->t);
                    e->a = k;
                    next(p);
                } else
                    unexpected(p);
                if (is(p, T_COLON)) {
                    next(p);
                    e->b = parse_binding_elem(p);
                } else {
                    if (kt.t != T_IDENT || (e->flags & NF_COMPUTED)) unexpected(p);
                    if (is_reserved(p, kt.s)) {
                        p->t = kt;
                        unexpected(p);
                    }
                    Node *r = mk_ref(p, kt.s);
                    r->line = kt.line;
                    if (is(p, T_ASSIGN)) {
                        next(p);
                        Node *ap = mk(p, N_ASSIGN_PAT);
                        ap->a = r;
                        ap->b = parse_assign(p, false);
                        e->b = ap;
                    } else
                        e->b = r;
                }
            }
            if (tail)
                tail->next = e;
            else
                n->a = e;
            tail = e;
            if (!is(p, T_RBRACE)) expect(p, T_COMMA);
        }
        next(p);
        return n;
    }
    if (is(p, T_IDENT)) {
        jsval a = p->t.s;
        if (is_reserved(p, a) || (strict(p) && (a == A(eval) || a == A(arguments)))) {
            if (a == A(eval) || a == A(arguments)) p_error(p, "Unexpected eval or arguments in strict mode");
            unexpected(p);
        }
        Node *r = mk_ref(p, a);
        next(p);
        return r;
    }
    unexpected(p);
}

/* ---------------- functions ---------------- */
static Node *parse_body(P *p, bool allow_directives);

static void add_simple_param_check(P *p, FuncNode *fn) {
    if (!fn->simple_params && (fn->flags & FN_STRICT)) {
        /* "use strict" in function with non-simple params is an error only when the directive is in that function */
    }
}

/* parses "(params) { body }" (or arrow body). cover_params: (Node*)1 = arrow with no params,
 * non-NULL = arrow with params given as a cover expression list */
static FuncNode *parse_function_rest(P *p, uint32_t flags, jsval name, uint32_t start, int line, Node *cover_params) {
    CHK_DEPTH(p);
    FuncNode *fn;
    Scope *outer = p->sc;
    FuncNode *ofn = p->fn;
    bool in_class = p->in_class_body;
    p->in_class_body = false;
    if (flags & FN_ARROW) {
        /* function node and scope were created by finish_arrow */
        fn = p->pending_arrow;
        p->pending_arrow = 0;
        p->fn = fn;
        p->sc = fn->scope;
    } else {
        fn = (FuncNode *)pal(p, sizeof(FuncNode));
        fn->flags = flags | (ofn->flags & FN_STRICT);
        fn->parent = ofn;
        fn->name = name;
        fn->line = line;
        fn->src_start = start;
        grow(p, (void ***)&p->funcs, &p->cfuncs, p->nfuncs);
        p->funcs[p->nfuncs++] = fn;
        p->fn = fn;
        Scope *fs = new_scope(p, SC_FUNC);
        fs->func = fn;
        fn->scope = fs;
        p->sc = fs;
    }
    fn->simple_params = true;
    int count = 0;
    bool seen_default = false;
    Node *tail = 0;
    if (flags & FN_ARROW) {
        if (cover_params != (Node *)1) {
            for (Node *e = cover_params; e;) {
                Node *nx = e->next;
                Node *pat;
                if (e->kind == N_SPREAD) {
                    pat = mk(p, N_REST);
                    pat->a = to_pattern(p, e->a, true);
                    fn->has_rest = true;
                    if (nx) p_error(p, "Rest parameter must be last formal parameter");
                } else
                    pat = to_pattern(p, e, true);
                pat->next = 0;
                if (pat->kind != N_IDENT) fn->simple_params = false;
                if (pat->kind == N_ASSIGN_PAT || pat->kind == N_REST) seen_default = true;
                if (!seen_default) fn->length++;
                if (pat->kind != N_REST) count++;
                if (tail)
                    tail->next = pat;
                else
                    fn->params = pat;
                tail = pat;
                e = nx;
            }
            for (Node *e = fn->params; e; e = e->next) declare_pattern(p, e, B_PARAM);
        }
    } else {
        expect(p, T_LPAREN);
        while (!is(p, T_RPAREN)) {
            Node *pat;
            if (is(p, T_ELLIPSIS)) {
                next(p);
                pat = mk(p, N_REST);
                pat->a = parse_binding_target(p);
                fn->has_rest = true;
                if (!is(p, T_RPAREN)) p_error(p, "Rest parameter must be last formal parameter");
            } else
                pat = parse_binding_elem(p);
            if (pat->kind != N_IDENT) fn->simple_params = false;
            if (pat->kind == N_ASSIGN_PAT || pat->kind == N_REST) seen_default = true;
            if (!seen_default) fn->length++;
            if (pat->kind != N_REST) count++;
            if (tail)
                tail->next = pat;
            else
                fn->params = pat;
            tail = pat;
            if (!is(p, T_RPAREN)) expect(p, T_COMMA);
        }
        next(p);
        for (Node *e = fn->params; e; e = e->next) declare_pattern(p, e, B_PARAM);
        if ((flags & FN_GETTER) && fn->params) p_error(p, "Getter must not have any formal parameters.");
        if ((flags & FN_SETTER) && (count != 1 || fn->has_rest)) p_error(p, "Setter must have exactly one formal parameter.");
    }
    fn->nparams = count;
    if ((flags & FN_ARROW) && !is(p, T_LBRACE)) {
        fn->flags |= FN_EXPR_BODY;
        Node *r = mk(p, N_RETURN);
        r->a = parse_assign(p, false);
        fn->body = r;
    } else {
        expect(p, T_LBRACE);
        fn->body = parse_body(p, true);
        if (!is(p, T_RBRACE)) unexpected(p);
        next(p);
    }
    if (!fn->body) fn->body = mk(p, N_EMPTY);
    fn->src_end = p->prev_end;
    /* named function expression: self binding */
    if ((flags & FN_EXPR) && !is_undef(name) && !find_in(fn->scope, name)) {
        fn->name_b = add_binding(p, fn->scope, name, B_FUNCNAME);
    }
    add_simple_param_check(p, fn);
    p->fn = ofn;
    p->sc = outer;
    p->in_class_body = in_class;
    END_DEPTH(p);
    return fn;
}

static Node *parse_body(P *p, bool allow_directives) {
    Node *head = 0, *tail = 0;
    bool directives = allow_directives;
    while (!is(p, T_RBRACE) && !is(p, T_EOF)) {
        if (directives) {
            if (is(p, T_STR)) {
                Tok t = p->t;
                bool is_strict = (t.end - t.pos == 12) && memcmp(p->s + t.pos + 1, "use strict", 10) == 0;
                Node *s = parse_stmt(p);
                if (is_strict && s->kind == N_EXPR && s->a->kind == N_STR) {
                    if (!p->fn->simple_params) p_error(p, "Illegal 'use strict' directive in function with non-simple parameter list");
                    p->fn->flags |= FN_STRICT;
                }
                if (!(s->kind == N_EXPR && s->a->kind == N_STR)) directives = false;
                if (tail)
                    tail->next = s;
                else
                    head = s;
                tail = s;
                while (tail->next) tail = tail->next;
                continue;
            }
            directives = false;
        }
        Node *s = parse_stmt(p);
        if (tail)
            tail->next = s;
        else
            head = s;
        tail = s;
        while (tail->next) tail = tail->next;
    }
    return head;
}

/* ---------------- classes ---------------- */
static FuncNode *synth_func(P *p, uint32_t flags, Scope *parent) {
    FuncNode *fn = (FuncNode *)pal(p, sizeof(FuncNode));
    fn->flags = flags | FN_STRICT;
    fn->parent = p->fn;
    fn->name = JS_UNDEF;
    fn->line = p->t.line;
    fn->simple_params = true;
    grow(p, (void ***)&p->funcs, &p->cfuncs, p->nfuncs);
    p->funcs[p->nfuncs++] = fn;
    Scope *saved = p->sc;
    FuncNode *sfn = p->fn;
    p->sc = parent;
    p->fn = fn;
    Scope *s = new_scope(p, SC_FUNC);
    s->func = fn;
    fn->scope = s;
    p->sc = saved;
    p->fn = sfn;
    return fn;
}

static Node *hidden_ref(P *p, jsval name, Scope *sc, FuncNode *fn) {
    Scope *ss = p->sc;
    FuncNode *sf = p->fn;
    p->sc = sc;
    p->fn = fn;
    Node *r = mk_ref(p, name);
    p->sc = ss;
    p->fn = sf;
    return r;
}
static jsval hidden_name(P *p, const char *pfx) {
    char buf[32];
    int n = 0;
    while (*pfx) buf[n++] = *pfx++;
    int v = p->hidden++;
    char t[12];
    int k = 0;
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k) buf[n++] = t[--k];
    buf[n] = 0;
    return atom_c(p->rt, buf);
}

static Node *parse_class(P *p, bool is_expr) {
    Tok ct = p->t;
    next(p); /* class */
    ClassNode *cls = (ClassNode *)pal(p, sizeof(ClassNode));
    cls->src_start = ct.pos;
    cls->name = JS_UNDEF;
    uint32_t saved_flags = p->fn->flags;
    p->fn->flags |= FN_STRICT;
    if (is(p, T_IDENT) && !is_id(p, A(extends))) {
        if (is_reserved(p, p->t.s)) unexpected(p);
        cls->name = p->t.s;
        next(p);
    } else if (!is_expr)
        unexpected(p);
    Node *decl = mk(p, is_expr ? N_CLASS : N_CLASS_DECL);
    decl->line = ct.line;
    decl->pos = ct.pos;
    decl->u.cls = cls;
    if (!is_expr) declare_lex(p, cls->name, B_CLASS);
    Scope *outer = p->sc;
    Scope *cs = new_scope(p, SC_CLASS);
    cls->scope = cs;
    p->sc = cs;
    if (!is_undef(cls->name)) {
        cls->name_b = add_binding(p, cs, cls->name, B_CONST);
    }
    if (is_id(p, A(extends))) {
        next(p);
        cls->heritage = parse_lhs(p);
    }
    expect(p, T_LBRACE);
    Node *mtail = 0;
    Node *fields_pm = 0, *fields_pm_tail = 0, *fields = 0, *fields_tail = 0, *statics = 0, *statics_tail = 0;
    while (!is(p, T_RBRACE)) {
        if (is(p, T_SEMI)) {
            next(p);
            continue;
        }
        Node *m = mk(p, N_CLASS_MEMBER);
        uint32_t mstart = p->t.pos;
        int mline = p->t.line;
        bool is_static = false;
        uint32_t fflags = 0;
        int kind = PK_METHOD;
        if (is_id(p, A(static))) {
            Tok nt = peek(p);
            if (nt.t != T_LPAREN && nt.t != T_ASSIGN && nt.t != T_SEMI && nt.t != T_RBRACE && !(nt.nl && nt.t != T_IDENT && nt.t != T_LBRACE && nt.t != T_LBRACK && nt.t != T_PRIVATE && nt.t != T_STAR && nt.t != T_STR && nt.t != T_NUM)) {
                is_static = true;
                next(p);
                if (is(p, T_LBRACE)) {
                    /* static block */
                    if (!cls->static_init) cls->static_init = synth_func(p, FN_METHOD | FN_STATIC_INIT, cs);
                    FuncNode *blk = synth_func(p, FN_METHOD | FN_STATIC_INIT, cls->static_init->scope);
                    Scope *ss = p->sc;
                    FuncNode *sf = p->fn;
                    p->fn = blk;
                    p->sc = blk->scope;
                    blk->src_start = p->t.pos;
                    next(p);
                    blk->body = parse_body(p, false);
                    expect(p, T_RBRACE);
                    blk->src_end = p->prev_end;
                    p->fn = sf;
                    p->sc = ss;
                    Node *sb = mk(p, N_CLASS_MEMBER);
                    sb->u.op = PK_STATIC_BLOCK;
                    sb->flags = NF_STATIC;
                    Node *fnn = mk(p, N_FUNC);
                    fnn->u.fn = blk;
                    sb->b = fnn;
                    if (statics_tail)
                        statics_tail->next = sb;
                    else
                        statics = sb;
                    statics_tail = sb;
                    continue;
                }
            }
        }
        if (is_id(p, A(async))) {
            Tok nt = peek(p);
            if (!nt.nl && nt.t != T_LPAREN && nt.t != T_ASSIGN && nt.t != T_SEMI && nt.t != T_RBRACE) {
                fflags |= FN_ASYNC;
                next(p);
            }
        }
        if (is(p, T_STAR)) {
            fflags |= FN_GEN;
            next(p);
        }
        if (!fflags && (is_id(p, A(get)) || is_id(p, A(set)))) {
            Tok nt = peek(p);
            if (nt.t != T_LPAREN && nt.t != T_ASSIGN && nt.t != T_SEMI && nt.t != T_RBRACE && !(nt.nl && nt.t == T_IDENT && false)) {
                kind = is_id(p, A(get)) ? PK_GET : PK_SET;
                next(p);
            }
        }
        /* key */
        Tok kt = p->t;
        bool is_private = false;
        if (is(p, T_LBRACK)) {
            next(p);
            m->flags |= NF_COMPUTED;
            m->a = parse_assign(p, false);
            expect(p, T_RBRACK);
        } else if (is(p, T_PRIVATE)) {
            is_private = true;
            m->flags |= NF_PRIVATE;
            jsval pn = p->t.s;
            if (pn == atom_c(p->rt, "#constructor")) p_error(p, "Classes may not have a private field named '#constructor'");
            Binding *b = find_in(cs, pn);
            if (b) {
                bool ok = b->kind == B_PRIVATE && ((kind == PK_GET && b->param_index == PK_SET) || (kind == PK_SET && b->param_index == PK_GET));
                if (!ok) redeclared(p, pn);
                b->param_index = -2;
            } else {
                b = add_binding(p, cs, pn, B_PRIVATE);
                b->param_index = kind;
                b->tdz = false;
                b->is_const = true;
            }
            Node *k = mk(p, N_STR);
            k->u.atom = pn;
            m->a = k;
            next(p);
        } else if (is(p, T_IDENT) || is(p, T_STR) || is(p, T_NUM) || is(p, T_BIGINT)) {
            Node *k = mk(p, N_STR);
            k->u.atom = prop_name_atom(p, &p->t);
            m->a = k;
            next(p);
        } else
            unexpected(p);
        if (is_static) m->flags |= NF_STATIC;
        if (is(p, T_LPAREN)) {
            bool is_ctor = !is_static && !(m->flags & (NF_COMPUTED | NF_PRIVATE)) && m->a->u.atom == A(constructor) &&
                           kt.t != T_NUM;
            if (is_ctor) {
                if (kind != PK_METHOD || fflags) p_error(p, "Class constructor may not be a%s", kind == PK_GET ? " getter" : kind == PK_SET ? " setter" : fflags & FN_ASYNC ? "n async method" : " generator");
                if (cls->ctor) p_error(p, "A class may only have one constructor");
                uint32_t f = FN_CTOR | FN_STRICT | (cls->heritage ? FN_DERIVED : 0);
                FuncNode *fn = parse_function_rest(p, f, is_undef(cls->name) ? A(empty) : cls->name, ct.pos, mline, 0);
                cls->ctor = fn;
                fn->cls = cls;
                continue;
            }
            uint32_t f = FN_METHOD | fflags | FN_STRICT;
            if (kind == PK_GET) f |= FN_GETTER;
            if (kind == PK_SET) f |= FN_SETTER;
            jsval nm = (m->flags & NF_COMPUTED) ? JS_UNDEF : m->a->u.atom;
            FuncNode *fn = parse_function_rest(p, f, nm, mstart, mline, 0);
            Node *fnode = mk(p, N_FUNC);
            fnode->u.fn = fn;
            m->b = fnode;
            m->u.op = kind;
            if (is_private && !is_static) {
                /* closure stored in a hidden binding, installed on instances by the field initializer */
                jsval hn = hidden_name(p, "%pm");
                Binding *hb = add_binding(p, cs, hn, B_HIDDEN);
                hb->tdz = false;
                m->c = hidden_ref(p, hn, cs, p->fn);
                if (!cls->field_init) cls->field_init = synth_func(p, FN_METHOD | FN_FIELD_INIT, cs);
                Node *fe = mk(p, N_CLASS_MEMBER);
                fe->u.op = kind;
                fe->flags = NF_PRIVATE;
                fe->a = hidden_ref(p, m->a->u.atom, cls->field_init->scope, cls->field_init);
                fe->b = hidden_ref(p, hn, cls->field_init->scope, cls->field_init);
                if (fields_pm_tail)
                    fields_pm_tail->next = fe;
                else
                    fields_pm = fe;
                fields_pm_tail = fe;
            }
            if (is_private) m->d = hidden_ref(p, m->a->u.atom, cs, p->fn);
            if (mtail)
                mtail->next = m;
            else
                cls->members = m;
            mtail = m;
            continue;
        }
        /* field */
        if (kind != PK_METHOD || fflags) unexpected(p);
        if (!(m->flags & (NF_COMPUTED | NF_PRIVATE)) && (m->a->u.atom == A(constructor) || (is_static && m->a->u.atom == A(prototype))))
            p_error(p, "Classes may not have a field named '%S'", vstr(m->a->u.atom));
        m->u.op = PK_FIELD;
        FuncNode **target = is_static ? &cls->static_init : &cls->field_init;
        if (!*target) *target = synth_func(p, FN_METHOD | (is_static ? FN_STATIC_INIT : FN_FIELD_INIT), cs);
        FuncNode *ifn = *target;
        Node *fe = mk(p, N_CLASS_MEMBER);
        fe->u.op = PK_FIELD;
        fe->flags = m->flags;
        fe->line = mline;
        if (m->flags & NF_COMPUTED) {
            jsval hn = hidden_name(p, "%fk");
            Binding *hb = add_binding(p, cs, hn, B_HIDDEN);
            hb->tdz = false;
            m->c = hidden_ref(p, hn, cs, p->fn); /* class-definition time: store key here */
            fe->a = hidden_ref(p, hn, ifn->scope, ifn);
        } else if (m->flags & NF_PRIVATE) {
            fe->a = hidden_ref(p, m->a->u.atom, ifn->scope, ifn);
        } else
            fe->a = m->a;
        if (is(p, T_ASSIGN)) {
            next(p);
            Scope *ss = p->sc;
            FuncNode *sf = p->fn;
            p->fn = ifn;
            p->sc = ifn->scope;
            fe->b = parse_assign(p, false);
            p->fn = sf;
            p->sc = ss;
        }
        semi(p);
        if (is_static) {
            if (statics_tail)
                statics_tail->next = fe;
            else
                statics = fe;
            statics_tail = fe;
        } else {
            if (fields_tail)
                fields_tail->next = fe;
            else
                fields = fe;
            fields_tail = fe;
        }
        /* member entry for computed key evaluation order */
        if (m->flags & NF_COMPUTED) {
            if (mtail)
                mtail->next = m;
            else
                cls->members = m;
            mtail = m;
        }
    }
    next(p);
    cls->src_end = p->prev_end;
    if (fields_pm_tail) {
        fields_pm_tail->next = fields;
        fields = fields_pm;
    }
    if (cls->field_init) cls->field_init->fields = fields;
    if (cls->static_init) cls->static_init->fields = statics;
    if (!cls->ctor) {
        FuncNode *fn = synth_func(p, FN_CTOR | FN_DEFAULT_CTOR | (cls->heritage ? FN_DERIVED : 0), cs);
        fn->name = is_undef(cls->name) ? A(empty) : cls->name;
        fn->src_start = cls->src_start;
        fn->src_end = cls->src_end;
        fn->cls = cls;
        fn->body = mk(p, N_EMPTY);
        if (cls->heritage) {
            fn->has_rest = true;
            /* references used by the synthesized super(...args) */
            fn->fields = hidden_ref(p, A(pfunc), fn->scope, fn);
            Node *r2 = hidden_ref(p, A(new_target), fn->scope, fn);
            Node *r3 = hidden_ref(p, A(this), fn->scope, fn);
            fn->fields->next = r2;
            r2->next = r3;
        }
        cls->ctor = fn;
    }
    cls->ctor->src_start = cls->src_start;
    cls->ctor->src_end = cls->src_end;
    p->sc = outer;
    p->fn->flags = (p->fn->flags & ~(uint32_t)FN_STRICT) | (saved_flags & FN_STRICT);
    return decl;
}

/* ---------------- statements ---------------- */
static Node *parse_var_decl(P *p, int vk, bool noin, bool for_head) {
    Node *n = mk(p, N_VAR);
    n->u.op = vk;
    Node *tail = 0;
    for (;;) {
        Node *d = mk(p, N_DECL);
        d->a = parse_binding_target(p);
        if (vk == VK_LET || vk == VK_CONST) {
            if (d->a->kind == N_IDENT && d->a->u.atom == A(let)) p_error(p, "let is disallowed as a lexically bound name");
        }
        declare_pattern(p, d->a, vk == VK_VAR ? B_VAR : vk == VK_LET ? B_LET : B_CONST);
        if (is(p, T_ASSIGN)) {
            next(p);
            d->b = parse_assign(p, noin);
        } else if (!for_head) {
            if (vk == VK_CONST) p_error(p, "Missing initializer in const declaration");
            if (d->a->kind != N_IDENT) p_error(p, "Missing initializer in destructuring declaration");
        }
        if (tail)
            tail->next = d;
        else
            n->a = d;
        tail = d;
        if (!is(p, T_COMMA)) break;
        next(p);
    }
    return n;
}

static bool is_let_decl(P *p) {
    if (!is_id(p, A(let))) return false;
    Tok nt = peek(p);
    if (nt.t == T_LBRACK || nt.t == T_LBRACE) return true;
    if (nt.t == T_IDENT && !(nt.s == A(in) || nt.s == A(instanceof))) {
        if (nt.nl && (nt.s == A(yield) || nt.s == A(await))) return false;
        return true;
    }
    return false;
}

static Node *parse_function_decl(P *p, bool is_async) {
    Tok ft = p->t;
    if (is_async) next(p);
    next(p); /* function */
    uint32_t f = is_async ? FN_ASYNC : 0;
    if (is(p, T_STAR)) {
        f |= FN_GEN;
        next(p);
    }
    if (!is(p, T_IDENT)) unexpected(p);
    jsval name = p->t.s;
    if (is_reserved(p, name) && !(name == A(yield) || name == A(await))) unexpected(p);
    next(p);
    Node *n = mk(p, N_FUNC_DECL);
    n->line = ft.line;
    n->pos = ft.pos;
    /* declare before parsing body so recursion resolves */
    Scope *s = p->sc;
    Binding *b = 0;
    if (s->kind == SC_FUNC && !s->is_script) {
        b = find_in(s, name);
        if (b && (b->kind == B_LET || b->kind == B_CONST || b->kind == B_CLASS)) redeclared(p, name);
        if (b && b->kind != B_FUNCNAME) {
            b->kind = B_FUNC;
        } else if (b) {
            b->kind = B_FUNC;
        } else
            b = add_binding(p, s, name, B_FUNC);
    } else if (s->is_script) {
        if (list_find_atom(p->script->glex, name)) redeclared(p, name);
    } else {
        b = declare_lex(p, name, B_FUNC);
        if (!strict(p) && !f) {
            /* Annex B: also a var in the function scope, unless a lexical binding would conflict */
            Scope *q = s->parent;
            bool conflict = false;
            while (q && q->kind != SC_FUNC) {
                Binding *x = find_in(q, name);
                if (x && x->kind != B_VAR && x->kind != B_FUNC) conflict = true;
                q = q->parent;
            }
            if (!conflict) {
                Scope *fs = q;
                if (fs && fs->is_script) {
                    if (!list_find_atom(p->script->glex, name)) {
                        declare_var(p, name);
                        n->flags |= NF_ANNEXB;
                    }
                } else if (fs) {
                    Binding *x = find_in(fs, name);
                    if (!x || x->kind == B_VAR || x->kind == B_FUNC || x->kind == B_PARAM) {
                        if (!x) add_binding(p, fs, name, B_VAR);
                        n->flags |= NF_ANNEXB;
                    }
                }
            }
        }
    }
    FuncNode *fn = parse_function_rest(p, f, name, ft.pos, ft.line, 0);
    n->u.fn = fn;
    if (b) b->decl_fn = fn;
    if (s->is_script) {
        /* last declaration wins */
        Node *g = mk(p, N_FUNC_DECL);
        g->u.fn = fn;
        g->u.fn = fn;
        Node **pp = &p->script->gfuncs;
        while (*pp) {
            if ((*pp)->u.fn->name == name) {
                Node *rm = *pp;
                *pp = rm->next;
                continue;
            }
            pp = &(*pp)->next;
        }
        list_add(&p->script->gfuncs, g);
    }
    return n;
}

static Node *parse_block(P *p) {
    Node *n = mk(p, N_BLOCK);
    expect(p, T_LBRACE);
    Scope *outer = p->sc;
    n->scope = new_scope(p, SC_BLOCK);
    p->sc = n->scope;
    Node *tail = 0;
    while (!is(p, T_RBRACE)) {
        if (is(p, T_EOF)) unexpected(p);
        Node *s = parse_stmt(p);
        if (tail)
            tail->next = s;
        else
            n->a = s;
        tail = s;
        while (tail->next) tail = tail->next;
    }
    next(p);
    p->sc = outer;
    return n;
}

static Node *parse_for(P *p) {
    Tok ft = p->t;
    next(p);
    bool is_await = false;
    if (is_id(p, A(await)) && (p->fn->flags & FN_ASYNC)) {
        is_await = true;
        next(p);
    }
    expect(p, T_LPAREN);
    Scope *outer = p->sc;
    Scope *ls = new_scope(p, SC_BLOCK);
    p->sc = ls;
    Node *init = 0;
    bool decl = false;
    int vk = -1;
    if (is(p, T_SEMI)) {
    } else if (is_id(p, A(var)) || is_id(p, A(const)) || is_let_decl(p)) {
        vk = is_id(p, A(var)) ? VK_VAR : is_id(p, A(const)) ? VK_CONST : VK_LET;
        next(p);
        init = parse_var_decl(p, vk, true, true);
        decl = true;
    } else {
        Mark m = mark(p);
        (void)m;
        bool starts_let = is_id(p, A(let));
        init = parse_expr(p, true);
        if (starts_let && is_id(p, A(of))) unexpected(p);
    }
    Node *n;
    if (init && (is_id(p, A(of)) || (is_id(p, A(in)) && !is_await))) {
        bool is_of = is_id(p, A(of));
        n = mk(p, is_of ? N_FOR_OF : N_FOR_IN);
        n->line = ft.line;
        if (is_await) n->flags |= NF_AWAIT;
        if (decl) {
            if (init->a->next) p_error(p, "Invalid left-hand side in for-%s loop: Must have a single binding.", is_of ? "of" : "in");
            if (init->a->b && (is_of || strict(p) || vk != VK_VAR || init->a->a->kind != N_IDENT))
                p_error(p, "for-%s loop variable declaration may not have an initializer.", is_of ? "of" : "in");
        } else {
            if ((init->kind == N_OBJECT || init->kind == N_ARRAY) && !(init->flags & NF_PAREN))
                init = to_pattern(p, init, false);
            else if (init->kind != N_IDENT && init->kind != N_MEMBER && init->kind != N_INDEX && init->kind != N_PRIV_MEMBER &&
                     init->kind != N_SUPER_MEMBER)
                p_error(p, "Invalid left-hand side in for-%s loop", is_of ? "of" : "in");
        }
        next(p);
        n->a = init;
        p->sc = outer; /* the iterated expression is evaluated outside the loop scope... */
        p->sc = ls;    /* ...but TDZ-wise inside; keeping it inside is simpler and equivalent here */
        n->b = is_of ? parse_assign(p, false) : parse_expr(p, false);
        expect(p, T_RPAREN);
        n->d = parse_stmt(p);
        n->scope = ls;
        p->sc = outer;
        return n;
    }
    if (is_await) unexpected(p);
    n = mk(p, N_FOR);
    n->line = ft.line;
    n->a = init;
    expect(p, T_SEMI);
    if (!is(p, T_SEMI)) n->b = parse_expr(p, false);
    expect(p, T_SEMI);
    if (!is(p, T_RPAREN)) n->c = parse_expr(p, false);
    expect(p, T_RPAREN);
    n->d = parse_stmt(p);
    n->scope = ls;
    p->sc = outer;
    return n;
}

static Node *parse_stmt(P *p) {
    CHK_DEPTH(p);
    Node *r = 0;
    Tok t = p->t;
    switch (t.t) {
    case T_LBRACE: r = parse_block(p); break;
    case T_SEMI:
        r = mk(p, N_EMPTY);
        next(p);
        break;
    case T_IDENT:
        if (t.escaped) goto expr;
        if (t.s == A(var)) {
            next(p);
            r = parse_var_decl(p, VK_VAR, false, false);
            semi(p);
        } else if (t.s == A(const)) {
            next(p);
            r = parse_var_decl(p, VK_CONST, false, false);
            semi(p);
        } else if (is_let_decl(p)) {
            next(p);
            r = parse_var_decl(p, VK_LET, false, false);
            semi(p);
        } else if (t.s == A(function)) {
            r = parse_function_decl(p, false);
        } else if (t.s == A(async) && peek(p).t == T_IDENT && peek(p).s == A(function) && !peek(p).nl) {
            r = parse_function_decl(p, true);
        } else if (t.s == A(class)) {
            r = parse_class(p, false);
        } else if (t.s == A(if)) {
            r = mk(p, N_IF);
            next(p);
            expect(p, T_LPAREN);
            r->a = parse_expr(p, false);
            expect(p, T_RPAREN);
            r->b = parse_stmt(p);
            if (is_id(p, A(else))) {
                next(p);
                r->c = parse_stmt(p);
            }
        } else if (t.s == A(for)) {
            r = parse_for(p);
        } else if (t.s == A(while)) {
            r = mk(p, N_WHILE);
            next(p);
            expect(p, T_LPAREN);
            r->a = parse_expr(p, false);
            expect(p, T_RPAREN);
            r->b = parse_stmt(p);
        } else if (t.s == A(do)) {
            r = mk(p, N_DO);
            next(p);
            r->a = parse_stmt(p);
            if (!is_id(p, A(while))) unexpected(p);
            next(p);
            expect(p, T_LPAREN);
            r->b = parse_expr(p, false);
            expect(p, T_RPAREN);
            if (is(p, T_SEMI)) next(p);
        } else if (t.s == A(return)) {
            if (p->fn->flags & FN_SCRIPT) p_error(p, "Illegal return statement");
            r = mk(p, N_RETURN);
            next(p);
            if (!is(p, T_SEMI) && !is(p, T_RBRACE) && !is(p, T_EOF) && !p->t.nl) r->a = parse_expr(p, false);
            semi(p);
        } else if (t.s == A(break) || t.s == A(continue)) {
            r = mk(p, t.s == A(break) ? N_BREAK : N_CONTINUE);
            next(p);
            r->u.atom = JS_UNDEF;
            if (is(p, T_IDENT) && !p->t.nl && !is_reserved(p, p->t.s)) {
                r->u.atom = p->t.s;
                next(p);
            }
            semi(p);
        } else if (t.s == A(throw)) {
            r = mk(p, N_THROW);
            next(p);
            if (p->t.nl) p_error(p, "Illegal newline after throw");
            r->a = parse_expr(p, false);
            semi(p);
        } else if (t.s == A(try)) {
            r = mk(p, N_TRY);
            next(p);
            r->a = parse_block(p);
            if (is_id(p, A(catch))) {
                next(p);
                Scope *outer = p->sc;
                Scope *cs = new_scope(p, SC_CATCH);
                p->sc = cs;
                r->scope = cs;
                if (is(p, T_LPAREN)) {
                    next(p);
                    r->b = parse_binding_target(p);
                    declare_pattern(p, r->b, B_CATCH);
                    expect(p, T_RPAREN);
                }
                r->c = parse_block(p);
                /* a let in the catch block may not shadow the catch parameter */
                for (Binding *b = r->c->scope->first; b; b = b->next)
                    if (find_in(cs, b->name) && b->kind != B_VAR && b->kind != B_FUNC) redeclared(p, b->name);
                p->sc = outer;
            }
            if (is_id(p, A(finally))) {
                next(p);
                r->d = parse_block(p);
            }
            if (!r->c && !r->d) p_error(p, "Missing catch or finally after try");
        } else if (t.s == A(switch)) {
            r = mk(p, N_SWITCH);
            next(p);
            expect(p, T_LPAREN);
            r->a = parse_expr(p, false);
            expect(p, T_RPAREN);
            expect(p, T_LBRACE);
            Scope *outer = p->sc;
            r->scope = new_scope(p, SC_BLOCK);
            p->sc = r->scope;
            Node *ctail = 0;
            bool has_default = false;
            while (!is(p, T_RBRACE)) {
                Node *c = mk(p, N_CASE);
                if (is_id(p, A(case))) {
                    next(p);
                    c->a = parse_expr(p, false);
                } else if (is_id(p, A(default))) {
                    if (has_default) p_error(p, "More than one default clause in switch statement");
                    has_default = true;
                    next(p);
                    c->flags = 1;
                } else
                    unexpected(p);
                expect(p, T_COLON);
                Node *btail = 0;
                while (!is(p, T_RBRACE) && !is_id(p, A(case)) && !is_id(p, A(default))) {
                    if (is(p, T_EOF)) unexpected(p);
                    Node *s = parse_stmt(p);
                    if (btail)
                        btail->next = s;
                    else
                        c->b = s;
                    btail = s;
                    while (btail->next) btail = btail->next;
                }
                if (ctail)
                    ctail->next = c;
                else
                    r->b = c;
                ctail = c;
            }
            next(p);
            p->sc = outer;
        } else if (t.s == A(debugger)) {
            r = mk(p, N_DEBUGGER);
            next(p);
            semi(p);
        } else if (t.s == A(with)) {
            p_error(p, strict(p) ? "Strict mode code may not include a with statement" : "'with' statements are not supported");
        } else if (t.s == A(import) || t.s == A(export)) {
            p_error(p, "Cannot use import statement outside a module");
        } else {
            Tok nt = peek(p);
            if (nt.t == T_COLON && !is_reserved(p, t.s)) {
                r = mk(p, N_LABELED);
                r->u.atom = t.s;
                next(p);
                next(p);
                if (is_id(p, A(function))) {
                    if (strict(p)) p_error(p, "In strict mode code, functions can only be declared at top level or inside a block.");
                }
                r->a = parse_stmt(p);
                break;
            }
            goto expr;
        }
        break;
    default:
    expr: {
        r = mk(p, N_EXPR);
        r->a = parse_expr(p, false);
        r->line = r->a->line;
        semi(p);
    }
    }
    END_DEPTH(p);
    return r;
}

int js_parse(js_rt *rt, const char *src, size_t len, const char *file, int flags, ParseResult *out) {
    P *p = (P *)calloc(1, sizeof(P));
    if (!p) return -1;
    memset(out, 0, sizeof *out);
    p->rt = rt;
    p->s = (const uint8_t *)src;
    p->n = (uint32_t)len;
    p->line = 1;
    p->ar = &out->arena;
    p->errmsg = out->errmsg;
    p->errline = &out->errline;
    out->src = str_new8(rt, (const uint8_t *)src, (uint32_t)len);
    out->file = str_utf8(rt, file ? file : "<anonymous>", strlen(file ? file : "<anonymous>"));
    int rc = 0;
    if (JS_SETJMP(p->jb) == 0) {
        FuncNode *fn = (FuncNode *)pal(p, sizeof(FuncNode));
        fn->flags = FN_SCRIPT | ((flags & CS_STRICT) ? FN_STRICT : 0);
        fn->name = JS_UNDEF;
        fn->simple_params = true;
        fn->line = 1;
        p->fn = fn;
        p->script = fn;
        Scope *s = new_scope(p, SC_FUNC);
        s->is_script = true;
        s->func = fn;
        fn->scope = s;
        p->sc = s;
        grow(p, (void ***)&p->funcs, &p->cfuncs, p->nfuncs);
        p->funcs[p->nfuncs++] = fn;
        next(p);
        fn->body = parse_body(p, true);
        if (!is(p, T_EOF)) unexpected(p);
        fn->src_end = (uint32_t)len;
        out->script = fn;
    } else {
        rc = -1;
    }
    out->refs = p->refs;
    out->nrefs = p->nrefs;
    if (rc) {
        free(p->refs);
        out->refs = 0;
    }
    free(p->scopes);
    free(p->funcs);
    free(p);
    return rc;
}
