/* AXIOM JavaScript engine: internal definitions shared by the engine's .c files. */
#pragma once
#include "js_port.h"
#include "js.h"

#define JS_INLINE static inline __attribute__((always_inline))
#define JS_NOINLINE __attribute__((noinline))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---------------- values ----------------
 * A jsval is a double unless its top 16 bits are >= 0xFFF9 (no arithmetic produces such NaNs;
 * raw bit patterns read from typed arrays are canonicalized). */
#define TAG_MISC 0xFFF9u
#define TAG_STR 0xFFFAu
#define TAG_OBJ 0xFFFBu
#define TAG_SYM 0xFFFCu
#define TAG_BIG 0xFFFDu
#define TAG_PTR 0xFFFEu /* internal GC cell, never visible to JS */
#define MKTAG(t, p) (((uint64_t)(t) << 48) | (uint64_t)(uintptr_t)(p))
#define JS_UNDEF MKTAG(TAG_MISC, 0)
#define JS_NULL MKTAG(TAG_MISC, 1)
#define JS_FALSE MKTAG(TAG_MISC, 2)
#define JS_TRUE MKTAG(TAG_MISC, 3)
#define JS_HOLE MKTAG(TAG_MISC, 4)   /* array hole / deleted property */
#define JS_UNINIT MKTAG(TAG_MISC, 5) /* TDZ */
#define JS_EXC MKTAG(TAG_MISC, 6)    /* "an exception is pending" */
#define JS_NAN_BITS 0x7FF8000000000000ull

JS_INLINE uint32_t vtag(jsval v) { return (uint32_t)(v >> 48); }
JS_INLINE bool is_num(jsval v) { return (v >> 48) < TAG_MISC; }
JS_INLINE bool is_str(jsval v) { return vtag(v) == TAG_STR; }
JS_INLINE bool is_obj(jsval v) { return vtag(v) == TAG_OBJ; }
JS_INLINE bool is_sym(jsval v) { return vtag(v) == TAG_SYM; }
JS_INLINE bool is_big(jsval v) { return vtag(v) == TAG_BIG; }
JS_INLINE bool is_ptr(jsval v) { return vtag(v) == TAG_PTR; }
JS_INLINE bool is_undef(jsval v) { return v == JS_UNDEF; }
JS_INLINE bool is_null(jsval v) { return v == JS_NULL; }
JS_INLINE bool is_nullish(jsval v) { return v == JS_UNDEF || v == JS_NULL; }
JS_INLINE bool is_boolv(jsval v) { return v == JS_TRUE || v == JS_FALSE; }
JS_INLINE bool is_gcval(jsval v) { return vtag(v) >= TAG_STR; }
JS_INLINE void *vptr(jsval v) { return (void *)(uintptr_t)(v & 0xFFFFFFFFFFFFull); }
JS_INLINE double vnum(jsval v) { union { uint64_t u; double d; } x; x.u = v; return x.d; }
JS_INLINE jsval mknum(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }
JS_INLINE jsval mkbool(bool b) { return b ? JS_TRUE : JS_FALSE; }
JS_INLINE uint64_t dbits(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }
JS_INLINE double bitsd(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }
JS_INLINE jsval mknum_canon(double d) { return d != d ? JS_NAN_BITS : mknum(d); }

/* ---------------- GC ---------------- */
enum { GC_STR = 1, GC_SYM, GC_OBJ, GC_FUNCPROTO, GC_ENV, GC_ACCESSOR, GC_CELL, GC_BIG };
typedef struct GCHdr {
    struct GCHdr *next;
    uint8_t type, mark;
    uint16_t flags;
    uint32_t aux;
} GCHdr;

/* strings: 8-bit (Latin-1) or 16-bit (UTF-16) code units; ropes are flattened on demand */
enum { STR_WIDE = 1, STR_ROPE = 2, STR_ATOM = 4, STR_IDXCHK = 8, STR_ISIDX = 16, STR_OWNBUF = 32, STR_PERM = 64, STR_ASCII = 128, STR_ASCIICHK = 256 };
typedef struct JSString {
    GCHdr h; /* h.aux: predefined atom id (atoms only) */
    uint32_t len;
    uint32_t hash; /* 0: not yet computed */
    union { uint8_t *s8; uint16_t *s16; struct JSString *left; };
    union { struct JSString *right; uint32_t idx; };
} JSString;

typedef struct JSSymbol {
    GCHdr h; /* flags: 1 = private name, 2 = registered (Symbol.for) */
    uint32_t hash;
    jsval desc; /* string or undefined */
    jsval cls;  /* private methods/accessors: name of the declaring class (brand check message) */
} JSSymbol;

typedef struct JSBig { /* BigInt: sign + magnitude */
    GCHdr h;           /* flags: 1 = negative */
    uint32_t n;        /* number of 32-bit limbs (0 for zero) */
    uint32_t d[];
} JSBig;

/* property flags */
enum { P_W = 1, P_E = 2, P_C = 4, P_ACC = 8, P_DEFAULT = 7 };
typedef struct Prop {
    jsval key; /* atom string or symbol; JS_HOLE if deleted */
    jsval val; /* value, or TAG_PTR Accessor */
    uint32_t flags;
    uint32_t _pad;
} Prop;

typedef struct Accessor {
    GCHdr h;
    jsval get, set; /* function or undefined */
} Accessor;

typedef struct Env {
    GCHdr h;
    struct Env *parent;
    uint32_t n;
    jsval slots[];
} Env;

/* object classes */
enum {
    CLS_OBJECT, CLS_ARRAY, CLS_FUNCTION, CLS_NATIVE, CLS_BOUND, CLS_ERROR, CLS_BOOLEAN, CLS_NUMBER, CLS_STRING,
    CLS_SYMBOL, CLS_BIGINT, CLS_DATE, CLS_REGEXP, CLS_ARGUMENTS, CLS_MAP, CLS_SET, CLS_WEAKMAP, CLS_WEAKSET,
    CLS_PROMISE, CLS_GENERATOR, CLS_ASYNC, CLS_ASYNC_GENERATOR, CLS_PROXY, CLS_HOST, CLS_ARRAY_ITER, CLS_MAP_ITER,
    CLS_SET_ITER, CLS_STRING_ITER, CLS_REGEXP_ITER, CLS_FORIN_ITER, CLS_ARRAYBUFFER, CLS_TYPEDARRAY, CLS_DATAVIEW,
    CLS_WEAKREF, CLS_FINREG, CLS_INTERNAL, CLS_COUNT
};
/* object flags */
enum {
    OF_EXTENSIBLE = 1, OF_EXOTIC = 2 /* own property access needs the slow path */, OF_LAZY = 4 /* function: name/length/prototype not yet created */,
    OF_HASIDX = 8 /* has array-index atom keys in props */, OF_FROZEN_EL = 16 /* dense elements frozen */, OF_SEALED_EL = 32,
    OF_SLOW_ARRAY = 64 /* array elements live in props */, OF_LEN_RO = 128 /* array length not writable */,
    OF_CALLABLE = 256, OF_CTOR = 512, OF_CLASS_CTOR = 1024, OF_HTMLDDA = 2048, OF_IS_ERROR_STACK = 4096
};

typedef struct JSObject {
    GCHdr h;
    uint8_t cls;
    uint8_t _r;
    uint16_t oflags;
    uint32_t nprops; /* used slots in props, including deleted ones */
    uint32_t cap;
    uint32_t ndel;
    struct JSObject *proto;
    Prop *props;
    uint32_t *hidx; /* open addressing hash: index+1 into props, 0 empty */
    uint32_t hmask;
    uint32_t _r2;
    union {
        struct { jsval *elems; uint32_t len, cap; struct Env *menv; uint16_t *pmap; } arr; /* Array, Arguments (menv/pmap: mapped arguments) */
        struct { struct FuncProto *fp; Env *env; struct JSObject *home; struct JSObject *fields; } fn;
        struct { js_native fn; void *data; jsval name; int32_t length; uint8_t api; } nat;
        struct { jsval target, bthis; jsval *args; uint32_t nargs; } bound;
        struct { jsval v; jsval v2; } prim; /* wrappers, Date, WeakRef */
        struct { const struct js_class *cls; void *ptr; } host;
        struct { void *p; jsval v; } data; /* Map/Set/Promise/Generator/RegExp/... private data */
        struct { jsval a, b; uint32_t i, n; uint8_t kind; } it; /* iterators; proxy: a=target b=handler */
        struct { int32_t line; JSString *file; } err;
    } u;
} JSObject;

JS_INLINE JSObject *vobj(jsval v) { return (JSObject *)vptr(v); }
JS_INLINE JSString *vstr(jsval v) { return (JSString *)vptr(v); }
JS_INLINE JSSymbol *vsym(jsval v) { return (JSSymbol *)vptr(v); }
JS_INLINE jsval mkobj(JSObject *o) { return MKTAG(TAG_OBJ, o); }
JS_INLINE jsval mkstr(JSString *s) { return MKTAG(TAG_STR, s); }
JS_INLINE jsval mksym(JSSymbol *s) { return MKTAG(TAG_SYM, s); }
JS_INLINE jsval mkptr(void *p) { return MKTAG(TAG_PTR, p); }
JS_INLINE bool is_cls(jsval v, int c) { return is_obj(v) && vobj(v)->cls == c; }

/* ---------------- compiled functions ---------------- */
enum {
    FP_STRICT = 1, FP_ARROW = 2, FP_METHOD = 4, FP_CLASS_CTOR = 8, FP_DERIVED = 16, FP_GENERATOR = 32, FP_ASYNC = 64,
    FP_NEEDS_ARGS = 128, FP_HAS_REST = 256, FP_USES_THIS = 512, FP_SCRIPT = 1024, FP_GETSET = 2048, FP_CTOR_KIND = 4096,
    FP_FIELD_INIT = 8192, FP_EXPR_NAME = 16384
};
typedef struct FuncProto {
    GCHdr h;
    uint8_t *code;
    uint32_t code_len;
    uint32_t nconsts;
    jsval *consts;
    uint32_t *lines; /* pairs (pc, line) */
    uint32_t nlines;
    uint32_t flags;
    uint16_t nparams, length, nlocals, max_stack;
    int32_t args_slot, rest_slot;
    jsval name;      /* atom or undefined */
    JSString *file;  /* script file name */
    JSString *src;   /* the whole script source as raw UTF-8 bytes */
    uint32_t src_start, src_end;
} FuncProto;

/* internal cell kinds (GC_CELL, h.aux) */
enum { CELL_RETURN = 1, CELL_TEMPLATE, CELL_BOX };
typedef struct Cell {
    GCHdr h;
    jsval a, b;
} Cell;

/* ---------------- runtime ---------------- */
typedef struct Frame {
    uint8_t *pc;
    jsval *argv; /* locals base: argv[-1] = this, argv[-2] = callee */
    jsval *sp;
    FuncProto *fp;
    JSObject *func;
    Env *env;
    JSObject *coro; /* generator / async state object */
    jsval new_target;
    uint32_t argc;
    uint32_t hbase;
    uint32_t flags;
} Frame;
enum { F_STOP = 1, F_CONSTRUCT = 2, F_CORO = 4 };

typedef struct Handler {
    uint32_t frame;
    uint32_t kind; /* 0 catch, 1 finally (also sees generator-return completions) */
    uint32_t pc_off;
    uint32_t sp_off; /* relative to frame argv */
    Env *env;
} Handler;

/* predefined atoms */
#define ATOM_LIST(X) \
    X(empty, "") X(length, "length") X(prototype, "prototype") X(constructor, "constructor") X(name, "name") \
    X(message, "message") X(stack, "stack") X(cause, "cause") X(errors, "errors") X(toString, "toString") \
    X(valueOf, "valueOf") X(toJSON, "toJSON") X(get, "get") X(set, "set") X(value, "value") X(writable, "writable") \
    X(enumerable, "enumerable") X(configurable, "configurable") X(next, "next") X(done, "done") X(return, "return") \
    X(throw, "throw") X(then, "then") X(catch, "catch") X(finally, "finally") X(callee, "callee") X(caller, "caller") \
    X(arguments, "arguments") X(__proto__, "__proto__") X(index, "index") X(input, "input") X(groups, "groups") \
    X(indices, "indices") X(lastIndex, "lastIndex") X(source, "source") X(flags, "flags") X(global, "global") \
    X(ignoreCase, "ignoreCase") X(multiline, "multiline") X(dotAll, "dotAll") X(unicode, "unicode") X(sticky, "sticky") \
    X(hasIndices, "hasIndices") X(raw, "raw") X(default, "default") X(undefined, "undefined") X(null, "null") \
    X(true, "true") X(false, "false") X(NaN, "NaN") X(Infinity, "Infinity") X(object, "object") X(function, "function") \
    X(string, "string") X(number, "number") X(boolean, "boolean") X(symbol, "symbol") X(bigint, "bigint") \
    X(anonymous, "anonymous") X(this, "this") X(new_target, "new.target") X(pfunc, "%func") X(let, "let") \
    X(static, "static") X(async, "async") X(await, "await") X(yield, "yield") X(of, "of") X(target, "target") \
    X(add, "add") X(has, "has") X(delete, "delete") X(size, "size") X(keys, "keys") X(values, "values") X(entries, "entries") \
    X(join, "join") X(Symbol_iterator, "[Symbol.iterator]") X(break, "break") X(case, "case") X(class, "class") \
    X(const, "const") X(continue, "continue") X(debugger, "debugger") X(do, "do") X(else, "else") X(export, "export") \
    X(extends, "extends") X(for, "for") X(if, "if") X(import, "import") X(in, "in") X(instanceof, "instanceof") X(new, "new") \
    X(super, "super") X(switch, "switch") X(try, "try") X(typeof, "typeof") X(var, "var") X(void, "void") X(while, "while") \
    X(with, "with") X(enum, "enum") X(implements, "implements") X(package, "package") X(protected, "protected") \
    X(interface, "interface") X(private, "private") X(public, "public") X(meta, "meta") X(from, "from") X(as, "as") \
    X(Error, "Error") X(Object, "Object") X(Array, "Array") X(Function, "Function") X(eval, "eval") X(use_strict, "use strict") \
    X(fieldinit, "<field init>") X(globalThis, "globalThis") X(callerSym, "%caller") X(lineNumber, "lineNumber") \
    X(fileName, "fileName") X(construct, "construct") X(apply, "apply") X(call, "call") X(bind, "bind") \
    X(getPrototypeOf, "getPrototypeOf") X(setPrototypeOf, "setPrototypeOf") X(isExtensible, "isExtensible") \
    X(preventExtensions, "preventExtensions") X(getOwnPropertyDescriptor, "getOwnPropertyDescriptor") \
    X(defineProperty, "defineProperty") X(deleteProperty, "deleteProperty") X(ownKeys, "ownKeys") X(status, "status") \
    X(reason, "reason") X(fulfilled, "fulfilled") X(rejected, "rejected") X(resolve, "resolve") X(reject, "reject") \
    X(promise, "promise") X(proxy, "proxy") X(revoke, "revoke") X(exec, "exec") X(byteLength, "byteLength") \
    X(buffer, "buffer") X(byteOffset, "byteOffset") X(unknown, "unknown")

enum {
#define X(id, s) A_##id,
    ATOM_LIST(X)
#undef X
    A__COUNT
};
#define ATOM(id) (rt->atoms[A_##id])

/* well-known symbols */
#define SYM_LIST(X) X(iterator) X(asyncIterator) X(hasInstance) X(toPrimitive) X(toStringTag) X(species) \
    X(isConcatSpreadable) X(unscopables) X(match) X(matchAll) X(replace) X(search) X(split)
enum {
#define X(id) S_##id,
    SYM_LIST(X)
#undef X
    S__COUNT
};
#define SYM(id) (rt->syms[S_##id])

/* error types */
enum { E_ERROR, E_TYPE, E_RANGE, E_SYNTAX, E_REFERENCE, E_URI, E_EVAL, E_AGGREGATE, E_INTERNAL, E__COUNT };

/* intrinsic objects */
enum {
    I_ObjectProto, I_FunctionProto, I_ArrayProto, I_ErrorProto, I_BooleanProto = I_ErrorProto + E__COUNT, I_NumberProto,
    I_StringProto, I_SymbolProto, I_BigIntProto, I_DateProto, I_RegExpProto, I_MapProto, I_SetProto, I_WeakMapProto,
    I_WeakSetProto, I_PromiseProto, I_IteratorProto, I_ArrayIterProto, I_MapIterProto, I_SetIterProto, I_StringIterProto,
    I_RegExpIterProto, I_GeneratorProto, I_GeneratorFunctionProto, I_AsyncFunctionProto, I_AsyncGeneratorProto,
    I_AsyncGeneratorFunctionProto, I_AsyncIteratorProto, I_AsyncFromSyncIterProto, I_ArrayBufferProto, I_TypedArrayProto,
    I_DataViewProto, I_WeakRefProto, I_FinRegProto,
    I_Promise, I_Array, I_Object, I_Function, I_ArrayValues, I_ArrayIterNext, I_ThrowTypeError, I_Error, I_RegExp,
    I_TypedArrayCtor, I_Symbol, I_Eval, I_PromiseThen, I_GeneratorFunction, I_AsyncFunction, I_AsyncGeneratorFunction,
    I_RegExpExec, I_ConsoleCounts,
    I_TA_FIRST, /* typed array prototypes and constructors: 2 per kind */
    I__COUNT = I_TA_FIRST + 2 * 12
};

typedef struct Job { jsval fn, a, b, c; int kind; } Job;

struct js_rt {
    struct js_host host;
    /* GC */
    GCHdr *gc_list;
    size_t bytes, threshold;
    bool gc_pending, in_gc;
    void **mstack;
    size_t msp, mcap;
    /* atoms */
    JSString **atab;
    uint32_t amask, acount, adel;
    jsval atoms[A__COUNT];
    jsval syms[S__COUNT];
    JSObject *intr[I__COUNT];
    JSObject *global;
    JSObject *global_lex; /* script-scope let/const/class */
    uint32_t lex_epoch;
    JSObject *symbol_registry;
    JSString *char_str[256];
    JSString *small_int_str[256];
    /* VM */
    jsval *stack, *stack_end, *sp;
    Frame *frames;
    int nframes, max_frames;
    Handler *handlers;
    uint32_t nhandlers, hcap;
    jsval new_target; /* for native constructors */
    int coro_status;  /* 0 done, 1 yield, 2 await */
    /* roots */
    jsval **roots;
    int nroots, rcap;
    /* exceptions */
    jsval exc;
    bool has_exc;
    bool terminating;
    int throw_line;
    JSString *throw_file;
    char *err_text;
    /* jobs */
    Job *jobs;
    uint32_t jhead, jtail, jcap;
    jsval *unhandled; /* rejected promises without handlers */
    uint32_t nunhandled, cunhandled;
    /* misc */
    int interrupt_count;
    char *stack_base;
    int api_depth;
    int native_depth;
    int rec_depth;   /* C-level recursion (natives calling JS etc.) */
    uint64_t rand_state[2];
    bool rand_seeded;
    void *regex_cache;
    uint32_t sym_counter;
    JSObject *cur_this_fn; /* scratch */
    int group_indent;      /* console.group */
    int compile_errtype;
};

#define JS_STACK_SLOTS (192 * 1024)
#define JS_MAX_FRAMES 9000
#define JS_MAX_REC 1000

/* ---------------- gc.c ---------------- */
void *gc_alloc(js_rt *rt, size_t size, int type);
void gc_collect(js_rt *rt);
void gc_mark_val(js_rt *rt, jsval v);
void gc_mark_ptr(js_rt *rt, void *p);
JS_INLINE void gc_maybe(js_rt *rt) { if (UNLIKELY(rt->gc_pending)) gc_collect(rt); }
jsval *push_tmp(js_rt *rt, jsval v);
bool check_stack(js_rt *rt);

/* ---------------- str.c ---------------- */
JSString *str_alloc8(js_rt *rt, uint32_t len);
JSString *str_alloc16(js_rt *rt, uint32_t len);
JSString *str_new8(js_rt *rt, const uint8_t *s, uint32_t len);
JSString *str_new16(js_rt *rt, const uint16_t *s, uint32_t len);
JSString *str_cstr(js_rt *rt, const char *s); /* ASCII/Latin-1 bytes */
JSString *str_utf8(js_rt *rt, const char *s, size_t len);
JSString *str_concat(js_rt *rt, JSString *a, JSString *b);
void str_flatten(js_rt *rt, JSString *s);
JS_INLINE JSString *str_flat(js_rt *rt, JSString *s) { if (UNLIKELY(s->h.flags & STR_ROPE)) str_flatten(rt, s); return s; }
JS_INLINE uint16_t str_at(JSString *s, uint32_t i) { return (s->h.flags & STR_WIDE) ? s->s16[i] : s->s8[i]; }
JSString *str_sub(js_rt *rt, JSString *s, uint32_t start, uint32_t end);
int str_cmp(js_rt *rt, JSString *a, JSString *b);
bool str_eq(js_rt *rt, JSString *a, JSString *b);
uint32_t str_hash(js_rt *rt, JSString *s);
jsval atomize(js_rt *rt, JSString *s);
jsval atom_c(js_rt *rt, const char *s);
jsval atom_n(js_rt *rt, const uint8_t *s, uint32_t len);
void atoms_sweep(js_rt *rt);
int64_t str_index(js_rt *rt, JSString *s); /* array index value or -1 */
JS_INLINE int64_t key_index(js_rt *rt, jsval key) {
    if (!is_str(key)) return -1;
    JSString *s = vstr(key);
    if (s->h.flags & STR_IDXCHK) return (s->h.flags & STR_ISIDX) ? (int64_t)s->idx : -1;
    return str_index(rt, s);
}
char *str_to_utf8(js_rt *rt, JSString *s, size_t *outlen); /* malloc'd */
bool str_is_ascii(js_rt *rt, JSString *s);
JSString *str_char(js_rt *rt, uint16_t c);
int32_t str_indexof(js_rt *rt, JSString *s, JSString *pat, int32_t from);
int32_t str_lastindexof(js_rt *rt, JSString *s, JSString *pat, int32_t from);

/* string builder */
typedef struct SB {
    js_rt *rt;
    uint8_t *b8;
    uint16_t *b16;
    uint32_t len, cap;
    bool wide, oom;
} SB;
void sb_init(SB *b, js_rt *rt);
void sb_putc(SB *b, uint32_t c); /* UTF-16 code unit */
void sb_putcp(SB *b, uint32_t cp); /* code point (surrogate pair if needed) */
void sb_puts(SB *b, const char *s); /* Latin-1 bytes */
void sb_putn(SB *b, const char *s, uint32_t n);
void sb_putstr(SB *b, JSString *s);
void sb_putsub(SB *b, JSString *s, uint32_t start, uint32_t end);
void sb_pututf8(SB *b, const char *s, size_t n);
JSString *sb_finish(SB *b); /* NULL (RangeError thrown) if too long */
void sb_free(SB *b);
#define JS_MAX_STRLEN ((1u << 29) - 24)

/* unicode helpers */
uint32_t uc_upper(uint32_t c);
uint32_t uc_lower(uint32_t c);
bool uc_is_space(uint32_t c);
bool uc_is_id_start(uint32_t c);
bool uc_is_id_part(uint32_t c);
int utf8_decode(const uint8_t *s, size_t n, uint32_t *cp); /* returns bytes used (>=1) */
int utf8_encode(uint32_t cp, char *out);

/* ---------------- num.c ---------------- */
double js_floor(double x);
double js_ceil(double x);
double js_trunc(double x);
double js_round(double x);
double js_fmod(double x, double y);
double js_pow(double x, double y);
double js_exp(double x);
double js_expm1(double x);
double js_log(double x);
double js_log1p(double x);
double js_log2(double x);
double js_log10(double x);
double js_sin(double x);
double js_cos(double x);
double js_tan(double x);
double js_asin(double x);
double js_acos(double x);
double js_atan(double x);
double js_atan2(double y, double x);
double js_sinh(double x);
double js_cosh(double x);
double js_tanh(double x);
double js_asinh(double x);
double js_acosh(double x);
double js_atanh(double x);
double js_cbrt(double x);
double js_hypot(double x, double y);
double js_ldexp(double x, int e);
JS_INLINE bool js_isnan(double d) { return d != d; }
JS_INLINE bool js_isfinite(double d) { return (dbits(d) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
JS_INLINE bool js_signbit(double d) { return (dbits(d) >> 63) != 0; }
#define JS_INF bitsd(0x7FF0000000000000ull)
#define JS_NAN bitsd(JS_NAN_BITS)
int num_to_cstr(double d, char *buf);        /* shortest round-trip; buf >= 32 bytes; returns length */
int num_to_radix(double d, int radix, char *buf); /* buf >= 2200 */
int num_to_fixed(double d, int f, char *buf);     /* buf >= 1200 */
int num_to_exp(double d, int f, char *buf);       /* f < 0: as many digits as necessary */
int num_to_prec(double d, int p, char *buf);
double js_strtod(const char *s, size_t n, size_t *used); /* decimal literal prefix (digits[.digits][e[+-]digits]) */
double str_to_number(js_rt *rt, JSString *s); /* StringToNumber */
double parse_int_digits(const char *s, size_t n, int radix, size_t *used);
JSString *num_to_str(js_rt *rt, double d);
int32_t to_int32(double d);
JS_INLINE uint32_t to_uint32(double d) { return (uint32_t)to_int32(d); }
double to_integer(double d); /* ToIntegerOrInfinity */

/* ---------------- obj.c ---------------- */
typedef struct PropDesc {
    jsval value, get, set;
    uint8_t flags; /* P_W | P_E | P_C */
    uint8_t has;   /* HAS_* */
} PropDesc;
enum { HAS_VALUE = 1, HAS_GET = 2, HAS_SET = 4, HAS_W = 8, HAS_E = 16, HAS_C = 32 };

JSObject *new_object(js_rt *rt, JSObject *proto, int cls);
JSObject *new_plain(js_rt *rt);
JSObject *new_array(js_rt *rt, uint32_t len);
JSObject *new_array_from(js_rt *rt, const jsval *v, uint32_t n);
int array_push(js_rt *rt, JSObject *a, jsval v); /* dense fast append */
bool array_reserve(js_rt *rt, JSObject *a, uint32_t n);
Prop *find_own(js_rt *rt, JSObject *o, jsval key);
Prop *add_prop(js_rt *rt, JSObject *o, jsval key, jsval val, uint32_t flags);
void del_prop_at(js_rt *rt, JSObject *o, Prop *p);
void materialize_fn(js_rt *rt, JSObject *o);
jsval def_prop(js_rt *rt, JSObject *o, jsval key, jsval val, uint32_t flags); /* create/overwrite own data prop, no checks */
void def_acc(js_rt *rt, JSObject *o, jsval key, jsval get, jsval set, uint32_t flags);
JSObject *def_method(js_rt *rt, JSObject *o, const char *name, js_native fn, int length, intptr_t magic);
JSObject *def_method_k(js_rt *rt, JSObject *o, jsval key, const char *fname, js_native fn, int length, intptr_t magic);
void def_getter(js_rt *rt, JSObject *o, const char *name, js_native fn, intptr_t magic);
void def_getter_k(js_rt *rt, JSObject *o, jsval key, const char *fname, js_native fn, intptr_t magic);
void def_value(js_rt *rt, JSObject *o, const char *name, jsval v); /* non-enumerable writable configurable */
JSObject *new_native(js_rt *rt, const char *name, js_native fn, int length, intptr_t magic, bool ctor);
JSObject *new_native_k(js_rt *rt, jsval name, js_native fn, int length, void *data, bool ctor);
JSObject *new_ctor(js_rt *rt, const char *name, js_native fn, int length, JSObject *proto);

jsval get_prop(js_rt *rt, jsval obj, jsval key, jsval receiver);
JS_INLINE jsval get_p(js_rt *rt, jsval obj, jsval key) { return get_prop(rt, obj, key, obj); }
jsval get_v(js_rt *rt, jsval obj, jsval key); /* any key value */
jsval get_idx(js_rt *rt, jsval obj, uint32_t i);
jsval get_c(js_rt *rt, jsval obj, const char *key);
int set_prop(js_rt *rt, jsval obj, jsval key, jsval val, jsval receiver, bool throw_);
JS_INLINE int set_p(js_rt *rt, jsval obj, jsval key, jsval val, bool th) { return set_prop(rt, obj, key, val, obj, th); }
int set_v(js_rt *rt, jsval obj, jsval key, jsval val, bool th);
int set_idx(js_rt *rt, jsval obj, uint32_t i, jsval val, bool th);
int has_prop(js_rt *rt, jsval obj, jsval key);
int has_own(js_rt *rt, JSObject *o, jsval key);
int delete_prop(js_rt *rt, jsval obj, jsval key, bool throw_);
int get_own_desc(js_rt *rt, JSObject *o, jsval key, PropDesc *d); /* -1 throw, 0 absent, 1 found */
int define_own(js_rt *rt, JSObject *o, jsval key, PropDesc *d, bool throw_);
int create_data_prop(js_rt *rt, JSObject *o, jsval key, jsval v, bool throw_);
int create_data_idx(js_rt *rt, JSObject *o, uint32_t i, jsval v);
enum { KEYS_STR = 1, KEYS_SYM = 2, KEYS_ENUM_ONLY = 4, KEYS_PRIVATE = 8 };
JSObject *own_keys(js_rt *rt, JSObject *o, int flags); /* array of keys, NULL on throw */
jsval get_proto(js_rt *rt, JSObject *o);  /* JS_EXC on throw */
int set_proto(js_rt *rt, JSObject *o, JSObject *proto, bool throw_);
int prevent_ext(js_rt *rt, JSObject *o);
int is_extensible(js_rt *rt, JSObject *o);
int set_integrity(js_rt *rt, JSObject *o, bool frozen);
int test_integrity(js_rt *rt, JSObject *o, bool frozen);
jsval to_key(js_rt *rt, jsval v); /* atom or symbol; JS_EXC */
jsval key_to_display(js_rt *rt, jsval key);
uint32_t key_hash(jsval key);
bool array_set_length(js_rt *rt, JSObject *a, uint32_t len);
void array_make_slow(js_rt *rt, JSObject *a);

jsval to_primitive(js_rt *rt, jsval v, int hint); /* hint 0 default 1 number 2 string */
int to_number(js_rt *rt, jsval v, double *out);
int to_numeric(js_rt *rt, jsval v, jsval *out); /* number or bigint */
JSString *to_string(js_rt *rt, jsval v);
JSString *to_string_prim(js_rt *rt, jsval v); /* v must be primitive, no throw except symbol */
jsval to_object(js_rt *rt, jsval v);
bool truthy(jsval v);
int to_int(js_rt *rt, jsval v, double *out); /* ToIntegerOrInfinity */
int to_int_clamp(js_rt *rt, jsval v, int64_t lo, int64_t hi, int64_t dflt, int64_t *out);
int to_length(js_rt *rt, jsval v, uint64_t *out);
int to_index(js_rt *rt, jsval v, uint64_t *out);
int len_of(js_rt *rt, jsval o, uint64_t *out);
bool same_value(js_rt *rt, jsval a, jsval b);
bool same_value_zero(js_rt *rt, jsval a, jsval b);
bool strict_eq(js_rt *rt, jsval a, jsval b);
int loose_eq(js_rt *rt, jsval a, jsval b);
const char *type_name(jsval v);
jsval typeof_val(js_rt *rt, jsval v);
bool is_callable(jsval v);
bool is_ctor(jsval v);
bool is_array(js_rt *rt, jsval v); /* -1? proxies: follows target */
JSObject *proto_from_ctor(js_rt *rt, jsval ctor, int dflt_intr); /* NULL on throw */
JSObject *species_ctor(js_rt *rt, JSObject *o, int dflt_intr, jsval *out);

/* errors */
jsval throw_error(js_rt *rt, int type, const char *fmt, ...);
jsval throw_val(js_rt *rt, jsval v);
JSObject *make_error(js_rt *rt, int type, JSString *msg);
jsval throw_not_obj(js_rt *rt, jsval v, const char *what);
void fmt_into(js_rt *rt, SB *b, const char *fmt, va_list ap);
JSString *jsfmt(js_rt *rt, const char *fmt, ...);
JSString *val_brief(js_rt *rt, jsval v); /* short description for messages (no side effects) */

/* ---------------- vm.c ---------------- */
jsval call_fn(js_rt *rt, jsval f, jsval this_, int argc, const jsval *argv);
jsval construct_fn(js_rt *rt, jsval f, int argc, const jsval *argv, jsval new_target);
jsval call_method(js_rt *rt, jsval obj, jsval key, int argc, const jsval *argv);
JSObject *make_closure(js_rt *rt, FuncProto *fp, Env *env);
jsval run_script(js_rt *rt, FuncProto *fp);
int line_of(FuncProto *fp, uint32_t pc_off);
JSString *capture_stack(js_rt *rt, int *line, JSString **file);
void cur_location(js_rt *rt, int *line, JSString **file);
jsval coro_resume(js_rt *rt, JSObject *co, int mode, jsval v); /* mode 0 next 1 throw 2 return */
jsval make_return_cell(js_rt *rt, jsval v);
bool is_return_cell(jsval v);
void enqueue_job(js_rt *rt, int kind, jsval fn, jsval a, jsval b, jsval c);
int run_jobs(js_rt *rt);
void report_exception(js_rt *rt, const char *prefix);
jsval func_name_of(js_rt *rt, JSObject *f);
JSObject *bound_create(js_rt *rt, jsval target, jsval bthis, int nargs, const jsval *args);
jsval eval_code(js_rt *rt, JSString *src, const char *file, bool is_fn_ctor);

typedef struct CoroData {
    int state; /* 0 suspended-start, 1 suspended-yield, 2 running, 3 completed, 4 awaiting-return (async gen) */
    jsval *slots;
    uint32_t nslots;
    Handler *handlers;
    uint32_t nhandlers;
    Frame fr; /* argv/sp relative offsets: argv=2, sp in fr.argc_sp */
    uint32_t sp_off;
    uint32_t pc_off;
    jsval promise; /* async: the result promise; async gen: unused */
    jsval resolve, reject;
    /* async generators: request queue */
    jsval *queue; /* triples: mode, value, promise */
    uint32_t qlen, qcap;
    int kind; /* 0 generator, 1 async function, 2 async generator */
} CoroData;

/* ---------------- parse.c / compile.c ---------------- */
FuncProto *compile_script(js_rt *rt, const char *src, size_t len, const char *file, int flags, JSString **err);
enum { CS_FUNCTION_CTOR = 1, CS_STRICT = 2, CS_EVAL = 4 };
/* regexp */
typedef struct Regex Regex;
Regex *re_compile(js_rt *rt, JSString *src, uint32_t flags, JSString **err);
void re_free(Regex *re);
int re_ncaps(Regex *re);
JSString *re_groupname(Regex *re, int i);
/* returns 1 match, 0 no match, -1 error (stack exhausted/interrupt) */
int re_exec(js_rt *rt, Regex *re, JSString *s, uint32_t start, int32_t *caps);
enum { RF_G = 1, RF_I = 2, RF_M = 4, RF_S = 8, RF_U = 16, RF_Y = 32, RF_D = 64, RF_V = 128 };
int re_parse_flags(JSString *f, uint32_t *out);

/* ---------------- builtins ---------------- */
void init_builtins(js_rt *rt);
void init_object(js_rt *rt);
void init_function(js_rt *rt);
void init_array(js_rt *rt);
void init_string(js_rt *rt);
void init_number(js_rt *rt);
void init_math(js_rt *rt);
void init_json(js_rt *rt);
void init_regexp(js_rt *rt);
void init_date(js_rt *rt);
void init_promise(js_rt *rt);
void init_collections(js_rt *rt);
void init_symbol(js_rt *rt);
void init_error(js_rt *rt);
void init_global(js_rt *rt);
void init_iter(js_rt *rt);
void init_typed(js_rt *rt);
void init_reflect(js_rt *rt);
void init_bigint(js_rt *rt);
void init_console(js_rt *rt);

#define NATIVE(fname) static jsval fname(js_rt *rt, jsval this_, int argc, jsval *argv, void *data)
#define ARG(i) ((i) < argc ? argv[i] : JS_UNDEF)
#define MAGIC ((int)(intptr_t)data)
#define TRY(x) do { if ((x) == JS_EXC) return JS_EXC; } while (0)
#define TRYI(x) do { if ((x) < 0) return JS_EXC; } while (0)

/* iterators */
jsval get_iterator(js_rt *rt, jsval obj, jsval *next_out, bool async); /* returns iterator object */
/* returns 1 value, 0 done, -1 throw */
int iter_step(js_rt *rt, jsval iter, jsval next, jsval *value);
void iter_close(js_rt *rt, jsval iter, bool preserve_exc);
jsval iter_result(js_rt *rt, jsval v, bool done);
JSObject *iter_to_array(js_rt *rt, jsval iterable); /* NULL on throw */
JSObject *make_array_iter(js_rt *rt, jsval target, int kind);
jsval array_iter_next_fast(js_rt *rt, JSObject *it, bool *done); /* for for-of */
bool array_iter_pristine(js_rt *rt, JSObject *it);
JSObject *make_generator(js_rt *rt, Frame *f, jsval *sp, uint8_t *pc, int kind);

/* promises */
JSObject *promise_new(js_rt *rt);
jsval promise_resolve_val(js_rt *rt, jsval ctor, jsval v); /* PromiseResolve */
void promise_resolve(js_rt *rt, JSObject *p, jsval v);       /* resolve function semantics */
void promise_reject(js_rt *rt, JSObject *p, jsval reason);
void promise_fulfill(js_rt *rt, JSObject *p, jsval v);
void promise_then_internal(js_rt *rt, JSObject *p, jsval onf, jsval onr, JSObject *result);
int promise_state(JSObject *p);
void async_await(js_rt *rt, JSObject *co, jsval v);
void async_gen_complete_step(js_rt *rt, JSObject *co, int what, jsval v);
jsval async_gen_enqueue(js_rt *rt, jsval gen, int mode, jsval v);
void async_gen_drain(js_rt *rt, JSObject *co);
void job_trampoline(js_rt *rt, Job *j);

/* regexp object helpers */
jsval regexp_create(js_rt *rt, jsval pattern, jsval flags, JSObject *proto);
jsval regexp_exec(js_rt *rt, jsval re, JSString *s); /* RegExpExec */
bool regexp_is_pristine(js_rt *rt, jsval re);

/* misc builtin helpers */
JSString *inspect_value(js_rt *rt, jsval v, int top); /* console-style formatting */
JSString *error_to_string(js_rt *rt, jsval v);
jsval json_stringify(js_rt *rt, jsval v, jsval replacer, jsval space);
JSString *func_to_string(js_rt *rt, JSObject *f);
jsval array_join(js_rt *rt, jsval obj, JSString *sep);
double date_now(js_rt *rt);
double random_double(js_rt *rt);
jsval weak_get(js_rt *rt, JSObject *map, jsval key, bool *found);
JSObject *make_typed_array(js_rt *rt, int kind, uint32_t len);
bool ta_get(js_rt *rt, JSObject *o, uint64_t i, jsval *out);
int ta_set(js_rt *rt, JSObject *o, uint64_t i, jsval v);
uint32_t ta_length(JSObject *o);
jsval proxy_get(js_rt *rt, JSObject *p, jsval key, jsval receiver);
int proxy_set(js_rt *rt, JSObject *p, jsval key, jsval v, jsval receiver);
int proxy_has(js_rt *rt, JSObject *p, jsval key);
int proxy_delete(js_rt *rt, JSObject *p, jsval key);
int proxy_get_own_desc(js_rt *rt, JSObject *p, jsval key, PropDesc *d);
int proxy_define(js_rt *rt, JSObject *p, jsval key, PropDesc *d);
JSObject *proxy_own_keys(js_rt *rt, JSObject *p);
jsval proxy_get_proto(js_rt *rt, JSObject *p);
int proxy_set_proto(js_rt *rt, JSObject *p, JSObject *proto);
int proxy_prevent_ext(js_rt *rt, JSObject *p);
int proxy_is_ext(js_rt *rt, JSObject *p);
jsval proxy_call(js_rt *rt, JSObject *p, jsval this_, int argc, jsval *argv);
jsval proxy_construct(js_rt *rt, JSObject *p, int argc, jsval *argv, jsval nt);
jsval desc_to_obj(js_rt *rt, PropDesc *d);
int obj_to_desc(js_rt *rt, jsval v, PropDesc *d);
void gc_trace_class(js_rt *rt, JSObject *o);
void gc_finalize_class(js_rt *rt, JSObject *o);
JSBig *big_from_double(js_rt *rt, double d);
double big_to_double(JSBig *b);
JSString *big_to_string(js_rt *rt, JSBig *b, int radix);
jsval big_from_string(js_rt *rt, JSString *s, bool literal); /* JS_UNDEF if syntax error (SyntaxError for literal) */
int big_cmp(JSBig *a, JSBig *b);
int big_cmp_num(JSBig *a, double d); /* -1,0,1 ; 2 if NaN */
jsval big_binop(js_rt *rt, int op, jsval a, jsval b);
jsval big_neg(js_rt *rt, jsval a);
bool big_is_zero(JSBig *b);
jsval to_bigint(js_rt *rt, jsval v);
jsval big_from_u64(js_rt *rt, uint64_t v, bool neg);
uint64_t big_to_u64(JSBig *b);

int strchr_safe(const char *s, uint32_t c);
JSString *get_substitution(js_rt *rt, JSString *matched, JSString *str, uint32_t pos, jsval *caps, int ncaps, jsval groups,
                           JSString *rep);
JSString *format_locale_number(js_rt *rt, double d, jsval opts);
void regex_cache_free(js_rt *rt);
void coro_trace(js_rt *rt, JSObject *o);
void coro_finalize(JSObject *o);
void promise_trace(js_rt *rt, JSObject *o);
void promise_finalize(JSObject *o);
void regexp_trace(js_rt *rt, JSObject *o);
void regexp_finalize(JSObject *o);
void map_trace(js_rt *rt, JSObject *o);
void map_finalize(JSObject *o);
void buffer_trace(js_rt *rt, JSObject *o);
void buffer_finalize(JSObject *o);

/* lazily-built data of collections */
JSObject *map_new(js_rt *rt, bool is_set);
int map_set_raw(js_rt *rt, JSObject *m, jsval k, jsval v);
