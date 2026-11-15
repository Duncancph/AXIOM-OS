/* AXIOM JavaScript engine: public API (js.h). */
#include "js_int.h"

void report_unhandled(js_rt *rt);

static const char *const atom_strs[A__COUNT] = {
#define X(id, s) s,
    ATOM_LIST(X)
#undef X
};
static const char *const sym_names[S__COUNT] = {
#define X(id) "Symbol." #id,
    SYM_LIST(X)
#undef X
};

typedef struct ApiGuard {
    bool outer;
} ApiGuard;
static void api_enter(js_rt *rt, ApiGuard *g) {
    g->outer = rt->api_depth == 0;
    if (g->outer) rt->stack_base = (char *)__builtin_frame_address(0);
    rt->api_depth++;
}
static void api_exit(js_rt *rt, ApiGuard *g) {
    rt->api_depth--;
    if (g->outer) rt->terminating = false;
}

js_rt *js_new(const struct js_host *host) {
    js_rt *rt = (js_rt *)calloc(1, sizeof(js_rt));
    if (!rt) return 0;
    if (host) rt->host = *host;
    rt->threshold = 8u * 1024 * 1024;
    rt->stack = (jsval *)calloc(JS_STACK_SLOTS + 8192, sizeof(jsval));
    rt->frames = (Frame *)calloc(JS_MAX_FRAMES + 16, sizeof(Frame));
    rt->jobs = (Job *)calloc(64, sizeof(Job));
    if (!rt->stack || !rt->frames || !rt->jobs) {
        free(rt->stack);
        free(rt->frames);
        free(rt->jobs);
        free(rt);
        return 0;
    }
    rt->jcap = 64;
    rt->stack_end = rt->stack + JS_STACK_SLOTS;
    rt->sp = rt->stack;
    rt->max_frames = JS_MAX_FRAMES;
    rt->exc = JS_UNDEF;
    rt->new_target = JS_UNDEF;
    rt->interrupt_count = 10000;
    ApiGuard g;
    api_enter(rt, &g);
    for (int i = 0; i < A__COUNT; i++) {
        jsval a = atom_c(rt, atom_strs[i]);
        vstr(a)->h.flags |= STR_PERM;
        rt->atoms[i] = a;
    }
    for (int i = 0; i < S__COUNT; i++) {
        JSSymbol *s = (JSSymbol *)gc_alloc(rt, sizeof(JSSymbol), GC_SYM);
        s->cls = JS_UNDEF;
        s->desc = mkstr(str_cstr(rt, sym_names[i]));
        s->hash = (uint32_t)(++rt->sym_counter * 2654435761u) | 1;
        rt->syms[i] = mksym(s);
    }
    init_builtins(rt);
    api_exit(rt, &g);
    return rt;
}

void js_free(js_rt *rt) {
    if (!rt) return;
    GCHdr *h = rt->gc_list;
    /* finalize host objects first, then free everything */
    for (GCHdr *x = h; x; x = x->next)
        if (x->type == GC_OBJ) {
            JSObject *o = (JSObject *)x;
            if (o->cls == CLS_HOST) {
                if (o->u.host.cls && o->u.host.cls->finalize) o->u.host.cls->finalize(o->u.host.ptr);
                o->u.host.cls = 0;
            }
        }
    while (h) {
        GCHdr *n = h->next;
        switch (h->type) {
        case GC_STR:
            if (h->flags & STR_OWNBUF) free(((JSString *)h)->s8);
            break;
        case GC_OBJ: {
            JSObject *o = (JSObject *)h;
            free(o->props);
            free(o->hidx);
            if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) {
                free(o->u.arr.elems);
                free(o->u.arr.pmap);
            }
            else if (o->cls == CLS_BOUND)
                free(o->u.bound.args);
            else if (o->cls != CLS_HOST)
                gc_finalize_class(rt, o);
            break;
        }
        case GC_FUNCPROTO: {
            FuncProto *fp = (FuncProto *)h;
            free(fp->code);
            free(fp->consts);
            free(fp->lines);
            break;
        }
        default: break;
        }
        free(h);
        h = n;
    }
    regex_cache_free(rt);
    free(rt->atab);
    free(rt->mstack);
    free(rt->stack);
    free(rt->frames);
    free(rt->handlers);
    free(rt->roots);
    free(rt->jobs);
    free(rt->unhandled);
    free(rt->err_text);
    free(rt);
}

/* ---------------- errors ---------------- */
static void set_err_text(js_rt *rt, JSString *s) {
    free(rt->err_text);
    rt->err_text = s ? str_to_utf8(rt, s, 0) : 0;
}

/* after a failed run: rt->exc holds the value; build "Type: message (file:line)" */
static void finish_error(js_rt *rt) {
    jsval e = rt->exc;
    rt->has_exc = false;
    jsval *root = push_tmp(rt, e);
    JSString *text = 0;
    int line = rt->throw_line;
    JSString *file = rt->throw_file;
    if (is_obj(e) && vobj(e)->cls == CLS_ERROR) {
        text = error_to_string(rt, e);
        if (vobj(e)->u.err.file) {
            line = vobj(e)->u.err.line;
            file = vobj(e)->u.err.file;
        }
    } else if (!is_obj(e)) {
        text = is_sym(e) ? vstr(key_to_display(rt, e)) : to_string_prim(rt, e);
        text = text ? jsfmt(rt, "Uncaught %S", text) : 0;
    } else
        text = jsfmt(rt, "Uncaught %S", inspect_value(rt, e, 0));
    if (rt->has_exc) {
        rt->has_exc = false;
        text = str_cstr(rt, "Uncaught exception");
    }
    if (!text) text = str_cstr(rt, "Uncaught exception");
    if (file) text = jsfmt(rt, "%S (%S:%d)", text, file, line);
    set_err_text(rt, text);
    rt->exc = e;
    rt->has_exc = false;
    rt->sp = root;
}

/* an exception happened in an API call that has no error result */
static void api_exc(js_rt *rt) {
    if (!rt->has_exc) return;
    if (rt->native_depth > 0 && !rt->terminating) return; /* stays pending: rethrown when the native returns */
    if (rt->terminating) {
        rt->has_exc = false;
        return;
    }
    jsval e = rt->exc;
    finish_error(rt);
    rt->exc = e;
    rt->has_exc = true;
    report_exception(rt, "Uncaught ");
}

/* ---------------- running code ---------------- */
int js_eval(js_rt *rt, const char *src, long len, const char *filename, jsval *result) {
    ApiGuard g;
    api_enter(rt, &g);
    if (result) *result = JS_UNDEF;
    if (rt->has_exc) {
        api_exit(rt, &g);
        return -1;
    }
    if (len < 0) len = (long)strlen(src);
    gc_maybe(rt);
    JSString *err = 0;
    FuncProto *fp = compile_script(rt, src, (size_t)len, filename, 0, &err);
    int rc = 0;
    if (!fp) {
        JSObject *e = make_error(rt, rt->compile_errtype == E_RANGE ? E_RANGE : E_SYNTAX, err);
        e->u.err.file = 0;
        rt->exc = mkobj(e);
        set_err_text(rt, jsfmt(rt, "SyntaxError: %S (%s:%d)", err, filename ? filename : "<anonymous>", rt->throw_line));
        rc = -1;
    } else {
        jsval r = run_script(rt, fp);
        if (r == JS_EXC) {
            finish_error(rt);
            rc = -1;
        } else if (result)
            *result = r;
    }
    api_exit(rt, &g);
    return rc;
}

int js_call(js_rt *rt, jsval fn, jsval this_, int argc, const jsval *argv, jsval *result) {
    ApiGuard g;
    api_enter(rt, &g);
    if (result) *result = JS_UNDEF;
    if (rt->has_exc) {
        api_exit(rt, &g);
        return -1;
    }
    jsval r = call_fn(rt, fn, this_, argc, argv);
    int rc = 0;
    if (r == JS_EXC) {
        finish_error(rt);
        rc = -1;
    } else if (result)
        *result = r;
    api_exit(rt, &g);
    return rc;
}

int js_construct(js_rt *rt, jsval ctor, int argc, const jsval *argv, jsval *result) {
    ApiGuard g;
    api_enter(rt, &g);
    if (result) *result = JS_UNDEF;
    if (rt->has_exc) {
        api_exit(rt, &g);
        return -1;
    }
    jsval r = construct_fn(rt, ctor, argc, argv, ctor);
    int rc = 0;
    if (r == JS_EXC) {
        finish_error(rt);
        rc = -1;
    } else if (result)
        *result = r;
    api_exit(rt, &g);
    return rc;
}

int js_run_jobs(js_rt *rt) {
    ApiGuard g;
    api_enter(rt, &g);
    int n = run_jobs(rt);
    if (!rt->terminating) report_unhandled(rt);
    rt->has_exc = false;
    api_exit(rt, &g);
    return n;
}

const char *js_error_text(js_rt *rt) { return rt->err_text ? rt->err_text : ""; }
jsval js_exception(js_rt *rt) { return rt->exc; }
bool js_has_exception(js_rt *rt) { return rt->has_exc; }
bool js_is_constructing(js_rt *rt) { return !is_undef(rt->new_target); }

/* ---------------- values ---------------- */
jsval js_undefined(void) { return JS_UNDEF; }
jsval js_null(void) { return JS_NULL; }
jsval js_bool(bool b) { return mkbool(b); }
jsval js_num(double d) { return mknum_canon(d); }
jsval js_int(int64_t i) { return mknum((double)i); }
jsval js_str(js_rt *rt, const char *utf8, long len) {
    if (len < 0) len = (long)strlen(utf8);
    JSString *s = str_utf8(rt, utf8, (size_t)len);
    return s ? mkstr(s) : mkstr(vstr(rt->atoms[A_empty]));
}
bool js_is_undefined(jsval v) { return is_undef(v); }
bool js_is_null(jsval v) { return is_null(v); }
bool js_is_bool(jsval v) { return is_boolv(v); }
bool js_is_number(jsval v) { return is_num(v); }
bool js_is_string(jsval v) { return is_str(v); }
bool js_is_object(jsval v) { return is_obj(v); }
bool js_is_function(jsval v) { return is_callable(v); }
bool js_is_array(jsval v) {
    for (int i = 0; i < 1000 && is_obj(v); i++) {
        JSObject *o = vobj(v);
        if (o->cls == CLS_ARRAY) return true;
        if (o->cls != CLS_PROXY) return false;
        v = o->u.it.a;
    }
    return false;
}
bool js_truthy(jsval v) { return truthy(v); }

double js_to_number(js_rt *rt, jsval v) {
    if (is_num(v)) return vnum(v);
    ApiGuard g;
    api_enter(rt, &g);
    double d;
    jsval *root = push_tmp(rt, v);
    if (to_number(rt, v, &d) < 0) {
        d = JS_NAN;
        api_exc(rt);
    }
    rt->sp = root;
    api_exit(rt, &g);
    return d;
}
int64_t js_to_int(js_rt *rt, jsval v) {
    double d = js_to_number(rt, v);
    if (d != d) return 0;
    d = js_trunc(d);
    if (d >= 9.2e18) return INT64_MAX;
    if (d <= -9.2e18) return INT64_MIN;
    return (int64_t)d;
}
static char *val_utf8(js_rt *rt, jsval v, size_t *len) {
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, v);
    JSString *s = to_string(rt, v);
    char *r = 0;
    if (!s)
        api_exc(rt);
    else
        r = str_to_utf8(rt, s, len);
    rt->sp = root;
    api_exit(rt, &g);
    return r;
}
long js_to_string(js_rt *rt, jsval v, char *out, long max) {
    size_t n = 0;
    char *u = val_utf8(rt, v, &n);
    if (!u) {
        if (max > 0) out[0] = 0;
        return -1;
    }
    if (max > 0) {
        size_t c = (long)n < max ? n : (size_t)max;
        memcpy(out, u, c);
        if ((long)n < max) out[n] = 0;
    }
    free(u);
    return (long)n;
}
char *js_to_cstring(js_rt *rt, jsval v) {
    char *u = val_utf8(rt, v, 0);
    if (!u) {
        u = (char *)malloc(1);
        if (u) u[0] = 0;
    }
    return u;
}
bool js_strict_equals(jsval a, jsval b) {
    if (is_num(a) && is_num(b)) return vnum(a) == vnum(b);
    if (is_str(a) && is_str(b)) {
        JSString *x = vstr(a), *y = vstr(b);
        if (x->len != y->len) return false;
        if (x == y) return true;
        /* compare without flattening (no runtime available) */
        if ((x->h.flags | y->h.flags) & STR_ROPE) {
            str_flatten(0, x);
            str_flatten(0, y);
        }
        for (uint32_t i = 0; i < x->len; i++)
            if (str_at(x, i) != str_at(y, i)) return false;
        return true;
    }
    if (is_big(a) && is_big(b)) return big_cmp((JSBig *)vptr(a), (JSBig *)vptr(b)) == 0;
    return a == b;
}

/* ---------------- objects ---------------- */
static jsval key_atom(js_rt *rt, const char *key) {
    size_t n = strlen(key);
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)key[i] >= 0x80) return atomize(rt, str_utf8(rt, key, n));
    return atom_n(rt, (const uint8_t *)key, (uint32_t)n);
}
jsval js_global(js_rt *rt) { return mkobj(rt->global); }
jsval js_object(js_rt *rt) { return mkobj(new_plain(rt)); }
jsval js_array(js_rt *rt) { return mkobj(new_array(rt, 0)); }

jsval js_get(js_rt *rt, jsval obj, const char *key) {
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, obj);
    jsval r = is_nullish(obj) ? JS_UNDEF : get_p(rt, obj, key_atom(rt, key));
    if (r == JS_EXC) {
        api_exc(rt);
        r = JS_UNDEF;
    }
    rt->sp = root;
    api_exit(rt, &g);
    return r;
}
void js_set(js_rt *rt, jsval obj, const char *key, jsval v) {
    if (!is_obj(obj)) return;
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, obj);
    push_tmp(rt, v);
    if (set_p(rt, obj, key_atom(rt, key), v, false) < 0) api_exc(rt);
    rt->sp = root;
    api_exit(rt, &g);
}
jsval js_get_index(js_rt *rt, jsval obj, uint32_t i) {
    if (is_nullish(obj)) return JS_UNDEF;
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, obj);
    jsval r = get_idx(rt, obj, i);
    if (r == JS_EXC) {
        api_exc(rt);
        r = JS_UNDEF;
    }
    rt->sp = root;
    api_exit(rt, &g);
    return r;
}
void js_set_index(js_rt *rt, jsval obj, uint32_t i, jsval v) {
    if (!is_obj(obj)) return;
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, obj);
    push_tmp(rt, v);
    if (set_idx(rt, obj, i, v, false) < 0) api_exc(rt);
    rt->sp = root;
    api_exit(rt, &g);
}
long js_length(js_rt *rt, jsval obj) {
    if (is_str(obj)) return (long)vstr(obj)->len;
    if (!is_obj(obj)) return 0;
    ApiGuard g;
    api_enter(rt, &g);
    jsval *root = push_tmp(rt, obj);
    uint64_t n = 0;
    if (len_of(rt, obj, &n) < 0) {
        api_exc(rt);
        n = 0;
    }
    rt->sp = root;
    api_exit(rt, &g);
    return (long)n;
}
void js_define(js_rt *rt, jsval obj, const char *key, jsval v, bool enumerable) {
    if (!is_obj(obj)) return;
    PropDesc d;
    d.value = v;
    d.flags = (uint8_t)(P_W | P_C | (enumerable ? P_E : 0));
    d.has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
    ApiGuard g;
    api_enter(rt, &g);
    if (define_own(rt, vobj(obj), key_atom(rt, key), &d, false) < 0) api_exc(rt);
    api_exit(rt, &g);
}
void js_define_accessor(js_rt *rt, jsval obj, const char *key, js_native getter, js_native setter, void *data) {
    if (!is_obj(obj)) return;
    jsval k = key_atom(rt, key);
    jsval gf = JS_UNDEF, sf = JS_UNDEF;
    if (getter) {
        JSObject *f = new_native_k(rt, mkstr(jsfmt(rt, "get %S", vstr(k))), getter, 0, data, false);
        f->u.nat.api = 2;
        gf = mkobj(f);
    }
    if (setter) {
        JSObject *f = new_native_k(rt, mkstr(jsfmt(rt, "set %S", vstr(k))), setter, 1, data, false);
        f->u.nat.api = 2;
        sf = mkobj(f);
    }
    def_acc(rt, vobj(obj), k, gf, sf, P_C);
}
jsval js_function(js_rt *rt, const char *name, js_native fn, int length, void *data) {
    JSObject *f = new_native_k(rt, key_atom(rt, name ? name : ""), fn, length, data, true);
    f->u.nat.api = 1;
    return mkobj(f);
}

static int err_type(const char *t) {
    static const char *const names[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "URIError", "EvalError", "AggregateError"};
    if (!t) return E_ERROR;
    for (int i = 0; i < 8; i++)
        if (!strcmp(names[i], t)) return i;
    return E_ERROR;
}
jsval js_throw(js_rt *rt, const char *error_type, const char *message) {
    JSObject *e = make_error(rt, err_type(error_type), message ? str_utf8(rt, message, strlen(message)) : 0);
    return throw_val(rt, mkobj(e));
}
jsval js_throw_value(js_rt *rt, jsval v) { return throw_val(rt, v); }
jsval js_new_error(js_rt *rt, const char *error_type, const char *message) {
    return mkobj(make_error(rt, err_type(error_type), message ? str_utf8(rt, message, strlen(message)) : 0));
}
jsval js_get_prototype(js_rt *rt, jsval obj) {
    if (!is_obj(obj)) return JS_NULL;
    ApiGuard g;
    api_enter(rt, &g);
    jsval r = get_proto(rt, vobj(obj));
    if (r == JS_EXC) {
        api_exc(rt);
        r = JS_NULL;
    }
    api_exit(rt, &g);
    return r;
}
void js_set_prototype(js_rt *rt, jsval obj, jsval proto) {
    if (!is_obj(obj) || (!is_obj(proto) && !is_null(proto))) return;
    ApiGuard g;
    api_enter(rt, &g);
    if (set_proto(rt, vobj(obj), is_obj(proto) ? vobj(proto) : 0, false) < 0) api_exc(rt);
    api_exit(rt, &g);
}

/* ---------------- host objects ---------------- */
jsval js_host_object(js_rt *rt, const struct js_class *cls, void *ptr, jsval proto) {
    JSObject *p = is_obj(proto) ? vobj(proto) : is_null(proto) ? 0 : rt->intr[I_ObjectProto];
    JSObject *o = new_object(rt, p, CLS_HOST);
    o->u.host.cls = cls;
    o->u.host.ptr = ptr;
    return mkobj(o);
}
void *js_host_ptr(jsval obj, const struct js_class *cls) {
    if (!is_obj(obj)) return 0;
    JSObject *o = vobj(obj);
    if (o->cls != CLS_HOST || o->u.host.cls != cls) return 0;
    return o->u.host.ptr;
}
void js_mark(js_rt *rt, jsval v) { gc_mark_val(rt, v); }

/* ---------------- roots & GC ---------------- */
void js_root(js_rt *rt, jsval *slot) {
    if (rt->nroots == rt->rcap) {
        int nc = rt->rcap ? rt->rcap * 2 : 64;
        jsval **n = (jsval **)realloc(rt->roots, (size_t)nc * sizeof(jsval *));
        if (!n) return;
        rt->roots = n;
        rt->rcap = nc;
    }
    rt->roots[rt->nroots++] = slot;
}
void js_unroot(js_rt *rt, jsval *slot) {
    for (int i = rt->nroots - 1; i >= 0; i--)
        if (rt->roots[i] == slot) {
            rt->roots[i] = rt->roots[--rt->nroots];
            return;
        }
}
void js_gc(js_rt *rt) { gc_collect(rt); }
long js_heap_bytes(js_rt *rt) { return (long)rt->bytes; }

void js_check_soon(js_rt *rt) { if (rt) rt->interrupt_count = 0; }
