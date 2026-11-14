/* AXIOM JavaScript engine: iteration protocol, array iterators, generator objects. */
#include "js_int.h"

jsval iter_result(js_rt *rt, jsval v, bool done) {
    JSObject *o = new_plain(rt);
    add_prop(rt, o, ATOM(value), v, P_DEFAULT);
    add_prop(rt, o, ATOM(done), mkbool(done), P_DEFAULT);
    return mkobj(o);
}

/* ---------------- array / typed-array iterators ---------------- */
JSObject *make_array_iter(js_rt *rt, jsval target, int kind) {
    JSObject *it = new_object(rt, rt->intr[I_ArrayIterProto], CLS_ARRAY_ITER);
    it->u.it.a = target;
    it->u.it.b = JS_UNDEF;
    it->u.it.i = 0;
    it->u.it.kind = (uint8_t)kind;
    return it;
}

jsval array_iter_next_fast(js_rt *rt, JSObject *it, bool *done) {
    *done = false;
    jsval t = it->u.it.a;
    if (is_undef(t)) {
        *done = true;
        return JS_UNDEF;
    }
    uint32_t i = it->u.it.i;
    uint64_t len;
    if (is_obj(t)) {
        JSObject *o = vobj(t);
        if (o->cls == CLS_ARRAY && !(o->oflags & OF_SLOW_ARRAY)) {
            len = o->u.arr.len;
            if (i >= len) {
                it->u.it.a = JS_UNDEF;
                *done = true;
                return JS_UNDEF;
            }
            it->u.it.i = i + 1;
            if (it->u.it.kind == 1) {
                jsval v = o->u.arr.elems[i];
                if (v != JS_HOLE) return v;
                return get_idx(rt, t, i);
            }
        } else if (o->cls == CLS_TYPEDARRAY) {
            len = ta_length(o);
            if (i >= len) {
                it->u.it.a = JS_UNDEF;
                *done = true;
                return JS_UNDEF;
            }
            it->u.it.i = i + 1;
        } else {
            if (len_of(rt, t, &len) < 0) return JS_EXC;
            if (i >= len) {
                it->u.it.a = JS_UNDEF;
                *done = true;
                return JS_UNDEF;
            }
            it->u.it.i = i + 1;
        }
    } else {
        it->u.it.a = JS_UNDEF;
        *done = true;
        return JS_UNDEF;
    }
    if (it->u.it.kind == 0) return mknum(i);
    jsval v = get_idx(rt, t, i);
    if (v == JS_EXC || it->u.it.kind == 1) return v;
    jsval pair[2] = {mknum(i), v};
    return mkobj(new_array_from(rt, pair, 2));
}

NATIVE(array_iter_next) {
    if (!is_cls(this_, CLS_ARRAY_ITER)) return throw_error(rt, E_TYPE, "next method called on incompatible receiver");
    bool done;
    jsval v = array_iter_next_fast(rt, vobj(this_), &done);
    TRY(v);
    return iter_result(rt, v, done);
}

bool array_iter_pristine(js_rt *rt, JSObject *it) { (void)rt; (void)it; return true; }

NATIVE(iter_self) { return this_; }

/* ---------------- generic protocol ---------------- */
jsval get_iterator(js_rt *rt, jsval obj, jsval *next_out, bool async) {
    jsval *root = push_tmp(rt, obj);
    jsval m;
    if (async) {
        m = get_v(rt, obj, SYM(asyncIterator));
        if (m == JS_EXC) {
            rt->sp = root;
            return JS_EXC;
        }
        if (is_nullish(m)) {
            jsval sn;
            jsval sit = get_iterator(rt, obj, &sn, false);
            if (sit == JS_EXC) {
                rt->sp = root;
                return JS_EXC;
            }
            JSObject *w = new_object(rt, rt->intr[I_AsyncFromSyncIterProto], CLS_INTERNAL);
            w->u.it.a = sit;
            w->u.it.b = sn;
            rt->sp = root;
            jsval nx = get_p(rt, mkobj(w), ATOM(next));
            *next_out = nx;
            return mkobj(w);
        }
    } else {
        if (is_nullish(obj)) {
            rt->sp = root;
            return throw_error(rt, E_TYPE, "%S is not iterable (cannot read property Symbol(Symbol.iterator))", val_brief(rt, obj));
        }
        m = get_v(rt, obj, SYM(iterator));
        if (m == JS_EXC) {
            rt->sp = root;
            return JS_EXC;
        }
    }
    if (!is_callable(m)) {
        rt->sp = root;
        if (is_obj(obj) || is_str(obj)) return throw_error(rt, E_TYPE, "%S is not iterable", val_brief(rt, obj));
        return throw_error(rt, E_TYPE, "%s %S is not iterable (cannot read property Symbol(Symbol.iterator))", type_name(obj),
                           val_brief(rt, obj));
    }
    jsval it = call_fn(rt, m, obj, 0, 0);
    if (it == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    if (!is_obj(it)) {
        rt->sp = root;
        return throw_error(rt, E_TYPE, "Result of the Symbol.iterator method is not an object");
    }
    push_tmp(rt, it);
    jsval n = get_p(rt, it, ATOM(next));
    rt->sp = root;
    if (n == JS_EXC) return JS_EXC;
    *next_out = n;
    return it;
}

int iter_step(js_rt *rt, jsval it, jsval next, jsval *value) {
    if (is_obj(it) && vobj(it)->cls == CLS_ARRAY_ITER && next == mkobj(rt->intr[I_ArrayIterNext])) {
        bool done;
        jsval v = array_iter_next_fast(rt, vobj(it), &done);
        if (v == JS_EXC) return -1;
        if (done) return 0;
        *value = v;
        return 1;
    }
    if (!is_callable(next)) {
        throw_error(rt, E_TYPE, "%S is not a function", val_brief(rt, next));
        return -1;
    }
    jsval r = call_fn(rt, next, it, 0, 0);
    if (r == JS_EXC) return -1;
    if (!is_obj(r)) {
        throw_error(rt, E_TYPE, "Iterator result %S is not an object", val_brief(rt, r));
        return -1;
    }
    jsval *root = push_tmp(rt, r);
    jsval d = get_p(rt, r, ATOM(done));
    if (d == JS_EXC) {
        rt->sp = root;
        return -1;
    }
    if (truthy(d)) {
        rt->sp = root;
        return 0;
    }
    jsval v = get_p(rt, r, ATOM(value));
    rt->sp = root;
    if (v == JS_EXC) return -1;
    *value = v;
    return 1;
}

void iter_close(js_rt *rt, jsval it, bool preserve_exc) {
    jsval saved = rt->exc;
    bool had = rt->has_exc;
    jsval *root = push_tmp(rt, it);
    push_tmp(rt, saved);
    rt->has_exc = false;
    jsval m = get_p(rt, it, ATOM(return));
    if (m != JS_EXC && !is_nullish(m)) {
        jsval r = call_fn(rt, m, it, 0, 0);
        if (r != JS_EXC && !is_obj(r) && !preserve_exc) throw_error(rt, E_TYPE, "Iterator result %S is not an object", val_brief(rt, r));
    }
    if (preserve_exc || had) {
        if (rt->terminating) {
            rt->sp = root;
            return;
        }
        rt->exc = saved;
        rt->has_exc = had;
    }
    rt->sp = root;
}

JSObject *iter_to_array(js_rt *rt, jsval iterable) {
    if (is_obj(iterable) && vobj(iterable)->cls == CLS_ARRAY && !(vobj(iterable)->oflags & OF_SLOW_ARRAY) &&
        !find_own(rt, vobj(iterable), SYM(iterator)) && vobj(iterable)->proto == rt->intr[I_ArrayProto]) {
        Prop *pp = find_own(rt, rt->intr[I_ArrayProto], SYM(iterator));
        Prop *np = find_own(rt, rt->intr[I_ArrayIterProto], ATOM(next));
        if (pp && pp->val == mkobj(rt->intr[I_ArrayValues]) && np && np->val == mkobj(rt->intr[I_ArrayIterNext])) {
            JSObject *s = vobj(iterable);
            JSObject *a = new_array(rt, 0);
            array_reserve(rt, a, s->u.arr.len);
            for (uint32_t i = 0; i < s->u.arr.len; i++) a->u.arr.elems[i] = s->u.arr.elems[i] == JS_HOLE ? JS_UNDEF : s->u.arr.elems[i];
            a->u.arr.len = s->u.arr.len;
            return a;
        }
    }
    jsval *root = push_tmp(rt, iterable);
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    jsval next;
    jsval it = get_iterator(rt, iterable, &next, false);
    if (it == JS_EXC) {
        rt->sp = root;
        return 0;
    }
    push_tmp(rt, it);
    push_tmp(rt, next);
    for (;;) {
        jsval v;
        int r = iter_step(rt, it, next, &v);
        if (r < 0) {
            rt->sp = root;
            return 0;
        }
        if (!r) break;
        array_push(rt, a, v);
    }
    rt->sp = root;
    return a;
}

/* ---------------- generators ---------------- */
static CoroData *gen_data(js_rt *rt, jsval t, int cls) {
    if (!is_cls(t, cls)) {
        throw_error(rt, E_TYPE, "%s method called on incompatible receiver %S", cls == CLS_GENERATOR ? "Generator" : "AsyncGenerator",
                    val_brief(rt, t));
        return 0;
    }
    return (CoroData *)vobj(t)->u.data.p;
}
NATIVE(gen_next) {
    CoroData *cd = gen_data(rt, this_, CLS_GENERATOR);
    if (!cd) return JS_EXC;
    int mode = MAGIC;
    if (cd->state == 3) {
        if (mode == 1) return throw_val(rt, ARG(0));
        return iter_result(rt, mode == 2 ? ARG(0) : JS_UNDEF, true);
    }
    jsval r = coro_resume(rt, vobj(this_), mode, ARG(0));
    if (r == JS_EXC) return JS_EXC;
    if (cd->state == 1) {
        if (cd->sp_off == 1) return r; /* yield* passes inner results through */
        return iter_result(rt, r, false);
    }
    return iter_result(rt, r, true);
}

NATIVE(async_gen_next) {
    return async_gen_enqueue(rt, this_, MAGIC, ARG(0));
}

/* ---------------- async-from-sync iterator ---------------- */
NATIVE(afs_unwrap) {
    /* data: done flag (0/1) */
    return iter_result(rt, ARG(0), MAGIC != 0);
}
NATIVE(afs_next) {
    int mode = MAGIC;
    JSObject *w = vobj(this_);
    JSObject *pr = promise_new(rt);
    jsval *root = push_tmp(rt, mkobj(pr));
    jsval r;
    if (mode == 0)
        r = call_fn(rt, w->u.it.b, w->u.it.a, argc > 0 ? 1 : 0, argv);
    else {
        jsval m = get_p(rt, w->u.it.a, mode == 1 ? ATOM(throw) : ATOM(return));
        if (m == JS_EXC) goto reject;
        if (is_nullish(m)) {
            if (mode == 2) {
                promise_resolve(rt, pr, iter_result(rt, ARG(0), true));
                rt->sp = root;
                return mkobj(pr);
            }
            throw_error(rt, E_TYPE, "The iterator does not provide a 'throw' method");
            goto reject;
        }
        r = call_fn(rt, m, w->u.it.a, argc > 0 ? 1 : 0, argv);
    }
    if (r == JS_EXC) goto reject;
    if (!is_obj(r)) {
        throw_error(rt, E_TYPE, "Iterator result %S is not an object", val_brief(rt, r));
        goto reject;
    }
    push_tmp(rt, r);
    {
        jsval d = get_p(rt, r, ATOM(done));
        if (d == JS_EXC) goto reject;
        jsval v = get_p(rt, r, ATOM(value));
        if (v == JS_EXC) goto reject;
        push_tmp(rt, v);
        jsval vp = promise_resolve_val(rt, mkobj(rt->intr[I_Promise]), v);
        if (vp == JS_EXC) goto reject;
        push_tmp(rt, vp);
        JSObject *onf = new_native_k(rt, ATOM(empty), afs_unwrap, 1, (void *)(intptr_t)(truthy(d) ? 1 : 0), false);
        push_tmp(rt, mkobj(onf));
        promise_then_internal(rt, vobj(vp), mkobj(onf), JS_UNDEF, pr);
    }
    rt->sp = root;
    return mkobj(pr);
reject:
    if (rt->terminating) return JS_EXC;
    {
        jsval e = rt->exc;
        rt->exc = JS_UNDEF;
        rt->has_exc = false;
        promise_reject(rt, pr, e);
    }
    rt->sp = root;
    return mkobj(pr);
}

void init_iter(js_rt *rt) {
    JSObject *ip = rt->intr[I_IteratorProto];
    def_method_k(rt, ip, SYM(iterator), "[Symbol.iterator]", iter_self, 0, 0);
    JSObject *aip = rt->intr[I_AsyncIteratorProto];
    def_method_k(rt, aip, SYM(asyncIterator), "[Symbol.asyncIterator]", iter_self, 0, 0);
    JSObject *arit = new_object(rt, ip, CLS_OBJECT);
    rt->intr[I_ArrayIterProto] = arit;
    rt->intr[I_ArrayIterNext] = def_method(rt, arit, "next", array_iter_next, 0, 0);
    def_prop(rt, arit, SYM(toStringTag), mkstr(str_cstr(rt, "Array Iterator")), P_C);
    JSObject *gp = rt->intr[I_GeneratorProto];
    def_method(rt, gp, "next", gen_next, 1, 0);
    def_method(rt, gp, "throw", gen_next, 1, 1);
    def_method(rt, gp, "return", gen_next, 1, 2);
    def_prop(rt, gp, SYM(toStringTag), mkstr(str_cstr(rt, "Generator")), P_C);
    JSObject *agp = rt->intr[I_AsyncGeneratorProto];
    def_method(rt, agp, "next", async_gen_next, 1, 0);
    def_method(rt, agp, "throw", async_gen_next, 1, 1);
    def_method(rt, agp, "return", async_gen_next, 1, 2);
    def_prop(rt, agp, SYM(toStringTag), mkstr(str_cstr(rt, "AsyncGenerator")), P_C);
    JSObject *afs = new_object(rt, aip, CLS_OBJECT);
    rt->intr[I_AsyncFromSyncIterProto] = afs;
    def_method(rt, afs, "next", afs_next, 1, 0);
    def_method(rt, afs, "throw", afs_next, 1, 1);
    def_method(rt, afs, "return", afs_next, 1, 2);
    /* the global Iterator constructor (minimal) */
    JSObject *ic = new_native(rt, "Iterator", iter_self, 0, 0, true);
    materialize_fn(rt, ic);
    def_prop(rt, ic, ATOM(prototype), mkobj(ip), 0);
    def_prop(rt, ip, ATOM(constructor), mkobj(ic), P_W | P_C);
    def_value(rt, rt->global, "Iterator", mkobj(ic));
}

void coro_trace(js_rt *rt, JSObject *o) {
    CoroData *cd = (CoroData *)o->u.data.p;
    if (!cd) return;
    for (uint32_t i = 0; i < cd->nslots; i++) gc_mark_val(rt, cd->slots[i]);
    for (uint32_t i = 0; i < cd->nhandlers; i++) gc_mark_ptr(rt, cd->handlers[i].env);
    gc_mark_ptr(rt, cd->fr.fp);
    gc_mark_ptr(rt, cd->fr.func);
    gc_mark_ptr(rt, cd->fr.env);
    gc_mark_val(rt, cd->fr.new_target);
    gc_mark_val(rt, cd->promise);
    gc_mark_val(rt, cd->resolve);
    gc_mark_val(rt, cd->reject);
    for (uint32_t i = 0; i < cd->qlen * 3; i++) gc_mark_val(rt, cd->queue[i]);
}
void coro_finalize(JSObject *o) {
    CoroData *cd = (CoroData *)o->u.data.p;
    if (!cd) return;
    free(cd->slots);
    free(cd->handlers);
    free(cd->queue);
    free(cd);
    o->u.data.p = 0;
}
