/* AXIOM JavaScript engine: Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry,
 * ArrayBuffer, typed arrays, DataView, structuredClone; GC hooks for class-private data. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

/* ---------------- Map / Set storage ---------------- */
typedef struct MEnt {
    jsval k, v;
    uint32_t next; /* chain: index+1 */
    uint32_t hash;
    bool del;
} MEnt;
typedef struct MData {
    MEnt *e;
    uint32_t n, cap, live;
    uint32_t *b;
    uint32_t nb;
    int iters;
} MData;

static MData *md(JSObject *o) { return (MData *)o->u.data.p; }

static uint32_t mix64(uint64_t u) {
    u ^= u >> 33;
    u *= 0xff51afd7ed558ccdull;
    u ^= u >> 33;
    u *= 0xc4ceb9fe1a85ec53ull;
    u ^= u >> 33;
    return (uint32_t)u;
}
static uint32_t hash_key(js_rt *rt, jsval k) {
    if (is_num(k)) {
        double d = vnum(k);
        if (d == 0) d = 0;
        return mix64(d != d ? JS_NAN_BITS : dbits(d));
    }
    if (is_str(k)) return mix64(str_hash(rt, vstr(k)));
    return mix64(k);
}
static jsval norm_key(jsval k) {
    if (is_num(k) && vnum(k) == 0) return mknum(0);
    return k;
}

JSObject *map_new(js_rt *rt, bool is_set) {
    JSObject *o = new_object(rt, rt->intr[is_set ? I_SetProto : I_MapProto], is_set ? CLS_SET : CLS_MAP);
    o->u.data.p = calloc(1, sizeof(MData));
    return o;
}
static int64_t map_find(js_rt *rt, MData *m, jsval k, uint32_t h) {
    if (!m->nb) return -1;
    for (uint32_t i = m->b[h & (m->nb - 1)]; i; i = m->e[i - 1].next) {
        MEnt *e = &m->e[i - 1];
        if (!e->del && e->hash == h && same_value_zero(rt, e->k, k)) return i - 1;
    }
    return -1;
}
static void map_rehash(MData *m, uint32_t nb) {
    free(m->b);
    m->b = (uint32_t *)calloc(nb, sizeof(uint32_t));
    m->nb = nb;
    for (uint32_t i = 0; i < m->n; i++) {
        MEnt *e = &m->e[i];
        if (e->del) {
            e->next = 0;
            continue;
        }
        uint32_t s = e->hash & (nb - 1);
        e->next = m->b[s];
        m->b[s] = i + 1;
    }
}
static void map_compact(MData *m) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (!m->e[i].del) m->e[k++] = m->e[i];
    m->n = k;
    map_rehash(m, m->nb);
}
int map_set_raw(js_rt *rt, JSObject *o, jsval k, jsval v) {
    MData *m = md(o);
    k = norm_key(k);
    uint32_t h = hash_key(rt, k);
    int64_t i = map_find(rt, m, k, h);
    if (i >= 0) {
        m->e[i].v = v;
        return 0;
    }
    if (m->n == m->cap) {
        if (m->iters == 0 && m->live < m->n / 2) map_compact(m);
        if (m->n == m->cap) {
            uint32_t nc = m->cap ? m->cap * 2 : 8;
            MEnt *ne = (MEnt *)realloc(m->e, nc * sizeof(MEnt));
            if (!ne) return -1;
            m->e = ne;
            m->cap = nc;
            rt->bytes += (size_t)(nc / 2) * sizeof(MEnt);
        }
    }
    if (m->live + 1 > m->nb) map_rehash(m, m->nb ? m->nb * 2 : 8);
    MEnt *e = &m->e[m->n];
    e->k = k;
    e->v = v;
    e->hash = h;
    e->del = false;
    uint32_t s = h & (m->nb - 1);
    e->next = m->b[s];
    m->b[s] = m->n + 1;
    m->n++;
    m->live++;
    return 0;
}
jsval weak_get(js_rt *rt, JSObject *o, jsval k, bool *found) {
    MData *m = md(o);
    k = norm_key(k);
    int64_t i = map_find(rt, m, k, hash_key(rt, k));
    *found = i >= 0;
    return i >= 0 ? m->e[i].v : JS_UNDEF;
}
void map_trace(js_rt *rt, JSObject *o) {
    if (o->cls == CLS_WEAKMAP || o->cls == CLS_WEAKSET) {
        gc_mark_val(rt, o->u.data.v);
        return;
    }
    MData *m = md(o);
    if (!m) return;
    for (uint32_t i = 0; i < m->n; i++)
        if (!m->e[i].del) {
            gc_mark_val(rt, m->e[i].k);
            gc_mark_val(rt, m->e[i].v);
        }
}
void map_finalize(JSObject *o) {
    if (o->cls == CLS_WEAKMAP || o->cls == CLS_WEAKSET) return;
    MData *m = md(o);
    if (m) {
        free(m->e);
        free(m->b);
        free(m);
    }
    o->u.data.p = 0;
}

static MData *this_map(js_rt *rt, jsval t, int cls, const char *name) {
    if (!is_cls(t, cls)) {
        throw_error(rt, E_TYPE, "Method %s.prototype.%s called on incompatible receiver %S", cls == CLS_MAP ? "Map" : "Set", name,
                    val_brief(rt, t));
        return 0;
    }
    return md(vobj(t));
}

static jsval collection_ctor(js_rt *rt, jsval iterable, bool is_set) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor %s requires 'new'", is_set ? "Set" : "Map");
    JSObject *proto = proto_from_ctor(rt, rt->new_target, is_set ? I_SetProto : I_MapProto);
    if (!proto) return JS_EXC;
    JSObject *o = map_new(rt, is_set);
    o->proto = proto;
    jsval ov = mkobj(o);
    push_tmp(rt, ov);
    if (is_nullish(iterable)) return ov;
    jsval adder = get_p(rt, ov, is_set ? ATOM(add) : ATOM(set));
    TRY(adder);
    push_tmp(rt, adder);
    if (!is_callable(adder)) THROW_TYPE("'%s' returned for property '%s' of object '#<%s>' is not a function", "undefined", is_set ? "add" : "set", is_set ? "Set" : "Map");
    jsval next;
    jsval it = get_iterator(rt, iterable, &next, false);
    TRY(it);
    push_tmp(rt, it);
    push_tmp(rt, next);
    for (;;) {
        jsval e;
        int r = iter_step(rt, it, next, &e);
        if (r < 0) return JS_EXC;
        if (!r) break;
        jsval *r2 = push_tmp(rt, e);
        jsval res;
        if (is_set) {
            res = call_fn(rt, adder, ov, 1, &e);
        } else {
            if (!is_obj(e)) {
                throw_error(rt, E_TYPE, "Iterator value %S is not an entry object", val_brief(rt, e));
                iter_close(rt, it, true);
                return JS_EXC;
            }
            jsval k = get_idx(rt, e, 0);
            if (k == JS_EXC) {
                iter_close(rt, it, true);
                return JS_EXC;
            }
            push_tmp(rt, k);
            jsval v = get_idx(rt, e, 1);
            if (v == JS_EXC) {
                iter_close(rt, it, true);
                return JS_EXC;
            }
            push_tmp(rt, v);
            jsval a[2] = {k, v};
            res = call_fn(rt, adder, ov, 2, a);
        }
        if (res == JS_EXC) {
            iter_close(rt, it, true);
            return JS_EXC;
        }
        rt->sp = r2;
    }
    return ov;
}
NATIVE(map_ctor) { return collection_ctor(rt, ARG(0), false); }
NATIVE(set_ctor) { return collection_ctor(rt, ARG(0), true); }

NATIVE(map_get) {
    MData *m = this_map(rt, this_, CLS_MAP, "get");
    if (!m) return JS_EXC;
    bool f;
    return weak_get(rt, vobj(this_), ARG(0), &f);
}
NATIVE(map_set) {
    if (!this_map(rt, this_, CLS_MAP, "set")) return JS_EXC;
    map_set_raw(rt, vobj(this_), ARG(0), ARG(1));
    return this_;
}
NATIVE(set_add) {
    if (!this_map(rt, this_, CLS_SET, "add")) return JS_EXC;
    map_set_raw(rt, vobj(this_), ARG(0), ARG(0));
    return this_;
}
NATIVE(coll_has) {
    int cls = MAGIC ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "has");
    if (!m) return JS_EXC;
    jsval k = norm_key(ARG(0));
    return mkbool(map_find(rt, m, k, hash_key(rt, k)) >= 0);
}
NATIVE(coll_delete) {
    int cls = MAGIC ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "delete");
    if (!m) return JS_EXC;
    jsval k = norm_key(ARG(0));
    int64_t i = map_find(rt, m, k, hash_key(rt, k));
    if (i < 0) return JS_FALSE;
    m->e[i].del = true;
    m->e[i].k = JS_UNDEF;
    m->e[i].v = JS_UNDEF;
    m->live--;
    return JS_TRUE;
}
NATIVE(coll_clear) {
    int cls = MAGIC ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "clear");
    if (!m) return JS_EXC;
    for (uint32_t i = 0; i < m->n; i++) {
        m->e[i].del = true;
        m->e[i].k = m->e[i].v = JS_UNDEF;
    }
    m->live = 0;
    if (m->iters == 0) {
        m->n = 0;
        if (m->nb) memset(m->b, 0, m->nb * sizeof(uint32_t));
    }
    return JS_UNDEF;
}
NATIVE(coll_size) {
    int cls = MAGIC ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "size");
    if (!m) return JS_EXC;
    return mknum(m->live);
}
NATIVE(coll_for_each) {
    int cls = MAGIC ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "forEach");
    if (!m) return JS_EXC;
    jsval cb = ARG(0);
    if (!is_callable(cb)) THROW_TYPE("%S is not a function", val_brief(rt, cb));
    m->iters++;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].del) continue;
        jsval a[3] = {m->e[i].v, m->e[i].k, this_};
        jsval r = call_fn(rt, cb, ARG(1), 3, a);
        if (r == JS_EXC) {
            m->iters--;
            return JS_EXC;
        }
    }
    m->iters--;
    return JS_UNDEF;
}
NATIVE(coll_iter) {
    int cls = (MAGIC & 4) ? CLS_SET : CLS_MAP;
    MData *m = this_map(rt, this_, cls, "entries");
    if (!m) return JS_EXC;
    JSObject *it = new_object(rt, rt->intr[cls == CLS_SET ? I_SetIterProto : I_MapIterProto], cls == CLS_SET ? CLS_SET_ITER : CLS_MAP_ITER);
    it->u.it.a = this_;
    it->u.it.i = 0;
    it->u.it.kind = (uint8_t)(MAGIC & 3);
    m->iters++;
    return mkobj(it);
}
NATIVE(coll_iter_next) {
    if (!is_cls(this_, CLS_MAP_ITER) && !is_cls(this_, CLS_SET_ITER)) THROW_TYPE("next method called on incompatible receiver");
    JSObject *it = vobj(this_);
    if (is_undef(it->u.it.a)) return iter_result(rt, JS_UNDEF, true);
    MData *m = md(vobj(it->u.it.a));
    while (it->u.it.i < m->n && m->e[it->u.it.i].del) it->u.it.i++;
    if (it->u.it.i >= m->n) {
        it->u.it.a = JS_UNDEF;
        if (m->iters > 0) m->iters--;
        return iter_result(rt, JS_UNDEF, true);
    }
    MEnt *e = &m->e[it->u.it.i++];
    jsval v;
    if (it->u.it.kind == 0)
        v = e->k;
    else if (it->u.it.kind == 1)
        v = e->v;
    else {
        jsval pair[2] = {e->k, e->v};
        v = mkobj(new_array_from(rt, pair, 2));
    }
    return iter_result(rt, v, false);
}

/* ES2025 Set methods (receiver-side simple implementations; `other` must be set-like) */
static int setlike(js_rt *rt, jsval o, jsval *has, jsval *keys, double *size) {
    if (!is_obj(o)) {
        throw_error(rt, E_TYPE, "The 'other' argument must be set-like");
        return -1;
    }
    jsval s = get_p(rt, o, ATOM(size));
    if (s == JS_EXC) return -1;
    if (to_number(rt, s, size) < 0) return -1;
    if (*size != *size) {
        throw_error(rt, E_TYPE, "The 'size' property must be a number");
        return -1;
    }
    *has = get_p(rt, o, ATOM(has));
    if (*has == JS_EXC) return -1;
    *keys = get_p(rt, o, ATOM(keys));
    if (*keys == JS_EXC) return -1;
    if (!is_callable(*has) || !is_callable(*keys)) {
        throw_error(rt, E_TYPE, "The 'other' argument must be set-like");
        return -1;
    }
    return 0;
}
NATIVE(set_op) {
    int op = MAGIC; /* 0 union 1 intersection 2 difference 3 symmetricDifference 4 isSubsetOf 5 isSupersetOf 6 isDisjointFrom */
    MData *m = this_map(rt, this_, CLS_SET, "union");
    if (!m) return JS_EXC;
    jsval other = ARG(0), has, keys;
    double osize;
    if (setlike(rt, other, &has, &keys, &osize) < 0) return JS_EXC;
    push_tmp(rt, has);
    push_tmp(rt, keys);
    JSObject *res = map_new(rt, true);
    push_tmp(rt, mkobj(res));
    if (op == 0 || op == 3 || op == 2 || op == 1) {
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].del) continue;
            jsval k = m->e[i].k;
            if (op == 0 || op == 3) {
                map_set_raw(rt, res, k, k);
                continue;
            }
            jsval r = call_fn(rt, has, other, 1, &k);
            TRY(r);
            if ((op == 1) == truthy(r)) map_set_raw(rt, res, k, k);
        }
        if (op == 0 || op == 3) {
            jsval it = call_fn(rt, keys, other, 0, 0);
            TRY(it);
            push_tmp(rt, it);
            jsval next = get_p(rt, it, ATOM(next));
            TRY(next);
            push_tmp(rt, next);
            jsval v;
            int s;
            while ((s = iter_step(rt, it, next, &v)) > 0) {
                v = norm_key(v);
                if (op == 0)
                    map_set_raw(rt, res, v, v);
                else {
                    MData *rm = md(res);
                    int64_t in_this = map_find(rt, m, v, hash_key(rt, v));
                    int64_t in_res = map_find(rt, rm, v, hash_key(rt, v));
                    if (in_this >= 0) {
                        if (in_res >= 0) {
                            rm->e[in_res].del = true;
                            rm->live--;
                        }
                    } else if (in_res < 0)
                        map_set_raw(rt, res, v, v);
                }
            }
            if (s < 0) return JS_EXC;
        }
        return mkobj(res);
    }
    if (op == 4 || op == 6) {
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].del) continue;
            jsval k = m->e[i].k;
            jsval r = call_fn(rt, has, other, 1, &k);
            TRY(r);
            if (op == 4 && !truthy(r)) return JS_FALSE;
            if (op == 6 && truthy(r)) return JS_FALSE;
        }
        return JS_TRUE;
    }
    /* isSupersetOf */
    jsval it = call_fn(rt, keys, other, 0, 0);
    TRY(it);
    push_tmp(rt, it);
    jsval next = get_p(rt, it, ATOM(next));
    TRY(next);
    push_tmp(rt, next);
    jsval v;
    int s;
    while ((s = iter_step(rt, it, next, &v)) > 0) {
        v = norm_key(v);
        if (map_find(rt, m, v, hash_key(rt, v)) < 0) {
            iter_close(rt, it, false);
            return JS_FALSE;
        }
    }
    if (s < 0) return JS_EXC;
    return JS_TRUE;
}

/* ---------------- WeakMap / WeakSet (keys carry a hidden property) ---------------- */
static jsval weak_key_sym(js_rt *rt, jsval t, int cls, const char *name) {
    if (!is_cls(t, cls)) return throw_error(rt, E_TYPE, "Method %s.prototype.%s called on incompatible receiver %S", cls == CLS_WEAKMAP ? "WeakMap" : "WeakSet", name, val_brief(rt, t));
    return vobj(t)->u.data.v;
}
static bool weak_ok_key(jsval k) { return is_obj(k) || (is_sym(k) && !(vsym(k)->h.flags & 2)); }
static jsval weak_ctor(js_rt *rt, jsval iterable, bool is_set) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor %s requires 'new'", is_set ? "WeakSet" : "WeakMap");
    JSObject *proto = proto_from_ctor(rt, rt->new_target, is_set ? I_WeakSetProto : I_WeakMapProto);
    if (!proto) return JS_EXC;
    JSObject *o = new_object(rt, proto, is_set ? CLS_WEAKSET : CLS_WEAKMAP);
    JSSymbol *s = (JSSymbol *)gc_alloc(rt, sizeof(JSSymbol), GC_SYM);
    s->h.flags = 1;
    s->desc = mkstr(str_cstr(rt, "#weak"));
    s->hash = (uint32_t)(++rt->sym_counter * 2654435761u) | 1;
    o->u.data.v = mksym(s);
    jsval ov = mkobj(o);
    push_tmp(rt, ov);
    if (is_nullish(iterable)) return ov;
    jsval adder = get_p(rt, ov, is_set ? ATOM(add) : ATOM(set));
    TRY(adder);
    push_tmp(rt, adder);
    jsval next;
    jsval it = get_iterator(rt, iterable, &next, false);
    TRY(it);
    push_tmp(rt, it);
    push_tmp(rt, next);
    for (;;) {
        jsval e;
        int r = iter_step(rt, it, next, &e);
        if (r < 0) return JS_EXC;
        if (!r) break;
        jsval *r2 = push_tmp(rt, e);
        jsval res;
        if (is_set)
            res = call_fn(rt, adder, ov, 1, &e);
        else {
            jsval k = get_idx(rt, e, 0);
            if (k == JS_EXC) return JS_EXC;
            push_tmp(rt, k);
            jsval v = get_idx(rt, e, 1);
            if (v == JS_EXC) return JS_EXC;
            jsval a[2] = {k, v};
            res = call_fn(rt, adder, ov, 2, a);
        }
        if (res == JS_EXC) {
            iter_close(rt, it, true);
            return JS_EXC;
        }
        rt->sp = r2;
    }
    return ov;
}
NATIVE(weakmap_ctor) { return weak_ctor(rt, ARG(0), false); }
NATIVE(weakset_ctor) { return weak_ctor(rt, ARG(0), true); }
static JSObject *weak_holder(js_rt *rt, jsval k) {
    if (is_obj(k)) return vobj(k);
    return 0; /* symbols as weak keys: unsupported storage, treated as absent */
}
NATIVE(weak_op) {
    int op = MAGIC & 7; /* 0 get 1 set/add 2 has 3 delete */
    int cls = (MAGIC & 8) ? CLS_WEAKSET : CLS_WEAKMAP;
    jsval sym = weak_key_sym(rt, this_, cls, op == 0 ? "get" : op == 1 ? (cls == CLS_WEAKSET ? "add" : "set") : op == 2 ? "has" : "delete");
    TRY(sym);
    jsval k = ARG(0);
    if (!weak_ok_key(k)) {
        if (op == 1) THROW_TYPE("Invalid value used %s", cls == CLS_WEAKSET ? "in weak set" : "as weak map key");
        return op == 0 ? JS_UNDEF : JS_FALSE;
    }
    JSObject *h = weak_holder(rt, k);
    if (!h) return op == 1 ? this_ : op == 0 ? JS_UNDEF : JS_FALSE;
    Prop *p = find_own(rt, h, sym);
    switch (op) {
    case 0: return p ? p->val : JS_UNDEF;
    case 1:
        if (p)
            p->val = cls == CLS_WEAKSET ? JS_TRUE : ARG(1);
        else
            add_prop(rt, h, sym, cls == CLS_WEAKSET ? JS_TRUE : ARG(1), 0);
        return this_;
    case 2: return mkbool(p != 0);
    default:
        if (!p) return JS_FALSE;
        del_prop_at(rt, h, p);
        return JS_TRUE;
    }
}

/* WeakRef / FinalizationRegistry (references are strong; cleanup callbacks never run) */
NATIVE(weakref_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor WeakRef requires 'new'");
    if (!weak_ok_key(ARG(0))) THROW_TYPE("WeakRef: invalid target");
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_WeakRefProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_WEAKREF);
    o->u.prim.v = argv[0];
    return mkobj(o);
}
NATIVE(weakref_deref) {
    if (!is_cls(this_, CLS_WEAKREF)) THROW_TYPE("WeakRef.prototype.deref called on incompatible receiver");
    return vobj(this_)->u.prim.v;
}
NATIVE(finreg_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor FinalizationRegistry requires 'new'");
    if (!is_callable(ARG(0))) THROW_TYPE("FinalizationRegistry: cleanup must be callable");
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_FinRegProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_FINREG);
    o->u.data.v = argv[0];
    return mkobj(o);
}
NATIVE(finreg_register) {
    if (!is_cls(this_, CLS_FINREG)) THROW_TYPE("FinalizationRegistry method called on incompatible receiver");
    if (!weak_ok_key(ARG(0))) THROW_TYPE("FinalizationRegistry.prototype.register: invalid target");
    return MAGIC ? JS_FALSE : JS_UNDEF;
}

/* ---------------- ArrayBuffer ---------------- */
typedef struct ABuf {
    uint8_t *data;
    uint32_t len;
    bool detached;
} ABuf;
typedef struct TA {
    JSObject *buf;
    uint32_t off, len;
    uint8_t kind;
    bool track; /* length tracks buffer */
} TA;
enum { TA_I8, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_BI64, TA_BU64, TA_F16_UNUSED, TA__N = 11 };
static const uint8_t ta_size[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
static const char *const ta_names[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array",
                                       "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"};

static ABuf *abuf(JSObject *o) { return (ABuf *)o->u.data.p; }
static TA *tad(JSObject *o) { return (TA *)o->u.data.p; }

static JSObject *new_abuf(js_rt *rt, uint64_t len, JSObject *proto) {
    if (len > 0x7FFFFFFF) {
        throw_error(rt, E_RANGE, "Array buffer allocation failed");
        return 0;
    }
    JSObject *o = new_object(rt, proto ? proto : rt->intr[I_ArrayBufferProto], CLS_ARRAYBUFFER);
    ABuf *b = (ABuf *)calloc(1, sizeof(ABuf));
    b->data = (uint8_t *)calloc(len ? len : 1, 1);
    if (!b->data) {
        free(b);
        throw_error(rt, E_RANGE, "Array buffer allocation failed");
        return 0;
    }
    b->len = (uint32_t)len;
    o->u.data.p = b;
    rt->bytes += len;
    if (rt->bytes > rt->threshold) rt->gc_pending = true;
    return o;
}
void buffer_trace(js_rt *rt, JSObject *o) {
    if (o->cls == CLS_TYPEDARRAY || o->cls == CLS_DATAVIEW) {
        TA *t = tad(o);
        if (t) gc_mark_ptr(rt, t->buf);
    }
}
void buffer_finalize(JSObject *o) {
    if (o->cls == CLS_ARRAYBUFFER) {
        ABuf *b = abuf(o);
        if (b) {
            free(b->data);
            free(b);
        }
    } else
        free(o->u.data.p);
    o->u.data.p = 0;
}

NATIVE(abuf_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor ArrayBuffer requires 'new'");
    uint64_t len;
    if (to_index(rt, ARG(0), &len) < 0) {
        rt->has_exc = false;
        return throw_error(rt, E_RANGE, "Invalid array buffer length");
    }
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_ArrayBufferProto);
    if (!p) return JS_EXC;
    JSObject *o = new_abuf(rt, len, p);
    return o ? mkobj(o) : JS_EXC;
}
NATIVE(abuf_byte_length) {
    if (!is_cls(this_, CLS_ARRAYBUFFER)) THROW_TYPE("Method ArrayBuffer.prototype.byteLength called on incompatible receiver");
    return mknum(abuf(vobj(this_))->len);
}
NATIVE(abuf_slice) {
    if (!is_cls(this_, CLS_ARRAYBUFFER)) THROW_TYPE("Method ArrayBuffer.prototype.slice called on incompatible receiver");
    ABuf *b = abuf(vobj(this_));
    double len = b->len, s, e;
    if (to_int(rt, ARG(0), &s) < 0) return JS_EXC;
    if (is_undef(ARG(1)))
        e = len;
    else if (to_int(rt, argv[1], &e) < 0)
        return JS_EXC;
    if (s < 0) s = s + len < 0 ? 0 : s + len;
    if (s > len) s = len;
    if (e < 0) e = e + len < 0 ? 0 : e + len;
    if (e > len) e = len;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    JSObject *o = new_abuf(rt, n, 0);
    if (!o) return JS_EXC;
    memcpy(abuf(o)->data, b->data + (uint32_t)s, n);
    return mkobj(o);
}
NATIVE(abuf_is_view) { return mkbool(is_cls(ARG(0), CLS_TYPEDARRAY) || is_cls(ARG(0), CLS_DATAVIEW)); }

/* ---------------- typed arrays ---------------- */
uint32_t ta_length(JSObject *o) {
    TA *t = tad(o);
    if (!t || abuf(t->buf)->detached) return 0;
    if (t->track) {
        uint32_t bl = abuf(t->buf)->len;
        return bl > t->off ? (bl - t->off) / ta_size[t->kind] : 0;
    }
    return t->len;
}
static jsval ta_read(js_rt *rt, int kind, uint8_t *p) {
    switch (kind) {
    case TA_I8: return mknum((int8_t)p[0]);
    case TA_U8:
    case TA_U8C: return mknum(p[0]);
    case TA_I16: {
        int16_t v;
        memcpy(&v, p, 2);
        return mknum(v);
    }
    case TA_U16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return mknum(v);
    }
    case TA_I32: {
        int32_t v;
        memcpy(&v, p, 4);
        return mknum(v);
    }
    case TA_U32: {
        uint32_t v;
        memcpy(&v, p, 4);
        return mknum(v);
    }
    case TA_F32: {
        float v;
        memcpy(&v, p, 4);
        return mknum_canon((double)v);
    }
    case TA_F64: {
        double v;
        memcpy(&v, p, 8);
        return mknum_canon(v);
    }
    case TA_BI64:
    case TA_BU64: {
        uint64_t v;
        memcpy(&v, p, 8);
        bool neg = kind == TA_BI64 && (int64_t)v < 0;
        return big_from_u64(rt, neg ? (uint64_t)0 - v : v, neg);
    }
    default: return JS_UNDEF;
    }
}
static int to_u8clamp(double d) {
    if (!(d > 0)) return 0;
    if (d >= 255) return 255;
    double f = js_floor(d);
    if (f + 0.5 < d) return (int)f + 1;
    if (d < f + 0.5) return (int)f;
    return ((int)f & 1) ? (int)f + 1 : (int)f;
}
static void ta_write_num(int kind, uint8_t *p, double d) {
    switch (kind) {
    case TA_I8:
    case TA_U8: p[0] = (uint8_t)to_int32(d); break;
    case TA_U8C: p[0] = (uint8_t)to_u8clamp(d); break;
    case TA_I16:
    case TA_U16: {
        uint16_t v = (uint16_t)to_int32(d);
        memcpy(p, &v, 2);
        break;
    }
    case TA_I32:
    case TA_U32: {
        uint32_t v = (uint32_t)to_int32(d);
        memcpy(p, &v, 4);
        break;
    }
    case TA_F32: {
        float v = (float)d;
        memcpy(p, &v, 4);
        break;
    }
    case TA_F64: memcpy(p, &d, 8); break;
    default: break;
    }
}
bool ta_get(js_rt *rt, JSObject *o, uint64_t i, jsval *out) {
    TA *t = tad(o);
    if (i >= ta_length(o)) return false;
    *out = ta_read(rt, t->kind, abuf(t->buf)->data + t->off + i * ta_size[t->kind]);
    return true;
}
int ta_set(js_rt *rt, JSObject *o, uint64_t i, jsval v) {
    TA *t = tad(o);
    if (t->kind == TA_BI64 || t->kind == TA_BU64) {
        jsval b = to_bigint(rt, v);
        if (b == JS_EXC) return -1;
        if (i >= ta_length(o)) return 0;
        uint64_t iv = big_to_u64((JSBig *)vptr(b));
        memcpy(abuf(t->buf)->data + t->off + i * 8, &iv, 8);
        return 0;
    }
    double d;
    if (is_num(v))
        d = vnum(v);
    else {
        jsval *root = push_tmp(rt, mkobj(o));
        int r = to_number(rt, v, &d);
        rt->sp = root;
        if (r < 0) return -1;
    }
    if (i >= ta_length(o)) return 0;
    ta_write_num(t->kind, abuf(t->buf)->data + t->off + i * ta_size[t->kind], d);
    return 0;
}
JSObject *make_typed_array(js_rt *rt, int kind, uint32_t len) {
    JSObject *b = new_abuf(rt, (uint64_t)len * ta_size[kind], 0);
    if (!b) return 0;
    jsval *root = push_tmp(rt, mkobj(b));
    JSObject *o = new_object(rt, rt->intr[I_TA_FIRST + kind * 2], CLS_TYPEDARRAY);
    TA *t = (TA *)calloc(1, sizeof(TA));
    t->buf = b;
    t->len = len;
    t->kind = (uint8_t)kind;
    o->u.data.p = t;
    rt->sp = root;
    return o;
}

static int ta_kind_of_ctor(js_rt *rt, jsval c) {
    for (int k = 0; k < TA__N; k++)
        if (c == mkobj(rt->intr[I_TA_FIRST + k * 2 + 1])) return k;
    return -1;
}

NATIVE(ta_ctor) {
    int kind = MAGIC;
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor %s requires 'new'", ta_names[kind]);
    JSObject *proto = proto_from_ctor(rt, rt->new_target, I_TA_FIRST + kind * 2);
    if (!proto) return JS_EXC;
    jsval a0 = ARG(0);
    JSObject *o;
    if (!is_obj(a0)) {
        uint64_t len;
        if (to_index(rt, a0, &len) < 0) {
            rt->has_exc = false;
            return throw_error(rt, E_RANGE, "Invalid typed array length: %S", val_brief(rt, a0));
        }
        if (len > 0x7FFFFFFF / ta_size[kind]) return throw_error(rt, E_RANGE, "Invalid typed array length: %S", num_to_str(rt, (double)len));
        o = make_typed_array(rt, kind, (uint32_t)len);
        if (!o) return JS_EXC;
    } else if (vobj(a0)->cls == CLS_ARRAYBUFFER) {
        ABuf *b = abuf(vobj(a0));
        uint64_t off, len;
        if (to_index(rt, ARG(1), &off) < 0) return JS_EXC;
        if (off % ta_size[kind]) return throw_error(rt, E_RANGE, "start offset of %s should be a multiple of %d", ta_names[kind], ta_size[kind]);
        bool track = is_undef(ARG(2));
        if (track) {
            if (b->len % ta_size[kind]) return throw_error(rt, E_RANGE, "byte length of %s should be a multiple of %d", ta_names[kind], ta_size[kind]);
            if (off > b->len) return throw_error(rt, E_RANGE, "Start offset %S is outside the bounds of the buffer", num_to_str(rt, (double)off));
            len = (b->len - off) / ta_size[kind];
        } else {
            if (to_index(rt, argv[2], &len) < 0) return JS_EXC;
            if (off + len * ta_size[kind] > b->len) return throw_error(rt, E_RANGE, "Invalid typed array length: %S", num_to_str(rt, (double)len));
        }
        o = new_object(rt, proto, CLS_TYPEDARRAY);
        TA *t = (TA *)calloc(1, sizeof(TA));
        t->buf = vobj(a0);
        t->off = (uint32_t)off;
        t->len = (uint32_t)len;
        t->kind = (uint8_t)kind;
        o->u.data.p = t;
        return mkobj(o);
    } else {
        /* typed array, iterable or array-like */
        JSObject *src;
        if (vobj(a0)->cls == CLS_TYPEDARRAY) {
            uint32_t n = ta_length(vobj(a0));
            o = make_typed_array(rt, kind, n);
            if (!o) return JS_EXC;
            push_tmp(rt, mkobj(o));
            for (uint32_t i = 0; i < n; i++) {
                jsval v;
                ta_get(rt, vobj(a0), i, &v);
                if ((kind == TA_BI64 || kind == TA_BU64) != is_big(v)) THROW_TYPE("Cannot mix BigInt and other types, use explicit conversions");
                if (ta_set(rt, o, i, v) < 0) return JS_EXC;
            }
            o->proto = proto;
            return mkobj(o);
        }
        jsval itm = get_p(rt, a0, SYM(iterator));
        TRY(itm);
        if (!is_nullish(itm)) {
            src = iter_to_array(rt, a0);
            if (!src) return JS_EXC;
        } else
            src = vobj(a0);
        push_tmp(rt, mkobj(src));
        uint64_t n;
        if (len_of(rt, mkobj(src), &n) < 0) return JS_EXC;
        o = make_typed_array(rt, kind, (uint32_t)n);
        if (!o) return JS_EXC;
        push_tmp(rt, mkobj(o));
        for (uint32_t i = 0; i < n; i++) {
            jsval v = get_idx(rt, mkobj(src), i);
            TRY(v);
            if (ta_set(rt, o, i, v) < 0) return JS_EXC;
        }
    }
    o->proto = proto;
    return mkobj(o);
}

static JSObject *this_ta(js_rt *rt, jsval t, const char *name) {
    if (!is_cls(t, CLS_TYPEDARRAY)) {
        throw_error(rt, E_TYPE, "this is not a typed array.");
        (void)name;
        return 0;
    }
    return vobj(t);
}
NATIVE(ta_getter) {
    JSObject *o = this_ta(rt, this_, "length");
    if (!o) return JS_EXC;
    TA *t = tad(o);
    switch (MAGIC) {
    case 0: return mknum(ta_length(o));
    case 1: return mknum((double)ta_length(o) * ta_size[t->kind]);
    case 2: return mknum(t->off);
    case 3: return mkobj(t->buf);
    default: return JS_UNDEF;
    }
}
NATIVE(ta_to_string_tag) {
    if (!is_cls(this_, CLS_TYPEDARRAY)) return JS_UNDEF;
    return mkstr(str_cstr(rt, ta_names[tad(vobj(this_))->kind]));
}
static JSObject *ta_create_same(js_rt *rt, JSObject *src, uint32_t len) {
    JSObject *o = make_typed_array(rt, tad(src)->kind, len);
    if (o) o->proto = src->proto;
    return o;
}
NATIVE(ta_subarray) {
    JSObject *o = this_ta(rt, this_, "subarray");
    if (!o) return JS_EXC;
    TA *t = tad(o);
    double len = ta_length(o), s, e;
    if (to_int(rt, ARG(0), &s) < 0) return JS_EXC;
    if (is_undef(ARG(1)))
        e = len;
    else if (to_int(rt, argv[1], &e) < 0)
        return JS_EXC;
    if (s < 0) s = s + len < 0 ? 0 : s + len;
    if (s > len) s = len;
    if (e < 0) e = e + len < 0 ? 0 : e + len;
    if (e > len) e = len;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    JSObject *r = new_object(rt, o->proto, CLS_TYPEDARRAY);
    TA *nt = (TA *)calloc(1, sizeof(TA));
    nt->buf = t->buf;
    nt->off = t->off + (uint32_t)s * ta_size[t->kind];
    nt->len = n;
    nt->kind = t->kind;
    r->u.data.p = nt;
    return mkobj(r);
}
NATIVE(ta_slice) {
    JSObject *o = this_ta(rt, this_, "slice");
    if (!o) return JS_EXC;
    double len = ta_length(o), s, e;
    if (to_int(rt, ARG(0), &s) < 0) return JS_EXC;
    if (is_undef(ARG(1)))
        e = len;
    else if (to_int(rt, argv[1], &e) < 0)
        return JS_EXC;
    if (s < 0) s = s + len < 0 ? 0 : s + len;
    if (s > len) s = len;
    if (e < 0) e = e + len < 0 ? 0 : e + len;
    if (e > len) e = len;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    JSObject *r = ta_create_same(rt, o, n);
    if (!r) return JS_EXC;
    TA *t = tad(o);
    memcpy(abuf(tad(r)->buf)->data, abuf(t->buf)->data + t->off + (uint32_t)s * ta_size[t->kind], (size_t)n * ta_size[t->kind]);
    return mkobj(r);
}
NATIVE(ta_set_n) {
    JSObject *o = this_ta(rt, this_, "set");
    if (!o) return JS_EXC;
    double off;
    if (to_int(rt, ARG(1), &off) < 0) return JS_EXC;
    if (off < 0) return throw_error(rt, E_RANGE, "offset is out of bounds");
    jsval src = ARG(0);
    uint32_t dl = ta_length(o);
    if (is_cls(src, CLS_TYPEDARRAY)) {
        uint32_t n = ta_length(vobj(src));
        if (off + n > dl) return throw_error(rt, E_RANGE, "offset is out of bounds");
        /* copy through a temporary in case of overlap */
        jsval *tmp = (jsval *)malloc((n ? n : 1) * sizeof(jsval));
        for (uint32_t i = 0; i < n; i++) ta_get(rt, vobj(src), i, &tmp[i]);
        for (uint32_t i = 0; i < n; i++) ta_set(rt, o, (uint64_t)off + i, tmp[i]);
        free(tmp);
        return JS_UNDEF;
    }
    jsval so = to_object(rt, src);
    TRY(so);
    push_tmp(rt, so);
    uint64_t n;
    if (len_of(rt, so, &n) < 0) return JS_EXC;
    if (off + (double)n > dl) return throw_error(rt, E_RANGE, "offset is out of bounds");
    for (uint64_t i = 0; i < n; i++) {
        jsval v = get_idx(rt, so, (uint32_t)i);
        TRY(v);
        if (ta_set(rt, o, (uint64_t)off + i, v) < 0) return JS_EXC;
    }
    return JS_UNDEF;
}
NATIVE(ta_map_filter) {
    int m = MAGIC; /* 0 map 1 filter */
    JSObject *o = this_ta(rt, this_, "map");
    if (!o) return JS_EXC;
    jsval cb = ARG(0);
    if (!is_callable(cb)) THROW_TYPE("%S is not a function", val_brief(rt, cb));
    uint32_t len = ta_length(o);
    if (m == 0) {
        JSObject *r = ta_create_same(rt, o, len);
        if (!r) return JS_EXC;
        push_tmp(rt, mkobj(r));
        for (uint32_t i = 0; i < len; i++) {
            jsval v;
            ta_get(rt, o, i, &v);
            jsval a[3] = {v, mknum(i), this_};
            jsval x = call_fn(rt, cb, ARG(1), 3, a);
            TRY(x);
            if (ta_set(rt, r, i, x) < 0) return JS_EXC;
        }
        return mkobj(r);
    }
    JSObject *kept = new_array(rt, 0);
    push_tmp(rt, mkobj(kept));
    for (uint32_t i = 0; i < len; i++) {
        jsval v;
        if (!ta_get(rt, o, i, &v)) v = JS_UNDEF;
        jsval a[3] = {v, mknum(i), this_};
        jsval x = call_fn(rt, cb, ARG(1), 3, a);
        TRY(x);
        if (truthy(x)) array_push(rt, kept, v);
    }
    JSObject *r = ta_create_same(rt, o, kept->u.arr.len);
    if (!r) return JS_EXC;
    for (uint32_t i = 0; i < kept->u.arr.len; i++) ta_set(rt, r, i, kept->u.arr.elems[i]);
    return mkobj(r);
}
static int num_cmp(const double *a, const double *b) {
    double x = *a, y = *b;
    if (x != x) return y != y ? 0 : 1;
    if (y != y) return -1;
    if (x < y) return -1;
    if (x > y) return 1;
    if (x == 0 && y == 0) return js_signbit(x) ? (js_signbit(y) ? 0 : -1) : (js_signbit(y) ? 1 : 0);
    return 0;
}
NATIVE(ta_sort) {
    JSObject *o = this_ta(rt, this_, "sort");
    if (!o) return JS_EXC;
    jsval cmp = ARG(0);
    if (!is_undef(cmp) && !is_callable(cmp)) THROW_TYPE("The comparison function must be either a function or undefined");
    uint32_t n = ta_length(o);
    JSObject *vals = new_array(rt, n);
    push_tmp(rt, mkobj(vals));
    for (uint32_t i = 0; i < n; i++) ta_get(rt, o, i, &vals->u.arr.elems[i]);
    /* insertion-merge sort (stable) */
    jsval *v = vals->u.arr.elems;
    JSObject *tmpa = new_array(rt, n);
    push_tmp(rt, mkobj(tmpa));
    jsval *tv = tmpa->u.arr.elems;
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t s = 0; s < n; s += 2 * w) {
            uint32_t mid = s + w < n ? s + w : n, e = s + 2 * w < n ? s + 2 * w : n;
            uint32_t i = s, j = mid, k = s;
            while (i < mid && j < e) {
                int c;
                if (is_undef(cmp)) {
                    if (is_big(v[i])) c = big_cmp((JSBig *)vptr(v[j]), (JSBig *)vptr(v[i]));
                    else {
                        double a = vnum(v[j]), b = vnum(v[i]);
                        c = num_cmp(&a, &b);
                    }
                } else {
                    jsval a[2] = {v[j], v[i]};
                    jsval r = call_fn(rt, cmp, JS_UNDEF, 2, a);
                    TRY(r);
                    double d;
                    if (to_number(rt, r, &d) < 0) return JS_EXC;
                    c = d < 0 ? -1 : d > 0 ? 1 : 0;
                }
                if (c < 0)
                    tv[k++] = v[j++];
                else
                    tv[k++] = v[i++];
            }
            while (i < mid) tv[k++] = v[i++];
            while (j < e) tv[k++] = v[j++];
        }
        memcpy(v, tv, n * sizeof(jsval));
    }
    for (uint32_t i = 0; i < n; i++) ta_set(rt, o, i, v[i]);
    return this_;
}
NATIVE(ta_static_from_of) {
    int k = ta_kind_of_ctor(rt, this_);
    if (k < 0) THROW_TYPE("%S is not a constructor", val_brief(rt, this_));
    if (MAGIC == 1) { /* of */
        JSObject *o = make_typed_array(rt, k, (uint32_t)argc);
        if (!o) return JS_EXC;
        push_tmp(rt, mkobj(o));
        for (int i = 0; i < argc; i++)
            if (ta_set(rt, o, (uint64_t)i, argv[i]) < 0) return JS_EXC;
        return mkobj(o);
    }
    jsval src = ARG(0), mapfn = ARG(1);
    JSObject *arr = iter_to_array(rt, src);
    if (!arr) {
        if (!is_obj(src)) return JS_EXC;
        rt->has_exc = false;
        arr = vobj(src);
    }
    push_tmp(rt, mkobj(arr));
    uint64_t n;
    if (len_of(rt, mkobj(arr), &n) < 0) return JS_EXC;
    JSObject *o = make_typed_array(rt, k, (uint32_t)n);
    if (!o) return JS_EXC;
    push_tmp(rt, mkobj(o));
    for (uint32_t i = 0; i < n; i++) {
        jsval v = get_idx(rt, mkobj(arr), i);
        TRY(v);
        if (is_callable(mapfn)) {
            jsval a[2] = {v, mknum(i)};
            v = call_fn(rt, mapfn, ARG(2), 2, a);
            TRY(v);
        }
        if (ta_set(rt, o, i, v) < 0) return JS_EXC;
    }
    return mkobj(o);
}
NATIVE(ta_abstract_ctor) { THROW_TYPE("Abstract class TypedArray not directly constructable"); }

/* ---------------- DataView ---------------- */
NATIVE(dv_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor DataView requires 'new'");
    if (!is_cls(ARG(0), CLS_ARRAYBUFFER)) THROW_TYPE("First argument to DataView constructor must be an ArrayBuffer");
    ABuf *b = abuf(vobj(argv[0]));
    uint64_t off, len;
    if (to_index(rt, ARG(1), &off) < 0) return JS_EXC;
    if (off > b->len) return throw_error(rt, E_RANGE, "Start offset %S is outside the bounds of the buffer", num_to_str(rt, (double)off));
    if (is_undef(ARG(2)))
        len = b->len - off;
    else {
        if (to_index(rt, argv[2], &len) < 0) return JS_EXC;
        if (off + len > b->len) return throw_error(rt, E_RANGE, "Invalid DataView length %S", num_to_str(rt, (double)len));
    }
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_DataViewProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_DATAVIEW);
    TA *t = (TA *)calloc(1, sizeof(TA));
    t->buf = vobj(argv[0]);
    t->off = (uint32_t)off;
    t->len = (uint32_t)len;
    o->u.data.p = t;
    return mkobj(o);
}
NATIVE(dv_getter) {
    if (!is_cls(this_, CLS_DATAVIEW)) THROW_TYPE("Receiver is not a DataView");
    TA *t = tad(vobj(this_));
    switch (MAGIC) {
    case 0: return mknum(t->len);
    case 1: return mknum(t->off);
    default: return mkobj(t->buf);
    }
}
NATIVE(dv_access) {
    int kind = MAGIC & 15;
    bool set = (MAGIC & 16) != 0;
    if (!is_cls(this_, CLS_DATAVIEW)) THROW_TYPE("Receiver is not a DataView");
    TA *t = tad(vobj(this_));
    uint64_t idx;
    if (to_index(rt, ARG(0), &idx) < 0) return JS_EXC;
    jsval val = JS_UNDEF;
    double d = 0;
    jsval bv = JS_UNDEF;
    if (set) {
        val = ARG(1);
        if (kind == TA_BI64 || kind == TA_BU64) {
            bv = to_bigint(rt, val);
            TRY(bv);
        } else if (to_number(rt, val, &d) < 0)
            return JS_EXC;
    }
    bool little = truthy(set ? ARG(2) : ARG(1));
    uint32_t sz = ta_size[kind];
    if (idx + sz > t->len) return throw_error(rt, E_RANGE, "Offset is outside the bounds of the DataView");
    uint8_t *p = abuf(t->buf)->data + t->off + idx;
    uint8_t tmp[8];
    if (set) {
        if (kind == TA_BI64 || kind == TA_BU64) {
            uint64_t iv = big_to_u64((JSBig *)vptr(bv));
            memcpy(tmp, &iv, 8);
        } else
            ta_write_num(kind, tmp, d);
        for (uint32_t i = 0; i < sz; i++) p[i] = little ? tmp[i] : tmp[sz - 1 - i];
        return JS_UNDEF;
    }
    for (uint32_t i = 0; i < sz; i++) tmp[i] = little ? p[i] : p[sz - 1 - i];
    if (kind == TA_BI64 || kind == TA_BU64) {
        uint64_t iv;
        memcpy(&iv, tmp, 8);
        bool neg = kind == TA_BI64 && (int64_t)iv < 0;
        return big_from_u64(rt, neg ? (uint64_t)0 - iv : iv, neg);
    }
    return ta_read(rt, kind, tmp);
}

/* ---------------- structuredClone ---------------- */
static jsval clone_val(js_rt *rt, jsval v, JSObject *memo, int depth);
static jsval clone_val(js_rt *rt, jsval v, JSObject *memo, int depth) {
    if (!is_obj(v)) {
        if (is_sym(v)) return throw_error(rt, E_ERROR, "%S could not be cloned.", vstr(key_to_display(rt, v)));
        return v;
    }
    if (depth > 2000 || check_stack(rt)) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    bool found;
    jsval prev = weak_get(rt, memo, v, &found);
    if (found) return prev;
    JSObject *o = vobj(v);
    jsval *root = rt->sp;
    JSObject *r;
    switch (o->cls) {
    case CLS_ARRAY: r = new_array(rt, 0); break;
    case CLS_DATE:
    case CLS_BOOLEAN:
    case CLS_NUMBER:
    case CLS_STRING:
        r = new_object(rt, o->proto, o->cls);
        r->u.prim.v = o->u.prim.v;
        map_set_raw(rt, memo, v, mkobj(r));
        return mkobj(r);
    case CLS_REGEXP: {
        jsval fl = get_p(rt, v, ATOM(flags));
        TRY(fl);
        jsval re = regexp_create(rt, o->u.data.v, fl, rt->intr[I_RegExpProto]);
        TRY(re);
        map_set_raw(rt, memo, v, re);
        return re;
    }
    case CLS_MAP:
    case CLS_SET: {
        r = map_new(rt, o->cls == CLS_SET);
        map_set_raw(rt, memo, v, mkobj(r));
        push_tmp(rt, mkobj(r));
        MData *m = md(o);
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].del) continue;
            jsval k = clone_val(rt, m->e[i].k, memo, depth + 1);
            TRY(k);
            push_tmp(rt, k);
            jsval val = clone_val(rt, m->e[i].v, memo, depth + 1);
            TRY(val);
            map_set_raw(rt, r, k, val);
        }
        rt->sp = root;
        return mkobj(r);
    }
    case CLS_ARRAYBUFFER: {
        ABuf *b = abuf(o);
        r = new_abuf(rt, b->len, 0);
        if (!r) return JS_EXC;
        memcpy(abuf(r)->data, b->data, b->len);
        map_set_raw(rt, memo, v, mkobj(r));
        return mkobj(r);
    }
    case CLS_TYPEDARRAY: {
        TA *t = tad(o);
        uint32_t n = ta_length(o);
        r = make_typed_array(rt, t->kind, n);
        if (!r) return JS_EXC;
        memcpy(abuf(tad(r)->buf)->data, abuf(t->buf)->data + t->off, (size_t)n * ta_size[t->kind]);
        map_set_raw(rt, memo, v, mkobj(r));
        return mkobj(r);
    }
    case CLS_ERROR: {
        jsval nm = get_p(rt, v, ATOM(name));
        TRY(nm);
        int type = E_ERROR;
        static const char *const en[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "URIError", "EvalError"};
        if (is_str(nm))
            for (int i = 0; i < 7; i++)
                if (str_eq(rt, vstr(nm), str_cstr(rt, en[i]))) type = i;
        jsval msg = get_p(rt, v, ATOM(message));
        TRY(msg);
        JSString *ms = is_undef(msg) ? 0 : to_string(rt, msg);
        r = make_error(rt, type, ms);
        jsval st = get_p(rt, v, ATOM(stack));
        if (is_str(st)) def_prop(rt, r, ATOM(stack), st, P_W | P_C);
        map_set_raw(rt, memo, v, mkobj(r));
        return mkobj(r);
    }
    case CLS_OBJECT:
    case CLS_ARGUMENTS: r = new_plain(rt); break;
    default:
        if (o->oflags & OF_CALLABLE) return throw_error(rt, E_ERROR, "%S could not be cloned.", val_brief(rt, v));
        return throw_error(rt, E_ERROR, "#<Object> could not be cloned.");
    }
    map_set_raw(rt, memo, v, mkobj(r));
    push_tmp(rt, mkobj(r));
    JSObject *keys = own_keys(rt, o, KEYS_STR | KEYS_ENUM_ONLY);
    if (!keys) return JS_EXC;
    push_tmp(rt, mkobj(keys));
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jsval k = keys->u.arr.elems[i];
        jsval pv = get_p(rt, v, k);
        TRY(pv);
        jsval cv = clone_val(rt, pv, memo, depth + 1);
        TRY(cv);
        if (create_data_prop(rt, r, k, cv, true) < 0) return JS_EXC;
    }
    rt->sp = root;
    return mkobj(r);
}
NATIVE(structured_clone) {
    JSObject *memo = map_new(rt, false);
    push_tmp(rt, mkobj(memo));
    return clone_val(rt, ARG(0), memo, 0);
}

/* ---------------- GC dispatch for class data ---------------- */
void gc_trace_class(js_rt *rt, JSObject *o) {
    switch (o->cls) {
    case CLS_GENERATOR:
    case CLS_ASYNC:
    case CLS_ASYNC_GENERATOR: coro_trace(rt, o); break;
    case CLS_PROMISE: promise_trace(rt, o); break;
    case CLS_REGEXP: regexp_trace(rt, o); break;
    case CLS_MAP:
    case CLS_SET:
    case CLS_WEAKMAP:
    case CLS_WEAKSET: map_trace(rt, o); break;
    case CLS_ARRAYBUFFER:
    case CLS_TYPEDARRAY:
    case CLS_DATAVIEW: buffer_trace(rt, o); break;
    case CLS_FORIN_ITER:
        gc_mark_val(rt, o->u.it.a);
        gc_mark_val(rt, o->u.it.b);
        break;
    case CLS_FINREG: gc_mark_val(rt, o->u.data.v); break;
    default: break;
    }
}
void gc_finalize_class(js_rt *rt, JSObject *o) {
    (void)rt;
    switch (o->cls) {
    case CLS_GENERATOR:
    case CLS_ASYNC:
    case CLS_ASYNC_GENERATOR: coro_finalize(o); break;
    case CLS_PROMISE: promise_finalize(o); break;
    case CLS_REGEXP: regexp_finalize(o); break;
    case CLS_MAP:
    case CLS_SET:
    case CLS_WEAKMAP:
    case CLS_WEAKSET: map_finalize(o); break;
    case CLS_ARRAYBUFFER:
    case CLS_TYPEDARRAY:
    case CLS_DATAVIEW: buffer_finalize(o); break;
    default: break;
    }
}

NATIVE(coll_species) { return this_; }

void init_collections(js_rt *rt) {
    JSObject *op = rt->intr[I_ObjectProto];
    JSObject *mp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_MapProto] = mp;
    JSObject *mc = new_ctor(rt, "Map", map_ctor, 0, mp);
    def_getter_k(rt, mc, SYM(species), "get [Symbol.species]", coll_species, 0);
    {
        /* Map.groupBy shares Object.groupBy's implementation (magic 1) */
        Prop *og = find_own(rt, vobj(get_c(rt, mkobj(rt->global), "Object")), atom_c(rt, "groupBy"));
        if (og) {
            JSObject *f = new_native(rt, "groupBy", vobj(og->val)->u.nat.fn, 2, 1, false);
            def_value(rt, mc, "groupBy", mkobj(f));
        }
    }
    def_method(rt, mp, "get", map_get, 1, 0);
    def_method(rt, mp, "set", map_set, 2, 0);
    def_method(rt, mp, "has", coll_has, 1, 0);
    def_method(rt, mp, "delete", coll_delete, 1, 0);
    def_method(rt, mp, "clear", coll_clear, 0, 0);
    def_getter(rt, mp, "size", coll_size, 0);
    def_method(rt, mp, "forEach", coll_for_each, 1, 0);
    def_method(rt, mp, "keys", coll_iter, 0, 0);
    def_method(rt, mp, "values", coll_iter, 0, 1);
    JSObject *ment = def_method(rt, mp, "entries", coll_iter, 0, 2);
    def_prop(rt, mp, SYM(iterator), mkobj(ment), P_W | P_C);
    def_prop(rt, mp, SYM(toStringTag), mkstr(str_cstr(rt, "Map")), P_C);
    JSObject *sp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_SetProto] = sp;
    JSObject *sc = new_ctor(rt, "Set", set_ctor, 0, sp);
    def_getter_k(rt, sc, SYM(species), "get [Symbol.species]", coll_species, 0);
    def_method(rt, sp, "add", set_add, 1, 0);
    def_method(rt, sp, "has", coll_has, 1, 1);
    def_method(rt, sp, "delete", coll_delete, 1, 1);
    def_method(rt, sp, "clear", coll_clear, 0, 1);
    def_getter(rt, sp, "size", coll_size, 1);
    def_method(rt, sp, "forEach", coll_for_each, 1, 1);
    JSObject *svals = def_method(rt, sp, "values", coll_iter, 0, 4 | 1);
    def_value(rt, sp, "keys", mkobj(svals));
    def_method(rt, sp, "entries", coll_iter, 0, 4 | 2);
    def_prop(rt, sp, SYM(iterator), mkobj(svals), P_W | P_C);
    def_prop(rt, sp, SYM(toStringTag), mkstr(str_cstr(rt, "Set")), P_C);
    const char *sops[] = {"union", "intersection", "difference", "symmetricDifference", "isSubsetOf", "isSupersetOf", "isDisjointFrom"};
    for (int i = 0; i < 7; i++) def_method(rt, sp, sops[i], set_op, 1, i);
    JSObject *mip = new_object(rt, rt->intr[I_IteratorProto], CLS_OBJECT);
    rt->intr[I_MapIterProto] = mip;
    def_method(rt, mip, "next", coll_iter_next, 0, 0);
    def_prop(rt, mip, SYM(toStringTag), mkstr(str_cstr(rt, "Map Iterator")), P_C);
    JSObject *sip = new_object(rt, rt->intr[I_IteratorProto], CLS_OBJECT);
    rt->intr[I_SetIterProto] = sip;
    def_method(rt, sip, "next", coll_iter_next, 0, 0);
    def_prop(rt, sip, SYM(toStringTag), mkstr(str_cstr(rt, "Set Iterator")), P_C);
    JSObject *wmp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_WeakMapProto] = wmp;
    new_ctor(rt, "WeakMap", weakmap_ctor, 0, wmp);
    def_method(rt, wmp, "get", weak_op, 1, 0);
    def_method(rt, wmp, "set", weak_op, 2, 1);
    def_method(rt, wmp, "has", weak_op, 1, 2);
    def_method(rt, wmp, "delete", weak_op, 1, 3);
    def_prop(rt, wmp, SYM(toStringTag), mkstr(str_cstr(rt, "WeakMap")), P_C);
    JSObject *wsp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_WeakSetProto] = wsp;
    new_ctor(rt, "WeakSet", weakset_ctor, 0, wsp);
    def_method(rt, wsp, "add", weak_op, 1, 8 | 1);
    def_method(rt, wsp, "has", weak_op, 1, 8 | 2);
    def_method(rt, wsp, "delete", weak_op, 1, 8 | 3);
    def_prop(rt, wsp, SYM(toStringTag), mkstr(str_cstr(rt, "WeakSet")), P_C);
    JSObject *wrp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_WeakRefProto] = wrp;
    new_ctor(rt, "WeakRef", weakref_ctor, 1, wrp);
    def_method(rt, wrp, "deref", weakref_deref, 0, 0);
    def_prop(rt, wrp, SYM(toStringTag), mkstr(str_cstr(rt, "WeakRef")), P_C);
    JSObject *frp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_FinRegProto] = frp;
    new_ctor(rt, "FinalizationRegistry", finreg_ctor, 1, frp);
    def_method(rt, frp, "register", finreg_register, 2, 0);
    def_method(rt, frp, "unregister", finreg_register, 1, 1);
    def_prop(rt, frp, SYM(toStringTag), mkstr(str_cstr(rt, "FinalizationRegistry")), P_C);
    def_method(rt, rt->global, "structuredClone", structured_clone, 1, 0);
}

void init_typed(js_rt *rt) {
    JSObject *op = rt->intr[I_ObjectProto];
    JSObject *abp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_ArrayBufferProto] = abp;
    JSObject *abc = new_ctor(rt, "ArrayBuffer", abuf_ctor, 1, abp);
    def_method(rt, abc, "isView", abuf_is_view, 1, 0);
    def_getter_k(rt, abc, SYM(species), "get [Symbol.species]", coll_species, 0);
    def_getter(rt, abp, "byteLength", abuf_byte_length, 0);
    def_method(rt, abp, "slice", abuf_slice, 2, 0);
    def_prop(rt, abp, SYM(toStringTag), mkstr(str_cstr(rt, "ArrayBuffer")), P_C);
    /* %TypedArray% */
    JSObject *tap = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_TypedArrayProto] = tap;
    JSObject *tac = new_native(rt, "TypedArray", ta_abstract_ctor, 0, 0, true);
    materialize_fn(rt, tac);
    def_prop(rt, tac, ATOM(prototype), mkobj(tap), 0);
    def_prop(rt, tap, ATOM(constructor), mkobj(tac), P_W | P_C);
    rt->intr[I_TypedArrayCtor] = tac;
    def_method(rt, tac, "from", ta_static_from_of, 1, 0);
    def_method(rt, tac, "of", ta_static_from_of, 0, 1);
    def_getter_k(rt, tac, SYM(species), "get [Symbol.species]", coll_species, 0);
    def_getter(rt, tap, "length", ta_getter, 0);
    def_getter(rt, tap, "byteLength", ta_getter, 1);
    def_getter(rt, tap, "byteOffset", ta_getter, 2);
    def_getter(rt, tap, "buffer", ta_getter, 3);
    def_getter_k(rt, tap, SYM(toStringTag), "get [Symbol.toStringTag]", ta_to_string_tag, 0);
    def_method(rt, tap, "subarray", ta_subarray, 2, 0);
    def_method(rt, tap, "slice", ta_slice, 2, 0);
    def_method(rt, tap, "set", ta_set_n, 1, 0);
    def_method(rt, tap, "map", ta_map_filter, 1, 0);
    def_method(rt, tap, "filter", ta_map_filter, 1, 1);
    def_method(rt, tap, "sort", ta_sort, 1, 0);
    /* generic array-like methods are shared with Array.prototype */
    const char *shared[] = {"at", "copyWithin", "entries", "every", "fill", "find", "findIndex", "findLast", "findLastIndex",
                            "forEach", "includes", "indexOf", "join", "keys", "lastIndexOf", "reduce", "reduceRight",
                            "reverse", "some", "toLocaleString", "toString", "values"};
    for (unsigned i = 0; i < sizeof shared / sizeof shared[0]; i++) {
        Prop *p = find_own(rt, rt->intr[I_ArrayProto], atom_c(rt, shared[i]));
        if (p) def_value(rt, tap, shared[i], p->val);
    }
    def_prop(rt, tap, SYM(iterator), mkobj(rt->intr[I_ArrayValues]), P_W | P_C);
    for (int k = 0; k < TA__N; k++) {
        JSObject *p = new_object(rt, tap, CLS_OBJECT);
        JSObject *c = new_ctor(rt, ta_names[k], ta_ctor, 3, p);
        c->u.nat.data = (void *)(intptr_t)k;
        c->proto = tac;
        def_prop(rt, c, atom_c(rt, "BYTES_PER_ELEMENT"), mknum(ta_size[k]), 0);
        def_prop(rt, p, atom_c(rt, "BYTES_PER_ELEMENT"), mknum(ta_size[k]), 0);
        rt->intr[I_TA_FIRST + k * 2] = p;
        rt->intr[I_TA_FIRST + k * 2 + 1] = c;
    }
    JSObject *dvp = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_DataViewProto] = dvp;
    new_ctor(rt, "DataView", dv_ctor, 1, dvp);
    def_getter(rt, dvp, "byteLength", dv_getter, 0);
    def_getter(rt, dvp, "byteOffset", dv_getter, 1);
    def_getter(rt, dvp, "buffer", dv_getter, 2);
    def_prop(rt, dvp, SYM(toStringTag), mkstr(str_cstr(rt, "DataView")), P_C);
    static const struct {
        const char *n;
        int k;
    } dv[] = {{"Int8", TA_I8}, {"Uint8", TA_U8}, {"Int16", TA_I16}, {"Uint16", TA_U16}, {"Int32", TA_I32}, {"Uint32", TA_U32},
              {"Float32", TA_F32}, {"Float64", TA_F64}, {"BigInt64", TA_BI64}, {"BigUint64", TA_BU64}};
    for (int i = 0; i < 10; i++) {
        char nm[32];
        size_t l = strlen(dv[i].n);
        memcpy(nm, "get", 3);
        memcpy(nm + 3, dv[i].n, l + 1);
        def_method(rt, dvp, nm, dv_access, 1, dv[i].k);
        memcpy(nm, "set", 3);
        def_method(rt, dvp, nm, dv_access, 2, dv[i].k | 16);
    }
}
