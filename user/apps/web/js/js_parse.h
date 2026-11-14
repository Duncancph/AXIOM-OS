/* AXIOM JavaScript engine: AST, scopes and parser interface (shared by parser and compiler). */
#pragma once
#include "js_int.h"

enum {
    N_NUM = 1, N_STR, N_TEMPLATE, N_TAGGED, N_REGEXP, N_IDENT, N_THIS, N_NEW_TARGET, N_SUPER_MEMBER, N_SUPER_CALL,
    N_NULL, N_TRUE, N_FALSE, N_ARRAY, N_HOLE, N_OBJECT, N_PROP, N_FUNC, N_CLASS, N_MEMBER, N_INDEX, N_PRIV_MEMBER,
    N_CALL, N_NEW, N_OPTCHAIN, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_COND, N_ASSIGN, N_SEQ, N_SPREAD, N_YIELD,
    N_AWAIT, N_PRIV_IN, N_ARRAY_PAT, N_OBJECT_PAT, N_PROP_PAT, N_ASSIGN_PAT, N_REST, N_BIGINT,
    /* statements */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_EMPTY, N_IF, N_FOR, N_FOR_IN, N_FOR_OF, N_WHILE, N_DO, N_RETURN, N_BREAK,
    N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE, N_LABELED, N_FUNC_DECL, N_CLASS_DECL, N_DEBUGGER,
    N_CLASS_MEMBER
};
/* node flags */
enum {
    NF_COMPUTED = 1, NF_OPTIONAL = 2, NF_PREFIX = 4, NF_DELEGATE = 8, NF_AWAIT = 16, NF_STATIC = 32, NF_PAREN = 64,
    NF_ANNEXB = 128, NF_SHORT_CHAIN = 256, NF_PRIVATE = 512
};
/* property kinds (u.op of N_PROP / N_CLASS_MEMBER) */
enum { PK_INIT, PK_GET, PK_SET, PK_METHOD, PK_SHORTHAND, PK_SHORTHAND_INIT, PK_SPREAD, PK_PROTO, PK_FIELD, PK_STATIC_BLOCK };
/* declaration kinds */
enum { VK_VAR, VK_LET, VK_CONST };

/* operators (u.op) */
enum {
    OPR_ADD = 1, OPR_SUB, OPR_MUL, OPR_DIV, OPR_MOD, OPR_POW, OPR_SHL, OPR_SAR, OPR_SHR, OPR_BAND, OPR_BOR, OPR_BXOR,
    OPR_EQ, OPR_NE, OPR_SEQ, OPR_SNE, OPR_LT, OPR_LE, OPR_GT, OPR_GE, OPR_IN, OPR_INSTANCEOF, OPR_AND, OPR_OR,
    OPR_NULLISH, OPR_NEG, OPR_PLUS, OPR_NOT, OPR_BITNOT, OPR_TYPEOF, OPR_VOID, OPR_DELETE, OPR_INC, OPR_DEC
};

typedef struct Node Node;
typedef struct Scope Scope;
typedef struct Binding Binding;
typedef struct FuncNode FuncNode;
typedef struct ClassNode ClassNode;

struct Node {
    uint16_t kind, flags;
    int32_t line;
    Node *next;
    Node *a, *b, *c, *d;
    union {
        double num;
        jsval atom;
        int op;
        FuncNode *fn;
        ClassNode *cls;
    } u;
    jsval str2;        /* raw template text, regexp flags, label */
    Scope *scope;      /* identifier refs: scope of the reference; scoped statements: their scope */
    Binding *bind;     /* resolved binding (NULL = global) */
    uint32_t pos, end; /* source offsets */
};

enum { SC_FUNC, SC_BLOCK, SC_CATCH, SC_CLASS };
struct Scope {
    uint8_t kind;
    bool has_env;
    bool is_script;
    Scope *parent;
    FuncNode *func;
    Binding *first, *last;
    int env_size;
};

enum {
    B_VAR, B_LET, B_CONST, B_CLASS, B_FUNC, B_PARAM, B_CATCH, B_THIS, B_NEWTARGET, B_ARGS, B_FUNCOBJ, B_FUNCNAME,
    B_PRIVATE, B_HIDDEN
};
struct Binding {
    jsval name;
    uint8_t kind;
    bool captured;
    bool is_const;
    bool tdz;
    int slot;    /* frame local, -1 if none */
    int env_idx; /* env slot if captured */
    int param_index; /* for simple params: the argument slot */
    Scope *scope;
    Binding *next;
    FuncNode *decl_fn; /* function declarations */
};

enum {
    FN_ARROW = 1, FN_ASYNC = 2, FN_GEN = 4, FN_METHOD = 8, FN_GETTER = 16, FN_SETTER = 32, FN_CTOR = 64,
    FN_DERIVED = 128, FN_STRICT = 256, FN_EXPR = 512, FN_FIELD_INIT = 1024, FN_DEFAULT_CTOR = 2048,
    FN_STATIC_INIT = 4096, FN_SCRIPT = 8192, FN_EXPR_BODY = 16384, FN_CLASS_FIELDS = 32768
};
struct FuncNode {
    jsval name;
    Node *params; /* list of patterns */
    Node *body;   /* statement list */
    Scope *scope;
    FuncNode *parent;
    uint32_t flags;
    int nparams;  /* formal slots (params before rest) */
    int length;   /* .length */
    bool simple_params;
    bool has_rest;
    bool uses_args;
    uint32_t src_start, src_end;
    int line;
    Binding *this_b, *nt_b, *args_b, *func_b, *name_b;
    ClassNode *cls;    /* constructor of this class */
    Node *fields;      /* field init / static init functions: list of N_CLASS_MEMBER */
    FuncProto *proto;  /* compiled */
    /* script only */
    Node *gvars;  /* list of N_IDENT-like entries (u.atom) for global var names */
    Node *gfuncs; /* list of N_FUNC_DECL nodes declared at script top level */
    Node *glex;   /* list: u.atom name, flags: 1 const */
};

struct ClassNode {
    jsval name;
    Node *heritage;
    FuncNode *ctor;
    Node *members; /* N_CLASS_MEMBER: u.op kind, flags NF_STATIC/NF_COMPUTED/NF_PRIVATE, a key, b value */
    Scope *scope;
    Binding *name_b;
    FuncNode *field_init;  /* instance fields + private methods */
    FuncNode *static_init; /* static fields and blocks */
    uint32_t src_start, src_end;
};

typedef struct Arena {
    struct ArenaBlk *blk;
} Arena;
void *arena_alloc(Arena *a, size_t n);
void arena_free(Arena *a);

typedef struct ParseResult {
    FuncNode *script;
    Arena arena;
    JSString *src;
    JSString *file;
    char errmsg[256];
    int errline;
} ParseResult;

/* returns 0 ok, -1 syntax error (errmsg filled) */
int js_parse(js_rt *rt, const char *src, size_t len, const char *file, int flags, ParseResult *out);
/* resolution + code generation; returns NULL with errmsg on error */
FuncProto *js_codegen(js_rt *rt, ParseResult *pr, int flags);
