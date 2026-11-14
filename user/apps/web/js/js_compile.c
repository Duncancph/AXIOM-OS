/* AXIOM JavaScript engine: identifier resolution and bytecode generation. */
#include "js_parse.h"
#include "js_ops.h"

enum { CT_LOOP, CT_SWITCH, CT_LABEL, CT_ENV, CT_TRY, CT_FINALLY, CT_FINBODY };
typedef struct Ctl {
    int kind;
    jsval labels[8];
    int nlabels;
    int brk, cont; /* patch chains (-1 = empty) */
    int iter_slot; /* for-of: close iterator when leaving */
    int fin;       /* CT_FINALLY: gosub patch chain */
    struct Ctl *up;
} Ctl;

typedef struct CG CG;
typedef struct FS {
    js_rt *rt;
    CG *cg;
    struct FS *up;
    FuncNode *fn;
    uint8_t *code;
    uint32_t len, cap;
    jsval *consts;
    uint32_t nconsts, cconsts;
    uint32_t *chash;
    uint32_t chcap;
    uint32_t *lines;
    uint32_t nlines, clines;
    int last_line;
    int nlocals;
    int depth, maxdepth;
    Ctl *ctl;
    Scope *cur;
    int completion;
    int chain;
    int args_slot, rest_slot;
    jsval pending_labels[8];
    int npending;
} FS;

struct CG {
    js_rt *rt;
    ParseResult *pr;
    js_jmp_buf jb;
    FS *cur;
};

#define RT (fs->rt)
#define A(id) (fs->rt->atoms[A_##id])

static __attribute__((noreturn)) void cg_error(FS *fs, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SB b;
    sb_init(&b, fs->rt);
    fmt_into(fs->rt, &b, fmt, ap);
    va_end(ap);
    JSString *m = sb_finish(&b);
    size_t ml;
    char *u = m ? str_to_utf8(fs->rt, m, &ml) : 0;
    if (u) {
        size_t k = ml < 255 ? ml : 255;
        memcpy(fs->cg->pr->errmsg, u, k);
        fs->cg->pr->errmsg[k] = 0;
        free(u);
    }
    fs->cg->pr->errline = line;
    JS_LONGJMP(fs->cg->jb);
}

/* ---------------- emission ---------------- */
static int delta_of(int op) {
    switch (op) {
    case OP_PUSH_UNDEF: case OP_PUSH_NULL: case OP_PUSH_TRUE: case OP_PUSH_FALSE: case OP_PUSH_UNINIT: case OP_PUSH_I8:
    case OP_PUSH_I32: case OP_PUSH_CONST: case OP_PUSH_THIS: case OP_PUSH_THIS_CHK: case OP_PUSH_NEW_TARGET:
    case OP_PUSH_FUNC: case OP_DUP: case OP_OVER: case OP_GET_LOC: case OP_GET_LOC_CHK: case OP_GET_ENV:
    case OP_GET_ENV_CHK: case OP_GET_GLOBAL: case OP_TYPEOF_GLOBAL: case OP_DELETE_GLOBAL: case OP_GET_METHOD:
    case OP_NEW_OBJECT: case OP_NEW_ARRAY: case OP_CLOSURE: case OP_NEW_PRIVATE: case OP_TEMPLATE: case OP_REGEXP:
    case OP_FOR_IN_NEXT: case OP_ITER_NEXT: case OP_ITER_VALUE: case OP_ITER_REST: case OP_GOSUB: case OP_ITER_CALL_NEXT:
        return 1;
    case OP_DUP2: return 2;
    case OP_POP: case OP_NIP: case OP_PUT_LOC: case OP_PUT_ENV: case OP_PUT_GLOBAL: case OP_DEF_GLOBAL_FUNC:
    case OP_INIT_GLOBAL_LEX: case OP_PUT_FIELD: case OP_GET_ELEM: case OP_DELETE_ELEM: case OP_GET_SUPER_FIELD:
    case OP_GET_PRIVATE: case OP_HAS_PRIVATE: case OP_IN: case OP_INSTANCEOF: case OP_DEFINE_FIELD:
    case OP_SET_PROTO_LIT: case OP_COPY_DATA_PROPS: case OP_ARRAY_APPEND: case OP_ARRAY_SPREAD: case OP_ADD:
    case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_BAND: case OP_BOR: case OP_BXOR:
    case OP_SHL: case OP_SAR: case OP_SHR: case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_JF: case OP_JT: case OP_JF_KEEP: case OP_JT_KEEP: case OP_JNN_KEEP:
    case OP_JNU_KEEP: case OP_INIT_FIELDS: case OP_RETURN: case OP_THROW: case OP_RETHROW: case OP_RET:
    case OP_SET_FIELDS: case OP_SET_HOME: case OP_FOR_IN_INIT: case OP_ITER_INIT: case OP_OBJ_REST:
        return -1;
    case OP_PUT_ELEM: case OP_PUT_SUPER_FIELD: case OP_GET_SUPER_ELEM: case OP_PUT_PRIVATE: case OP_DEFINE_ELEM:
    case OP_DEFINE_METHOD: case OP_CALL_ARRAY: case OP_NEW_ARRAY_ARGS: case OP_SUPER_CALL_ARRAY:
        return -2;
    case OP_PUT_SUPER_ELEM: case OP_DEF_PRIVATE: return -3;
    default: return 0;
    }
}

static void ensure(FS *fs, uint32_t n) {
    if (fs->len + n <= fs->cap) return;
    uint32_t nc = fs->cap ? fs->cap * 2 : 256;
    while (nc < fs->len + n) nc *= 2;
    uint8_t *c = (uint8_t *)realloc(fs->code, nc);
    if (!c) cg_error(fs, 0, "out of memory");
    fs->code = c;
    fs->cap = nc;
}
static void adj(FS *fs, int d) {
    fs->depth += d;
    if (fs->depth < 0) fs->depth = 0;
    if (fs->depth > fs->maxdepth) fs->maxdepth = fs->depth;
}
static void op(FS *fs, int o) {
    ensure(fs, 1);
    fs->code[fs->len++] = (uint8_t)o;
    adj(fs, delta_of(o));
}
static void u8_(FS *fs, uint32_t v) {
    ensure(fs, 1);
    fs->code[fs->len++] = (uint8_t)v;
}
static void u16_(FS *fs, uint32_t v) {
    ensure(fs, 2);
    fs->code[fs->len++] = (uint8_t)v;
    fs->code[fs->len++] = (uint8_t)(v >> 8);
}
static void u32_(FS *fs, uint32_t v) {
    ensure(fs, 4);
    for (int i = 0; i < 4; i++) fs->code[fs->len++] = (uint8_t)(v >> (8 * i));
}
static void put32(FS *fs, uint32_t pos, uint32_t v) {
    for (int i = 0; i < 4; i++) fs->code[pos + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(FS *fs, uint32_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)fs->code[pos + i] << (8 * i);
    return v;
}
static int here(FS *fs) { return (int)fs->len; }

/* jump with operand to patch later; returns operand position */
static int jump(FS *fs, int o) {
    op(fs, o);
    int pos = (int)fs->len;
    u32_(fs, 0);
    return pos;
}
static void patch_to(FS *fs, int pos, int target) { put32(fs, (uint32_t)pos, (uint32_t)(target - (pos + 4))); }
static void patch_here(FS *fs, int pos) { patch_to(fs, pos, here(fs)); }
static void jump_back(FS *fs, int o, int target) {
    op(fs, o);
    int pos = (int)fs->len;
    u32_(fs, (uint32_t)(target - (pos + 4)));
}
/* patch chains: operand holds previous chain head (+1), 0 = end */
static void chain_add(FS *fs, int *head, int pos) {
    put32(fs, (uint32_t)pos, (uint32_t)(*head + 1));
    *head = pos;
}
static void chain_resolve(FS *fs, int head, int target) {
    while (head >= 0) {
        int prev = (int)get32(fs, (uint32_t)head) - 1;
        patch_to(fs, head, target);
        head = prev;
    }
}

static void set_line(FS *fs, int line) {
    if (line <= 0 || line == fs->last_line) return;
    if (fs->nlines + 2 > fs->clines) {
        fs->clines = fs->clines ? fs->clines * 2 : 64;
        fs->lines = (uint32_t *)realloc(fs->lines, fs->clines * sizeof(uint32_t));
    }
    if (fs->nlines >= 2 && fs->lines[fs->nlines - 2] == fs->len) {
        fs->lines[fs->nlines - 1] = (uint32_t)line;
    } else {
        fs->lines[fs->nlines++] = fs->len;
        fs->lines[fs->nlines++] = (uint32_t)line;
    }
    fs->last_line = line;
}

static uint32_t cst(FS *fs, jsval v) {
    if (!fs->chash || fs->nconsts * 2 >= fs->chcap) {
        uint32_t nc = fs->chcap ? fs->chcap * 2 : 64;
        uint32_t *h = (uint32_t *)calloc(nc, sizeof(uint32_t));
        if (!h) cg_error(fs, 0, "out of memory");
        for (uint32_t i = 0; i < fs->nconsts; i++) {
            uint64_t k = fs->consts[i];
            uint32_t j = (uint32_t)((k ^ (k >> 29)) * 2654435761u) & (nc - 1);
            while (h[j]) j = (j + 1) & (nc - 1);
            h[j] = i + 1;
        }
        free(fs->chash);
        fs->chash = h;
        fs->chcap = nc;
    }
    uint32_t j = (uint32_t)((v ^ (v >> 29)) * 2654435761u) & (fs->chcap - 1);
    while (fs->chash[j]) {
        uint32_t i = fs->chash[j] - 1;
        if (fs->consts[i] == v) return i;
        j = (j + 1) & (fs->chcap - 1);
    }
    if (fs->nconsts == fs->cconsts) {
        fs->cconsts = fs->cconsts ? fs->cconsts * 2 : 16;
        fs->consts = (jsval *)realloc(fs->consts, fs->cconsts * sizeof(jsval));
    }
    fs->consts[fs->nconsts] = v;
    fs->chash[j] = fs->nconsts + 1;
    return fs->nconsts++;
}
static uint32_t cst_new(FS *fs, jsval v) { /* never shared */
    if (fs->nconsts == fs->cconsts) {
        fs->cconsts = fs->cconsts ? fs->cconsts * 2 : 16;
        fs->consts = (jsval *)realloc(fs->consts, fs->cconsts * sizeof(jsval));
    }
    fs->consts[fs->nconsts] = v;
    return fs->nconsts++;
}

static void push_num(FS *fs, double d) {
    if (d == (int8_t)d && !(d == 0 && js_signbit(d))) {
        op(fs, OP_PUSH_I8);
        u8_(fs, (uint8_t)(int8_t)d);
    } else if (d == (int32_t)d && !(d == 0 && js_signbit(d))) {
        op(fs, OP_PUSH_I32);
        u32_(fs, (uint32_t)(int32_t)d);
    } else {
        op(fs, OP_PUSH_CONST);
        u32_(fs, cst(fs, mknum(d)));
    }
}
static void push_const(FS *fs, jsval v) {
    op(fs, OP_PUSH_CONST);
    u32_(fs, cst(fs, v));
}
static int new_local(FS *fs) {
    if (fs->nlocals >= 65000) cg_error(fs, 0, "too many local variables");
    return fs->nlocals++;
}
static void loc_op(FS *fs, int o, int slot) {
    op(fs, o);
    u16_(fs, (uint32_t)slot);
}

/* ---------------- resolution ---------------- */
static Binding *implicit(js_rt *rt, Arena *ar, Scope *s, jsval name, int kind) {
    Binding *b = 0;
    for (Binding *x = s->first; x; x = x->next)
        if (x->name == name) return x;
    b = (Binding *)arena_alloc(ar, sizeof(Binding));
    b->name = name;
    b->kind = (uint8_t)kind;
    b->scope = s;
    b->slot = -1;
    b->env_idx = -1;
    b->param_index = -1;
    if (s->last)
        s->last->next = b;
    else
        s->first = b;
    s->last = b;
    FuncNode *f = s->func;
    if (kind == B_THIS) f->this_b = b;
    if (kind == B_NEWTARGET) f->nt_b = b;
    if (kind == B_ARGS) {
        f->args_b = b;
        f->uses_args = true;
    }
    if (kind == B_FUNCOBJ) f->func_b = b;
    if (kind == B_THIS && (f->flags & FN_DERIVED)) b->tdz = true;
    (void)rt;
    return b;
}

static void resolve_all(js_rt *rt, ParseResult *pr) {
    jsval a_this = rt->atoms[A_this], a_nt = rt->atoms[A_new_target], a_func = rt->atoms[A_pfunc],
          a_args = rt->atoms[A_arguments];
    for (int i = 0; i < pr->nrefs; i++) {
        Node *r = pr->refs[i];
        jsval name = r->u.atom;
        Scope *s0 = r->scope;
        FuncNode *rf = s0->func;
        Binding *found = 0;
        for (Scope *s = s0; s; s = s->parent) {
            for (Binding *b = s->first; b; b = b->next)
                if (b->name == name) {
                    found = b;
                    break;
                }
            if (found) break;
            if (s->kind == SC_FUNC) {
                FuncNode *f = s->func;
                bool arrow = (f->flags & FN_ARROW) != 0;
                if (!arrow) {
                    if (name == a_this) {
                        found = implicit(rt, &pr->arena, s, name, B_THIS);
                        break;
                    }
                    if (name == a_nt) {
                        found = implicit(rt, &pr->arena, s, name, B_NEWTARGET);
                        break;
                    }
                    if (name == a_func) {
                        found = implicit(rt, &pr->arena, s, name, B_FUNCOBJ);
                        break;
                    }
                    if (name == a_args && !s->is_script) {
                        found = implicit(rt, &pr->arena, s, name, B_ARGS);
                        break;
                    }
                }
                if (s->is_script) break;
            }
        }
        r->bind = found;
        if (found && found->scope->func != rf) found->captured = true;
    }
}

/* assign env slots for captured bindings of a scope (idempotent) */
static void prepare_scope(Scope *s) {
    if (s->env_size || s->has_env) return;
    int n = 0;
    for (Binding *b = s->first; b; b = b->next)
        if (b->captured) b->env_idx = n++;
    s->env_size = n;
    s->has_env = n > 0;
}

static int env_hops(FS *fs, Scope *target) {
    int h = 0;
    for (Scope *s = fs->cur; s && s != target; s = s->parent)
        if (s->has_env) h++;
    return h;
}

/* ---------------- variable access ---------------- */
static void gen_expr(FS *fs, Node *n);
static void gen_stmt(FS *fs, Node *n);
static void gen_stmts(FS *fs, Node *n);
static void gen_function(FS *fs, FuncNode *fn);
static void gen_class(FS *fs, ClassNode *cls, jsval inferred);
static void gen_pattern(FS *fs, Node *pat, int mode);

enum { M_ASSIGN, M_INIT_LET, M_INIT_VAR };

static void global_op(FS *fs, int o, jsval name) {
    op(fs, o);
    u32_(fs, cst(fs, name));
    if (o == OP_GET_GLOBAL || o == OP_PUT_GLOBAL || o == OP_SET_GLOBAL) {
        u32_(fs, 0xFFFFFFFFu);
        u32_(fs, 0);
    }
}


static void get_binding(FS *fs, Binding *b, jsval name) {
    if (!b) {
        global_op(fs, OP_GET_GLOBAL, name);
        return;
    }
    if (!b->captured || b->env_idx < 0) {
        switch (b->kind) {
        case B_THIS:
            op(fs, (b->scope->func->flags & FN_DERIVED) ? OP_PUSH_THIS_CHK : OP_PUSH_THIS);
            return;
        case B_NEWTARGET: op(fs, OP_PUSH_NEW_TARGET); return;
        case B_FUNCOBJ: op(fs, OP_PUSH_FUNC); return;
        default: break;
        }
        if (b->slot < 0) cg_error(fs, 0, "internal: unallocated binding '%S'", vstr(b->name));
        if (b->tdz) {
            loc_op(fs, OP_GET_LOC_CHK, b->slot);
            u32_(fs, cst(fs, b->name));
        } else
            loc_op(fs, OP_GET_LOC, b->slot);
        return;
    }
    int hops = env_hops(fs, b->scope);
    if (hops > 255) cg_error(fs, 0, "scope nesting too deep");
    op(fs, b->tdz ? OP_GET_ENV_CHK : OP_GET_ENV);
    u8_(fs, (uint32_t)hops);
    u16_(fs, (uint32_t)b->env_idx);
    if (b->tdz) u32_(fs, cst(fs, b->name));
}

/* store top of stack; keep=leave value */
static void put_binding(FS *fs, Binding *b, jsval name, int mode, bool keep) {
    if (!b) {
        if (mode == M_INIT_LET) {
            if (keep) op(fs, OP_DUP);
            global_op(fs, OP_INIT_GLOBAL_LEX, name);
            return;
        }
        global_op(fs, keep ? OP_SET_GLOBAL : OP_PUT_GLOBAL, name);
        return;
    }
    if (mode == M_ASSIGN) {
        if (b->is_const || b->kind == B_PRIVATE || b->kind == B_HIDDEN) {
            if (b->tdz) { /* TDZ error takes precedence */
                get_binding(fs, b, name);
                op(fs, OP_POP);
            }
            op(fs, OP_THROW_CONST);
            u32_(fs, cst(fs, name));
            return;
        }
        if (b->kind == B_FUNCNAME) {
            if (fs->fn->flags & FN_STRICT) {
                op(fs, OP_THROW_CONST);
                u32_(fs, cst(fs, name));
            } else if (!keep)
                op(fs, OP_POP);
            return;
        }
    }
    bool chk = mode == M_ASSIGN && b->tdz;
    if (!b->captured || b->env_idx < 0) {
        if (b->slot < 0) cg_error(fs, 0, "internal: unallocated binding '%S'", vstr(b->name));
        if (chk) {
            loc_op(fs, OP_SET_LOC_CHK, b->slot);
            u32_(fs, cst(fs, b->name));
            if (!keep) op(fs, OP_POP);
        } else
            loc_op(fs, keep ? OP_SET_LOC : OP_PUT_LOC, b->slot);
        return;
    }
    int hops = env_hops(fs, b->scope);
    if (chk) {
        op(fs, OP_SET_ENV_CHK);
        u8_(fs, (uint32_t)hops);
        u16_(fs, (uint32_t)b->env_idx);
        u32_(fs, cst(fs, b->name));
        if (!keep) op(fs, OP_POP);
        return;
    }
    op(fs, keep ? OP_SET_ENV : OP_PUT_ENV);
    u8_(fs, (uint32_t)hops);
    u16_(fs, (uint32_t)b->env_idx);
}

static void init_uninit(FS *fs, Binding *b) {
    op(fs, OP_PUSH_UNINIT);
    put_binding(fs, b, b->name, M_INIT_LET, false);
}

/* allocate storage, push env, TDZ-init, hoist functions */
static void enter_scope(FS *fs, Scope *s, int line) {
    prepare_scope(s);
    for (Binding *b = s->first; b; b = b->next)
        if (!b->captured && b->slot < 0 && b->kind != B_THIS && b->kind != B_NEWTARGET && b->kind != B_FUNCOBJ)
            b->slot = new_local(fs);
    fs->cur = s;
    if (s->has_env) {
        op(fs, OP_PUSH_ENV);
        u16_(fs, (uint32_t)s->env_size);
        Ctl *c = (Ctl *)calloc(1, sizeof(Ctl));
        c->kind = CT_ENV;
        c->brk = c->cont = c->fin = -1;
        c->iter_slot = -1;
        c->up = fs->ctl;
        fs->ctl = c;
    }
    for (Binding *b = s->first; b; b = b->next)
        if (b->tdz && b->kind != B_THIS) init_uninit(fs, b);
    for (Binding *b = s->first; b; b = b->next)
        if (b->kind == B_FUNC && b->decl_fn) {
            gen_function(fs, b->decl_fn);
            put_binding(fs, b, b->name, M_INIT_LET, false);
        }
    (void)line;
}
static void leave_scope(FS *fs, Scope *s) {
    if (s->has_env) {
        op(fs, OP_POP_ENV);
        Ctl *c = fs->ctl;
        fs->ctl = c->up;
        free(c);
    }
    fs->cur = s->parent;
}

/* ---------------- expressions ---------------- */
static void infer_name(Node *v, jsval name) {
    if (!v) return;
    if (v->kind == N_FUNC && is_undef(v->u.fn->name)) v->u.fn->name = name;
    if (v->kind == N_CLASS && is_undef(v->u.cls->name) && v->u.cls->ctor) v->u.cls->ctor->name = name;
}
static bool is_anon_fn(Node *v) {
    return v && ((v->kind == N_FUNC && is_undef(v->u.fn->name)) || (v->kind == N_CLASS && is_undef(v->u.cls->name)));
}

static void describe(FS *fs, SB *b, Node *n, int depth) {
    if (depth > 6) {
        sb_puts(b, "(intermediate value)");
        return;
    }
    switch (n->kind) {
    case N_IDENT: sb_putstr(b, vstr(n->u.atom)); return;
    case N_THIS: sb_puts(b, "this"); return;
    case N_MEMBER:
        describe(fs, b, n->a, depth + 1);
        sb_puts(b, n->flags & NF_OPTIONAL ? "?." : ".");
        sb_putstr(b, vstr(n->u.atom));
        return;
    case N_PRIV_MEMBER:
        describe(fs, b, n->a, depth + 1);
        sb_puts(b, ".");
        sb_putstr(b, vstr(n->b->u.atom));
        return;
    case N_SUPER_MEMBER:
        sb_puts(b, "(intermediate value).");
        if (!(n->flags & NF_COMPUTED)) sb_putstr(b, vstr(n->u.atom));
        return;
    case N_INDEX:
        describe(fs, b, n->a, depth + 1);
        if (n->b->kind == N_STR) {
            sb_puts(b, ".");
            sb_putstr(b, vstr(n->b->u.atom));
        } else if (n->b->kind == N_NUM) {
            char t[40];
            int k = num_to_cstr(n->b->u.num, t);
            sb_puts(b, "[");
            sb_putn(b, t, (uint32_t)k);
            sb_puts(b, "]");
        } else if (n->b->kind == N_IDENT) {
            sb_puts(b, "[");
            sb_putstr(b, vstr(n->b->u.atom));
            sb_puts(b, "]");
        } else
            sb_puts(b, "[...]");
        return;
    case N_CALL:
        describe(fs, b, n->a, depth + 1);
        sb_puts(b, "(...)");
        return;
    case N_OPTCHAIN: describe(fs, b, n->a, depth); return;
    default: sb_puts(b, "(intermediate value)"); return;
    }
}
static uint32_t desc_const(FS *fs, Node *callee) {
    SB b;
    sb_init(&b, fs->rt);
    describe(fs, &b, callee, 0);
    JSString *s = sb_finish(&b);
    return cst(fs, atomize(fs->rt, s));
}

static void gen_args_array(FS *fs, Node *args) {
    op(fs, OP_NEW_ARRAY);
    for (Node *a = args; a; a = a->next) {
        if (a->kind == N_SPREAD) {
            gen_expr(fs, a->a);
            op(fs, OP_ARRAY_SPREAD);
        } else {
            gen_expr(fs, a);
            op(fs, OP_ARRAY_APPEND);
        }
    }
}
static bool has_spread(Node *args) {
    for (Node *a = args; a; a = a->next)
        if (a->kind == N_SPREAD) return true;
    return false;
}
static int count_list(Node *n) {
    int c = 0;
    for (; n; n = n->next) c++;
    return c;
}

static void emit_call(FS *fs, Node *args, uint32_t desc, int line) {
    set_line(fs, line);
    if (has_spread(args)) {
        gen_args_array(fs, args);
        op(fs, OP_CALL_ARRAY);
        u32_(fs, desc);
        return;
    }
    int argc = 0;
    for (Node *a = args; a; a = a->next) {
        gen_expr(fs, a);
        argc++;
    }
    set_line(fs, line);
    op(fs, OP_CALL);
    u16_(fs, (uint32_t)argc);
    u32_(fs, desc);
    adj(fs, -(argc + 1));
}

static void opt_check(FS *fs, int depth, int npop) {
    op(fs, OP_OPT_CHK);
    u8_(fs, (uint32_t)depth);
    u8_(fs, (uint32_t)npop);
    int pos = (int)fs->len;
    u32_(fs, 0);
    chain_add(fs, &fs->chain, pos);
}

static void gen_home_proto(FS *fs, Node *funcref) {
    get_binding(fs, funcref->bind, funcref->u.atom);
    op(fs, OP_HOME_PROTO);
}

/* push [func, this] for a call */
static void gen_callee(FS *fs, Node *c) {
    switch (c->kind) {
    case N_MEMBER:
        gen_expr(fs, c->a);
        if (c->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        set_line(fs, c->line);
        op(fs, OP_GET_METHOD);
        u32_(fs, cst(fs, c->u.atom));
        u32_(fs, 0xFFFFFFFFu);
        return;
    case N_INDEX:
        gen_expr(fs, c->a);
        if (c->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        gen_expr(fs, c->b);
        set_line(fs, c->line);
        op(fs, OP_GET_ELEM_METHOD);
        return;
    case N_PRIV_MEMBER:
        gen_expr(fs, c->a);
        if (c->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        op(fs, OP_DUP);
        get_binding(fs, c->b->bind, c->b->u.atom);
        op(fs, OP_GET_PRIVATE);
        op(fs, OP_SWAP);
        return;
    case N_SUPER_MEMBER:
        gen_home_proto(fs, c->a);
        get_binding(fs, c->c->bind, c->c->u.atom);
        if (c->flags & NF_COMPUTED) {
            op(fs, OP_DUP2);
            gen_expr(fs, c->b);
            op(fs, OP_GET_SUPER_ELEM); /* [proto this f] */
            op(fs, OP_ROT3R);          /* [f proto this] */
            op(fs, OP_NIP);            /* [f this] */
            return;
        }
        op(fs, OP_GET_SUPER_METHOD);
        u32_(fs, cst(fs, c->u.atom));
        return;
    default:
        gen_expr(fs, c);
        op(fs, OP_PUSH_UNDEF);
        return;
    }
}

static void gen_call(FS *fs, Node *n) {
    Node *c = n->a;
    if (c->kind == N_SUPER_MEMBER && (c->flags & NF_COMPUTED)) {
        /* [proto this] -> need [func this] */
        gen_home_proto(fs, c->a);
        get_binding(fs, c->c->bind, c->c->u.atom);
        op(fs, OP_DUP2);
        gen_expr(fs, c->b);
        op(fs, OP_GET_SUPER_ELEM); /* [proto this f] */
        op(fs, OP_ROT3R);          /* [f proto this] */
        op(fs, OP_NIP);            /* [f this] */
    } else
        gen_callee(fs, c);
    if (n->flags & NF_OPTIONAL) opt_check(fs, 1, 2);
    emit_call(fs, n->b, desc_const(fs, c), n->line);
}

static void gen_template(FS *fs, Node *n) {
    Node *q = n->a, *e = n->b;
    bool first = true;
    if (vstr(q->u.atom)->len || !e) {
        push_const(fs, q->u.atom);
        first = false;
    }
    q = q->next;
    while (e) {
        gen_expr(fs, e);
        op(fs, OP_TO_STRING);
        if (!first) op(fs, OP_ADD);
        first = false;
        if (q && vstr(q->u.atom)->len) {
            push_const(fs, q->u.atom);
            op(fs, OP_ADD);
        }
        e = e->next;
        if (q) q = q->next;
    }
}

static jsval template_object(FS *fs, Node *t) {
    js_rt *rt = fs->rt;
    int n = count_list(t->a);
    JSObject *cooked = new_array(rt, 0), *raw = new_array(rt, 0);
    for (Node *q = t->a; q; q = q->next) {
        array_push(rt, cooked, q->u.atom);
        array_push(rt, raw, q->str2);
    }
    (void)n;
    set_integrity(rt, raw, true);
    def_prop(rt, cooked, ATOM(raw), mkobj(raw), 0);
    set_integrity(rt, cooked, true);
    return mkobj(cooked);
}

static void gen_tagged(FS *fs, Node *n) {
    gen_callee(fs, n->a);
    jsval tobj = template_object(fs, n->b);
    op(fs, OP_PUSH_CONST);
    u32_(fs, cst_new(fs, tobj));
    int argc = 1;
    for (Node *e = n->b->b; e; e = e->next) {
        gen_expr(fs, e);
        argc++;
    }
    set_line(fs, n->line);
    op(fs, OP_CALL);
    u16_(fs, (uint32_t)argc);
    u32_(fs, desc_const(fs, n->a));
    adj(fs, -(argc + 1));
}

static int binop_code(int o) {
    switch (o) {
    case OPR_ADD: return OP_ADD;
    case OPR_SUB: return OP_SUB;
    case OPR_MUL: return OP_MUL;
    case OPR_DIV: return OP_DIV;
    case OPR_MOD: return OP_MOD;
    case OPR_POW: return OP_POW;
    case OPR_SHL: return OP_SHL;
    case OPR_SAR: return OP_SAR;
    case OPR_SHR: return OP_SHR;
    case OPR_BAND: return OP_BAND;
    case OPR_BOR: return OP_BOR;
    case OPR_BXOR: return OP_BXOR;
    case OPR_EQ: return OP_EQ;
    case OPR_NE: return OP_NE;
    case OPR_SEQ: return OP_SEQ;
    case OPR_SNE: return OP_SNE;
    case OPR_LT: return OP_LT;
    case OPR_LE: return OP_LE;
    case OPR_GT: return OP_GT;
    case OPR_GE: return OP_GE;
    case OPR_IN: return OP_IN;
    case OPR_INSTANCEOF: return OP_INSTANCEOF;
    default: return OP_NOP;
    }
}

static void gen_object(FS *fs, Node *n) {
    op(fs, OP_NEW_OBJECT);
    for (Node *p = n->a; p; p = p->next) {
        set_line(fs, p->line);
        switch (p->u.op) {
        case PK_SPREAD:
            gen_expr(fs, p->a);
            op(fs, OP_COPY_DATA_PROPS);
            break;
        case PK_PROTO:
            gen_expr(fs, p->b);
            op(fs, OP_SET_PROTO_LIT);
            break;
        case PK_INIT:
        case PK_SHORTHAND:
            if (p->flags & NF_COMPUTED) {
                gen_expr(fs, p->a);
                op(fs, OP_TO_PROPKEY);
                gen_expr(fs, p->b);
                if (is_anon_fn(p->b)) {
                    op(fs, OP_SET_FUNC_NAME);
                    u8_(fs, 0);
                }
                op(fs, OP_DEFINE_ELEM);
            } else {
                if (p->u.op == PK_INIT) infer_name(p->b, p->a->u.atom);
                gen_expr(fs, p->b);
                op(fs, OP_DEFINE_FIELD);
                u32_(fs, cst(fs, p->a->u.atom));
            }
            break;
        case PK_SHORTHAND_INIT: cg_error(fs, p->line, "Invalid shorthand property initializer");
        case PK_METHOD:
        case PK_GET:
        case PK_SET:
            if (p->flags & NF_COMPUTED) {
                gen_expr(fs, p->a);
                op(fs, OP_TO_PROPKEY);
            } else
                push_const(fs, p->a->u.atom);
            gen_function(fs, p->b->u.fn);
            op(fs, OP_DEFINE_METHOD);
            u8_(fs, p->u.op == PK_GET ? 1 : p->u.op == PK_SET ? 2 : 0);
            u8_(fs, 1);
            break;
        default: break;
        }
    }
}

static void gen_array(FS *fs, Node *n) {
    op(fs, OP_NEW_ARRAY);
    for (Node *e = n->a; e; e = e->next) {
        if (e->kind == N_HOLE)
            op(fs, OP_ARRAY_HOLE);
        else if (e->kind == N_SPREAD) {
            gen_expr(fs, e->a);
            op(fs, OP_ARRAY_SPREAD);
        } else {
            gen_expr(fs, e);
            op(fs, OP_ARRAY_APPEND);
        }
    }
}

/* reference kinds for assignment */
static void gen_assign(FS *fs, Node *n, bool keep) {
    Node *t = n->a;
    int o = n->u.op;
    if (o == 0) {
        switch (t->kind) {
        case N_IDENT:
            infer_name(n->b, t->u.atom);
            gen_expr(fs, n->b);
            set_line(fs, n->line);
            put_binding(fs, t->bind, t->u.atom, M_ASSIGN, keep);
            return;
        case N_MEMBER:
            gen_expr(fs, t->a);
            gen_expr(fs, n->b);
            set_line(fs, n->line);
            op(fs, OP_PUT_FIELD);
            u32_(fs, cst(fs, t->u.atom));
            u32_(fs, 0xFFFFFFFFu);
            break;
        case N_INDEX:
            gen_expr(fs, t->a);
            gen_expr(fs, t->b);
            gen_expr(fs, n->b);
            set_line(fs, n->line);
            op(fs, OP_PUT_ELEM);
            break;
        case N_PRIV_MEMBER:
            gen_expr(fs, t->a);
            get_binding(fs, t->b->bind, t->b->u.atom);
            gen_expr(fs, n->b);
            op(fs, OP_PUT_PRIVATE);
            break;
        case N_SUPER_MEMBER:
            gen_home_proto(fs, t->a);
            get_binding(fs, t->c->bind, t->c->u.atom);
            if (t->flags & NF_COMPUTED) {
                gen_expr(fs, t->b);
                gen_expr(fs, n->b);
                op(fs, OP_PUT_SUPER_ELEM);
            } else {
                gen_expr(fs, n->b);
                op(fs, OP_PUT_SUPER_FIELD);
                u32_(fs, cst(fs, t->u.atom));
            }
            break;
        case N_ARRAY_PAT:
        case N_OBJECT_PAT:
            gen_expr(fs, n->b);
            if (keep) op(fs, OP_DUP);
            gen_pattern(fs, t, M_ASSIGN);
            return;
        default: cg_error(fs, n->line, "Invalid left-hand side in assignment");
        }
        if (!keep) op(fs, OP_POP);
        return;
    }
    bool logical = o == OPR_AND || o == OPR_OR || o == OPR_NULLISH;
    int jop = o == OPR_AND ? OP_JF_KEEP : o == OPR_OR ? OP_JT_KEEP : OP_JNN_KEEP;
    switch (t->kind) {
    case N_IDENT:
        get_binding(fs, t->bind, t->u.atom);
        if (logical) {
            int j = jump(fs, jop);
            infer_name(n->b, t->u.atom);
            gen_expr(fs, n->b);
            put_binding(fs, t->bind, t->u.atom, M_ASSIGN, true);
            patch_here(fs, j);
            adj(fs, 1);
            if (!keep) op(fs, OP_POP);
            return;
        }
        gen_expr(fs, n->b);
        set_line(fs, n->line);
        op(fs, binop_code(o));
        put_binding(fs, t->bind, t->u.atom, M_ASSIGN, keep);
        return;
    case N_MEMBER:
    case N_PRIV_MEMBER:
    case N_SUPER_MEMBER:
    case N_INDEX: {
        int nref; /* items under the value: obj (1), obj key (2), proto this (2), proto this key(3) */
        if (t->kind == N_MEMBER) {
            gen_expr(fs, t->a);
            op(fs, OP_DUP);
            op(fs, OP_GET_FIELD);
            u32_(fs, cst(fs, t->u.atom));
            u32_(fs, 0xFFFFFFFFu);
            nref = 1;
        } else if (t->kind == N_INDEX) {
            gen_expr(fs, t->a);
            gen_expr(fs, t->b);
            op(fs, OP_TO_PROPKEY);
            op(fs, OP_DUP2);
            op(fs, OP_GET_ELEM);
            nref = 2;
        } else if (t->kind == N_PRIV_MEMBER) {
            gen_expr(fs, t->a);
            get_binding(fs, t->b->bind, t->b->u.atom);
            op(fs, OP_DUP2);
            op(fs, OP_GET_PRIVATE);
            nref = 2;
        } else {
            gen_home_proto(fs, t->a);
            get_binding(fs, t->c->bind, t->c->u.atom);
            if (t->flags & NF_COMPUTED) {
                gen_expr(fs, t->b);
                op(fs, OP_TO_PROPKEY);
                /* [proto this key] -> [proto this key proto this key] via temps */
                int tk = new_local(fs), tp = new_local(fs), tt = new_local(fs);
                loc_op(fs, OP_PUT_LOC, tk);
                loc_op(fs, OP_PUT_LOC, tt);
                loc_op(fs, OP_PUT_LOC, tp);
                loc_op(fs, OP_GET_LOC, tp);
                loc_op(fs, OP_GET_LOC, tt);
                loc_op(fs, OP_GET_LOC, tk);
                loc_op(fs, OP_GET_LOC, tp);
                loc_op(fs, OP_GET_LOC, tt);
                loc_op(fs, OP_GET_LOC, tk);
                op(fs, OP_GET_SUPER_ELEM);
                nref = 3;
            } else {
                op(fs, OP_DUP2);
                op(fs, OP_GET_SUPER_FIELD);
                u32_(fs, cst(fs, t->u.atom));
                nref = 2;
            }
        }
        int jshort = -1;
        if (logical) {
            jshort = jump(fs, jop);
            gen_expr(fs, n->b);
        } else {
            gen_expr(fs, n->b);
            set_line(fs, n->line);
            op(fs, binop_code(o));
        }
        /* store */
        if (t->kind == N_MEMBER) {
            op(fs, OP_PUT_FIELD);
            u32_(fs, cst(fs, t->u.atom));
            u32_(fs, 0xFFFFFFFFu);
        } else if (t->kind == N_INDEX)
            op(fs, OP_PUT_ELEM);
        else if (t->kind == N_PRIV_MEMBER)
            op(fs, OP_PUT_PRIVATE);
        else if (nref == 3)
            op(fs, OP_PUT_SUPER_ELEM);
        else {
            op(fs, OP_PUT_SUPER_FIELD);
            u32_(fs, cst(fs, t->u.atom));
        }
        if (logical) {
            int jend = jump(fs, OP_JMP);
            patch_here(fs, jshort);
            /* [refs... cur] -> [cur] */
            for (int i = 0; i < nref; i++) op(fs, OP_NIP);
            adj(fs, nref);
            patch_here(fs, jend);
            adj(fs, -nref);
        }
        if (!keep) op(fs, OP_POP);
        return;
    }
    default: cg_error(fs, n->line, "Invalid left-hand side in assignment");
    }
}

static void gen_update(FS *fs, Node *n, bool keep) {
    Node *t = n->a;
    int inc = n->u.op == OPR_INC ? OP_INC : OP_DEC;
    bool post = !(n->flags & NF_PREFIX) && keep;
    switch (t->kind) {
    case N_IDENT: {
        Binding *b = t->bind;
        if (!keep && b && !b->captured && b->slot >= 0 && !b->tdz && !b->is_const && b->kind != B_FUNCNAME &&
            inc == OP_INC) {
            loc_op(fs, OP_INC_LOC, b->slot);
            return;
        }
        get_binding(fs, b, t->u.atom);
        if (post) {
            op(fs, OP_TO_NUMERIC);
            op(fs, OP_DUP);
        }
        op(fs, inc);
        put_binding(fs, b, t->u.atom, M_ASSIGN, keep && !post);
        return;
    }
    case N_MEMBER:
        gen_expr(fs, t->a);
        op(fs, OP_DUP);
        op(fs, OP_GET_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        u32_(fs, 0xFFFFFFFFu);
        if (post) {
            op(fs, OP_TO_NUMERIC);
            op(fs, OP_DUP);
            op(fs, OP_ROT3R);
        }
        op(fs, inc);
        op(fs, OP_PUT_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        u32_(fs, 0xFFFFFFFFu);
        break;
    case N_INDEX:
    case N_PRIV_MEMBER:
        gen_expr(fs, t->a);
        if (t->kind == N_INDEX) {
            gen_expr(fs, t->b);
            op(fs, OP_TO_PROPKEY);
            op(fs, OP_DUP2);
            op(fs, OP_GET_ELEM);
        } else {
            get_binding(fs, t->b->bind, t->b->u.atom);
            op(fs, OP_DUP2);
            op(fs, OP_GET_PRIVATE);
        }
        if (post) {
            op(fs, OP_TO_NUMERIC);
            op(fs, OP_DUP);
            op(fs, OP_ROT4R);
        }
        op(fs, inc);
        op(fs, t->kind == N_INDEX ? OP_PUT_ELEM : OP_PUT_PRIVATE);
        break;
    case N_SUPER_MEMBER: {
        if (t->flags & NF_COMPUTED) cg_error(fs, n->line, "unsupported: update of computed super property");
        gen_home_proto(fs, t->a);
        get_binding(fs, t->c->bind, t->c->u.atom);
        op(fs, OP_DUP2);
        op(fs, OP_GET_SUPER_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        if (post) {
            op(fs, OP_TO_NUMERIC);
            op(fs, OP_DUP);
            op(fs, OP_ROT4R);
        }
        op(fs, inc);
        op(fs, OP_PUT_SUPER_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        break;
    }
    default: cg_error(fs, n->line, "Invalid left-hand side expression in postfix operation");
    }
    if (post)
        op(fs, OP_POP);
    else if (!keep)
        op(fs, OP_POP);
}

static void gen_delete(FS *fs, Node *n) {
    Node *t = n->a;
    if (t->kind == N_OPTCHAIN) {
        int saved = fs->chain;
        fs->chain = -1;
        Node *m = t->a;
        if (m->kind == N_MEMBER || m->kind == N_INDEX) {
            gen_expr(fs, m->a);
            if (m->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
            if (m->kind == N_MEMBER) {
                op(fs, OP_DELETE_FIELD);
                u32_(fs, cst(fs, m->u.atom));
            } else {
                gen_expr(fs, m->b);
                op(fs, OP_DELETE_ELEM);
            }
        } else {
            gen_expr(fs, m);
            op(fs, OP_POP);
            op(fs, OP_PUSH_TRUE);
        }
        chain_resolve(fs, fs->chain, here(fs));
        fs->chain = saved;
        return;
    }
    switch (t->kind) {
    case N_MEMBER:
        gen_expr(fs, t->a);
        op(fs, OP_DELETE_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        return;
    case N_INDEX:
        gen_expr(fs, t->a);
        gen_expr(fs, t->b);
        op(fs, OP_DELETE_ELEM);
        return;
    case N_IDENT:
        if (!t->bind) {
            global_op(fs, OP_DELETE_GLOBAL, t->u.atom);
            return;
        }
        op(fs, OP_PUSH_FALSE);
        return;
    case N_SUPER_MEMBER:
        op(fs, OP_THROW_ERR);
        u8_(fs, E_REFERENCE);
        u32_(fs, cst(fs, atom_c(fs->rt, "Unsupported reference to 'super'")));
        op(fs, OP_PUSH_TRUE);
        return;
    default:
        gen_expr(fs, t);
        op(fs, OP_POP);
        op(fs, OP_PUSH_TRUE);
        return;
    }
}

static void gen_super_call(FS *fs, Node *n) {
    /* refs: a=%func c=new.target d=this */
    get_binding(fs, n->a->bind, n->a->u.atom);
    op(fs, OP_GET_SUPER_CTOR);
    get_binding(fs, n->c->bind, n->c->u.atom);
    set_line(fs, n->line);
    if (has_spread(n->b)) {
        gen_args_array(fs, n->b);
        set_line(fs, n->line);
        op(fs, OP_SUPER_CALL_ARRAY);
    } else {
        int argc = 0;
        for (Node *a = n->b; a; a = a->next) {
            gen_expr(fs, a);
            argc++;
        }
        set_line(fs, n->line);
        op(fs, OP_SUPER_CALL);
        u16_(fs, (uint32_t)argc);
        adj(fs, -(argc + 1));
    }
    /* bind this */
    Binding *tb = n->d->bind;
    op(fs, OP_BIND_THIS);
    if (!tb || !tb->captured) {
        u8_(fs, 0xFF);
        u16_(fs, 0);
    } else {
        u8_(fs, (uint32_t)env_hops(fs, tb->scope));
        u16_(fs, (uint32_t)tb->env_idx);
    }
    get_binding(fs, n->a->bind, n->a->u.atom);
    op(fs, OP_INIT_FIELDS);
}

static void gen_expr(FS *fs, Node *n) {
    if (check_stack(fs->rt)) cg_error(fs, n->line, "Maximum call stack size exceeded");
    switch (n->kind) {
    case N_NUM: push_num(fs, n->u.num); return;
    case N_STR: push_const(fs, n->u.atom); return;
    case N_BIGINT: {
        jsval b = big_from_string(fs->rt, vstr(n->u.atom), true);
        if (!is_big(b)) cg_error(fs, n->line, "Invalid BigInt literal");
        op(fs, OP_PUSH_CONST);
        u32_(fs, cst_new(fs, b));
        return;
    }
    case N_TEMPLATE: gen_template(fs, n); return;
    case N_TAGGED: gen_tagged(fs, n); return;
    case N_REGEXP:
        op(fs, OP_REGEXP);
        u32_(fs, cst(fs, n->u.atom));
        u32_(fs, cst(fs, atomize(fs->rt, vstr(n->str2))));
        return;
    case N_IDENT:
        if (!n->bind && n->u.atom == A(undefined)) {
            op(fs, OP_PUSH_UNDEF);
            return;
        }
        set_line(fs, n->line);
        get_binding(fs, n->bind, n->u.atom);
        return;
    case N_THIS:
    case N_NEW_TARGET:
        get_binding(fs, n->bind, n->u.atom);
        return;
    case N_NULL: op(fs, OP_PUSH_NULL); return;
    case N_TRUE: op(fs, OP_PUSH_TRUE); return;
    case N_FALSE: op(fs, OP_PUSH_FALSE); return;
    case N_ARRAY: gen_array(fs, n); return;
    case N_OBJECT: gen_object(fs, n); return;
    case N_FUNC: gen_function(fs, n->u.fn); return;
    case N_CLASS: gen_class(fs, n->u.cls, JS_UNDEF); return;
    case N_MEMBER:
        gen_expr(fs, n->a);
        if (n->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        set_line(fs, n->line);
        op(fs, OP_GET_FIELD);
        u32_(fs, cst(fs, n->u.atom));
        u32_(fs, 0xFFFFFFFFu);
        return;
    case N_INDEX:
        gen_expr(fs, n->a);
        if (n->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        gen_expr(fs, n->b);
        set_line(fs, n->line);
        op(fs, OP_GET_ELEM);
        return;
    case N_PRIV_MEMBER:
        gen_expr(fs, n->a);
        if (n->flags & NF_OPTIONAL) opt_check(fs, 0, 1);
        get_binding(fs, n->b->bind, n->b->u.atom);
        set_line(fs, n->line);
        op(fs, OP_GET_PRIVATE);
        return;
    case N_SUPER_MEMBER:
        gen_home_proto(fs, n->a);
        get_binding(fs, n->c->bind, n->c->u.atom);
        if (n->flags & NF_COMPUTED) {
            gen_expr(fs, n->b);
            op(fs, OP_GET_SUPER_ELEM);
        } else {
            op(fs, OP_GET_SUPER_FIELD);
            u32_(fs, cst(fs, n->u.atom));
        }
        return;
    case N_SUPER_CALL: gen_super_call(fs, n); return;
    case N_CALL: gen_call(fs, n); return;
    case N_NEW: {
        gen_expr(fs, n->a);
        op(fs, OP_PUSH_UNDEF);
        uint32_t d = desc_const(fs, n->a);
        if (has_spread(n->b)) {
            gen_args_array(fs, n->b);
            set_line(fs, n->line);
            op(fs, OP_NEW_ARRAY_ARGS);
            u32_(fs, d);
        } else {
            int argc = 0;
            for (Node *a = n->b; a; a = a->next) {
                gen_expr(fs, a);
                argc++;
            }
            set_line(fs, n->line);
            op(fs, OP_NEW);
            u16_(fs, (uint32_t)argc);
            u32_(fs, d);
            adj(fs, -(argc + 1));
        }
        return;
    }
    case N_OPTCHAIN: {
        int saved = fs->chain;
        fs->chain = -1;
        gen_expr(fs, n->a);
        chain_resolve(fs, fs->chain, here(fs));
        fs->chain = saved;
        return;
    }
    case N_UNARY:
        switch (n->u.op) {
        case OPR_DELETE: gen_delete(fs, n); return;
        case OPR_TYPEOF:
            if (n->a->kind == N_IDENT && !n->a->bind) {
                global_op(fs, OP_TYPEOF_GLOBAL, n->a->u.atom);
                return;
            }
            gen_expr(fs, n->a);
            op(fs, OP_TYPEOF);
            return;
        case OPR_VOID:
            gen_expr(fs, n->a);
            op(fs, OP_POP);
            op(fs, OP_PUSH_UNDEF);
            return;
        case OPR_NEG:
            if (n->a->kind == N_NUM) {
                push_num(fs, -n->a->u.num);
                return;
            }
            gen_expr(fs, n->a);
            op(fs, OP_NEG);
            return;
        case OPR_PLUS: gen_expr(fs, n->a); op(fs, OP_PLUS); return;
        case OPR_NOT: gen_expr(fs, n->a); op(fs, OP_NOT); return;
        case OPR_BITNOT: gen_expr(fs, n->a); op(fs, OP_BITNOT); return;
        default: break;
        }
        break;
    case N_UPDATE: gen_update(fs, n, true); return;
    case N_BINARY:
        gen_expr(fs, n->a);
        gen_expr(fs, n->b);
        set_line(fs, n->line);
        op(fs, binop_code(n->u.op));
        return;
    case N_LOGICAL: {
        gen_expr(fs, n->a);
        int j = jump(fs, n->u.op == OPR_AND ? OP_JF_KEEP : n->u.op == OPR_OR ? OP_JT_KEEP : OP_JNN_KEEP);
        gen_expr(fs, n->b);
        patch_here(fs, j);
        return;
    }
    case N_COND: {
        gen_expr(fs, n->a);
        int jf = jump(fs, OP_JF);
        gen_expr(fs, n->b);
        int je = jump(fs, OP_JMP);
        patch_here(fs, jf);
        adj(fs, -1);
        gen_expr(fs, n->c);
        patch_here(fs, je);
        return;
    }
    case N_ASSIGN: gen_assign(fs, n, true); return;
    case N_SEQ:
        for (Node *e = n->a; e; e = e->next) {
            gen_expr(fs, e);
            if (e->next) op(fs, OP_POP);
        }
        return;
    case N_YIELD: {
        bool async = (fs->fn->flags & FN_ASYNC) != 0;
        if (n->flags & NF_DELEGATE) {
            if (async) cg_error(fs, n->line, "yield* in async generators is not supported");
            gen_expr(fs, n->a);
            int k = new_local(fs);
            new_local(fs);
            loc_op(fs, OP_ITER_INIT, k);
            u8_(fs, 0);
            op(fs, OP_PUSH_UNDEF);
            op(fs, OP_PUSH_I8);
            u8_(fs, 0);
            int top = here(fs);
            op(fs, OP_DELEGATE);
            u16_(fs, (uint32_t)k);
            int jd = (int)fs->len;
            u32_(fs, 0);
            adj(fs, -1);
            op(fs, OP_YIELD_RAW);
            adj(fs, 1);
            jump_back(fs, OP_JMP, top);
            patch_to(fs, jd, here(fs));
            adj(fs, -1);
            return;
        }
        if (n->a)
            gen_expr(fs, n->a);
        else
            op(fs, OP_PUSH_UNDEF);
        if (async) op(fs, OP_AWAIT);
        op(fs, OP_YIELD);
        return;
    }
    case N_AWAIT:
        gen_expr(fs, n->a);
        op(fs, OP_AWAIT);
        return;
    case N_PRIV_IN:
        get_binding(fs, n->a->bind, n->a->u.atom);
        gen_expr(fs, n->b);
        op(fs, OP_HAS_PRIVATE);
        return;
    case N_SPREAD: cg_error(fs, n->line, "Unexpected token '...'");
    default: break;
    }
    cg_error(fs, n->line, "internal: cannot compile expression kind %d", n->kind);
}

/* evaluate for side effects only */
static void gen_effect(FS *fs, Node *n) {
    if (n->kind == N_ASSIGN) {
        gen_assign(fs, n, false);
        return;
    }
    if (n->kind == N_UPDATE) {
        gen_update(fs, n, false);
        return;
    }
    gen_expr(fs, n);
    op(fs, OP_POP);
}

/* ---------------- patterns ---------------- */
/* stores the value on top of the stack into target (consumes it) */
static void gen_target_store(FS *fs, Node *t, int mode) {
    switch (t->kind) {
    case N_IDENT: put_binding(fs, t->bind, t->u.atom, mode, false); return;
    case N_MEMBER:
        gen_expr(fs, t->a);
        op(fs, OP_SWAP);
        op(fs, OP_PUT_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        u32_(fs, 0xFFFFFFFFu);
        op(fs, OP_POP);
        return;
    case N_INDEX:
        gen_expr(fs, t->a);
        gen_expr(fs, t->b);
        op(fs, OP_ROT3L);
        op(fs, OP_PUT_ELEM);
        op(fs, OP_POP);
        return;
    case N_PRIV_MEMBER:
        gen_expr(fs, t->a);
        get_binding(fs, t->b->bind, t->b->u.atom);
        op(fs, OP_ROT3L);
        op(fs, OP_PUT_PRIVATE);
        op(fs, OP_POP);
        return;
    case N_SUPER_MEMBER:
        if (t->flags & NF_COMPUTED) cg_error(fs, t->line, "unsupported destructuring target");
        gen_home_proto(fs, t->a);
        get_binding(fs, t->c->bind, t->c->u.atom);
        op(fs, OP_ROT3L);
        op(fs, OP_PUT_SUPER_FIELD);
        u32_(fs, cst(fs, t->u.atom));
        op(fs, OP_POP);
        return;
    case N_ARRAY_PAT:
    case N_OBJECT_PAT: gen_pattern(fs, t, mode); return;
    case N_ASSIGN_PAT: {
        int j = jump(fs, OP_JNU_KEEP);
        if (t->a->kind == N_IDENT) infer_name(t->b, t->a->u.atom);
        gen_expr(fs, t->b);
        patch_here(fs, j);
        gen_target_store(fs, t->a, mode);
        return;
    }
    default: cg_error(fs, t->line, "Invalid destructuring assignment target");
    }
}

static void gen_pattern(FS *fs, Node *pat, int mode) {
    if (pat->kind == N_IDENT || pat->kind == N_MEMBER || pat->kind == N_INDEX || pat->kind == N_ASSIGN_PAT ||
        pat->kind == N_PRIV_MEMBER || pat->kind == N_SUPER_MEMBER) {
        gen_target_store(fs, pat, mode);
        return;
    }
    if (pat->kind == N_ARRAY_PAT) {
        int k = new_local(fs);
        new_local(fs);
        loc_op(fs, OP_ITER_INIT, k);
        u8_(fs, 0);
        for (Node *e = pat->a; e; e = e->next) {
            if (e->kind == N_HOLE) {
                loc_op(fs, OP_ITER_VALUE, k);
                op(fs, OP_POP);
            } else if (e->kind == N_REST) {
                loc_op(fs, OP_ITER_REST, k);
                gen_target_store(fs, e->a, mode);
            } else {
                loc_op(fs, OP_ITER_VALUE, k);
                gen_target_store(fs, e, mode);
            }
        }
        loc_op(fs, OP_ITER_CLOSE_IF, k);
        return;
    }
    if (pat->kind == N_OBJECT_PAT) {
        op(fs, OP_REQ_COERCIBLE);
        int t = new_local(fs);
        loc_op(fs, OP_PUT_LOC, t);
        bool has_rest = false;
        for (Node *e = pat->a; e; e = e->next)
            if (e->kind == N_REST) has_rest = true;
        int ex = -1;
        if (has_rest) {
            ex = new_local(fs);
            op(fs, OP_NEW_ARRAY);
            loc_op(fs, OP_PUT_LOC, ex);
        }
        for (Node *e = pat->a; e; e = e->next) {
            if (e->kind == N_REST) {
                loc_op(fs, OP_GET_LOC, t);
                loc_op(fs, OP_GET_LOC, ex);
                op(fs, OP_OBJ_REST);
                u16_(fs, 0);
                gen_target_store(fs, e->a, mode);
                continue;
            }
            Node *target = e->b;
            if (!(e->flags & NF_COMPUTED)) {
                if (has_rest) {
                    loc_op(fs, OP_GET_LOC, ex);
                    push_const(fs, e->a->u.atom);
                    op(fs, OP_ARRAY_APPEND);
                    op(fs, OP_POP);
                }
                loc_op(fs, OP_GET_LOC, t);
                op(fs, OP_GET_FIELD);
                u32_(fs, cst(fs, e->a->u.atom));
                u32_(fs, 0xFFFFFFFFu);
                if (target->kind == N_ASSIGN_PAT && target->a->kind == N_IDENT) infer_name(target->b, target->a->u.atom);
            } else {
                gen_expr(fs, e->a);
                op(fs, OP_TO_PROPKEY);
                if (has_rest) {
                    op(fs, OP_DUP);
                    loc_op(fs, OP_GET_LOC, ex);
                    op(fs, OP_SWAP);
                    op(fs, OP_ARRAY_APPEND);
                    op(fs, OP_POP);
                }
                loc_op(fs, OP_GET_LOC, t);
                op(fs, OP_SWAP);
                op(fs, OP_GET_ELEM);
            }
            gen_target_store(fs, target, mode);
        }
        return;
    }
    cg_error(fs, pat->line, "Invalid destructuring assignment target");
}

/* ---------------- statements ---------------- */
static Ctl *push_ctl(FS *fs, int kind) {
    Ctl *c = (Ctl *)calloc(1, sizeof(Ctl));
    if (!c) cg_error(fs, 0, "out of memory");
    c->kind = kind;
    c->brk = c->cont = c->fin = -1;
    c->iter_slot = -1;
    c->up = fs->ctl;
    for (int i = 0; i < fs->npending; i++) c->labels[c->nlabels++] = fs->pending_labels[i];
    fs->npending = 0;
    fs->ctl = c;
    return c;
}
static void pop_ctl(FS *fs, Ctl *c) {
    fs->ctl = c->up;
    free(c);
}

/* emit cleanup for leaving control entry c (when jumping out past it) */
static void unwind_one(FS *fs, Ctl *c, bool for_return) {
    switch (c->kind) {
    case CT_ENV:
        if (!for_return) op(fs, OP_POP_ENV);
        break;
    case CT_TRY: op(fs, OP_TRY_END); break;
    case CT_FINALLY: {
        op(fs, OP_TRY_END);
        if (!for_return) op(fs, OP_PUSH_UNDEF);
        int pos = jump(fs, OP_GOSUB);
        fs->len -= 4;
        u32_(fs, 0);
        chain_add(fs, &c->fin, pos);
        adj(fs, -1);
        if (!for_return) op(fs, OP_POP);
        break;
    }
    case CT_FINBODY:
        if (!for_return) {
            op(fs, OP_POP);
            op(fs, OP_POP);
        }
        break;
    case CT_LOOP:
        if (c->iter_slot >= 0) {
            op(fs, OP_TRY_END);
            loc_op(fs, OP_ITER_CLOSE, c->iter_slot);
        }
        break;
    default: break;
    }
}

static void gen_break(FS *fs, Node *n, bool is_cont) {
    jsval label = n->u.atom;
    Ctl *target = 0;
    for (Ctl *c = fs->ctl; c; c = c->up) {
        if (is_undef(label)) {
            if (c->kind == CT_LOOP || (!is_cont && c->kind == CT_SWITCH)) {
                target = c;
                break;
            }
        } else {
            bool has = false;
            for (int i = 0; i < c->nlabels; i++)
                if (c->labels[i] == label) has = true;
            if (has) {
                if (is_cont && c->kind != CT_LOOP) cg_error(fs, n->line, "Illegal continue statement: '%S' does not denote an iteration statement", vstr(label));
                target = c;
                break;
            }
        }
    }
    if (!target) {
        if (!is_undef(label)) cg_error(fs, n->line, "Undefined label '%S'", vstr(label));
        cg_error(fs, n->line, is_cont ? "Illegal continue statement: no surrounding iteration statement" : "Illegal break statement");
    }
    for (Ctl *c = fs->ctl; c != target; c = c->up) unwind_one(fs, c, false);
    if (!is_cont && target->kind == CT_LOOP && target->iter_slot >= 0) unwind_one(fs, target, false);
    int pos = jump(fs, OP_JMP);
    chain_add(fs, is_cont ? &target->cont : &target->brk, pos);
}

static void gen_return_value(FS *fs, int line) {
    /* value on stack */
    FuncNode *fn = fs->fn;
    if ((fn->flags & FN_DERIVED) && (fn->flags & FN_CTOR)) {
        int j = jump(fs, OP_JNU_KEEP);
        Binding *tb = fn->this_b;
        if (tb)
            get_binding(fs, tb, tb->name);
        else
            op(fs, OP_PUSH_THIS_CHK);
        patch_here(fs, j);
    }
    if ((fn->flags & FN_ASYNC) && (fn->flags & FN_GEN)) op(fs, OP_AWAIT);
    for (Ctl *c = fs->ctl; c; c = c->up) unwind_one(fs, c, true);
    set_line(fs, line);
    op(fs, OP_RETURN);
}

static void gen_implicit_return(FS *fs) {
    FuncNode *fn = fs->fn;
    if (fn->flags & FN_SCRIPT) {
        loc_op(fs, OP_GET_LOC, fs->completion);
        op(fs, OP_RETURN);
        return;
    }
    if ((fn->flags & FN_DERIVED) && (fn->flags & FN_CTOR)) {
        Binding *tb = fn->this_b;
        if (tb)
            get_binding(fs, tb, tb->name);
        else
            op(fs, OP_PUSH_THIS_CHK);
        op(fs, OP_RETURN);
        return;
    }
    op(fs, OP_RETURN_UNDEF);
}

static void gen_var_decl(FS *fs, Node *n) {
    int mode = n->u.op == VK_VAR ? M_INIT_VAR : M_INIT_LET;
    for (Node *d = n->a; d; d = d->next) {
        set_line(fs, d->line);
        if (d->b) {
            if (d->a->kind == N_IDENT) infer_name(d->b, d->a->u.atom);
            gen_expr(fs, d->b);
            gen_pattern(fs, d->a, mode);
        } else if (n->u.op != VK_VAR) {
            op(fs, OP_PUSH_UNDEF);
            gen_pattern(fs, d->a, mode);
        }
    }
}


static void gen_for_in_of(FS *fs, Node *n) {
    bool is_of = n->kind == N_FOR_OF;
    bool is_await = (n->flags & NF_AWAIT) != 0;
    Scope *ls = n->scope;
    Node *lhs = n->a;
    bool decl = lhs->kind == N_VAR;
    int vk = decl ? lhs->u.op : -1;
    /* TDZ scope for the iterated expression */
    enter_scope(fs, ls, n->line);
    gen_expr(fs, n->b);
    leave_scope(fs, ls);
    /* reset slot allocation of loop scope bindings is not needed: slots are reused per iteration */
    int k = new_local(fs);
    new_local(fs);
    Ctl *c;
    int top;
    if (is_of) {
        loc_op(fs, OP_ITER_INIT, k);
        u8_(fs, is_await ? 1 : 0);
    } else
        loc_op(fs, OP_FOR_IN_INIT, k);
    c = push_ctl(fs, CT_LOOP);
    top = here(fs);
    int jexit;
    if (is_of && is_await) {
        loc_op(fs, OP_ITER_CALL_NEXT, k);
        op(fs, OP_AWAIT);
        jexit = jump(fs, OP_ITER_RES);
        adj(fs, 1);
    } else {
        loc_op(fs, is_of ? OP_ITER_NEXT : OP_FOR_IN_NEXT, k);
        jexit = (int)fs->len;
        u32_(fs, 0);
    }
    int jh = -1;
    if (is_of) {
        c->iter_slot = k;
        op(fs, OP_TRY);
        u8_(fs, 1);
        jh = (int)fs->len;
        u32_(fs, 0);
    }
    /* per-iteration scope */
    enter_scope(fs, ls, n->line);
    if (decl) {
        Node *d = lhs->a;
        gen_pattern(fs, d->a, vk == VK_VAR ? M_INIT_VAR : M_INIT_LET);
    } else
        gen_pattern(fs, lhs, M_ASSIGN);
    gen_stmt(fs, n->d);
    leave_scope(fs, ls);
    chain_resolve(fs, c->cont, here(fs));
    if (is_of) op(fs, OP_TRY_END);
    jump_back(fs, OP_JMP, top);
    if (is_of) {
        /* exception handler: close iterator and rethrow */
        patch_here(fs, jh);
        adj(fs, 1);
        loc_op(fs, OP_ITER_CLOSE_QUIET, k);
        op(fs, OP_RETHROW);
    }
    patch_here(fs, jexit);
    chain_resolve(fs, c->brk, here(fs));
    pop_ctl(fs, c);
}

static void gen_try(FS *fs, Node *n) {
    Ctl *cf = 0;
    int jfh = -1;
    if (n->d) {
        op(fs, OP_TRY);
        u8_(fs, 1);
        jfh = (int)fs->len;
        u32_(fs, 0);
        cf = push_ctl(fs, CT_FINALLY);
    }
    if (n->c) {
        op(fs, OP_TRY);
        u8_(fs, 0);
        int jc = (int)fs->len;
        u32_(fs, 0);
        Ctl *ct = push_ctl(fs, CT_TRY);
        gen_stmt(fs, n->a);
        op(fs, OP_TRY_END);
        pop_ctl(fs, ct);
        int jafter = jump(fs, OP_JMP);
        patch_here(fs, jc);
        adj(fs, 1);
        enter_scope(fs, n->scope, n->line);
        if (n->b)
            gen_pattern(fs, n->b, M_INIT_LET);
        else
            op(fs, OP_POP);
        gen_stmt(fs, n->c);
        leave_scope(fs, n->scope);
        patch_here(fs, jafter);
    } else
        gen_stmt(fs, n->a);
    if (n->d) {
        op(fs, OP_TRY_END);
        int fin_chain = cf->fin;
        pop_ctl(fs, cf);
        op(fs, OP_PUSH_UNDEF);
        int g1 = jump(fs, OP_GOSUB);
        fs->len -= 4;
        u32_(fs, 0);
        chain_add(fs, &fin_chain, g1);
        op(fs, OP_POP);
        adj(fs, -1);
        int jend = jump(fs, OP_JMP);
        /* exception path */
        patch_here(fs, jfh);
        adj(fs, 1);
        int g2 = jump(fs, OP_GOSUB);
        fs->len -= 4;
        u32_(fs, 0);
        chain_add(fs, &fin_chain, g2);
        adj(fs, -1);
        op(fs, OP_RETHROW);
        /* finally body: stack [completion, retaddr] */
        chain_resolve(fs, fin_chain, here(fs));
        adj(fs, 2);
        Ctl *fb = push_ctl(fs, CT_FINBODY);
        gen_stmt(fs, n->d);
        pop_ctl(fs, fb);
        op(fs, OP_RET);
        adj(fs, -1);
        patch_here(fs, jend);
    }
}

static void gen_switch(FS *fs, Node *n) {
    gen_expr(fs, n->a);
    enter_scope(fs, n->scope, n->line);
    int t = new_local(fs);
    loc_op(fs, OP_PUT_LOC, t);
    Ctl *c = push_ctl(fs, CT_SWITCH);
    int ncase = count_list(n->b);
    int *jmps = (int *)calloc((size_t)ncase + 1, sizeof(int));
    int i = 0, defidx = -1;
    for (Node *cs = n->b; cs; cs = cs->next, i++) {
        if (cs->flags & 1) {
            defidx = i;
            continue;
        }
        loc_op(fs, OP_GET_LOC, t);
        gen_expr(fs, cs->a);
        op(fs, OP_SEQ);
        jmps[i] = jump(fs, OP_JT);
    }
    int jdef = jump(fs, OP_JMP);
    i = 0;
    for (Node *cs = n->b; cs; cs = cs->next, i++) {
        if (i == defidx)
            patch_here(fs, jdef);
        else
            patch_here(fs, jmps[i]);
        gen_stmts(fs, cs->b);
    }
    if (defidx < 0) patch_here(fs, jdef);
    free(jmps);
    chain_resolve(fs, c->brk, here(fs));
    pop_ctl(fs, c);
    leave_scope(fs, n->scope);
}

static void gen_stmt(FS *fs, Node *n) {
    if (!n) return;
    if (check_stack(fs->rt)) cg_error(fs, n->line, "Maximum call stack size exceeded");
    set_line(fs, n->line);
    int base_depth = fs->depth;
    switch (n->kind) {
    case N_EMPTY:
    case N_DEBUGGER: break;
    case N_EXPR:
        if (fs->fn->flags & FN_SCRIPT) {
            gen_expr(fs, n->a);
            loc_op(fs, OP_PUT_LOC, fs->completion);
        } else
            gen_effect(fs, n->a);
        break;
    case N_VAR: gen_var_decl(fs, n); break;
    case N_FUNC_DECL:
        if (n->flags & NF_ANNEXB) {
            /* copy the block binding to the var binding of the same name */
            FuncNode *fn = n->u.fn;
            Binding *bb = 0;
            for (Scope *s = fs->cur; s; s = s->parent) {
                for (Binding *b = s->first; b; b = b->next)
                    if (b->name == fn->name && b->kind == B_FUNC) {
                        bb = b;
                        break;
                    }
                if (bb) break;
            }
            Binding *vb = 0;
            Scope *s = fs->cur;
            while (s && s->kind != SC_FUNC) s = s->parent;
            if (s && !s->is_script) {
                for (Binding *b = s->first; b; b = b->next)
                    if (b->name == fn->name) vb = b;
            }
            if (bb) {
                get_binding(fs, bb, bb->name);
                put_binding(fs, vb, fn->name, M_INIT_VAR, false);
            }
        }
        break;
    case N_CLASS_DECL: {
        ClassNode *cls = n->u.cls;
        gen_class(fs, cls, JS_UNDEF);
        /* binding in the enclosing scope */
        Binding *b = 0;
        for (Scope *s = fs->cur; s; s = s->parent) {
            for (Binding *x = s->first; x; x = x->next)
                if (x->name == cls->name && x->kind == B_CLASS) {
                    b = x;
                    break;
                }
            if (b || s->kind == SC_FUNC) break;
        }
        put_binding(fs, b, cls->name, M_INIT_LET, false);
        break;
    }
    case N_BLOCK:
        enter_scope(fs, n->scope, n->line);
        gen_stmts(fs, n->a);
        leave_scope(fs, n->scope);
        break;
    case N_IF: {
        gen_expr(fs, n->a);
        int jf = jump(fs, OP_JF);
        gen_stmt(fs, n->b);
        if (n->c) {
            int je = jump(fs, OP_JMP);
            patch_here(fs, jf);
            gen_stmt(fs, n->c);
            patch_here(fs, je);
        } else
            patch_here(fs, jf);
        break;
    }
    case N_WHILE: {
        Ctl *c = push_ctl(fs, CT_LOOP);
        int top = here(fs);
        gen_expr(fs, n->a);
        int jf = jump(fs, OP_JF);
        gen_stmt(fs, n->b);
        chain_resolve(fs, c->cont, top);
        jump_back(fs, OP_JMP, top);
        patch_here(fs, jf);
        chain_resolve(fs, c->brk, here(fs));
        pop_ctl(fs, c);
        break;
    }
    case N_DO: {
        Ctl *c = push_ctl(fs, CT_LOOP);
        int top = here(fs);
        gen_stmt(fs, n->a);
        chain_resolve(fs, c->cont, here(fs));
        gen_expr(fs, n->b);
        jump_back(fs, OP_JT, top);
        chain_resolve(fs, c->brk, here(fs));
        pop_ctl(fs, c);
        break;
    }
    case N_FOR: {
        Scope *ls = n->scope;
        jsval saved_labels[8];
        int nsl = fs->npending;
        memcpy(saved_labels, fs->pending_labels, sizeof saved_labels);
        fs->npending = 0;
        enter_scope(fs, ls, n->line);
        if (n->a) {
            if (n->a->kind == N_VAR)
                gen_var_decl(fs, n->a);
            else
                gen_effect(fs, n->a);
        }
        if (ls->has_env) op(fs, OP_COPY_ENV);
        memcpy(fs->pending_labels, saved_labels, sizeof saved_labels);
        fs->npending = nsl;
        Ctl *c = push_ctl(fs, CT_LOOP);
        int jtest = jump(fs, OP_JMP);
        int top = here(fs);
        gen_stmt(fs, n->d);
        chain_resolve(fs, c->cont, here(fs));
        if (ls->has_env) op(fs, OP_COPY_ENV);
        if (n->c) gen_effect(fs, n->c);
        patch_here(fs, jtest);
        if (n->b) {
            gen_expr(fs, n->b);
            jump_back(fs, OP_JT, top);
        } else
            jump_back(fs, OP_JMP, top);
        chain_resolve(fs, c->brk, here(fs));
        pop_ctl(fs, c);
        leave_scope(fs, ls);
        break;
    }
    case N_FOR_IN:
    case N_FOR_OF: gen_for_in_of(fs, n); break;
    case N_RETURN:
        if (n->a)
            gen_expr(fs, n->a);
        else
            op(fs, OP_PUSH_UNDEF);
        gen_return_value(fs, n->line);
        break;
    case N_BREAK: gen_break(fs, n, false); break;
    case N_CONTINUE: gen_break(fs, n, true); break;
    case N_THROW:
        gen_expr(fs, n->a);
        set_line(fs, n->line);
        op(fs, OP_THROW);
        break;
    case N_TRY: gen_try(fs, n); break;
    case N_SWITCH: gen_switch(fs, n); break;
    case N_LABELED: {
        Node *s = n->a;
        if (fs->npending < 8) fs->pending_labels[fs->npending++] = n->u.atom;
        if (s->kind == N_FOR || s->kind == N_WHILE || s->kind == N_DO || s->kind == N_FOR_IN || s->kind == N_FOR_OF ||
            s->kind == N_LABELED) {
            gen_stmt(fs, s);
        } else {
            Ctl *c = push_ctl(fs, CT_LABEL);
            gen_stmt(fs, s);
            chain_resolve(fs, c->brk, here(fs));
            pop_ctl(fs, c);
        }
        fs->npending = 0;
        break;
    }
    default: cg_error(fs, n->line, "internal: cannot compile statement kind %d", n->kind);
    }
    fs->npending = 0;
    fs->depth = base_depth;
}
static void gen_stmts(FS *fs, Node *n) {
    for (; n; n = n->next) gen_stmt(fs, n);
}

/* ---------------- classes ---------------- */
static void gen_field_entries(FS *fs, Node *list) {
    for (Node *e = list; e; e = e->next) {
        set_line(fs, e->line);
        if (e->u.op == PK_STATIC_BLOCK) {
            gen_function(fs, e->b->u.fn);
            op(fs, OP_PUSH_THIS);
            op(fs, OP_CALL);
            u16_(fs, 0);
            u32_(fs, 0xFFFFFFFFu);
            adj(fs, -1);
            op(fs, OP_POP);
            continue;
        }
        if (e->u.op != PK_FIELD) {
            /* private method install: a = private name ref, b = hidden closure ref */
            op(fs, OP_PUSH_THIS);
            get_binding(fs, e->a->bind, e->a->u.atom);
            get_binding(fs, e->b->bind, e->b->u.atom);
            op(fs, OP_DEF_PRIVATE);
            u8_(fs, e->u.op == PK_GET ? 2 : e->u.op == PK_SET ? 3 : 1);
            continue;
        }
        op(fs, OP_PUSH_THIS);
        if (e->flags & NF_PRIVATE) {
            get_binding(fs, e->a->bind, e->a->u.atom);
        } else if (e->flags & NF_COMPUTED) {
            get_binding(fs, e->a->bind, e->a->u.atom);
        } else
            push_const(fs, e->a->u.atom);
        if (e->b) {
            if (!(e->flags & (NF_COMPUTED))) infer_name(e->b, e->a->u.atom);
            gen_expr(fs, e->b);
            if ((e->flags & NF_COMPUTED) && is_anon_fn(e->b)) {
                op(fs, OP_SET_FUNC_NAME);
                u8_(fs, 0);
            }
        } else
            op(fs, OP_PUSH_UNDEF);
        if (e->flags & NF_PRIVATE) {
            op(fs, OP_DEF_PRIVATE);
            u8_(fs, 0);
        } else {
            op(fs, OP_DEFINE_ELEM);
            op(fs, OP_POP);
        }
    }
}

static FuncProto *compile_fn(FS *parent, FuncNode *fn);

static void gen_class(FS *fs, ClassNode *cls, jsval inferred) {
    (void)inferred;
    Scope *cs = cls->scope;
    enter_scope(fs, cs, 0);
    if (cls->heritage) gen_expr(fs, cls->heritage);
    FuncProto *ctor = compile_fn(fs, cls->ctor);
    op(fs, OP_CLASS);
    u32_(fs, cst_new(fs, mkptr(ctor)));
    u8_(fs, cls->heritage ? 1 : 0);
    adj(fs, cls->heritage ? 1 : 2);
    /* private names */
    for (Binding *b = cs->first; b; b = b->next)
        if (b->kind == B_PRIVATE) {
            op(fs, OP_NEW_PRIVATE);
            u32_(fs, cst(fs, b->name));
            put_binding(fs, b, b->name, M_INIT_LET, false);
        }
    for (Node *m = cls->members; m; m = m->next) {
        set_line(fs, m->line);
        if (m->u.op == PK_FIELD) {
            /* computed field key: evaluate now */
            gen_expr(fs, m->a);
            op(fs, OP_TO_PROPKEY);
            put_binding(fs, m->c->bind, m->c->u.atom, M_INIT_LET, false);
            continue;
        }
        bool is_static = (m->flags & NF_STATIC) != 0;
        if ((m->flags & NF_PRIVATE) && !is_static) {
            op(fs, OP_DUP); /* proto */
            gen_function(fs, m->b->u.fn);
            op(fs, OP_SET_HOME);
            put_binding(fs, m->c->bind, m->c->u.atom, M_INIT_LET, false);
            continue;
        }
        op(fs, is_static ? OP_OVER : OP_DUP);
        if (m->flags & NF_PRIVATE)
            get_binding(fs, m->d->bind, m->d->u.atom);
        else if (m->flags & NF_COMPUTED) {
            gen_expr(fs, m->a);
            op(fs, OP_TO_PROPKEY);
        } else
            push_const(fs, m->a->u.atom);
        gen_function(fs, m->b->u.fn);
        op(fs, OP_DEFINE_METHOD);
        u8_(fs, m->u.op == PK_GET ? 1 : m->u.op == PK_SET ? 2 : 0);
        u8_(fs, (m->flags & NF_PRIVATE) ? 2 : 0);
        op(fs, OP_POP);
    }
    if (cls->field_init) {
        gen_function(fs, cls->field_init);
        op(fs, OP_SET_FIELDS);
    }
    op(fs, OP_POP); /* proto */
    if (cls->name_b) {
        op(fs, OP_DUP);
        put_binding(fs, cls->name_b, cls->name_b->name, M_INIT_LET, false);
    }
    if (cls->static_init) {
        op(fs, OP_DUP);
        gen_function(fs, cls->static_init);
        op(fs, OP_SET_HOME);
        op(fs, OP_OVER);
        op(fs, OP_CALL);
        u16_(fs, 0);
        u32_(fs, 0xFFFFFFFFu);
        adj(fs, -1);
        op(fs, OP_POP);
    }
    leave_scope(fs, cs);
}

/* ---------------- functions ---------------- */
static uint32_t fp_flags(FuncNode *fn) {
    uint32_t f = 0;
    if (fn->flags & FN_STRICT) f |= FP_STRICT;
    if (fn->flags & FN_ARROW) f |= FP_ARROW;
    if (fn->flags & FN_METHOD) f |= FP_METHOD;
    if (fn->flags & (FN_GETTER | FN_SETTER)) f |= FP_GETSET;
    if (fn->flags & FN_CTOR) f |= FP_CLASS_CTOR | FP_STRICT;
    if (fn->flags & FN_DERIVED) f |= FP_DERIVED;
    if (fn->flags & FN_GEN) f |= FP_GENERATOR;
    if (fn->flags & FN_ASYNC) f |= FP_ASYNC;
    if (fn->flags & FN_SCRIPT) f |= FP_SCRIPT;
    if (fn->flags & (FN_FIELD_INIT | FN_STATIC_INIT)) f |= FP_FIELD_INIT;
    return f;
}

static FuncProto *compile_fn(FS *parent, FuncNode *fn) {
    js_rt *rt = parent ? parent->rt : 0;
    FS fsv;
    FS *fs = &fsv;
    memset(fs, 0, sizeof *fs);
    fs->rt = parent->rt;
    fs->cg = parent->cg;
    fs->up = parent;
    fs->fn = fn;
    fs->chain = -1;
    fs->last_line = -1;
    fs->args_slot = -1;
    fs->rest_slot = -1;
    fs->cur = fn->scope->parent;
    fs->cg->cur = fs;
    (void)rt;
    if (check_stack(fs->rt)) cg_error(fs, fn->line, "Maximum call stack size exceeded");

    Scope *sc = fn->scope;
    prepare_scope(sc);
    fs->nlocals = fn->nparams;
    if (fn->has_rest) fs->rest_slot = new_local(fs);
    /* params: simple identifiers live in their argument slot */
    int idx = 0;
    for (Node *p = fn->params; p; p = p->next) {
        if (p->kind == N_IDENT && p->bind) {
            p->bind->param_index = idx;
            if (!p->bind->captured) p->bind->slot = idx;
        } else if (p->kind == N_REST && p->a->kind == N_IDENT && p->a->bind) {
            if (!p->a->bind->captured) p->a->bind->slot = fs->rest_slot;
        }
        if (p->kind != N_REST) idx++;
    }
    if (fn->args_b) {
        fs->args_slot = new_local(fs);
        if (!fn->args_b->captured) fn->args_b->slot = fs->args_slot;
    }
    if (fn->flags & FN_SCRIPT) fs->completion = new_local(fs);
    /* remaining bindings of the function scope */
    for (Binding *b = sc->first; b; b = b->next)
        if (!b->captured && b->slot < 0 && b->kind != B_THIS && b->kind != B_NEWTARGET && b->kind != B_FUNCOBJ)
            b->slot = new_local(fs);
    fs->cur = sc;
    set_line(fs, fn->line);

    /* prologue */
    if ((fn->flags & FN_CTOR) && !(fn->flags & FN_DERIVED) && fn->cls && fn->cls->field_init) {
        op(fs, OP_PUSH_THIS);
        op(fs, OP_PUSH_FUNC);
        op(fs, OP_INIT_FIELDS);
        op(fs, OP_POP);
    }
    if (sc->has_env) {
        op(fs, OP_PUSH_ENV);
        u16_(fs, (uint32_t)sc->env_size);
    }
    for (Binding *b = sc->first; b; b = b->next) {
        if (!b->captured) continue;
        switch (b->kind) {
        case B_THIS:
            op(fs, (fn->flags & FN_DERIVED) ? OP_PUSH_UNINIT : OP_PUSH_THIS);
            break;
        case B_NEWTARGET: op(fs, OP_PUSH_NEW_TARGET); break;
        case B_FUNCOBJ: op(fs, OP_PUSH_FUNC); break;
        case B_ARGS: loc_op(fs, OP_GET_LOC, fs->args_slot); break;
        case B_PARAM:
            if (b->param_index >= 0) {
                loc_op(fs, OP_GET_LOC, b->param_index);
                break;
            }
            continue;
        default: continue;
        }
        op(fs, OP_PUT_ENV);
        u8_(fs, 0);
        u16_(fs, (uint32_t)b->env_idx);
    }
    if (fn->name_b) {
        op(fs, OP_PUSH_FUNC);
        put_binding(fs, fn->name_b, fn->name_b->name, M_INIT_LET, false);
    }
    for (Binding *b = sc->first; b; b = b->next)
        if (b->tdz && b->kind != B_THIS) init_uninit(fs, b);
    /* parameter initialization */
    idx = 0;
    for (Node *p = fn->params; p; p = p->next) {
        if (p->kind == N_IDENT) {
            idx++;
            continue;
        }
        if (p->kind == N_REST) {
            if (p->a->kind == N_IDENT && p->a->bind && !p->a->bind->captured) continue;
            loc_op(fs, OP_GET_LOC, fs->rest_slot);
            gen_pattern(fs, p->a, M_INIT_LET);
            continue;
        }
        loc_op(fs, OP_GET_LOC, idx);
        gen_pattern(fs, p, M_INIT_LET);
        idx++;
    }
    if (fn->flags & FN_DEFAULT_CTOR) {
        if (fn->flags & FN_DERIVED) {
            Node *rf = fn->fields, *rnt = rf->next, *rthis = rnt->next;
            get_binding(fs, rf->bind, rf->u.atom);
            op(fs, OP_GET_SUPER_CTOR);
            get_binding(fs, rnt->bind, rnt->u.atom);
            loc_op(fs, OP_GET_LOC, fs->rest_slot);
            op(fs, OP_SUPER_CALL_ARRAY);
            op(fs, OP_BIND_THIS);
            if (!rthis->bind || !rthis->bind->captured) {
                u8_(fs, 0xFF);
                u16_(fs, 0);
            } else {
                u8_(fs, 0);
                u16_(fs, (uint32_t)rthis->bind->env_idx);
            }
            get_binding(fs, rf->bind, rf->u.atom);
            op(fs, OP_INIT_FIELDS);
            op(fs, OP_RETURN);
        } else
            op(fs, OP_RETURN_UNDEF);
    } else {
        /* hoisted function declarations */
        for (Binding *b = sc->first; b; b = b->next)
            if (b->kind == B_FUNC && b->decl_fn) {
                gen_function(fs, b->decl_fn);
                put_binding(fs, b, b->name, M_INIT_LET, false);
            }
        if (fn->flags & FN_SCRIPT) {
            for (Node *g = fn->gvars; g; g = g->next) global_op(fs, OP_DEF_GLOBAL_VAR, g->u.atom);
            for (Node *g = fn->glex; g; g = g->next) {
                op(fs, OP_DEF_GLOBAL_LEX);
                u32_(fs, cst(fs, g->u.atom));
                u8_(fs, g->flags & 1);
            }
            for (Node *g = fn->gfuncs; g; g = g->next) {
                gen_function(fs, g->u.fn);
                global_op(fs, OP_DEF_GLOBAL_FUNC, g->u.fn->name);
            }
        }
        if (fn->flags & FN_GEN) op(fs, OP_INITIAL_YIELD);
        if (fn->flags & (FN_FIELD_INIT | FN_STATIC_INIT)) {
            gen_field_entries(fs, fn->fields);
            if (fn->body) gen_stmts(fs, fn->body);
        } else
            gen_stmts(fs, fn->body);
        gen_implicit_return(fs);
    }

    /* build FuncProto */
    FuncProto *fp = (FuncProto *)gc_alloc(fs->rt, sizeof(FuncProto), GC_FUNCPROTO);
    if (!fp) cg_error(fs, fn->line, "out of memory");
    fp->code = fs->code;
    fp->code_len = fs->len;
    fp->consts = fs->consts;
    fp->nconsts = fs->nconsts;
    fp->lines = fs->lines;
    fp->nlines = fs->nlines;
    fp->flags = fp_flags(fn);
    if (fn->uses_args) fp->flags |= FP_NEEDS_ARGS;
    if (fn->has_rest) fp->flags |= FP_HAS_REST;
    fp->nparams = (uint16_t)fn->nparams;
    fp->length = (uint16_t)fn->length;
    fp->nlocals = (uint16_t)fs->nlocals;
    fp->max_stack = (uint16_t)(fs->maxdepth + 8 > 65535 ? 65535 : fs->maxdepth + 8);
    fp->args_slot = fs->args_slot;
    fp->rest_slot = fs->rest_slot;
    fp->name = fn->name;
    fp->file = fs->cg->pr->file;
    fp->src = fs->cg->pr->src;
    fp->src_start = fn->src_start;
    fp->src_end = fn->src_end;
    free(fs->chash);
    fs->code = 0;
    fs->consts = 0;
    fs->lines = 0;
    while (fs->ctl) {
        Ctl *c = fs->ctl;
        fs->ctl = c->up;
        free(c);
    }
    fs->cg->cur = parent;
    fn->proto = fp;
    return fp;
}

static void gen_function(FS *fs, FuncNode *fn) {
    FuncProto *fp = compile_fn(fs, fn);
    op(fs, OP_CLOSURE);
    u32_(fs, cst_new(fs, mkptr(fp)));
}

static void free_fs_chain(FS *fs) {
    for (; fs; fs = fs->up) {
        free(fs->code);
        free(fs->consts);
        free(fs->lines);
        free(fs->chash);
        fs->code = 0;
        fs->consts = 0;
        fs->lines = 0;
        fs->chash = 0;
        while (fs->ctl) {
            Ctl *c = fs->ctl;
            fs->ctl = c->up;
            free(c);
        }
    }
}

FuncProto *js_codegen(js_rt *rt, ParseResult *pr, int flags) {
    (void)flags;
    CG *cg = (CG *)calloc(1, sizeof(CG));
    if (!cg) return 0;
    cg->rt = rt;
    cg->pr = pr;
    resolve_all(rt, pr);
    FuncProto *res = 0;
    /* a dummy parent state so compile_fn has a context */
    FS root;
    memset(&root, 0, sizeof root);
    root.rt = rt;
    root.cg = cg;
    root.fn = pr->script;
    if (JS_SETJMP(cg->jb) == 0) {
        res = compile_fn(&root, pr->script);
    } else {
        free_fs_chain(cg->cur);
        res = 0;
    }
    free(cg);
    return res;
}

FuncProto *compile_script(js_rt *rt, const char *src, size_t len, const char *file, int flags, JSString **err) {
    ParseResult pr;
    *err = 0;
    if (js_parse(rt, src, len, file, flags, &pr) < 0) {
        *err = jsfmt(rt, "%s (%s:%d)", pr.errmsg, file ? file : "<anonymous>", pr.errline);
        arena_free(&pr.arena);
        return 0;
    }
    FuncProto *fp = js_codegen(rt, &pr, flags);
    if (!fp) *err = jsfmt(rt, "%s (%s:%d)", pr.errmsg, file ? file : "<anonymous>", pr.errline);
    free(pr.refs);
    arena_free(&pr.arena);
    return fp;
}
