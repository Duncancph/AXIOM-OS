/* AXIOM JavaScript engine: objects, properties, arrays, type conversions, errors. */
#include "js_int.h"

/* ---------------- object allocation ---------------- */
JSObject *new_object(js_rt *rt, JSObject *proto, int cls) {
    JSObject *o = (JSObject *)gc_alloc(rt, sizeof(JSObject), GC_OBJ);
    if (!o) return 0;
    o->cls = (uint8_t)cls;
    o->oflags = OF_EXTENSIBLE;
    o->proto = proto;
    switch (cls) {
    case CLS_ARRAY: case CLS_ARGUMENTS: case CLS_STRING: case CLS_TYPEDARRAY: case CLS_HOST: case CLS_PROXY:
        o->oflags |= OF_EXOTIC;
        break;
    default: break;
    }
    if (cls == CLS_ARRAY || cls == CLS_ARGUMENTS) {
        o->u.arr.elems = 0;
        o->u.arr.len = o->u.arr.cap = 0;
    }
    return o;
}
JSObject *new_plain(js_rt *rt) { return new_object(rt, rt->intr[I_ObjectProto], CLS_OBJECT); }

bool array_reserve(js_rt *rt, JSObject *a, uint32_t n) {
    if (n <= a->u.arr.cap) return true;
    uint32_t nc = a->u.arr.cap ? a->u.arr.cap : 4;
    while (nc < n) nc = nc > 0x40000000 ? n : nc * 2;
    jsval *e = (jsval *)realloc(a->u.arr.elems, (size_t)nc * sizeof(jsval));
    if (!e) return false;
    rt->bytes += (size_t)(nc - a->u.arr.cap) * 8;
    if (rt->bytes > rt->threshold) rt->gc_pending = true;
    a->u.arr.elems = e;
    a->u.arr.cap = nc;
    return true;
}
JSObject *new_array(js_rt *rt, uint32_t len) {
    JSObject *a = new_object(rt, rt->intr[I_ArrayProto], CLS_ARRAY);
    if (len) {
        if (len > 100000000 || !array_reserve(rt, a, len)) {
            a->u.arr.len = 0;
            array_make_slow(rt, a);
            a->u.arr.len = len;
            return a;
        }
        for (uint32_t i = 0; i < len; i++) a->u.arr.elems[i] = JS_HOLE;
        a->u.arr.len = len;
    }
    return a;
}
JSObject *new_array_from(js_rt *rt, const jsval *v, uint32_t n) {
    JSObject *a = new_object(rt, rt->intr[I_ArrayProto], CLS_ARRAY);
    if (n && array_reserve(rt, a, n)) {
        memcpy(a->u.arr.elems, v, (size_t)n * sizeof(jsval));
        a->u.arr.len = n;
    }
    return a;
}
int array_push(js_rt *rt, JSObject *a, jsval v) {
    if (!(a->oflags & OF_SLOW_ARRAY) && (a->oflags & OF_EXTENSIBLE) && !(a->oflags & OF_LEN_RO)) {
        if (a->u.arr.len >= a->u.arr.cap && !array_reserve(rt, a, a->u.arr.len + 1)) return -1;
        a->u.arr.elems[a->u.arr.len++] = v;
        return 0;
    }
    return set_idx(rt, mkobj(a), a->u.arr.len, v, true) < 0 ? -1 : 0;
}

/* ---------------- property table ---------------- */
uint32_t key_hash(jsval key) {
    if (is_str(key)) return vstr(key)->hash;
    if (is_sym(key)) return vsym(key)->hash;
    return 0;
}

static void rebuild_hidx(js_rt *rt, JSObject *o) {
    uint32_t cap = 16;
    while (cap < o->nprops * 2 + 2) cap *= 2;
    uint32_t *h = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!h) return;
    free(o->hidx);
    o->hidx = h;
    o->hmask = cap - 1;
    for (uint32_t i = 0; i < o->nprops; i++) {
        jsval k = o->props[i].key;
        if (k == JS_HOLE) continue;
        uint32_t j = key_hash(k) & o->hmask;
        while (h[j]) j = (j + 1) & o->hmask;
        h[j] = i + 1;
    }
}

static inline Prop *find_raw(JSObject *o, jsval key) {
    if (!o->hidx) {
        Prop *p = o->props, *e = p + o->nprops;
        for (; p < e; p++)
            if (p->key == key) return p;
        return 0;
    }
    uint32_t j = key_hash(key) & o->hmask;
    for (;;) {
        uint32_t ix = o->hidx[j];
        if (!ix) return 0;
        if (o->props[ix - 1].key == key) return &o->props[ix - 1];
        j = (j + 1) & o->hmask;
    }
}

Prop *find_own(js_rt *rt, JSObject *o, jsval key) {
    if (UNLIKELY(o->oflags & OF_LAZY) &&
        (key == ATOM(length) || key == ATOM(name) || key == ATOM(prototype) || key == ATOM(arguments) ||
         key == ATOM(caller)))
        materialize_fn(rt, o);
    return find_raw(o, key);
}

Prop *add_prop(js_rt *rt, JSObject *o, jsval key, jsval val, uint32_t flags) {
    if (o->nprops == o->cap) {
        uint32_t nc = o->cap ? o->cap * 2 : 4;
        Prop *np = (Prop *)realloc(o->props, (size_t)nc * sizeof(Prop));
        if (!np) return 0;
        o->props = np;
        rt->bytes += (size_t)(nc - o->cap) * sizeof(Prop);
        o->cap = nc;
    }
    Prop *p = &o->props[o->nprops++];
    p->key = key;
    p->val = val;
    p->flags = flags;
    p->_pad = 0;
    if (is_str(key) && key_index(rt, key) >= 0) o->oflags |= OF_HASIDX;
    if (o->hidx) {
        if (o->nprops * 2 > o->hmask)
            rebuild_hidx(rt, o);
        else {
            uint32_t j = key_hash(key) & o->hmask;
            while (o->hidx[j]) j = (j + 1) & o->hmask;
            o->hidx[j] = o->nprops;
        }
    } else if (o->nprops > 8)
        rebuild_hidx(rt, o);
    return p;
}

void del_prop_at(js_rt *rt, JSObject *o, Prop *p) {
    p->key = JS_HOLE;
    p->val = JS_UNDEF;
    p->flags = 0;
    o->ndel++;
    if (p == &o->props[o->nprops - 1] && !o->hidx) {
        o->nprops--;
        o->ndel--;
        return;
    }
    if (o->ndel > 8 && o->ndel * 2 > o->nprops) {
        uint32_t k = 0;
        for (uint32_t i = 0; i < o->nprops; i++)
            if (o->props[i].key != JS_HOLE) o->props[k++] = o->props[i];
        o->nprops = k;
        o->ndel = 0;
        if (o->hidx) {
            if (k > 8)
                rebuild_hidx(rt, o);
            else {
                free(o->hidx);
                o->hidx = 0;
            }
        }
    }
}

jsval def_prop(js_rt *rt, JSObject *o, jsval key, jsval val, uint32_t flags) {
    Prop *p = find_own(rt, o, key);
    if (p) {
        p->val = val;
        p->flags = flags;
    } else
        add_prop(rt, o, key, val, flags);
    return val;
}
void def_acc(js_rt *rt, JSObject *o, jsval key, jsval get, jsval set, uint32_t flags) {
    Accessor *a = (Accessor *)gc_alloc(rt, sizeof(Accessor), GC_ACCESSOR);
    a->get = get;
    a->set = set;
    def_prop(rt, o, key, mkptr(a), (flags & (P_E | P_C)) | P_ACC);
}
void def_value(js_rt *rt, JSObject *o, const char *name, jsval v) { def_prop(rt, o, atom_c(rt, name), v, P_W | P_C); }

JSObject *new_native_k(js_rt *rt, jsval name, js_native fn, int length, void *data, bool ctor) {
    JSObject *f = new_object(rt, rt->intr[I_FunctionProto], CLS_NATIVE);
    f->oflags |= OF_CALLABLE | OF_LAZY | (ctor ? OF_CTOR : 0);
    f->u.nat.fn = fn;
    f->u.nat.data = data;
    f->u.nat.name = name;
    f->u.nat.length = length;
    f->u.nat.api = 0;
    return f;
}
JSObject *new_native(js_rt *rt, const char *name, js_native fn, int length, intptr_t magic, bool ctor) {
    return new_native_k(rt, atom_c(rt, name), fn, length, (void *)magic, ctor);
}
JSObject *def_method_k(js_rt *rt, JSObject *o, jsval key, const char *fname, js_native fn, int length, intptr_t magic) {
    JSObject *f = new_native_k(rt, atom_c(rt, fname), fn, length, (void *)magic, false);
    def_prop(rt, o, key, mkobj(f), P_W | P_C);
    return f;
}
JSObject *def_method(js_rt *rt, JSObject *o, const char *name, js_native fn, int length, intptr_t magic) {
    jsval k = atom_c(rt, name);
    JSObject *f = new_native_k(rt, k, fn, length, (void *)magic, false);
    def_prop(rt, o, k, mkobj(f), P_W | P_C);
    return f;
}
void def_getter_k(js_rt *rt, JSObject *o, jsval key, const char *fname, js_native fn, intptr_t magic) {
    JSObject *f = new_native_k(rt, atom_c(rt, fname), fn, 0, (void *)magic, false);
    def_acc(rt, o, key, mkobj(f), JS_UNDEF, P_C);
}
void def_getter(js_rt *rt, JSObject *o, const char *name, js_native fn, intptr_t magic) {
    char buf[64];
    size_t n = strlen(name);
    memcpy(buf, "get ", 4);
    if (n > 58) n = 58;
    memcpy(buf + 4, name, n);
    buf[4 + n] = 0;
    def_getter_k(rt, o, atom_c(rt, name), buf, fn, magic);
}
JSObject *new_ctor(js_rt *rt, const char *name, js_native fn, int length, JSObject *proto) {
    JSObject *f = new_native(rt, name, fn, length, 0, true);
    materialize_fn(rt, f);
    if (proto) {
        def_prop(rt, f, ATOM(prototype), mkobj(proto), 0);
        def_prop(rt, proto, ATOM(constructor), mkobj(f), P_W | P_C);
    }
    def_prop(rt, rt->global, atom_c(rt, name), mkobj(f), P_W | P_C);
    return f;
}

void materialize_fn(js_rt *rt, JSObject *o) {
    if (!(o->oflags & OF_LAZY)) return;
    o->oflags &= (uint16_t)~OF_LAZY;
    if (o->cls == CLS_NATIVE) {
        add_prop(rt, o, ATOM(length), mknum(o->u.nat.length), P_C);
        add_prop(rt, o, ATOM(name), is_undef(o->u.nat.name) ? ATOM(empty) : o->u.nat.name, P_C);
        if (o->u.nat.api == 1) {
            JSObject *p = new_plain(rt);
            add_prop(rt, p, ATOM(constructor), mkobj(o), P_W | P_C);
            add_prop(rt, o, ATOM(prototype), mkobj(p), P_W);
        }
        return;
    }
    if (o->cls != CLS_FUNCTION) return;
    FuncProto *fp = o->u.fn.fp;
    add_prop(rt, o, ATOM(length), mknum(fp->length), P_C);
    add_prop(rt, o, ATOM(name), is_undef(fp->name) ? ATOM(empty) : fp->name, P_C);
    uint32_t f = fp->flags;
    if (!(f & (FP_STRICT | FP_ARROW | FP_METHOD | FP_GENERATOR | FP_ASYNC | FP_GETSET | FP_CLASS_CTOR))) {
        add_prop(rt, o, ATOM(arguments), JS_NULL, 0);
        add_prop(rt, o, ATOM(caller), JS_NULL, 0);
    }
    if (f & FP_GENERATOR) {
        JSObject *p = new_object(rt, rt->intr[(f & FP_ASYNC) ? I_AsyncGeneratorProto : I_GeneratorProto], CLS_OBJECT);
        add_prop(rt, o, ATOM(prototype), mkobj(p), P_W);
    } else if (!(f & (FP_ARROW | FP_METHOD | FP_ASYNC | FP_GETSET)) && !(f & FP_CLASS_CTOR)) {
        JSObject *p = new_plain(rt);
        add_prop(rt, p, ATOM(constructor), mkobj(o), P_W | P_C);
        add_prop(rt, o, ATOM(prototype), mkobj(p), P_W);
    }
}

/* ---------------- arrays ---------------- */
void array_make_slow(js_rt *rt, JSObject *a) {
    if (a->oflags & OF_SLOW_ARRAY) return;
    uint32_t n = a->u.arr.len;
    jsval *el = a->u.arr.elems;
    uint32_t fl = P_DEFAULT;
    if (a->oflags & OF_FROZEN_EL) fl = P_E;
    else if (a->oflags & OF_SEALED_EL) fl = P_E | P_W;
    /* index properties must come before others in key order: we insert them now; own_keys sorts anyway */
    for (uint32_t i = 0; i < n && el; i++) {
        if (el[i] == JS_HOLE) continue;
        JSString *ks = num_to_str(rt, i);
        add_prop(rt, a, atomize(rt, ks), el[i], fl);
    }
    free(el);
    a->u.arr.elems = 0;
    a->u.arr.cap = 0;
    a->oflags |= OF_SLOW_ARRAY;
}

bool array_set_length(js_rt *rt, JSObject *a, uint32_t len) {
    if (a->oflags & OF_LEN_RO) return len == a->u.arr.len;
    uint32_t old = a->u.arr.len;
    if (!(a->oflags & OF_SLOW_ARRAY)) {
        if (len < old) {
            if (a->oflags & (OF_SEALED_EL | OF_FROZEN_EL)) {
                for (uint32_t i = len; i < old; i++)
                    if (a->u.arr.elems[i] != JS_HOLE) return false;
            }
            a->u.arr.len = len;
            if (a->u.arr.cap > 64 && len < a->u.arr.cap / 4) {
                jsval *e = (jsval *)realloc(a->u.arr.elems, (size_t)(len ? len : 1) * sizeof(jsval));
                if (e) {
                    a->u.arr.elems = e;
                    a->u.arr.cap = len ? len : 1;
                }
            }
            return true;
        }
        if (len > old) {
            if (len - old > 1000000 || !array_reserve(rt, a, len)) {
                array_make_slow(rt, a);
                a->u.arr.len = len;
                return true;
            }
            for (uint32_t i = old; i < len; i++) a->u.arr.elems[i] = JS_HOLE;
            a->u.arr.len = len;
        }
        return true;
    }
    if (len < old) {
        /* delete index props >= len */
        for (uint32_t i = 0; i < a->nprops; i++) {
            Prop *p = &a->props[i];
            if (p->key == JS_HOLE) continue;
            int64_t ix = key_index(rt, p->key);
            if (ix >= (int64_t)len) {
                if (!(p->flags & P_C)) {
                    a->u.arr.len = (uint32_t)ix + 1;
                    return false;
                }
                del_prop_at(rt, a, p);
                i = (uint32_t)-1; /* table may have been compacted: restart */
            }
        }
    }
    a->u.arr.len = len;
    return true;
}

/* ---------------- host keys ---------------- */
static bool host_key_cstr(js_rt *rt, jsval key, char **out, bool *alloc) {
    if (!is_str(key)) return false;
    JSString *s = vstr(key);
    str_flat(rt, s);
    if (str_is_ascii(rt, s)) {
        *out = (char *)s->s8;
        *alloc = false;
        return true;
    }
    *out = str_to_utf8(rt, s, 0);
    *alloc = true;
    return *out != 0;
}

/* own property lookup for any non-proxy object.
 * returns 0 not found, 1 value in *out, 2 accessor in *acc */
static int lookup_own(js_rt *rt, JSObject *o, jsval key, jsval *out, Accessor **acc, bool *stop) {
    if (o->oflags & OF_EXOTIC) {
        switch (o->cls) {
        case CLS_ARRAY:
            if (key == ATOM(length)) {
                *out = mknum(o->u.arr.len);
                return 1;
            }
            if (!(o->oflags & OF_SLOW_ARRAY)) {
                int64_t ix = key_index(rt, key);
                if (ix >= 0) {
                    if (ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                        *out = o->u.arr.elems[ix];
                        return 1;
                    }
                    return 0;
                }
            }
            break;
        case CLS_ARGUMENTS: {
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                uint16_t *pm = o->u.arr.pmap;
                if (pm && ix < pm[0] && pm[1 + ix] != 0xFFFF)
                    *out = o->u.arr.menv->slots[pm[1 + ix]];
                else
                    *out = o->u.arr.elems[ix];
                return 1;
            }
            break;
        }
        case CLS_STRING: {
            JSString *s = vstr(o->u.prim.v);
            if (key == ATOM(length)) {
                *out = mknum(s->len);
                return 1;
            }
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < s->len) {
                str_flat(rt, s);
                *out = mkstr(str_char(rt, str_at(s, (uint32_t)ix)));
                return 1;
            }
            break;
        }
        case CLS_TYPEDARRAY: {
            int64_t ix = key_index(rt, key);
            if (ix >= 0) {
                if (!ta_get(rt, o, (uint64_t)ix, out)) *out = JS_UNDEF;
                if (stop) *stop = true;
                return 1;
            }
            break;
        }
        case CLS_HOST:
            if (o->u.host.cls && o->u.host.cls->get) {
                char *k;
                bool al;
                if (host_key_cstr(rt, key, &k, &al)) {
                    jsval v = JS_UNDEF;
                    bool h = o->u.host.cls->get(rt, mkobj(o), o->u.host.ptr, k, &v);
                    if (al) free(k);
                    if (rt->has_exc) {
                        *out = JS_EXC;
                        return 1;
                    }
                    if (h) {
                        *out = v;
                        return 1;
                    }
                }
            }
            break;
        default: break;
        }
    }
    Prop *p = find_own(rt, o, key);
    if (!p) return 0;
    if (p->flags & P_ACC) {
        *acc = (Accessor *)vptr(p->val);
        return 2;
    }
    *out = p->val;
    return 1;
}

static JSObject *proto_of_prim(js_rt *rt, jsval v) {
    if (is_str(v)) return rt->intr[I_StringProto];
    if (is_num(v)) return rt->intr[I_NumberProto];
    if (is_boolv(v)) return rt->intr[I_BooleanProto];
    if (is_sym(v)) return rt->intr[I_SymbolProto];
    if (is_big(v)) return rt->intr[I_BigIntProto];
    return 0;
}

jsval get_prop(js_rt *rt, jsval obj, jsval key, jsval receiver) {
    JSObject *o;
    if (LIKELY(is_obj(obj)))
        o = vobj(obj);
    else {
        if (is_str(obj)) {
            JSString *s = vstr(obj);
            if (key == ATOM(length)) return mknum(s->len);
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < s->len) {
                str_flat(rt, s);
                return mkstr(str_char(rt, str_at(s, (uint32_t)ix)));
            }
        }
        o = proto_of_prim(rt, obj);
        if (!o) {
            jsval kd = key_to_display(rt, key);
            return throw_error(rt, E_TYPE, "Cannot read properties of %s (reading '%S')", is_null(obj) ? "null" : "undefined",
                               vstr(kd));
        }
    }
    int depth = 0;
    while (o) {
        if (UNLIKELY(o->cls == CLS_PROXY)) return proxy_get(rt, o, key, receiver);
        jsval v;
        Accessor *a;
        int r = lookup_own(rt, o, key, &v, &a, 0);
        if (r == 1) return v;
        if (r == 2) {
            if (is_undef(a->get)) return JS_UNDEF;
            return call_fn(rt, a->get, receiver, 0, 0);
        }
        o = o->proto;
        if (++depth > 100000) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    return JS_UNDEF;
}

jsval get_c(js_rt *rt, jsval obj, const char *key) { return get_p(rt, obj, atom_c(rt, key)); }

jsval get_idx(js_rt *rt, jsval obj, uint32_t i) {
    if (is_obj(obj)) {
        JSObject *o = vobj(obj);
        if ((o->cls == CLS_ARRAY && !(o->oflags & OF_SLOW_ARRAY)) || (o->cls == CLS_ARGUMENTS && !o->u.arr.pmap)) {
            if (i < o->u.arr.len && o->u.arr.elems[i] != JS_HOLE) return o->u.arr.elems[i];
        }
    } else if (is_str(obj)) {
        JSString *s = vstr(obj);
        if (i < s->len) {
            str_flat(rt, s);
            return mkstr(str_char(rt, str_at(s, i)));
        }
    }
    jsval k = atomize(rt, num_to_str(rt, i));
    return get_p(rt, obj, k);
}

jsval to_key(js_rt *rt, jsval v) {
    if (is_str(v)) return atomize(rt, vstr(v));
    if (is_sym(v)) return v;
    if (is_num(v)) return atomize(rt, num_to_str(rt, vnum(v)));
    if (is_obj(v)) {
        v = to_primitive(rt, v, 2);
        if (v == JS_EXC) return JS_EXC;
        if (is_sym(v)) return v;
    }
    JSString *s = to_string(rt, v);
    if (!s) return JS_EXC;
    return atomize(rt, s);
}

jsval get_v(js_rt *rt, jsval obj, jsval key) {
    if (is_num(key)) {
        double d = vnum(key);
        if (d >= 0 && d < 4294967295.0 && d == (uint32_t)d) return get_idx(rt, obj, (uint32_t)d);
    }
    if (is_nullish(obj)) {
        jsval k = to_key(rt, key);
        if (k == JS_EXC) return JS_EXC;
        return get_p(rt, obj, k);
    }
    jsval *root = push_tmp(rt, obj);
    jsval k = to_key(rt, key);
    rt->sp = root;
    if (k == JS_EXC) return JS_EXC;
    return get_p(rt, obj, k);
}

/* ---------------- [[Set]] ---------------- */
static bool strstr_js(const char *h, const char *n) {
    size_t ln = strlen(n);
    for (; *h; h++)
        if (!strncmp(h, n, ln)) return true;
    return false;
}
static int fail_set_o(js_rt *rt, bool th, jsval key, const char *why, jsval obj) {
    if (!th) return 0;
    jsval kd = key_to_display(rt, key);
    JSString *od = is_obj(obj) ? val_brief(rt, obj) : 0;
    if (od && strstr_js(why, "of object"))
        throw_error(rt, E_TYPE, "Cannot assign to read only property '%S' of %s '%S'", vstr(kd), (vobj(obj)->oflags & OF_CALLABLE) ? "function" : "object", od);
    else
        throw_error(rt, E_TYPE, why, vstr(kd));
    return -1;
}
#define fail_set(rt, th, key, why) fail_set_o(rt, th, key, why, receiver)

static bool chain_has_idx(JSObject *o) {
    for (; o; o = o->proto)
        if (o->oflags & (OF_HASIDX | OF_EXOTIC)) return true;
    return false;
}

/* define/overwrite a data property on receiver per OrdinarySet step 2 */
static int set_on_receiver(js_rt *rt, jsval receiver, jsval key, jsval val, bool th) {
    jsval obj = receiver;
    (void)obj;
    if (!is_obj(receiver)) {
        if (!th) return 0;
        jsval kd = key_to_display(rt, key);
        throw_error(rt, E_TYPE, "Cannot create property '%S' on %s", vstr(kd), type_name(receiver));
        return -1;
    }
    JSObject *r = vobj(receiver);
    PropDesc d;
    int e = get_own_desc(rt, r, key, &d);
    if (e < 0) return -1;
    if (e) {
        if (d.has & (HAS_GET | HAS_SET)) return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
        if (!(d.flags & P_W)) return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
        PropDesc nd;
        memset(&nd, 0, sizeof nd);
        nd.value = val;
        nd.has = HAS_VALUE;
        return define_own(rt, r, key, &nd, th);
    }
    return create_data_prop(rt, r, key, val, th);
}

int set_prop(js_rt *rt, jsval obj, jsval key, jsval val, jsval receiver, bool th) {
    JSObject *o;
    if (!is_obj(obj)) {
        if (is_nullish(obj)) {
            jsval kd = key_to_display(rt, key);
            throw_error(rt, E_TYPE, "Cannot set properties of %s (setting '%S')", is_null(obj) ? "null" : "undefined", vstr(kd));
            return -1;
        }
        o = proto_of_prim(rt, obj);
        if (is_str(obj) && (key == ATOM(length) || (key_index(rt, key) >= 0 && key_index(rt, key) < vstr(obj)->len)))
            return fail_set(rt, th, key, "Cannot assign to read only property '%S' of string");
    } else {
        o = vobj(obj);
        /* fast path: own writable data property on a plain object */
        if (receiver == obj && !(o->oflags & OF_EXOTIC)) {
            Prop *p = find_own(rt, o, key);
            if (p && (p->flags & (P_W | P_ACC)) == P_W) {
                p->val = val;
                return 1;
            }
        }
    }
    int depth = 0;
    while (o) {
        if (o->cls == CLS_PROXY) return proxy_set(rt, o, key, val, receiver) < 0 ? -1 : 1;
        if (o->oflags & OF_EXOTIC) {
            if (o->cls == CLS_ARRAY && mkobj(o) == receiver) {
                if (key == ATOM(length)) {
                    double d;
                    if (to_number(rt, val, &d) < 0) return -1;
                    uint32_t nl = to_uint32(d);
                    if ((double)nl != d) {
                        throw_error(rt, E_RANGE, "Invalid array length");
                        return -1;
                    }
                    if (!array_set_length(rt, o, nl)) return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
                    return 1;
                }
                int64_t ix = key_index(rt, key);
                if (ix >= 0 && !(o->oflags & OF_SLOW_ARRAY)) {
                    uint32_t i = (uint32_t)ix;
                    if (i < o->u.arr.len && o->u.arr.elems[i] != JS_HOLE) {
                        if (o->oflags & OF_FROZEN_EL) return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
                        o->u.arr.elems[i] = val;
                        return 1;
                    }
                    if (!chain_has_idx(o->proto)) {
                        if (!(o->oflags & OF_EXTENSIBLE)) return fail_set(rt, th, key, "Cannot add property %S, object is not extensible");
                        if (i >= o->u.arr.len && (o->oflags & OF_LEN_RO)) return fail_set(rt, th, key, "Cannot add property %S, object is not extensible");
                        if (i < o->u.arr.len) {
                            o->u.arr.elems[i] = val;
                            return 1;
                        }
                        if (i <= o->u.arr.len + 1024 || i < o->u.arr.cap * 2) {
                            if (!array_reserve(rt, o, i + 1)) return -1;
                            for (uint32_t k = o->u.arr.len; k < i; k++) o->u.arr.elems[k] = JS_HOLE;
                            o->u.arr.elems[i] = val;
                            o->u.arr.len = i + 1;
                            return 1;
                        }
                        array_make_slow(rt, o);
                    }
                }
            } else if (o->cls == CLS_ARGUMENTS && mkobj(o) == receiver) {
                int64_t ix = key_index(rt, key);
                if (ix >= 0 && ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                    o->u.arr.elems[ix] = val;
                    uint16_t *pm = o->u.arr.pmap;
                    if (pm && ix < pm[0] && pm[1 + ix] != 0xFFFF) o->u.arr.menv->slots[pm[1 + ix]] = val;
                    return 1;
                }
            } else if (o->cls == CLS_TYPEDARRAY) {
                int64_t ix = key_index(rt, key);
                if (ix >= 0) {
                    if (mkobj(o) == receiver) return ta_set(rt, o, (uint64_t)ix, val) < 0 ? -1 : 1;
                    if ((uint64_t)ix >= ta_length(o)) return 1;
                }
            } else if (o->cls == CLS_HOST && mkobj(o) == receiver && o->u.host.cls && o->u.host.cls->set) {
                char *k;
                bool al;
                if (host_key_cstr(rt, key, &k, &al)) {
                    bool h = o->u.host.cls->set(rt, mkobj(o), o->u.host.ptr, k, val);
                    if (al) free(k);
                    if (rt->has_exc) return -1;
                    if (h) return 1;
                }
            }
        }
        jsval v;
        Accessor *a;
        bool stop = false;
        int r;
        if (o->cls == CLS_HOST) {
            Prop *p = find_own(rt, o, key);
            if (!p) r = 0;
            else if (p->flags & P_ACC) {
                a = (Accessor *)vptr(p->val);
                r = 2;
            } else {
                v = p->val;
                r = 1;
            }
        } else
            r = lookup_own(rt, o, key, &v, &a, &stop);
        if (r == 2) {
            if (is_undef(a->set)) return fail_set(rt, th, key, "Cannot set property %S of #<Object> which has only a getter");
            jsval res = call_fn(rt, a->set, receiver, 1, &val);
            return res == JS_EXC ? -1 : 1;
        }
        if (r == 1) {
            if (v == JS_EXC) return -1;
            /* data property found: check writability */
            if (o->cls == CLS_STRING || (o->cls == CLS_ARRAY && key == ATOM(length) && (o->oflags & OF_LEN_RO)))
                return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
            if (o->cls == CLS_TYPEDARRAY && stop) return 1;
            Prop *p = (o->cls == CLS_ARRAY && key_index(rt, key) >= 0 && !(o->oflags & OF_SLOW_ARRAY)) ? 0 : find_own(rt, o, key);
            if (p && !(p->flags & P_W)) return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
            if (!p && o->cls == CLS_ARRAY && (o->oflags & OF_FROZEN_EL))
                return fail_set(rt, th, key, "Cannot assign to read only property '%S' of object");
            if (mkobj(o) == receiver && p) {
                p->val = val;
                return 1;
            }
            return set_on_receiver(rt, receiver, key, val, th);
        }
        o = o->proto;
        if (++depth > 100000) {
            throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
            return -1;
        }
    }
    return set_on_receiver(rt, receiver, key, val, th);
}

int set_v(js_rt *rt, jsval obj, jsval key, jsval val, bool th) {
    if (is_num(key)) {
        double d = vnum(key);
        if (d >= 0 && d < 4294967295.0 && d == (uint32_t)d) return set_idx(rt, obj, (uint32_t)d, val, th);
    }
    jsval *root = push_tmp(rt, obj);
    push_tmp(rt, val);
    jsval k = to_key(rt, key);
    rt->sp = root;
    if (k == JS_EXC) return -1;
    return set_p(rt, obj, k, val, th);
}

int set_idx(js_rt *rt, jsval obj, uint32_t i, jsval val, bool th) {
    if (is_obj(obj)) {
        JSObject *o = vobj(obj);
        if (o->cls == CLS_ARRAY && !(o->oflags & (OF_SLOW_ARRAY | OF_FROZEN_EL))) {
            if (i < o->u.arr.len && o->u.arr.elems[i] != JS_HOLE) {
                o->u.arr.elems[i] = val;
                return 1;
            }
            if (i == o->u.arr.len && (o->oflags & OF_EXTENSIBLE) && !(o->oflags & OF_LEN_RO) && !chain_has_idx(o->proto)) {
                if (!array_reserve(rt, o, i + 1)) return -1;
                o->u.arr.elems[o->u.arr.len++] = val;
                return 1;
            }
        }
    }
    jsval k = atomize(rt, num_to_str(rt, i));
    return set_p(rt, obj, k, val, th);
}

/* ---------------- own property descriptors ---------------- */
int get_own_desc(js_rt *rt, JSObject *o, jsval key, PropDesc *d) {
    memset(d, 0, sizeof *d);
    if (o->cls == CLS_PROXY) return proxy_get_own_desc(rt, o, key, d);
    if (o->oflags & OF_EXOTIC) {
        switch (o->cls) {
        case CLS_ARRAY:
            if (key == ATOM(length)) {
                d->value = mknum(o->u.arr.len);
                d->flags = (o->oflags & OF_LEN_RO) ? 0 : P_W;
                d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                return 1;
            }
            if (!(o->oflags & OF_SLOW_ARRAY)) {
                int64_t ix = key_index(rt, key);
                if (ix >= 0) {
                    if (ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                        d->value = o->u.arr.elems[ix];
                        d->flags = (o->oflags & OF_FROZEN_EL) ? P_E : (o->oflags & OF_SEALED_EL) ? P_E | P_W : P_DEFAULT;
                        d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                        return 1;
                    }
                    return 0;
                }
            }
            break;
        case CLS_ARGUMENTS: {
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                uint16_t *pm = o->u.arr.pmap;
                d->value = (pm && ix < pm[0] && pm[1 + ix] != 0xFFFF) ? o->u.arr.menv->slots[pm[1 + ix]] : o->u.arr.elems[ix];
                d->flags = P_DEFAULT;
                d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                return 1;
            }
            break;
        }
        case CLS_STRING: {
            JSString *s = vstr(o->u.prim.v);
            if (key == ATOM(length)) {
                d->value = mknum(s->len);
                d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                return 1;
            }
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < s->len) {
                str_flat(rt, s);
                d->value = mkstr(str_char(rt, str_at(s, (uint32_t)ix)));
                d->flags = P_E;
                d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                return 1;
            }
            break;
        }
        case CLS_TYPEDARRAY: {
            int64_t ix = key_index(rt, key);
            if (ix >= 0) {
                jsval v;
                if (!ta_get(rt, o, (uint64_t)ix, &v)) return 0;
                d->value = v;
                d->flags = P_DEFAULT;
                d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                return 1;
            }
            break;
        }
        case CLS_HOST:
            if (o->u.host.cls && o->u.host.cls->get) {
                char *k;
                bool al;
                if (host_key_cstr(rt, key, &k, &al)) {
                    jsval v = JS_UNDEF;
                    bool h = o->u.host.cls->get(rt, mkobj(o), o->u.host.ptr, k, &v);
                    if (al) free(k);
                    if (rt->has_exc) return -1;
                    if (h) {
                        d->value = v;
                        d->flags = P_DEFAULT;
                        d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
                        return 1;
                    }
                }
            }
            break;
        default: break;
        }
    }
    Prop *p = find_own(rt, o, key);
    if (!p) return 0;
    d->flags = (uint8_t)(p->flags & (P_E | P_C | P_W));
    if (p->flags & P_ACC) {
        Accessor *a = (Accessor *)vptr(p->val);
        d->get = a->get;
        d->set = a->set;
        d->flags &= (uint8_t)~P_W;
        d->has = HAS_GET | HAS_SET | HAS_E | HAS_C;
    } else {
        d->value = p->val;
        d->has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
    }
    return 1;
}

int has_own(js_rt *rt, JSObject *o, jsval key) {
    PropDesc d;
    return get_own_desc(rt, o, key, &d);
}

static int reject(js_rt *rt, bool th, jsval key) {
    if (!th) return 0;
    jsval kd = key_to_display(rt, key);
    throw_error(rt, E_TYPE, "Cannot redefine property: %S", vstr(kd));
    return -1;
}

int define_own(js_rt *rt, JSObject *o, jsval key, PropDesc *d, bool th) {
    if (o->cls == CLS_PROXY) {
        int r = proxy_define(rt, o, key, d);
        if (r == 0 && th) return reject(rt, th, key);
        return r;
    }
    if (o->oflags & OF_EXOTIC) {
        if (o->cls == CLS_ARRAY) {
            if (key == ATOM(length)) {
                if (d->has & (HAS_GET | HAS_SET)) return reject(rt, th, key);
                if ((d->has & HAS_C) && (d->flags & P_C)) return reject(rt, th, key);
                if ((d->has & HAS_E) && (d->flags & P_E)) return reject(rt, th, key);
                if (d->has & HAS_VALUE) {
                    double dv;
                    if (to_number(rt, d->value, &dv) < 0) return -1;
                    uint32_t nl = to_uint32(dv);
                    if ((double)nl != dv) {
                        throw_error(rt, E_RANGE, "Invalid array length");
                        return -1;
                    }
                    if ((o->oflags & OF_LEN_RO) && nl != o->u.arr.len) return reject(rt, th, key);
                    if (!array_set_length(rt, o, nl)) return reject(rt, th, key);
                }
                if ((d->has & HAS_W) && !(d->flags & P_W)) o->oflags |= OF_LEN_RO;
                else if ((d->has & HAS_W) && (o->oflags & OF_LEN_RO)) return reject(rt, th, key);
                return 1;
            }
            int64_t ix = key_index(rt, key);
            if (ix >= 0) {
                if ((uint32_t)ix >= o->u.arr.len && (o->oflags & OF_LEN_RO)) return reject(rt, th, key);
                if (!(o->oflags & OF_SLOW_ARRAY)) {
                    bool plain = !(d->has & (HAS_GET | HAS_SET));
                    bool exists = ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE;
                    uint8_t want = P_DEFAULT;
                    if (d->has & HAS_W) want = (uint8_t)((want & ~P_W) | (d->flags & P_W));
                    if (d->has & HAS_E) want = (uint8_t)((want & ~P_E) | (d->flags & P_E));
                    if (d->has & HAS_C) want = (uint8_t)((want & ~P_C) | (d->flags & P_C));
                    if (!exists) {
                        if (!(d->has & HAS_W)) want &= (uint8_t)~P_W;
                        if (!(d->has & HAS_E)) want &= (uint8_t)~P_E;
                        if (!(d->has & HAS_C)) want &= (uint8_t)~P_C;
                    }
                    if (plain && want == P_DEFAULT && !(o->oflags & (OF_FROZEN_EL | OF_SEALED_EL))) {
                        if (!exists && !(o->oflags & OF_EXTENSIBLE)) return reject(rt, th, key);
                        if ((uint32_t)ix >= o->u.arr.len) {
                            if ((uint32_t)ix > o->u.arr.len + 100000 || !array_reserve(rt, o, (uint32_t)ix + 1)) {
                                array_make_slow(rt, o);
                                goto generic;
                            }
                            for (uint32_t k = o->u.arr.len; k < (uint32_t)ix; k++) o->u.arr.elems[k] = JS_HOLE;
                            o->u.arr.len = (uint32_t)ix + 1;
                        }
                        o->u.arr.elems[ix] = (d->has & HAS_VALUE) ? d->value : (exists ? o->u.arr.elems[ix] : JS_UNDEF);
                        return 1;
                    }
                    array_make_slow(rt, o);
                }
                int r;
            generic:
                r = 1;
                (void)r;
                /* fall to generic with length update */
                {
                    PropDesc cur;
                    Prop *p = find_own(rt, o, key);
                    (void)cur;
                    if (!p && !(o->oflags & OF_EXTENSIBLE)) return reject(rt, th, key);
                }
                int res;
                {
                    /* generic path below, then bump length */
                    o->oflags &= (uint16_t)~OF_EXOTIC;
                    res = define_own(rt, o, key, d, th);
                    o->oflags |= OF_EXOTIC;
                }
                if (res == 1 && (uint32_t)ix >= o->u.arr.len) o->u.arr.len = (uint32_t)ix + 1;
                return res;
            }
        } else if (o->cls == CLS_ARGUMENTS) {
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                /* move element to an ordinary property (and unmap it) */
                uint16_t *pm = o->u.arr.pmap;
                jsval v = (pm && ix < pm[0] && pm[1 + ix] != 0xFFFF) ? o->u.arr.menv->slots[pm[1 + ix]] : o->u.arr.elems[ix];
                if (pm && ix < pm[0]) pm[1 + ix] = 0xFFFF;
                o->u.arr.elems[ix] = JS_HOLE;
                add_prop(rt, o, key, v, P_DEFAULT);
            }
        } else if (o->cls == CLS_STRING) {
            int64_t ix = key_index(rt, key);
            JSString *s = vstr(o->u.prim.v);
            if (key == ATOM(length) || (ix >= 0 && ix < s->len)) {
                PropDesc cur;
                get_own_desc(rt, o, key, &cur);
                if ((d->has & HAS_VALUE) && !same_value(rt, d->value, cur.value)) return reject(rt, th, key);
                if ((d->has & HAS_W) && (d->flags & P_W)) return reject(rt, th, key);
                if ((d->has & HAS_C) && (d->flags & P_C)) return reject(rt, th, key);
                if (d->has & (HAS_GET | HAS_SET)) return reject(rt, th, key);
                return 1;
            }
        } else if (o->cls == CLS_TYPEDARRAY) {
            int64_t ix = key_index(rt, key);
            if (ix >= 0) {
                if ((uint64_t)ix >= ta_length(o)) return reject(rt, th, key);
                if (d->has & (HAS_GET | HAS_SET)) return reject(rt, th, key);
                if (d->has & HAS_VALUE) return ta_set(rt, o, (uint64_t)ix, d->value) < 0 ? -1 : 1;
                return 1;
            }
        }
    }
    Prop *p = find_own(rt, o, key);
    bool acc = (d->has & (HAS_GET | HAS_SET)) != 0;
    if (!p) {
        if (!(o->oflags & OF_EXTENSIBLE)) {
            if (!th) return 0;
            jsval kd = key_to_display(rt, key);
            throw_error(rt, E_TYPE, "Cannot define property %S, object is not extensible", vstr(kd));
            return -1;
        }
        uint32_t fl = 0;
        if ((d->has & HAS_E) && (d->flags & P_E)) fl |= P_E;
        if ((d->has & HAS_C) && (d->flags & P_C)) fl |= P_C;
        if (acc) {
            def_acc(rt, o, key, (d->has & HAS_GET) ? d->get : JS_UNDEF, (d->has & HAS_SET) ? d->set : JS_UNDEF, fl);
        } else {
            if ((d->has & HAS_W) && (d->flags & P_W)) fl |= P_W;
            add_prop(rt, o, key, (d->has & HAS_VALUE) ? d->value : JS_UNDEF, fl);
        }
        return 1;
    }
    bool cur_acc = (p->flags & P_ACC) != 0;
    if (!(p->flags & P_C)) {
        if ((d->has & HAS_C) && (d->flags & P_C)) return reject(rt, th, key);
        if ((d->has & HAS_E) && ((d->flags & P_E) != (p->flags & P_E))) return reject(rt, th, key);
        bool generic = !(d->has & (HAS_VALUE | HAS_W | HAS_GET | HAS_SET));
        if (!generic) {
            if (acc != cur_acc) return reject(rt, th, key);
            if (!cur_acc) {
                if (!(p->flags & P_W)) {
                    if ((d->has & HAS_W) && (d->flags & P_W)) return reject(rt, th, key);
                    if ((d->has & HAS_VALUE) && !same_value(rt, d->value, p->val)) return reject(rt, th, key);
                }
            } else {
                Accessor *a = (Accessor *)vptr(p->val);
                if ((d->has & HAS_GET) && d->get != a->get) return reject(rt, th, key);
                if ((d->has & HAS_SET) && d->set != a->set) return reject(rt, th, key);
            }
        }
    }
    uint32_t fl = p->flags & (P_E | P_C);
    if (d->has & HAS_E) fl = (fl & ~(uint32_t)P_E) | (d->flags & P_E);
    if (d->has & HAS_C) fl = (fl & ~(uint32_t)P_C) | (d->flags & P_C);
    if (acc) {
        Accessor *a;
        if (cur_acc) {
            Accessor *old = (Accessor *)vptr(p->val);
            a = (Accessor *)gc_alloc(rt, sizeof(Accessor), GC_ACCESSOR);
            a->get = old->get;
            a->set = old->set;
        } else {
            a = (Accessor *)gc_alloc(rt, sizeof(Accessor), GC_ACCESSOR);
            a->get = a->set = JS_UNDEF;
        }
        if (d->has & HAS_GET) a->get = d->get;
        if (d->has & HAS_SET) a->set = d->set;
        p->val = mkptr(a);
        p->flags = fl | P_ACC;
    } else if (cur_acc && (d->has & (HAS_VALUE | HAS_W))) {
        p->val = (d->has & HAS_VALUE) ? d->value : JS_UNDEF;
        p->flags = fl | (((d->has & HAS_W) && (d->flags & P_W)) ? P_W : 0);
    } else if (!cur_acc) {
        if (d->has & HAS_VALUE) p->val = d->value;
        uint32_t w = p->flags & P_W;
        if (d->has & HAS_W) w = d->flags & P_W;
        p->flags = fl | w;
    } else
        p->flags = fl | P_ACC;
    return 1;
}

int create_data_prop(js_rt *rt, JSObject *o, jsval key, jsval v, bool th) {
    if (!(o->oflags & OF_EXOTIC)) {
        Prop *p = find_own(rt, o, key);
        if (!p) {
            if (!(o->oflags & OF_EXTENSIBLE)) {
                if (!th) return 0;
                jsval kd = key_to_display(rt, key);
                throw_error(rt, E_TYPE, "Cannot add property %S, object is not extensible", vstr(kd));
                return -1;
            }
            add_prop(rt, o, key, v, P_DEFAULT);
            return 1;
        }
        if ((p->flags & P_C) || ((p->flags & (P_W | P_E | P_ACC)) == (P_W | P_E))) {
            p->val = v;
            p->flags = P_DEFAULT;
            return 1;
        }
    }
    PropDesc d;
    d.value = v;
    d.flags = P_DEFAULT;
    d.has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
    return define_own(rt, o, key, &d, th);
}
int create_data_idx(js_rt *rt, JSObject *o, uint32_t i, jsval v) {
    if (o->cls == CLS_ARRAY && !(o->oflags & (OF_SLOW_ARRAY | OF_FROZEN_EL | OF_SEALED_EL | OF_LEN_RO)) &&
        (o->oflags & OF_EXTENSIBLE)) {
        if (i < o->u.arr.len) {
            o->u.arr.elems[i] = v;
            return 1;
        }
        if (i == o->u.arr.len) {
            if (!array_reserve(rt, o, i + 1)) return -1;
            o->u.arr.elems[o->u.arr.len++] = v;
            return 1;
        }
    }
    return create_data_prop(rt, o, atomize(rt, num_to_str(rt, i)), v, true);
}

/* ---------------- has / delete ---------------- */
int has_prop(js_rt *rt, jsval obj, jsval key) {
    if (!is_obj(obj)) return 0;
    JSObject *o = vobj(obj);
    int depth = 0;
    while (o) {
        if (o->cls == CLS_PROXY) return proxy_has(rt, o, key);
        int r = has_own(rt, o, key);
        if (r) return r;
        if (o->cls == CLS_TYPEDARRAY && key_index(rt, key) >= 0) return 0;
        o = o->proto;
        if (++depth > 100000) break;
    }
    return 0;
}

int delete_prop(js_rt *rt, jsval obj, jsval key, bool th) {
    if (!is_obj(obj)) {
        if (is_str(obj) && (key == ATOM(length) || (key_index(rt, key) >= 0 && key_index(rt, key) < vstr(obj)->len))) {
            if (!th) return 0;
            jsval kd = key_to_display(rt, key);
            throw_error(rt, E_TYPE, "Cannot delete property '%S' of [object String]", vstr(kd));
            return -1;
        }
        return 1;
    }
    JSObject *o = vobj(obj);
    if (o->cls == CLS_PROXY) {
        int r = proxy_delete(rt, o, key);
        if (r == 0 && th) {
            jsval kd = key_to_display(rt, key);
            throw_error(rt, E_TYPE, "'deleteProperty' on proxy: trap returned falsish for property '%S'", vstr(kd));
            return -1;
        }
        return r;
    }
    if (o->oflags & OF_EXOTIC) {
        if (o->cls == CLS_ARRAY) {
            if (key == ATOM(length)) goto nodelete;
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && !(o->oflags & OF_SLOW_ARRAY)) {
                if (ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                    if (o->oflags & (OF_FROZEN_EL | OF_SEALED_EL)) goto nodelete;
                    o->u.arr.elems[ix] = JS_HOLE;
                }
                return 1;
            }
        } else if (o->cls == CLS_ARGUMENTS) {
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && ix < o->u.arr.len && o->u.arr.elems[ix] != JS_HOLE) {
                o->u.arr.elems[ix] = JS_HOLE;
                if (o->u.arr.pmap && ix < o->u.arr.pmap[0]) o->u.arr.pmap[1 + ix] = 0xFFFF;
                return 1;
            }
        } else if (o->cls == CLS_STRING) {
            int64_t ix = key_index(rt, key);
            if (key == ATOM(length) || (ix >= 0 && ix < vstr(o->u.prim.v)->len)) goto nodelete;
        } else if (o->cls == CLS_TYPEDARRAY) {
            int64_t ix = key_index(rt, key);
            if (ix >= 0 && (uint64_t)ix < ta_length(o)) goto nodelete;
        }
    }
    {
        Prop *p = find_own(rt, o, key);
        if (!p) return 1;
        if (!(p->flags & P_C)) goto nodelete;
        del_prop_at(rt, o, p);
        return 1;
    }
nodelete:
    if (!th) return 0;
    {
        jsval kd = key_to_display(rt, key);
        throw_error(rt, E_TYPE, "Cannot delete property '%S' of %S", vstr(kd), val_brief(rt, obj));
    }
    return -1;
}

/* ---------------- keys ---------------- */
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void sort_u32(uint32_t *v, uint32_t n) {
    /* insertion sort for small, else simple heap-free merge via shell sort */
    for (uint32_t gap = n / 2; gap > 0; gap /= 2)
        for (uint32_t i = gap; i < n; i++) {
            uint32_t t = v[i], j = i;
            while (j >= gap && cmp_u32(&v[j - gap], &t) > 0) {
                v[j] = v[j - gap];
                j -= gap;
            }
            v[j] = t;
        }
}

JSObject *own_keys(js_rt *rt, JSObject *o, int flags) {
    if (o->cls == CLS_PROXY) {
        JSObject *k = proxy_own_keys(rt, o);
        if (!k) return 0;
        if (flags == (KEYS_STR | KEYS_SYM)) return k;
        /* filter */
        jsval *root = push_tmp(rt, mkobj(k));
        JSObject *r = new_array(rt, 0);
        push_tmp(rt, mkobj(r));
        for (uint32_t i = 0; i < k->u.arr.len; i++) {
            jsval key = k->u.arr.elems[i];
            if (is_sym(key) ? !(flags & KEYS_SYM) : !(flags & KEYS_STR)) continue;
            if (flags & KEYS_ENUM_ONLY) {
                PropDesc d;
                int e = get_own_desc(rt, o, key, &d);
                if (e < 0) {
                    rt->sp = root;
                    return 0;
                }
                if (!e || !(d.flags & P_E)) continue;
            }
            array_push(rt, r, key);
        }
        rt->sp = root;
        return r;
    }
    if (o->oflags & OF_LAZY) materialize_fn(rt, o);
    JSObject *r = new_array(rt, 0);
    if (flags & KEYS_STR) {
        uint32_t n = 0;
        if ((o->cls == CLS_ARRAY && !(o->oflags & OF_SLOW_ARRAY)) || o->cls == CLS_ARGUMENTS) {
            for (uint32_t i = 0; i < o->u.arr.len; i++)
                if (o->u.arr.elems[i] != JS_HOLE) array_push(rt, r, atomize(rt, num_to_str(rt, i)));
        } else if (o->cls == CLS_STRING) {
            n = vstr(o->u.prim.v)->len;
            for (uint32_t i = 0; i < n; i++) array_push(rt, r, atomize(rt, num_to_str(rt, i)));
        } else if (o->cls == CLS_TYPEDARRAY) {
            n = ta_length(o);
            for (uint32_t i = 0; i < n; i++) array_push(rt, r, atomize(rt, num_to_str(rt, i)));
        }
        if (o->oflags & OF_HASIDX) {
            uint32_t cnt = 0;
            for (uint32_t i = 0; i < o->nprops; i++)
                if (o->props[i].key != JS_HOLE && key_index(rt, o->props[i].key) >= 0 &&
                    (!(flags & KEYS_ENUM_ONLY) || (o->props[i].flags & P_E)))
                    cnt++;
            if (cnt) {
                uint32_t *ix = (uint32_t *)malloc(cnt * sizeof(uint32_t));
                uint32_t k = 0;
                for (uint32_t i = 0; i < o->nprops; i++)
                    if (o->props[i].key != JS_HOLE && key_index(rt, o->props[i].key) >= 0 &&
                        (!(flags & KEYS_ENUM_ONLY) || (o->props[i].flags & P_E)))
                        ix[k++] = (uint32_t)key_index(rt, o->props[i].key);
                sort_u32(ix, cnt);
                for (uint32_t i = 0; i < cnt; i++) array_push(rt, r, atomize(rt, num_to_str(rt, ix[i])));
                free(ix);
            }
        }
        if (!(flags & KEYS_ENUM_ONLY)) {
            if (o->cls == CLS_ARRAY || o->cls == CLS_STRING) array_push(rt, r, ATOM(length));
        }
        for (uint32_t i = 0; i < o->nprops; i++) {
            Prop *p = &o->props[i];
            if (!is_str(p->key)) continue;
            if ((o->oflags & OF_HASIDX) && key_index(rt, p->key) >= 0) continue;
            if ((flags & KEYS_ENUM_ONLY) && !(p->flags & P_E)) continue;
            array_push(rt, r, p->key);
        }
    }
    if (flags & KEYS_SYM) {
        for (uint32_t i = 0; i < o->nprops; i++) {
            Prop *p = &o->props[i];
            if (!is_sym(p->key)) continue;
            if ((vsym(p->key)->h.flags & 1) && !(flags & KEYS_PRIVATE)) continue;
            if ((flags & KEYS_ENUM_ONLY) && !(p->flags & P_E)) continue;
            array_push(rt, r, p->key);
        }
    }
    return r;
}

/* ---------------- prototype / extensibility ---------------- */
jsval get_proto(js_rt *rt, JSObject *o) {
    if (o->cls == CLS_PROXY) return proxy_get_proto(rt, o);
    return o->proto ? mkobj(o->proto) : JS_NULL;
}
int set_proto(js_rt *rt, JSObject *o, JSObject *proto, bool th) {
    if (o->cls == CLS_PROXY) {
        int r = proxy_set_proto(rt, o, proto);
        if (r == 0 && th) {
            throw_error(rt, E_TYPE, "'setPrototypeOf' on proxy: trap returned falsish");
            return -1;
        }
        return r;
    }
    if (o->proto == proto) return 1;
    if (!(o->oflags & OF_EXTENSIBLE) || o == rt->intr[I_ObjectProto]) {
        if (!th) return 0;
        throw_error(rt, E_TYPE, "%S is not extensible", val_brief(rt, mkobj(o)));
        return -1;
    }
    for (JSObject *p = proto; p; p = p->proto) {
        if (p == o) {
            if (!th) return 0;
            throw_error(rt, E_TYPE, "Cyclic __proto__ value");
            return -1;
        }
        if (p->cls == CLS_PROXY) break;
    }
    o->proto = proto;
    return 1;
}
int prevent_ext(js_rt *rt, JSObject *o) {
    if (o->cls == CLS_PROXY) return proxy_prevent_ext(rt, o);
    o->oflags &= (uint16_t)~OF_EXTENSIBLE;
    return 1;
}
int is_extensible(js_rt *rt, JSObject *o) {
    if (o->cls == CLS_PROXY) return proxy_is_ext(rt, o);
    return (o->oflags & OF_EXTENSIBLE) != 0;
}
int set_integrity(js_rt *rt, JSObject *o, bool frozen) {
    if (o->cls == CLS_PROXY) {
        int r = proxy_prevent_ext(rt, o);
        if (r <= 0) return r;
        JSObject *keys = own_keys(rt, o, KEYS_STR | KEYS_SYM);
        if (!keys) return -1;
        jsval *root = push_tmp(rt, mkobj(keys));
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            PropDesc d;
            memset(&d, 0, sizeof d);
            d.has = HAS_C;
            if (frozen) {
                PropDesc cur;
                int e = get_own_desc(rt, o, keys->u.arr.elems[i], &cur);
                if (e < 0) {
                    rt->sp = root;
                    return -1;
                }
                if (!e) continue;
                if (!(cur.has & (HAS_GET | HAS_SET))) d.has |= HAS_W;
            }
            if (define_own(rt, o, keys->u.arr.elems[i], &d, true) < 0) {
                rt->sp = root;
                return -1;
            }
        }
        rt->sp = root;
        return 1;
    }
    if (o->oflags & OF_LAZY) materialize_fn(rt, o);
    o->oflags &= (uint16_t)~OF_EXTENSIBLE;
    if (o->cls == CLS_ARRAY) {
        o->oflags |= frozen ? (OF_FROZEN_EL | OF_SEALED_EL | OF_LEN_RO) : OF_SEALED_EL;
    } else if (o->cls == CLS_ARGUMENTS) {
        for (uint32_t i = 0; i < o->u.arr.len; i++)
            if (o->u.arr.elems[i] != JS_HOLE) {
                add_prop(rt, o, atomize(rt, num_to_str(rt, i)), o->u.arr.elems[i], P_DEFAULT);
                o->u.arr.elems[i] = JS_HOLE;
            }
    } else if (o->cls == CLS_TYPEDARRAY && frozen && ta_length(o) > 0) {
        throw_error(rt, E_TYPE, "Cannot freeze array buffer views with elements");
        return -1;
    }
    for (uint32_t i = 0; i < o->nprops; i++) {
        Prop *p = &o->props[i];
        if (p->key == JS_HOLE) continue;
        p->flags &= ~(uint32_t)P_C;
        if (frozen && !(p->flags & P_ACC)) p->flags &= ~(uint32_t)P_W;
    }
    return 1;
}
int test_integrity(js_rt *rt, JSObject *o, bool frozen) {
    int e = is_extensible(rt, o);
    if (e != 0) return e < 0 ? -1 : 0;
    JSObject *keys = own_keys(rt, o, KEYS_STR | KEYS_SYM);
    if (!keys) return -1;
    jsval *root = push_tmp(rt, mkobj(keys));
    int res = 1;
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        PropDesc d;
        int r = get_own_desc(rt, o, keys->u.arr.elems[i], &d);
        if (r < 0) {
            res = -1;
            break;
        }
        if (!r) continue;
        if (d.flags & P_C) {
            res = 0;
            break;
        }
        if (frozen && (d.has & HAS_VALUE) && (d.flags & P_W)) {
            res = 0;
            break;
        }
    }
    rt->sp = root;
    return res;
}

/* ---------------- conversions ---------------- */
bool truthy(jsval v) {
    if (is_num(v)) {
        double d = vnum(v);
        return d == d && d != 0;
    }
    if (v == JS_TRUE) return true;
    if (v == JS_FALSE || is_nullish(v)) return false;
    if (is_str(v)) return vstr(v)->len != 0;
    if (is_obj(v)) return !(vobj(v)->oflags & OF_HTMLDDA);
    if (is_big(v)) return !big_is_zero((JSBig *)vptr(v));
    return true;
}

jsval to_primitive(js_rt *rt, jsval v, int hint) {
    if (!is_obj(v)) return v;
    jsval *root = push_tmp(rt, v);
    jsval ex = get_p(rt, v, SYM(toPrimitive));
    if (ex == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    if (!is_nullish(ex)) {
        if (!is_callable(ex)) {
            rt->sp = root;
            return throw_error(rt, E_TYPE, "Symbol.toPrimitive is not a function");
        }
        jsval h = hint == 0 ? ATOM(default) : hint == 1 ? ATOM(number) : ATOM(string);
        jsval r = call_fn(rt, ex, v, 1, &h);
        rt->sp = root;
        if (r == JS_EXC) return JS_EXC;
        if (is_obj(r)) return throw_error(rt, E_TYPE, "Cannot convert object to primitive value");
        return r;
    }
    jsval m1 = hint == 2 ? ATOM(toString) : ATOM(valueOf);
    jsval m2 = hint == 2 ? ATOM(valueOf) : ATOM(toString);
    for (int i = 0; i < 2; i++) {
        jsval f = get_p(rt, v, i ? m2 : m1);
        if (f == JS_EXC) {
            rt->sp = root;
            return JS_EXC;
        }
        if (is_callable(f)) {
            jsval r = call_fn(rt, f, v, 0, 0);
            if (r == JS_EXC || !is_obj(r)) {
                rt->sp = root;
                return r;
            }
        }
    }
    rt->sp = root;
    return throw_error(rt, E_TYPE, "Cannot convert object to primitive value");
}

int to_number(js_rt *rt, jsval v, double *out) {
    if (LIKELY(is_num(v))) {
        *out = vnum(v);
        return 0;
    }
    if (is_str(v)) {
        *out = str_to_number(rt, vstr(v));
        return 0;
    }
    if (v == JS_TRUE) {
        *out = 1;
        return 0;
    }
    if (v == JS_FALSE || v == JS_NULL) {
        *out = 0;
        return 0;
    }
    if (v == JS_UNDEF) {
        *out = JS_NAN;
        return 0;
    }
    if (is_sym(v)) {
        throw_error(rt, E_TYPE, "Cannot convert a Symbol value to a number");
        return -1;
    }
    if (is_big(v)) {
        throw_error(rt, E_TYPE, "Cannot convert a BigInt value to a number");
        return -1;
    }
    if (is_obj(v)) {
        jsval p = to_primitive(rt, v, 1);
        if (p == JS_EXC) return -1;
        return to_number(rt, p, out);
    }
    *out = JS_NAN;
    return 0;
}
int to_numeric(js_rt *rt, jsval v, jsval *out) {
    if (is_num(v) || is_big(v)) {
        *out = v;
        return 0;
    }
    if (is_obj(v)) {
        v = to_primitive(rt, v, 1);
        if (v == JS_EXC) return -1;
        if (is_big(v)) {
            *out = v;
            return 0;
        }
    }
    double d;
    if (to_number(rt, v, &d) < 0) return -1;
    *out = mknum(d);
    return 0;
}

JSString *to_string_prim(js_rt *rt, jsval v) {
    if (is_str(v)) return vstr(v);
    if (is_num(v)) return num_to_str(rt, vnum(v));
    if (v == JS_TRUE) return vstr(ATOM(true));
    if (v == JS_FALSE) return vstr(ATOM(false));
    if (v == JS_NULL) return vstr(ATOM(null));
    if (v == JS_UNDEF) return vstr(ATOM(undefined));
    if (is_big(v)) return big_to_string(rt, (JSBig *)vptr(v), 10);
    if (is_sym(v)) {
        throw_error(rt, E_TYPE, "Cannot convert a Symbol value to a string");
        return 0;
    }
    return vstr(ATOM(empty));
}
JSString *to_string(js_rt *rt, jsval v) {
    if (LIKELY(is_str(v))) return vstr(v);
    if (is_obj(v)) {
        jsval p = to_primitive(rt, v, 2);
        if (p == JS_EXC) return 0;
        return to_string_prim(rt, p);
    }
    return to_string_prim(rt, v);
}

jsval to_object(js_rt *rt, jsval v) {
    if (is_obj(v)) return v;
    if (is_nullish(v)) return throw_error(rt, E_TYPE, "Cannot convert undefined or null to object");
    int cls = is_str(v) ? CLS_STRING : is_num(v) ? CLS_NUMBER : is_boolv(v) ? CLS_BOOLEAN : is_sym(v) ? CLS_SYMBOL : CLS_BIGINT;
    JSObject *o = new_object(rt, proto_of_prim(rt, v), cls);
    o->u.prim.v = v;
    return mkobj(o);
}

int to_int(js_rt *rt, jsval v, double *out) {
    double d;
    if (to_number(rt, v, &d) < 0) return -1;
    *out = to_integer(d);
    return 0;
}
int to_int_clamp(js_rt *rt, jsval v, int64_t lo, int64_t hi, int64_t dflt, int64_t *out) {
    if (is_undef(v)) {
        *out = dflt;
        return 0;
    }
    double d;
    if (to_int(rt, v, &d) < 0) return -1;
    if (d < (double)lo) d = (double)lo;
    if (d > (double)hi) d = (double)hi;
    *out = (int64_t)d;
    return 0;
}
int to_length(js_rt *rt, jsval v, uint64_t *out) {
    double d;
    if (to_int(rt, v, &d) < 0) return -1;
    if (d <= 0)
        *out = 0;
    else if (d > 9007199254740991.0)
        *out = 9007199254740991ull;
    else
        *out = (uint64_t)d;
    return 0;
}
int to_index(js_rt *rt, jsval v, uint64_t *out) {
    if (is_undef(v)) {
        *out = 0;
        return 0;
    }
    double d;
    if (to_int(rt, v, &d) < 0) return -1;
    if (d < 0 || d > 9007199254740991.0) {
        throw_error(rt, E_RANGE, "Invalid index");
        return -1;
    }
    *out = (uint64_t)d;
    return 0;
}
int len_of(js_rt *rt, jsval o, uint64_t *out) {
    if (is_obj(o) && vobj(o)->cls == CLS_ARRAY) {
        *out = vobj(o)->u.arr.len;
        return 0;
    }
    jsval l = get_p(rt, o, ATOM(length));
    if (l == JS_EXC) return -1;
    return to_length(rt, l, out);
}

/* ---------------- equality ---------------- */
bool strict_eq(js_rt *rt, jsval a, jsval b) {
    if (is_num(a) && is_num(b)) return vnum(a) == vnum(b);
    if (is_str(a) && is_str(b)) return str_eq(rt, vstr(a), vstr(b));
    if (is_big(a) && is_big(b)) return big_cmp((JSBig *)vptr(a), (JSBig *)vptr(b)) == 0;
    return a == b;
}
bool same_value_zero(js_rt *rt, jsval a, jsval b) {
    if (is_num(a) && is_num(b)) {
        double x = vnum(a), y = vnum(b);
        return x == y || (x != x && y != y);
    }
    return strict_eq(rt, a, b);
}
bool same_value(js_rt *rt, jsval a, jsval b) {
    if (is_num(a) && is_num(b)) {
        double x = vnum(a), y = vnum(b);
        if (x != x && y != y) return true;
        return dbits(x) == dbits(y);
    }
    return strict_eq(rt, a, b);
}
int loose_eq(js_rt *rt, jsval a, jsval b) {
    for (int guard = 0; guard < 8; guard++) {
        if (is_num(a) && is_num(b)) return vnum(a) == vnum(b);
        if (is_nullish(a) || is_nullish(b)) {
            if (is_nullish(a) && is_nullish(b)) return 1;
            if (is_obj(a) && (vobj(a)->oflags & OF_HTMLDDA)) return 1;
            if (is_obj(b) && (vobj(b)->oflags & OF_HTMLDDA)) return 1;
            return 0;
        }
        if ((vtag(a) == vtag(b) && !is_num(a) && !is_nullish(a)) || (is_boolv(a) && is_boolv(b))) return strict_eq(rt, a, b);
        if (is_num(a) && is_str(b)) return vnum(a) == str_to_number(rt, vstr(b));
        if (is_str(a) && is_num(b)) return str_to_number(rt, vstr(a)) == vnum(b);
        if (is_big(a) && is_str(b)) {
            jsval bb = big_from_string(rt, vstr(b), false);
            if (bb == JS_EXC) return -1;
            if (!is_big(bb)) return 0;
            return big_cmp((JSBig *)vptr(a), (JSBig *)vptr(bb)) == 0;
        }
        if (is_str(a) && is_big(b)) {
            jsval t = a;
            a = b;
            b = t;
            continue;
        }
        if (is_boolv(a)) {
            a = mknum(a == JS_TRUE);
            continue;
        }
        if (is_boolv(b)) {
            b = mknum(b == JS_TRUE);
            continue;
        }
        if (is_obj(b) && !is_obj(a)) {
            jsval *root = push_tmp(rt, a);
            b = to_primitive(rt, b, 0);
            rt->sp = root;
            if (b == JS_EXC) return -1;
            continue;
        }
        if (is_obj(a) && !is_obj(b)) {
            jsval *root = push_tmp(rt, b);
            a = to_primitive(rt, a, 0);
            rt->sp = root;
            if (a == JS_EXC) return -1;
            continue;
        }
        if (is_big(a) && is_num(b)) return big_cmp_num((JSBig *)vptr(a), vnum(b)) == 0;
        if (is_num(a) && is_big(b)) return big_cmp_num((JSBig *)vptr(b), vnum(a)) == 0;
        return 0;
    }
    return 0;
}

const char *type_name(jsval v) {
    if (is_num(v)) return "number";
    if (is_str(v)) return "string";
    if (is_boolv(v)) return "boolean";
    if (is_undef(v)) return "undefined";
    if (is_null(v)) return "null";
    if (is_sym(v)) return "symbol";
    if (is_big(v)) return "bigint";
    if (is_obj(v)) return (vobj(v)->oflags & OF_CALLABLE) ? "function" : "object";
    return "undefined";
}
jsval typeof_val(js_rt *rt, jsval v) {
    if (is_num(v)) return ATOM(number);
    if (is_str(v)) return ATOM(string);
    if (is_boolv(v)) return ATOM(boolean);
    if (is_undef(v)) return ATOM(undefined);
    if (is_null(v)) return ATOM(object);
    if (is_sym(v)) return ATOM(symbol);
    if (is_big(v)) return ATOM(bigint);
    if (is_obj(v)) {
        if (vobj(v)->oflags & OF_HTMLDDA) return ATOM(undefined);
        return (vobj(v)->oflags & OF_CALLABLE) ? ATOM(function) : ATOM(object);
    }
    return ATOM(undefined);
}
bool is_callable(jsval v) { return is_obj(v) && (vobj(v)->oflags & OF_CALLABLE); }
bool is_ctor(jsval v) { return is_obj(v) && (vobj(v)->oflags & OF_CTOR); }
bool is_array(js_rt *rt, jsval v) {
    for (int i = 0; i < 1000 && is_obj(v); i++) {
        JSObject *o = vobj(v);
        if (o->cls == CLS_ARRAY) return true;
        if (o->cls != CLS_PROXY) return false;
        v = o->u.it.a;
    }
    return false;
}

JSObject *proto_from_ctor(js_rt *rt, jsval ctor, int dflt) {
    if (!is_obj(ctor)) return rt->intr[dflt];
    jsval p = get_p(rt, ctor, ATOM(prototype));
    if (p == JS_EXC) return 0;
    if (is_obj(p)) return vobj(p);
    return rt->intr[dflt];
}

JSObject *species_ctor(js_rt *rt, JSObject *o, int dflt, jsval *out) {
    jsval c = get_p(rt, mkobj(o), ATOM(constructor));
    if (c == JS_EXC) return 0;
    if (is_undef(c)) {
        *out = mkobj(rt->intr[dflt]);
        return rt->intr[dflt];
    }
    if (!is_obj(c)) {
        throw_error(rt, E_TYPE, "object.constructor is not an object");
        return 0;
    }
    jsval s = get_p(rt, c, SYM(species));
    if (s == JS_EXC) return 0;
    if (is_nullish(s)) {
        *out = mkobj(rt->intr[dflt]);
        return rt->intr[dflt];
    }
    if (!is_ctor(s)) {
        throw_error(rt, E_TYPE, "object.constructor[Symbol.species] is not a constructor");
        return 0;
    }
    *out = s;
    return vobj(s);
}

/* ---------------- errors & formatting ---------------- */
jsval key_to_display(js_rt *rt, jsval key) {
    if (is_str(key)) return key;
    if (is_sym(key)) {
        JSSymbol *s = vsym(key);
        SB b;
        sb_init(&b, rt);
        if (s->h.flags & 1) {
            if (is_str(s->desc)) sb_putstr(&b, vstr(s->desc));
        } else {
            sb_puts(&b, "Symbol(");
            if (is_str(s->desc)) sb_putstr(&b, vstr(s->desc));
            sb_puts(&b, ")");
        }
        return mkstr(sb_finish(&b));
    }
    JSString *s = to_string_prim(rt, key);
    return s ? mkstr(s) : ATOM(empty);
}

JSString *val_brief(js_rt *rt, jsval v) {
    if (is_str(v)) return vstr(v);
    if (is_sym(v)) return vstr(key_to_display(rt, v));
    if (!is_obj(v)) return to_string_prim(rt, v);
    JSObject *o = vobj(v);
    if (o->oflags & OF_CALLABLE) return func_to_string(rt, o);
    if (o->cls == CLS_ARRAY) return str_cstr(rt, "[object Array]");
    /* #<Ctor> */
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, "#<");
    JSObject *p = o->proto;
    jsval nm = JS_UNDEF;
    if (p) {
        Prop *c = find_own(rt, p, ATOM(constructor));
        if (c && !(c->flags & P_ACC) && is_obj(c->val)) nm = func_name_of(rt, vobj(c->val));
    }
    if (is_str(nm) && vstr(nm)->len)
        sb_putstr(&b, vstr(nm));
    else
        sb_puts(&b, "Object");
    sb_puts(&b, ">");
    return sb_finish(&b);
}

void fmt_into(js_rt *rt, SB *b, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            /* format strings are UTF-8 */
            if ((unsigned char)*p >= 0x80) {
                uint32_t cp;
                int n = utf8_decode((const uint8_t *)p, strlen(p), &cp);
                sb_putcp(b, cp);
                p += n - 1;
            } else
                sb_putc(b, (unsigned char)*p);
            continue;
        }
        p++;
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            sb_pututf8(b, s, strlen(s));
            break;
        }
        case 'S': {
            JSString *s = va_arg(ap, JSString *);
            if (s) sb_putstr(b, s);
            break;
        }
        case 'v': {
            jsval v = va_arg(ap, jsval);
            sb_putstr(b, val_brief(rt, v));
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            char t[16];
            int n = num_to_cstr(v, t);
            sb_putn(b, t, (uint32_t)n);
            break;
        }
        case 'u': {
            unsigned v = va_arg(ap, unsigned);
            char t[16];
            int n = num_to_cstr(v, t);
            sb_putn(b, t, (uint32_t)n);
            break;
        }
        case '%': sb_putc(b, '%'); break;
        default: break;
        }
    }
}
JSString *jsfmt(js_rt *rt, const char *fmt, ...) {
    SB b;
    sb_init(&b, rt);
    va_list ap;
    va_start(ap, fmt);
    fmt_into(rt, &b, fmt, ap);
    va_end(ap);
    return sb_finish(&b);
}

static const char *const err_names[E__COUNT] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
                                                 "URIError", "EvalError", "AggregateError", "InternalError"};

JSObject *make_error(js_rt *rt, int type, JSString *msg) {
    JSObject *e = new_object(rt, rt->intr[I_ErrorProto + type], CLS_ERROR);
    if (msg) add_prop(rt, e, ATOM(message), mkstr(msg), P_W | P_C);
    int line = 0;
    JSString *file = 0;
    JSString *st = capture_stack(rt, &line, &file);
    e->u.err.line = line;
    e->u.err.file = file;
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, err_names[type]);
    if (msg && msg->len) {
        sb_puts(&b, ": ");
        sb_putstr(&b, msg);
    }
    if (st) sb_putstr(&b, st);
    add_prop(rt, e, ATOM(stack), mkstr(sb_finish(&b)), P_W | P_C);
    return e;
}

jsval throw_val(js_rt *rt, jsval v) {
    rt->exc = v;
    rt->has_exc = true;
    int line = 0;
    JSString *file = 0;
    cur_location(rt, &line, &file);
    rt->throw_line = line;
    rt->throw_file = file;
    return JS_EXC;
}

jsval throw_error(js_rt *rt, int type, const char *fmt, ...) {
    SB b;
    sb_init(&b, rt);
    va_list ap;
    va_start(ap, fmt);
    fmt_into(rt, &b, fmt, ap);
    va_end(ap);
    JSString *msg = sb_finish(&b);
    JSObject *e = make_error(rt, type, msg);
    return throw_val(rt, mkobj(e));
}

jsval throw_not_obj(js_rt *rt, jsval v, const char *what) {
    return throw_error(rt, E_TYPE, "%s called on non-object", what);
}
