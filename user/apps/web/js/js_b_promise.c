/* AXIOM JavaScript engine: Promise, microtasks, async function / async generator drivers. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

enum { J_FULFILL = 1, J_REJECT, J_THENABLE, J_CALL };

typedef struct PData {
    int state; /* 0 pending 1 fulfilled 2 rejected */
    bool handled;
    jsval *rx; /* reactions: onF, onR, capP, capCell */
    uint32_t nr, cr;
} PData;

static PData *pd(JSObject *p) { return (PData *)p->u.data.p; }
int promise_state(JSObject *p) { return pd(p) ? pd(p)->state : 0; }

static JSObject *promise_alloc(js_rt *rt, JSObject *proto) {
    JSObject *p = new_object(rt, proto, CLS_PROMISE);
    p->u.data.p = calloc(1, sizeof(PData));
    p->u.data.v = JS_UNDEF;
    return p;
}
JSObject *promise_new(js_rt *rt) { return promise_alloc(rt, rt->intr[I_PromiseProto]); }

void promise_trace(js_rt *rt, JSObject *o) {
    gc_mark_val(rt, o->u.data.v);
    PData *d = pd(o);
    if (!d) return;
    for (uint32_t i = 0; i < d->nr * 4; i++) gc_mark_val(rt, d->rx[i]);
}
void promise_finalize(JSObject *o) {
    PData *d = pd(o);
    if (d) {
        free(d->rx);
        free(d);
    }
    o->u.data.p = 0;
}

static void track_unhandled(js_rt *rt, JSObject *p, bool add) {
    if (add) {
        if (rt->nunhandled == rt->cunhandled) {
            rt->cunhandled = rt->cunhandled ? rt->cunhandled * 2 : 16;
            rt->unhandled = (jsval *)realloc(rt->unhandled, rt->cunhandled * sizeof(jsval));
        }
        rt->unhandled[rt->nunhandled++] = mkobj(p);
        return;
    }
    for (uint32_t i = 0; i < rt->nunhandled; i++)
        if (rt->unhandled[i] == mkobj(p)) {
            rt->unhandled[i] = rt->unhandled[--rt->nunhandled];
            return;
        }
}

static void trigger(js_rt *rt, JSObject *p, int state, jsval v) {
    PData *d = pd(p);
    for (uint32_t i = 0; i < d->nr; i++) {
        jsval *r = &d->rx[i * 4];
        enqueue_job(rt, state == 1 ? J_FULFILL : J_REJECT, state == 1 ? r[0] : r[1], v, r[2], r[3]);
    }
    free(d->rx);
    d->rx = 0;
    d->nr = d->cr = 0;
}
void promise_fulfill(js_rt *rt, JSObject *p, jsval v) {
    PData *d = pd(p);
    if (d->state) return;
    d->state = 1;
    p->u.data.v = v;
    trigger(rt, p, 1, v);
}
void promise_reject(js_rt *rt, JSObject *p, jsval reason) {
    PData *d = pd(p);
    if (d->state) return;
    d->state = 2;
    p->u.data.v = reason;
    if (!d->handled) track_unhandled(rt, p, true);
    trigger(rt, p, 2, reason);
}

/* the [[Resolve]] logic (without the already-resolved guard) */
void promise_resolve(js_rt *rt, JSObject *p, jsval v) {
    if (pd(p)->state) return;
    if (v == mkobj(p)) {
        JSObject *e = make_error(rt, E_TYPE, str_cstr(rt, "Chaining cycle detected for promise #<Promise>"));
        promise_reject(rt, p, mkobj(e));
        return;
    }
    if (!is_obj(v)) {
        promise_fulfill(rt, p, v);
        return;
    }
    jsval *root = push_tmp(rt, mkobj(p));
    push_tmp(rt, v);
    jsval then = get_p(rt, v, ATOM(then));
    if (then == JS_EXC) {
        if (rt->terminating) {
            rt->sp = root;
            return;
        }
        jsval e = rt->exc;
        rt->exc = JS_UNDEF;
        rt->has_exc = false;
        promise_reject(rt, p, e);
        rt->sp = root;
        return;
    }
    if (!is_callable(then)) {
        promise_fulfill(rt, p, v);
        rt->sp = root;
        return;
    }
    enqueue_job(rt, J_THENABLE, then, v, mkobj(p), JS_UNDEF);
    rt->sp = root;
}

static Cell *new_cell(js_rt *rt, jsval a, jsval b) {
    Cell *c = (Cell *)gc_alloc(rt, sizeof(Cell), GC_CELL);
    c->h.aux = CELL_BOX;
    c->a = a;
    c->b = b;
    return c;
}

/* natives that need a GC-visible value keep it in a hidden property */
static JSObject *native_with(js_rt *rt, js_native fn, int len, jsval keep) {
    JSObject *f = new_native_k(rt, ATOM(empty), fn, len, 0, false);
    f->u.nat.data = f;
    add_prop(rt, f, rt->atoms[A_callerSym], keep, 0);
    return f;
}
static jsval kept(JSObject *self) { return self->props[self->nprops - 1].val; }
#define SELF ((JSObject *)data)
#define KEPT (kept(SELF))

NATIVE(resolving_fn) {
    /* KEPT = Cell(a = promise, b = Cell(flag)) ; length 1 = resolve, 2 = reject (via magic in name slot) */
    Cell *c = (Cell *)vptr(KEPT);
    Cell *flag = (Cell *)vptr(c->b);
    if (flag->a == JS_TRUE) return JS_UNDEF;
    flag->a = JS_TRUE;
    JSObject *p = vobj(c->a);
    if (SELF->u.nat.length == 1)
        promise_resolve(rt, p, ARG(0));
    else
        promise_reject(rt, p, ARG(0));
    return JS_UNDEF;
}
static void make_resolving(js_rt *rt, JSObject *p, jsval *res, jsval *rej) {
    Cell *flag = new_cell(rt, JS_FALSE, JS_UNDEF);
    Cell *c = new_cell(rt, mkobj(p), mkptr(flag));
    JSObject *r1 = native_with(rt, resolving_fn, 1, mkptr(c));
    *res = mkobj(r1);
    push_tmp(rt, *res);
    JSObject *r2 = native_with(rt, resolving_fn, 1, mkptr(c));
    r2->u.nat.length = 2;
    materialize_fn(rt, r2);
    def_prop(rt, r2, ATOM(length), mknum(1), P_C);
    *rej = mkobj(r2);
    push_tmp(rt, *rej);
}

void promise_then_internal(js_rt *rt, JSObject *p, jsval onf, jsval onr, JSObject *result) {
    PData *d = pd(p);
    jsval cp = result ? mkobj(result) : JS_UNDEF;
    jsval r[4] = {is_callable(onf) ? onf : JS_UNDEF, is_callable(onr) ? onr : JS_UNDEF, cp, JS_UNDEF};
    if (d->state == 0) {
        if (d->nr == d->cr) {
            d->cr = d->cr ? d->cr * 2 : 2;
            d->rx = (jsval *)realloc(d->rx, d->cr * 4 * sizeof(jsval));
        }
        memcpy(&d->rx[d->nr * 4], r, sizeof r);
        d->nr++;
    } else if (d->state == 1)
        enqueue_job(rt, J_FULFILL, r[0], p->u.data.v, r[2], r[3]);
    else {
        if (!d->handled) track_unhandled(rt, p, false);
        enqueue_job(rt, J_REJECT, r[1], p->u.data.v, r[2], r[3]);
    }
    d->handled = true;
}
static void then_with_cap(js_rt *rt, JSObject *p, jsval onf, jsval onr, jsval capP, jsval capCell) {
    PData *d = pd(p);
    jsval r[4] = {is_callable(onf) ? onf : JS_UNDEF, is_callable(onr) ? onr : JS_UNDEF, capP, capCell};
    if (d->state == 0) {
        if (d->nr == d->cr) {
            d->cr = d->cr ? d->cr * 2 : 2;
            d->rx = (jsval *)realloc(d->rx, d->cr * 4 * sizeof(jsval));
        }
        memcpy(&d->rx[d->nr * 4], r, sizeof r);
        d->nr++;
    } else if (d->state == 1)
        enqueue_job(rt, J_FULFILL, r[0], p->u.data.v, r[2], r[3]);
    else {
        if (!d->handled) track_unhandled(rt, p, false);
        enqueue_job(rt, J_REJECT, r[1], p->u.data.v, r[2], r[3]);
    }
    d->handled = true;
}

/* NewPromiseCapability(C): returns promise; *cell = Cell(resolve, reject) or JS_UNDEF for intrinsic */
NATIVE(cap_executor) {
    Cell *c = (Cell *)vptr(KEPT);
    if (!is_undef(c->a) || !is_undef(c->b)) THROW_TYPE("Promise executor has already been invoked with non-undefined arguments");
    c->a = ARG(0);
    c->b = ARG(1);
    return JS_UNDEF;
}
static jsval new_capability(js_rt *rt, jsval C, jsval *cell) {
    *cell = JS_UNDEF;
    if (C == mkobj(rt->intr[I_Promise])) return mkobj(promise_new(rt));
    if (!is_ctor(C)) return throw_error(rt, E_TYPE, "Promise resolve or reject function is not callable");
    Cell *c = new_cell(rt, JS_UNDEF, JS_UNDEF);
    jsval *root = push_tmp(rt, mkptr(c));
    JSObject *ex = native_with(rt, cap_executor, 2, mkptr(c));
    jsval exv = mkobj(ex);
    push_tmp(rt, exv);
    jsval p = construct_fn(rt, C, 1, &exv, C);
    rt->sp = root;
    if (p == JS_EXC) return JS_EXC;
    if (!is_callable(c->a) || !is_callable(c->b)) return throw_error(rt, E_TYPE, "Promise resolve or reject function is not callable");
    *cell = mkptr(c);
    return p;
}
static void cap_settle(js_rt *rt, jsval capP, jsval capCell, bool ok, jsval v) {
    if (is_ptr(capCell)) {
        Cell *c = (Cell *)vptr(capCell);
        jsval r = call_fn(rt, ok ? c->a : c->b, JS_UNDEF, 1, &v);
        if (r == JS_EXC && !rt->terminating) report_exception(rt, "Uncaught ");
        return;
    }
    if (is_cls(capP, CLS_PROMISE)) {
        if (ok)
            promise_resolve(rt, vobj(capP), v);
        else
            promise_reject(rt, vobj(capP), v);
    }
}

void job_trampoline(js_rt *rt, Job *j) {
    switch (j->kind) {
    case J_FULFILL:
    case J_REJECT: {
        jsval h = j->fn;
        bool ok;
        jsval res;
        if (is_undef(h)) {
            ok = j->kind == J_FULFILL;
            res = j->a;
        } else {
            res = call_fn(rt, h, JS_UNDEF, 1, &j->a);
            if (res == JS_EXC) {
                if (rt->terminating) return;
                ok = false;
                res = rt->exc;
                rt->exc = JS_UNDEF;
                rt->has_exc = false;
            } else
                ok = true;
        }
        push_tmp(rt, res);
        if (is_undef(j->b) && is_undef(j->c)) {
            if (!ok && !is_undef(h)) {
                rt->exc = res;
                rt->has_exc = true;
                report_exception(rt, "Uncaught ");
            }
            return;
        }
        cap_settle(rt, j->b, j->c, ok, res);
        return;
    }
    case J_THENABLE: {
        JSObject *p = vobj(j->b);
        jsval res, rej;
        make_resolving(rt, p, &res, &rej);
        jsval args[2] = {res, rej};
        jsval r = call_fn(rt, j->fn, j->a, 2, args);
        if (r == JS_EXC) {
            if (rt->terminating) return;
            jsval e = rt->exc;
            rt->exc = JS_UNDEF;
            rt->has_exc = false;
            call_fn(rt, rej, JS_UNDEF, 1, &e);
            rt->has_exc = false;
        }
        return;
    }
    case J_CALL: {
        jsval r = call_fn(rt, j->fn, JS_UNDEF, 0, 0);
        if (r == JS_EXC && !rt->terminating) report_exception(rt, "Uncaught ");
        return;
    }
    default: return;
    }
}

/* PromiseResolve(C, x) */
jsval promise_resolve_val(js_rt *rt, jsval C, jsval x) {
    if (is_cls(x, CLS_PROMISE)) {
        jsval *root = push_tmp(rt, x);
        jsval c = get_p(rt, x, ATOM(constructor));
        rt->sp = root;
        if (c == JS_EXC) return JS_EXC;
        if (c == C) return x;
    }
    jsval cell;
    jsval *root = push_tmp(rt, x);
    jsval p = new_capability(rt, C, &cell);
    if (p == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    push_tmp(rt, p);
    if (is_ptr(cell)) {
        jsval r = call_fn(rt, ((Cell *)vptr(cell))->a, JS_UNDEF, 1, &x);
        rt->sp = root;
        if (r == JS_EXC) return JS_EXC;
        return p;
    }
    promise_resolve(rt, vobj(p), x);
    rt->sp = root;
    return p;
}

/* ---------------- Promise constructor & prototype ---------------- */
NATIVE(promise_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Promise constructor cannot be invoked without 'new'");
    jsval ex = ARG(0);
    if (!is_callable(ex)) THROW_TYPE("Promise resolver %S is not a function", val_brief(rt, ex));
    JSObject *proto = proto_from_ctor(rt, rt->new_target, I_PromiseProto);
    if (!proto) return JS_EXC;
    JSObject *p = promise_alloc(rt, proto);
    push_tmp(rt, mkobj(p));
    jsval res, rej;
    make_resolving(rt, p, &res, &rej);
    jsval args[2] = {res, rej};
    jsval r = call_fn(rt, ex, JS_UNDEF, 2, args);
    if (r == JS_EXC) {
        if (rt->terminating) return JS_EXC;
        jsval e = rt->exc;
        rt->exc = JS_UNDEF;
        rt->has_exc = false;
        push_tmp(rt, e);
        if (call_fn(rt, rej, JS_UNDEF, 1, &e) == JS_EXC) return JS_EXC;
    }
    return mkobj(p);
}

NATIVE(promise_then) {
    if (!is_cls(this_, CLS_PROMISE)) THROW_TYPE("Method Promise.prototype.then called on incompatible receiver %S", val_brief(rt, this_));
    JSObject *p = vobj(this_);
    jsval C;
    if (p->proto == rt->intr[I_PromiseProto] && !find_own(rt, p, ATOM(constructor))) {
        Prop *cp = find_own(rt, rt->intr[I_PromiseProto], ATOM(constructor));
        C = cp && !(cp->flags & P_ACC) && cp->val == mkobj(rt->intr[I_Promise]) ? cp->val : JS_UNDEF;
        if (is_undef(C) && !species_ctor(rt, p, I_Promise, &C)) return JS_EXC;
    } else if (!species_ctor(rt, p, I_Promise, &C))
        return JS_EXC;
    jsval cell;
    jsval np = new_capability(rt, C, &cell);
    TRY(np);
    push_tmp(rt, np);
    then_with_cap(rt, p, ARG(0), ARG(1), np, cell);
    return np;
}
NATIVE(promise_catch) {
    jsval args[2] = {JS_UNDEF, ARG(0)};
    return call_method(rt, this_, ATOM(then), 2, args);
}

/* finally: KEPT = Cell(onFinally, C) */
NATIVE(finally_value_thunk) { return KEPT; }
NATIVE(finally_thrower) { return throw_val(rt, KEPT); }
NATIVE(finally_handler) {
    Cell *c = (Cell *)vptr(KEPT);
    bool is_catch = SELF->u.nat.length == 2;
    jsval r = call_fn(rt, c->a, JS_UNDEF, 0, 0);
    TRY(r);
    jsval p = promise_resolve_val(rt, c->b, r);
    TRY(p);
    push_tmp(rt, p);
    JSObject *k = native_with(rt, is_catch ? finally_thrower : finally_value_thunk, 0, ARG(0));
    jsval kv = mkobj(k);
    push_tmp(rt, kv);
    return call_method(rt, p, ATOM(then), 1, &kv);
}
NATIVE(promise_finally) {
    if (!is_obj(this_)) THROW_TYPE("Method Promise.prototype.finally called on incompatible receiver %S", val_brief(rt, this_));
    jsval C;
    if (!species_ctor(rt, vobj(this_), I_Promise, &C)) return JS_EXC;
    jsval of = ARG(0);
    if (!is_callable(of)) {
        jsval a[2] = {of, of};
        return call_method(rt, this_, ATOM(then), 2, a);
    }
    Cell *c = new_cell(rt, of, C);
    push_tmp(rt, mkptr(c));
    JSObject *tf = native_with(rt, finally_handler, 1, mkptr(c));
    push_tmp(rt, mkobj(tf));
    JSObject *cf = native_with(rt, finally_handler, 1, mkptr(c));
    cf->u.nat.length = 2;
    materialize_fn(rt, cf);
    def_prop(rt, cf, ATOM(length), mknum(1), P_C);
    push_tmp(rt, mkobj(cf));
    jsval a[2] = {mkobj(tf), mkobj(cf)};
    return call_method(rt, this_, ATOM(then), 2, a);
}

NATIVE(promise_resolve_n) {
    if (!is_obj(this_)) THROW_TYPE("PromiseResolve called on non-object");
    return promise_resolve_val(rt, this_, ARG(0));
}
NATIVE(promise_reject_n) {
    jsval cell;
    jsval p = new_capability(rt, this_, &cell);
    TRY(p);
    push_tmp(rt, p);
    cap_settle(rt, p, cell, false, ARG(0));
    return p;
}
NATIVE(promise_with_resolvers) {
    jsval cell;
    jsval p = new_capability(rt, this_, &cell);
    TRY(p);
    push_tmp(rt, p);
    jsval res, rej;
    if (is_ptr(cell)) {
        res = ((Cell *)vptr(cell))->a;
        rej = ((Cell *)vptr(cell))->b;
    } else
        make_resolving(rt, vobj(p), &res, &rej);
    JSObject *o = new_plain(rt);
    add_prop(rt, o, ATOM(promise), p, P_DEFAULT);
    add_prop(rt, o, ATOM(resolve), res, P_DEFAULT);
    add_prop(rt, o, ATOM(reject), rej, P_DEFAULT);
    return mkobj(o);
}

/* combinators. state object (CLS_INTERNAL): it.a = values array, it.b = capability Cell(resolve, reject), it.i = remaining */
static void comb_done(js_rt *rt, JSObject *st, bool any) {
    Cell *cap = (Cell *)vptr(st->u.it.b);
    jsval v = st->u.it.a;
    if (any) {
        JSObject *e = make_error(rt, E_AGGREGATE, str_cstr(rt, "All promises were rejected"));
        add_prop(rt, e, ATOM(errors), v, P_W | P_C);
        v = mkobj(e);
        push_tmp(rt, v);
    }
    jsval r = call_fn(rt, any ? cap->b : cap->a, JS_UNDEF, 1, &v);
    (void)r;
}
static bool comb_enter(JSObject *self, JSObject **st, uint32_t *idx) {
    Cell *c = (Cell *)vptr(kept(self));
    Cell *f2 = (Cell *)vptr(c->b); /* b = Cell(index, called) */
    if (f2->b == JS_TRUE) return false;
    f2->b = JS_TRUE;
    *st = vobj(c->a);
    *idx = (uint32_t)vnum(f2->a);
    return true;
}
NATIVE(all_elem) {
    JSObject *st;
    uint32_t idx;
    if (!comb_enter(SELF, &st, &idx)) return JS_UNDEF;
    JSObject *vals = vobj(st->u.it.a);
    if (idx < vals->u.arr.len) vals->u.arr.elems[idx] = ARG(0);
    if (--st->u.it.i == 0) comb_done(rt, st, false);
    return JS_UNDEF;
}
NATIVE(settled_elem) {
    JSObject *st;
    uint32_t idx;
    if (!comb_enter(SELF, &st, &idx)) return JS_UNDEF;
    bool rej = SELF->u.nat.length == 2;
    JSObject *o = new_plain(rt);
    add_prop(rt, o, ATOM(status), rej ? ATOM(rejected) : ATOM(fulfilled), P_DEFAULT);
    add_prop(rt, o, rej ? ATOM(reason) : ATOM(value), ARG(0), P_DEFAULT);
    JSObject *vals = vobj(st->u.it.a);
    if (idx < vals->u.arr.len) vals->u.arr.elems[idx] = mkobj(o);
    if (--st->u.it.i == 0) comb_done(rt, st, false);
    return JS_UNDEF;
}
NATIVE(any_elem) {
    JSObject *st;
    uint32_t idx;
    if (!comb_enter(SELF, &st, &idx)) return JS_UNDEF;
    JSObject *vals = vobj(st->u.it.a);
    if (idx < vals->u.arr.len) vals->u.arr.elems[idx] = ARG(0);
    if (--st->u.it.i == 0) comb_done(rt, st, true);
    return JS_UNDEF;
}

static jsval reject_cap_with_exc(js_rt *rt, jsval capP, Cell *cap) {
    if (rt->terminating) return JS_EXC;
    jsval e = rt->exc;
    rt->exc = JS_UNDEF;
    rt->has_exc = false;
    push_tmp(rt, e);
    if (call_fn(rt, cap->b, JS_UNDEF, 1, &e) == JS_EXC) return JS_EXC;
    return capP;
}

NATIVE(promise_combinator) {
    int kind = MAGIC; /* 0 all 1 allSettled 2 any 3 race */
    jsval C = this_;
    jsval cell;
    jsval capP = new_capability(rt, C, &cell);
    TRY(capP);
    push_tmp(rt, capP);
    Cell *cap;
    if (is_ptr(cell))
        cap = (Cell *)vptr(cell);
    else {
        jsval res, rej;
        make_resolving(rt, vobj(capP), &res, &rej);
        cap = new_cell(rt, res, rej);
    }
    push_tmp(rt, mkptr(cap));
    jsval presolve = get_p(rt, C, ATOM(resolve));
    if (presolve == JS_EXC) return reject_cap_with_exc(rt, capP, cap);
    push_tmp(rt, presolve);
    if (!is_callable(presolve)) {
        throw_error(rt, E_TYPE, "Promise resolve or reject function is not callable");
        return reject_cap_with_exc(rt, capP, cap);
    }
    jsval next;
    jsval it = get_iterator(rt, ARG(0), &next, false);
    if (it == JS_EXC) return reject_cap_with_exc(rt, capP, cap);
    push_tmp(rt, it);
    push_tmp(rt, next);
    JSObject *vals = new_array(rt, 0);
    push_tmp(rt, mkobj(vals));
    JSObject *st = new_object(rt, 0, CLS_INTERNAL);
    st->u.it.a = mkobj(vals);
    st->u.it.b = mkptr(cap);
    st->u.it.i = 1;
    push_tmp(rt, mkobj(st));
    uint32_t index = 0;
    for (;;) {
        jsval v;
        int s = iter_step(rt, it, next, &v);
        if (s < 0) return reject_cap_with_exc(rt, capP, cap);
        if (!s) break;
        jsval *r2 = push_tmp(rt, v);
        if (kind != 3) array_push(rt, vals, JS_UNDEF);
        jsval np = call_fn(rt, presolve, C, 1, &v);
        if (np == JS_EXC) {
            jsval e = rt->exc;
            push_tmp(rt, e);
            iter_close(rt, it, true);
            return reject_cap_with_exc(rt, capP, cap);
        }
        push_tmp(rt, np);
        jsval onf, onr;
        if (kind == 3) {
            onf = cap->a;
            onr = cap->b;
        } else {
            Cell *ic = new_cell(rt, mknum(index), JS_FALSE);
            Cell *c = new_cell(rt, mkobj(st), mkptr(ic));
            push_tmp(rt, mkptr(c));
            st->u.it.i++;
            if (kind == 0) {
                onf = mkobj(native_with(rt, all_elem, 1, mkptr(c)));
                onr = cap->b;
            } else if (kind == 1) {
                onf = mkobj(native_with(rt, settled_elem, 1, mkptr(c)));
                push_tmp(rt, onf);
                JSObject *rj = native_with(rt, settled_elem, 1, mkptr(c));
                rj->u.nat.length = 2;
                materialize_fn(rt, rj);
                def_prop(rt, rj, ATOM(length), mknum(1), P_C);
                onr = mkobj(rj);
            } else {
                onf = cap->a;
                onr = mkobj(native_with(rt, any_elem, 1, mkptr(c)));
            }
            push_tmp(rt, onf);
            push_tmp(rt, onr);
        }
        jsval a[2] = {onf, onr};
        jsval tr = call_method(rt, np, ATOM(then), 2, a);
        if (tr == JS_EXC) {
            jsval e = rt->exc;
            push_tmp(rt, e);
            iter_close(rt, it, true);
            return reject_cap_with_exc(rt, capP, cap);
        }
        index++;
        rt->sp = r2;
    }
    if (kind != 3 && --st->u.it.i == 0) comb_done(rt, st, kind == 2);
    return capP;
}

NATIVE(promise_species) { return this_; }

/* ---------------- async functions / generators ---------------- */
static void async_gen_after(js_rt *rt, JSObject *co, jsval r);
static void coro_continue(js_rt *rt, JSObject *co, int mode, jsval v) {
    jsval *root = push_tmp(rt, mkobj(co));
    push_tmp(rt, v);
    CoroData *cd = (CoroData *)co->u.data.p;
    jsval r = coro_resume(rt, co, mode, v);
    if (co->cls == CLS_ASYNC) {
        if (r == JS_EXC && !rt->terminating) {
            jsval e = rt->exc;
            rt->exc = JS_UNDEF;
            rt->has_exc = false;
            promise_reject(rt, vobj(cd->promise), e);
        }
        rt->sp = root;
        return;
    }
    async_gen_after(rt, co, r);
    async_gen_drain(rt, co);
    rt->sp = root;
}
NATIVE(await_fulfilled) {
    coro_continue(rt, vobj(KEPT), 0, ARG(0));
    if (rt->has_exc && !rt->terminating) rt->has_exc = false;
    return JS_UNDEF;
}
NATIVE(await_rejected) {
    coro_continue(rt, vobj(KEPT), 1, ARG(0));
    if (rt->has_exc && !rt->terminating) rt->has_exc = false;
    return JS_UNDEF;
}
void async_await(js_rt *rt, JSObject *co, jsval v) {
    jsval *root = push_tmp(rt, mkobj(co));
    push_tmp(rt, v);
    jsval p = promise_resolve_val(rt, mkobj(rt->intr[I_Promise]), v);
    if (p == JS_EXC) {
        if (rt->terminating) {
            rt->sp = root;
            return;
        }
        jsval e = rt->exc;
        rt->exc = JS_UNDEF;
        rt->has_exc = false;
        JSObject *rp = promise_new(rt);
        pd(rp)->handled = true;
        promise_reject(rt, rp, e);
        p = mkobj(rp);
    }
    push_tmp(rt, p);
    JSObject *onf = native_with(rt, await_fulfilled, 1, mkobj(co));
    push_tmp(rt, mkobj(onf));
    JSObject *onr = native_with(rt, await_rejected, 1, mkobj(co));
    push_tmp(rt, mkobj(onr));
    promise_then_internal(rt, vobj(p), mkobj(onf), mkobj(onr), 0);
    rt->sp = root;
}

static void queue_shift(CoroData *cd) {
    memmove(cd->queue, cd->queue + 3, (size_t)(cd->qlen - 1) * 3 * sizeof(jsval));
    cd->qlen--;
}
static void async_gen_after(js_rt *rt, JSObject *co, jsval r) {
    CoroData *cd = (CoroData *)co->u.data.p;
    if (r != JS_EXC && rt->coro_status == 2) return; /* awaiting */
    if (!cd->qlen) return;
    JSObject *p = vobj(cd->queue[2]);
    jsval *root = push_tmp(rt, mkobj(p));
    queue_shift(cd);
    if (r == JS_EXC) {
        if (rt->terminating) {
            rt->sp = root;
            return;
        }
        jsval e = rt->exc;
        rt->exc = JS_UNDEF;
        rt->has_exc = false;
        cd->state = 3;
        promise_reject(rt, p, e);
    } else {
        push_tmp(rt, r);
        bool done = rt->coro_status == 0;
        if (done) cd->state = 3;
        promise_resolve(rt, p, iter_result(rt, r, done));
    }
    rt->sp = root;
}
void async_gen_drain(js_rt *rt, JSObject *co) {
    CoroData *cd = (CoroData *)co->u.data.p;
    jsval *root = push_tmp(rt, mkobj(co));
    while (cd->qlen && (cd->state == 0 || cd->state == 1 || cd->state == 3)) {
        int mode = (int)vnum(cd->queue[0]);
        jsval v = cd->queue[1];
        JSObject *p = vobj(cd->queue[2]);
        if (cd->state == 0 && mode != 0) cd->state = 3;
        if (cd->state == 3) {
            push_tmp(rt, mkobj(p));
            push_tmp(rt, v);
            queue_shift(cd);
            if (mode == 1)
                promise_reject(rt, p, v);
            else
                promise_resolve(rt, p, iter_result(rt, mode == 2 ? v : JS_UNDEF, true));
            continue;
        }
        jsval r = coro_resume(rt, co, mode, v);
        async_gen_after(rt, co, r);
        if (rt->terminating) break;
    }
    rt->sp = root;
}
jsval async_gen_enqueue(js_rt *rt, jsval gen, int mode, jsval v) {
    JSObject *p = promise_new(rt);
    jsval *root = push_tmp(rt, mkobj(p));
    if (!is_cls(gen, CLS_ASYNC_GENERATOR)) {
        JSObject *e = make_error(rt, E_TYPE, str_cstr(rt, "AsyncGenerator method called on incompatible receiver"));
        promise_reject(rt, p, mkobj(e));
        rt->sp = root;
        return mkobj(p);
    }
    JSObject *co = vobj(gen);
    CoroData *cd = (CoroData *)co->u.data.p;
    if (cd->qlen == cd->qcap) {
        cd->qcap = cd->qcap ? cd->qcap * 2 : 4;
        cd->queue = (jsval *)realloc(cd->queue, cd->qcap * 3 * sizeof(jsval));
    }
    cd->queue[cd->qlen * 3] = mknum(mode);
    cd->queue[cd->qlen * 3 + 1] = v;
    cd->queue[cd->qlen * 3 + 2] = mkobj(p);
    cd->qlen++;
    if (cd->state != 2 && cd->state != 4) async_gen_drain(rt, co);
    rt->sp = root;
    return mkobj(p);
}
void async_gen_complete_step(js_rt *rt, JSObject *co, int what, jsval v) { (void)rt; (void)co; (void)what; (void)v; }

NATIVE(queue_microtask) {
    if (!is_callable(ARG(0))) THROW_TYPE("The \"callback\" argument must be of type function. Received %S", val_brief(rt, ARG(0)));
    enqueue_job(rt, J_CALL, argv[0], JS_UNDEF, JS_UNDEF, JS_UNDEF);
    return JS_UNDEF;
}

void report_unhandled(js_rt *rt) {
    while (rt->nunhandled) {
        jsval p = rt->unhandled[--rt->nunhandled];
        if (pd(vobj(p))->handled) continue;
        pd(vobj(p))->handled = true;
        rt->exc = vobj(p)->u.data.v;
        rt->has_exc = true;
        report_exception(rt, "Uncaught (in promise) ");
    }
}

void init_promise(js_rt *rt) {
    JSObject *pp = new_object(rt, rt->intr[I_ObjectProto], CLS_OBJECT);
    rt->intr[I_PromiseProto] = pp;
    JSObject *c = new_ctor(rt, "Promise", promise_ctor, 1, pp);
    rt->intr[I_Promise] = c;
    rt->intr[I_PromiseThen] = def_method(rt, pp, "then", promise_then, 2, 0);
    def_method(rt, pp, "catch", promise_catch, 1, 0);
    def_method(rt, pp, "finally", promise_finally, 1, 0);
    def_prop(rt, pp, SYM(toStringTag), mkstr(str_cstr(rt, "Promise")), P_C);
    def_method(rt, c, "resolve", promise_resolve_n, 1, 0);
    def_method(rt, c, "reject", promise_reject_n, 1, 0);
    def_method(rt, c, "all", promise_combinator, 1, 0);
    def_method(rt, c, "allSettled", promise_combinator, 1, 1);
    def_method(rt, c, "any", promise_combinator, 1, 2);
    def_method(rt, c, "race", promise_combinator, 1, 3);
    def_method(rt, c, "withResolvers", promise_with_resolvers, 0, 0);
    def_getter_k(rt, c, SYM(species), "get [Symbol.species]", promise_species, 0);
    def_method(rt, rt->global, "queueMicrotask", queue_microtask, 1, 0);
}
