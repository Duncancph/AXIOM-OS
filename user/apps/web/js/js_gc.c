/* AXIOM JavaScript engine: precise mark & sweep garbage collector.
 * Collection only happens at safe points (interpreter back-edges/calls and js_gc), never inside
 * an allocation, so engine C code may hold fresh values in locals until it next runs JS code. */
#include "js_int.h"

void *gc_alloc(js_rt *rt, size_t size, int type) {
    GCHdr *h = (GCHdr *)calloc(1, size);
    if (!h) return 0;
    h->type = (uint8_t)type;
    h->next = rt->gc_list;
    rt->gc_list = h;
    rt->bytes += size + 16;
    if (rt->bytes > rt->threshold) rt->gc_pending = true;
    return h;
}

static void mpush(js_rt *rt, void *p) {
    GCHdr *h = (GCHdr *)p;
    if (!h || h->mark) return;
    h->mark = 1;
    if (rt->msp == rt->mcap) {
        size_t nc = rt->mcap ? rt->mcap * 2 : 4096;
        void **n = (void **)realloc(rt->mstack, nc * sizeof(void *));
        if (!n) return; /* out of memory while marking: object stays marked, children may leak-mark */
        rt->mstack = n;
        rt->mcap = nc;
    }
    rt->mstack[rt->msp++] = p;
}

void gc_mark_ptr(js_rt *rt, void *p) { mpush(rt, p); }
void gc_mark_val(js_rt *rt, jsval v) {
    if (is_gcval(v)) mpush(rt, vptr(v));
}
#define MV(v) gc_mark_val(rt, (v))
#define MP(p) mpush(rt, (p))

static void trace_obj(js_rt *rt, JSObject *o) {
    MP(o->proto);
    for (uint32_t i = 0; i < o->nprops; i++) {
        MV(o->props[i].key);
        MV(o->props[i].val);
    }
    switch (o->cls) {
    case CLS_ARRAY:
    case CLS_ARGUMENTS:
        for (uint32_t i = 0; i < o->u.arr.len; i++) MV(o->u.arr.elems[i]);
        MP(o->u.arr.menv);
        break;
    case CLS_FUNCTION:
        MP(o->u.fn.fp);
        MP(o->u.fn.env);
        MP(o->u.fn.home);
        MP(o->u.fn.fields);
        break;
    case CLS_NATIVE:
        MV(o->u.nat.name);
        if (o->u.nat.api == 2) MV((jsval)(uintptr_t)0); /* nothing */
        break;
    case CLS_BOUND:
        MV(o->u.bound.target);
        MV(o->u.bound.bthis);
        for (uint32_t i = 0; i < o->u.bound.nargs; i++) MV(o->u.bound.args[i]);
        break;
    case CLS_BOOLEAN: case CLS_NUMBER: case CLS_STRING: case CLS_SYMBOL: case CLS_BIGINT: case CLS_DATE:
    case CLS_WEAKREF:
        MV(o->u.prim.v);
        MV(o->u.prim.v2);
        break;
    case CLS_ERROR:
        MP(o->u.err.file);
        break;
    case CLS_HOST:
        if (o->u.host.cls && o->u.host.cls->trace) o->u.host.cls->trace(rt, o->u.host.ptr);
        break;
    case CLS_ARRAY_ITER: case CLS_MAP_ITER: case CLS_SET_ITER: case CLS_STRING_ITER: case CLS_REGEXP_ITER:
    case CLS_PROXY: case CLS_INTERNAL:
        MV(o->u.it.a);
        MV(o->u.it.b);
        break;
    default:
        gc_trace_class(rt, o);
        break;
    }
}

static void trace(js_rt *rt, GCHdr *h) {
    switch (h->type) {
    case GC_STR: {
        JSString *s = (JSString *)h;
        if (s->h.flags & STR_ROPE) {
            MP(s->left);
            MP(s->right);
        }
        break;
    }
    case GC_SYM: MV(((JSSymbol *)h)->desc); MV(((JSSymbol *)h)->cls); break;
    case GC_OBJ: trace_obj(rt, (JSObject *)h); break;
    case GC_FUNCPROTO: {
        FuncProto *fp = (FuncProto *)h;
        for (uint32_t i = 0; i < fp->nconsts; i++) MV(fp->consts[i]);
        MV(fp->name);
        MP(fp->file);
        MP(fp->src);
        break;
    }
    case GC_ENV: {
        Env *e = (Env *)h;
        MP(e->parent);
        for (uint32_t i = 0; i < e->n; i++) MV(e->slots[i]);
        break;
    }
    case GC_ACCESSOR: MV(((Accessor *)h)->get); MV(((Accessor *)h)->set); break;
    case GC_CELL: MV(((Cell *)h)->a); MV(((Cell *)h)->b); break;
    default: break;
    }
}

static void finalize(js_rt *rt, GCHdr *h) {
    switch (h->type) {
    case GC_STR: {
        JSString *s = (JSString *)h;
        if (s->h.flags & STR_OWNBUF) free(s->s8);
        break;
    }
    case GC_OBJ: {
        JSObject *o = (JSObject *)h;
        free(o->props);
        free(o->hidx);
        switch (o->cls) {
        case CLS_ARRAY: case CLS_ARGUMENTS: free(o->u.arr.elems); free(o->u.arr.pmap); break;
        case CLS_BOUND: free(o->u.bound.args); break;
        case CLS_HOST:
            if (o->u.host.cls && o->u.host.cls->finalize) o->u.host.cls->finalize(o->u.host.ptr);
            break;
        case CLS_OBJECT: case CLS_FUNCTION: case CLS_NATIVE: case CLS_ERROR: case CLS_BOOLEAN: case CLS_NUMBER:
        case CLS_STRING: case CLS_SYMBOL: case CLS_BIGINT: case CLS_DATE: case CLS_WEAKREF: case CLS_ARRAY_ITER:
        case CLS_MAP_ITER: case CLS_SET_ITER: case CLS_STRING_ITER: case CLS_REGEXP_ITER: case CLS_PROXY:
        case CLS_INTERNAL:
            break;
        default: gc_finalize_class(rt, o); break;
        }
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
}

static void mark_roots(js_rt *rt) {
    for (int i = 0; i < A__COUNT; i++) MV(rt->atoms[i]);
    for (int i = 0; i < S__COUNT; i++) MV(rt->syms[i]);
    for (int i = 0; i < I__COUNT; i++) MP(rt->intr[i]);
    MP(rt->global);
    MP(rt->global_lex);
    MP(rt->symbol_registry);
    for (int i = 0; i < 256; i++) {
        MP(rt->char_str[i]);
        MP(rt->small_int_str[i]);
    }
    for (jsval *p = rt->stack; p < rt->sp; p++) MV(*p);
    for (int i = 0; i < rt->nframes; i++) {
        Frame *f = &rt->frames[i];
        MP(f->fp);
        MP(f->func);
        MP(f->env);
        MP(f->coro);
        MV(f->new_target);
    }
    for (uint32_t i = 0; i < rt->nhandlers; i++) MP(rt->handlers[i].env);
    for (int i = 0; i < rt->nroots; i++) MV(*rt->roots[i]);
    MV(rt->exc);
    MV(rt->new_target);
    MP(rt->throw_file);
    for (uint32_t i = rt->jhead; i != rt->jtail; i = (i + 1) % rt->jcap) {
        Job *j = &rt->jobs[i];
        MV(j->fn);
        MV(j->a);
        MV(j->b);
        MV(j->c);
    }
    for (uint32_t i = 0; i < rt->nunhandled; i++) MV(rt->unhandled[i]);
}

void gc_collect(js_rt *rt) {
    if (rt->in_gc) return;
    rt->in_gc = true;
    rt->gc_pending = false;
    mark_roots(rt);
    while (rt->msp) {
        GCHdr *h = (GCHdr *)rt->mstack[--rt->msp];
        trace(rt, h);
    }
    atoms_sweep(rt);
    GCHdr **pp = &rt->gc_list;
    size_t live = 0;
    while (*pp) {
        GCHdr *h = *pp;
        if (h->mark || (h->type == GC_STR && (h->flags & STR_PERM))) {
            h->mark = 0;
            pp = &h->next;
            live++;
        } else {
            *pp = h->next;
            finalize(rt, h);
            free(h);
        }
    }
    /* recompute byte estimate from survivors: keep it simple, scale */
    (void)live;
    size_t est = 0;
    for (GCHdr *h = rt->gc_list; h; h = h->next) {
        switch (h->type) {
        case GC_STR: est += sizeof(JSString) + ((JSString *)h)->len * ((h->flags & STR_WIDE) ? 2 : 1); break;
        case GC_OBJ: {
            JSObject *o = (JSObject *)h;
            est += sizeof(JSObject) + o->cap * sizeof(Prop);
            if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) est += o->u.arr.cap * 8;
            break;
        }
        case GC_ENV: est += sizeof(Env) + ((Env *)h)->n * 8; break;
        case GC_FUNCPROTO: est += sizeof(FuncProto) + ((FuncProto *)h)->code_len; break;
        default: est += 48; break;
        }
        est += 16;
    }
    rt->bytes = est;
    rt->threshold = est * 2;
    if (rt->threshold < 8u * 1024 * 1024) rt->threshold = 8u * 1024 * 1024;
    if (rt->mcap > 65536) {
        free(rt->mstack);
        rt->mstack = 0;
        rt->mcap = 0;
    }
    rt->in_gc = false;
}

jsval *push_tmp(js_rt *rt, jsval v) {
    jsval *p = rt->sp;
    if (p >= rt->stack_end) p = rt->stack_end - 1; /* reserve area exhausted: should not happen */
    else rt->sp++;
    *p = v;
    return p;
}

bool check_stack(js_rt *rt) {
    char *here = (char *)__builtin_frame_address(0);
    if (!rt->stack_base) return false;
    long used = rt->stack_base - here;
    return used > (long)JS_C_STACK_LIMIT;
}
