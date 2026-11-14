/* AXIOM JavaScript engine: core builtins (Object, Function, Symbol, Error, Boolean, globals, Reflect, Proxy). */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

/* ---------------- property descriptors ---------------- */
jsval desc_to_obj(js_rt *rt, PropDesc *d) {
    JSObject *o = new_plain(rt);
    if (d->has & (HAS_GET | HAS_SET)) {
        add_prop(rt, o, ATOM(get), (d->has & HAS_GET) ? d->get : JS_UNDEF, P_DEFAULT);
        add_prop(rt, o, ATOM(set), (d->has & HAS_SET) ? d->set : JS_UNDEF, P_DEFAULT);
    } else {
        add_prop(rt, o, ATOM(value), d->value, P_DEFAULT);
        add_prop(rt, o, ATOM(writable), mkbool(d->flags & P_W), P_DEFAULT);
    }
    add_prop(rt, o, ATOM(enumerable), mkbool(d->flags & P_E), P_DEFAULT);
    add_prop(rt, o, ATOM(configurable), mkbool(d->flags & P_C), P_DEFAULT);
    return mkobj(o);
}

int obj_to_desc(js_rt *rt, jsval v, PropDesc *d) {
    memset(d, 0, sizeof *d);
    if (!is_obj(v)) {
        throw_error(rt, E_TYPE, "Property description must be an object: %S", val_brief(rt, v));
        return -1;
    }
    jsval *root = push_tmp(rt, v);
    struct {
        int atom;
        int has;
        int flag;
    } fl[] = {{A_enumerable, HAS_E, P_E}, {A_configurable, HAS_C, P_C}, {A_writable, HAS_W, P_W}};
    for (int i = 0; i < 3; i++) {
        jsval k = rt->atoms[fl[i].atom];
        int h = has_prop(rt, v, k);
        if (h < 0) goto fail;
        if (h) {
            jsval x = get_p(rt, v, k);
            if (x == JS_EXC) goto fail;
            d->has |= (uint8_t)fl[i].has;
            if (truthy(x)) d->flags |= (uint8_t)fl[i].flag;
        }
    }
    int h = has_prop(rt, v, ATOM(value));
    if (h < 0) goto fail;
    if (h) {
        d->value = get_p(rt, v, ATOM(value));
        if (d->value == JS_EXC) goto fail;
        push_tmp(rt, d->value);
        d->has |= HAS_VALUE;
    }
    h = has_prop(rt, v, ATOM(get));
    if (h < 0) goto fail;
    if (h) {
        d->get = get_p(rt, v, ATOM(get));
        if (d->get == JS_EXC) goto fail;
        push_tmp(rt, d->get);
        if (!is_undef(d->get) && !is_callable(d->get)) {
            throw_error(rt, E_TYPE, "Getter must be a function: %S", val_brief(rt, d->get));
            goto fail;
        }
        d->has |= HAS_GET;
    }
    h = has_prop(rt, v, ATOM(set));
    if (h < 0) goto fail;
    if (h) {
        d->set = get_p(rt, v, ATOM(set));
        if (d->set == JS_EXC) goto fail;
        push_tmp(rt, d->set);
        if (!is_undef(d->set) && !is_callable(d->set)) {
            throw_error(rt, E_TYPE, "Setter must be a function: %S", val_brief(rt, d->set));
            goto fail;
        }
        d->has |= HAS_SET;
    }
    if ((d->has & (HAS_GET | HAS_SET)) && (d->has & (HAS_VALUE | HAS_W))) {
        throw_error(rt, E_TYPE, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        goto fail;
    }
    (void)root;
    return 0;
fail:
    return -1;
}

/* ---------------- Object ---------------- */
static JSObject *need_obj(js_rt *rt, jsval v, const char *fn) {
    if (!is_obj(v)) {
        throw_error(rt, E_TYPE, "%s called on non-object", fn);
        return 0;
    }
    return vobj(v);
}

NATIVE(object_ctor) {
    jsval nt = rt->new_target;
    if (!is_undef(nt) && nt != mkobj(rt->intr[I_Object])) {
        JSObject *p = proto_from_ctor(rt, nt, I_ObjectProto);
        if (!p) return JS_EXC;
        return mkobj(new_object(rt, p, CLS_OBJECT));
    }
    jsval v = ARG(0);
    if (is_nullish(v)) return mkobj(new_plain(rt));
    return to_object(rt, v);
}

NATIVE(object_keys) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    push_tmp(rt, o);
    int m = MAGIC; /* 0 keys 1 values 2 entries */
    JSObject *keys = own_keys(rt, vobj(o), KEYS_STR | (vobj(o)->cls == CLS_PROXY ? 0 : KEYS_ENUM_ONLY));
    if (!keys) return JS_EXC;
    push_tmp(rt, mkobj(keys));
    if (vobj(o)->cls == CLS_PROXY) {
        JSObject *f = new_array(rt, 0);
        push_tmp(rt, mkobj(f));
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            PropDesc d;
            int e = get_own_desc(rt, vobj(o), keys->u.arr.elems[i], &d);
            if (e < 0) return JS_EXC;
            if (e && (d.flags & P_E)) array_push(rt, f, keys->u.arr.elems[i]);
        }
        keys = f;
    }
    if (m == 0) return mkobj(keys);
    JSObject *r = new_array(rt, 0);
    push_tmp(rt, mkobj(r));
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jsval k = keys->u.arr.elems[i];
        /* skip keys deleted/made non-enumerable during iteration */
        if (i > 0) {
            PropDesc d;
            int e = get_own_desc(rt, vobj(o), k, &d);
            if (e < 0) return JS_EXC;
            if (!e || !(d.flags & P_E)) continue;
        }
        jsval v = get_p(rt, o, k);
        TRY(v);
        if (m == 1)
            array_push(rt, r, v);
        else {
            jsval pair[2] = {k, v};
            array_push(rt, r, mkobj(new_array_from(rt, pair, 2)));
        }
    }
    return mkobj(r);
}

NATIVE(object_assign) {
    jsval t = to_object(rt, ARG(0));
    TRY(t);
    push_tmp(rt, t);
    for (int i = 1; i < argc; i++) {
        jsval s = argv[i];
        if (is_nullish(s)) continue;
        jsval so = to_object(rt, s);
        TRY(so);
        jsval *r2 = push_tmp(rt, so);
        JSObject *keys = own_keys(rt, vobj(so), KEYS_STR | KEYS_SYM);
        if (!keys) return JS_EXC;
        push_tmp(rt, mkobj(keys));
        for (uint32_t k = 0; k < keys->u.arr.len; k++) {
            jsval key = keys->u.arr.elems[k];
            PropDesc d;
            int e = get_own_desc(rt, vobj(so), key, &d);
            if (e < 0) return JS_EXC;
            if (!e || !(d.flags & P_E)) continue;
            jsval v = (d.has & HAS_VALUE) && vobj(so)->cls != CLS_PROXY ? d.value : get_p(rt, so, key);
            TRY(v);
            if (set_p(rt, t, key, v, true) < 0) return JS_EXC;
        }
        rt->sp = r2;
    }
    return t;
}

NATIVE(object_freeze) {
    jsval o = ARG(0);
    if (!is_obj(o)) return o;
    int m = MAGIC; /* 0 freeze 1 seal 2 preventExtensions */
    int r = m == 2 ? prevent_ext(rt, vobj(o)) : set_integrity(rt, vobj(o), m == 0);
    if (r < 0) return JS_EXC;
    if (r == 0) THROW_TYPE("Cannot %s", m == 0 ? "freeze" : m == 1 ? "seal" : "prevent extensions");
    return o;
}
NATIVE(object_isfrozen) {
    jsval o = ARG(0);
    int m = MAGIC;
    if (!is_obj(o)) return mkbool(m != 2);
    int r = m == 2 ? is_extensible(rt, vobj(o)) : test_integrity(rt, vobj(o), m == 0);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}

NATIVE(object_define_property) {
    JSObject *o = need_obj(rt, ARG(0), "Object.defineProperty");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    push_tmp(rt, k);
    PropDesc d;
    if (obj_to_desc(rt, ARG(2), &d) < 0) return JS_EXC;
    if (define_own(rt, o, k, &d, true) < 0) return JS_EXC;
    return argv[0];
}

static int define_props(js_rt *rt, JSObject *o, jsval props) {
    jsval po = to_object(rt, props);
    if (po == JS_EXC) return -1;
    push_tmp(rt, po);
    JSObject *keys = own_keys(rt, vobj(po), KEYS_STR | KEYS_SYM);
    if (!keys) return -1;
    push_tmp(rt, mkobj(keys));
    uint32_t n = keys->u.arr.len;
    PropDesc *ds = (PropDesc *)calloc(n ? n : 1, sizeof(PropDesc));
    jsval *ks = (jsval *)calloc(n ? n : 1, sizeof(jsval));
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        jsval k = keys->u.arr.elems[i];
        PropDesc pd;
        int e = get_own_desc(rt, vobj(po), k, &pd);
        if (e < 0) goto fail;
        if (!e || !(pd.flags & P_E)) continue;
        jsval dv = get_p(rt, po, k);
        if (dv == JS_EXC) goto fail;
        push_tmp(rt, dv);
        if (obj_to_desc(rt, dv, &ds[c]) < 0) goto fail;
        ks[c++] = k;
    }
    for (uint32_t i = 0; i < c; i++)
        if (define_own(rt, o, ks[i], &ds[i], true) < 0) goto fail;
    free(ds);
    free(ks);
    return 0;
fail:
    free(ds);
    free(ks);
    return -1;
}

NATIVE(object_define_properties) {
    JSObject *o = need_obj(rt, ARG(0), "Object.defineProperties");
    if (!o) return JS_EXC;
    if (define_props(rt, o, ARG(1)) < 0) return JS_EXC;
    return argv[0];
}

NATIVE(object_create) {
    jsval p = ARG(0);
    if (!is_obj(p) && !is_null(p)) THROW_TYPE("Object prototype may only be an Object or null: %S", val_brief(rt, p));
    JSObject *o = new_object(rt, is_obj(p) ? vobj(p) : 0, CLS_OBJECT);
    push_tmp(rt, mkobj(o));
    if (!is_undef(ARG(1)) && define_props(rt, o, argv[1]) < 0) return JS_EXC;
    return mkobj(o);
}

NATIVE(object_get_own_names) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    JSObject *k = own_keys(rt, vobj(o), MAGIC == 0 ? KEYS_STR : KEYS_SYM);
    return k ? mkobj(k) : JS_EXC;
}

NATIVE(object_get_own_desc) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    push_tmp(rt, o);
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    PropDesc d;
    int e = get_own_desc(rt, vobj(o), k, &d);
    if (e < 0) return JS_EXC;
    if (!e) return JS_UNDEF;
    return desc_to_obj(rt, &d);
}
NATIVE(object_get_own_descs) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    push_tmp(rt, o);
    JSObject *keys = own_keys(rt, vobj(o), KEYS_STR | KEYS_SYM);
    if (!keys) return JS_EXC;
    push_tmp(rt, mkobj(keys));
    JSObject *r = new_plain(rt);
    push_tmp(rt, mkobj(r));
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        PropDesc d;
        int e = get_own_desc(rt, vobj(o), keys->u.arr.elems[i], &d);
        if (e < 0) return JS_EXC;
        if (e) create_data_prop(rt, r, keys->u.arr.elems[i], desc_to_obj(rt, &d), true);
    }
    return mkobj(r);
}

NATIVE(object_get_proto) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    return get_proto(rt, vobj(o));
}
NATIVE(object_set_proto) {
    jsval o = ARG(0), p = ARG(1);
    if (is_nullish(o)) THROW_TYPE("Object.setPrototypeOf called on null or undefined");
    if (!is_obj(p) && !is_null(p)) THROW_TYPE("Object prototype may only be an Object or null: %S", val_brief(rt, p));
    if (!is_obj(o)) return o;
    if (set_proto(rt, vobj(o), is_obj(p) ? vobj(p) : 0, true) < 0) return JS_EXC;
    return o;
}
NATIVE(object_is) { return mkbool(same_value(rt, ARG(0), ARG(1))); }
NATIVE(object_from_entries) {
    JSObject *r = new_plain(rt);
    push_tmp(rt, mkobj(r));
    jsval next;
    jsval it = get_iterator(rt, ARG(0), &next, false);
    TRY(it);
    push_tmp(rt, it);
    push_tmp(rt, next);
    for (;;) {
        jsval e;
        int s = iter_step(rt, it, next, &e);
        if (s < 0) return JS_EXC;
        if (!s) break;
        jsval *r2 = push_tmp(rt, e);
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
        jsval kk = to_key(rt, k);
        if (kk == JS_EXC) {
            iter_close(rt, it, true);
            return JS_EXC;
        }
        create_data_prop(rt, r, kk, v, true);
        rt->sp = r2;
    }
    return mkobj(r);
}
NATIVE(object_has_own) {
    jsval o = to_object(rt, ARG(0));
    TRY(o);
    push_tmp(rt, o);
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    int r = has_own(rt, vobj(o), k);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(object_group_by) {
    jsval cb = ARG(1);
    if (!is_callable(cb)) THROW_TYPE("%S is not a function", val_brief(rt, cb));
    bool is_map = MAGIC == 1;
    JSObject *r = is_map ? map_new(rt, false) : new_object(rt, 0, CLS_OBJECT);
    push_tmp(rt, mkobj(r));
    jsval next;
    jsval it = get_iterator(rt, ARG(0), &next, false);
    TRY(it);
    push_tmp(rt, it);
    push_tmp(rt, next);
    double idx = 0;
    for (;;) {
        jsval e;
        int s = iter_step(rt, it, next, &e);
        if (s < 0) return JS_EXC;
        if (!s) break;
        jsval *r2 = push_tmp(rt, e);
        jsval args[2] = {e, mknum(idx++)};
        jsval k = call_fn(rt, cb, JS_UNDEF, 2, args);
        if (k == JS_EXC) {
            iter_close(rt, it, true);
            return JS_EXC;
        }
        push_tmp(rt, k);
        jsval group;
        if (is_map) {
            if (is_num(k) && vnum(k) == 0) k = mknum(0);
            bool found;
            group = weak_get(rt, r, k, &found);
            if (!found) {
                group = mkobj(new_array(rt, 0));
                map_set_raw(rt, r, k, group);
            }
        } else {
            jsval kk = to_key(rt, k);
            if (kk == JS_EXC) {
                iter_close(rt, it, true);
                return JS_EXC;
            }
            Prop *p = find_own(rt, r, kk);
            if (p)
                group = p->val;
            else {
                group = mkobj(new_array(rt, 0));
                add_prop(rt, r, kk, group, P_DEFAULT);
            }
        }
        array_push(rt, vobj(group), e);
        rt->sp = r2;
    }
    return mkobj(r);
}

NATIVE(objproto_has_own) {
    jsval k = to_key(rt, ARG(0));
    TRY(k);
    push_tmp(rt, k);
    jsval o = to_object(rt, this_);
    TRY(o);
    int r = has_own(rt, vobj(o), k);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(objproto_is_proto_of) {
    jsval v = ARG(0);
    if (!is_obj(v)) return JS_FALSE;
    jsval o = to_object(rt, this_);
    TRY(o);
    JSObject *x = vobj(v);
    for (int g = 0; g < 100000; g++) {
        jsval p = get_proto(rt, x);
        TRY(p);
        if (!is_obj(p)) return JS_FALSE;
        if (vobj(p) == vobj(o)) return JS_TRUE;
        x = vobj(p);
    }
    return JS_FALSE;
}
NATIVE(objproto_prop_is_enum) {
    jsval k = to_key(rt, ARG(0));
    TRY(k);
    push_tmp(rt, k);
    jsval o = to_object(rt, this_);
    TRY(o);
    PropDesc d;
    int e = get_own_desc(rt, vobj(o), k, &d);
    if (e < 0) return JS_EXC;
    return mkbool(e && (d.flags & P_E));
}
static const char *class_tag(js_rt *rt, JSObject *o) {
    if (is_array(rt, mkobj(o))) return "Array";
    switch (o->cls) {
    case CLS_ARGUMENTS: return "Arguments";
    case CLS_ERROR: return "Error";
    case CLS_BOOLEAN: return "Boolean";
    case CLS_NUMBER: return "Number";
    case CLS_STRING: return "String";
    case CLS_DATE: return "Date";
    case CLS_REGEXP: return "RegExp";
    case CLS_HOST: return o->u.host.cls && o->u.host.cls->name ? o->u.host.cls->name : "Object";
    default: break;
    }
    if (o->oflags & OF_CALLABLE) return "Function";
    return "Object";
}
NATIVE(objproto_to_string) {
    if (is_undef(this_)) return mkstr(str_cstr(rt, "[object Undefined]"));
    if (is_null(this_)) return mkstr(str_cstr(rt, "[object Null]"));
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    const char *bt = class_tag(rt, vobj(o));
    jsval tag = get_p(rt, o, SYM(toStringTag));
    TRY(tag);
    if (is_str(tag)) return mkstr(jsfmt(rt, "[object %S]", vstr(tag)));
    return mkstr(jsfmt(rt, "[object %s]", bt));
}
NATIVE(objproto_to_locale) { return call_method(rt, this_, ATOM(toString), 0, 0); }
NATIVE(objproto_value_of) { return to_object(rt, this_); }
NATIVE(objproto_proto_get) {
    jsval o = to_object(rt, this_);
    TRY(o);
    return get_proto(rt, vobj(o));
}
NATIVE(objproto_proto_set) {
    if (is_nullish(this_)) THROW_TYPE("Object.prototype.__proto__ called on null or undefined");
    jsval p = ARG(0);
    if (!is_obj(p) && !is_null(p)) return JS_UNDEF;
    if (!is_obj(this_)) return JS_UNDEF;
    if (set_proto(rt, vobj(this_), is_obj(p) ? vobj(p) : 0, true) < 0) return JS_EXC;
    return JS_UNDEF;
}
NATIVE(objproto_define_getter) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    if (!is_callable(ARG(1))) THROW_TYPE("Object.prototype.__define%sGetter__: Expecting function", "");
    jsval k = to_key(rt, ARG(0));
    TRY(k);
    PropDesc d;
    memset(&d, 0, sizeof d);
    if (MAGIC == 0) {
        d.get = argv[1];
        d.has = HAS_GET | HAS_E | HAS_C;
    } else {
        d.set = argv[1];
        d.has = HAS_SET | HAS_E | HAS_C;
    }
    d.flags = P_E | P_C;
    if (define_own(rt, vobj(o), k, &d, true) < 0) return JS_EXC;
    return JS_UNDEF;
}

void init_object(js_rt *rt) {
    JSObject *op = rt->intr[I_ObjectProto];
    JSObject *ctor = new_ctor(rt, "Object", object_ctor, 1, op);
    rt->intr[I_Object] = ctor;
    def_method(rt, ctor, "keys", object_keys, 1, 0);
    def_method(rt, ctor, "values", object_keys, 1, 1);
    def_method(rt, ctor, "entries", object_keys, 1, 2);
    def_method(rt, ctor, "assign", object_assign, 2, 0);
    def_method(rt, ctor, "freeze", object_freeze, 1, 0);
    def_method(rt, ctor, "seal", object_freeze, 1, 1);
    def_method(rt, ctor, "preventExtensions", object_freeze, 1, 2);
    def_method(rt, ctor, "isFrozen", object_isfrozen, 1, 0);
    def_method(rt, ctor, "isSealed", object_isfrozen, 1, 1);
    def_method(rt, ctor, "isExtensible", object_isfrozen, 1, 2);
    def_method(rt, ctor, "defineProperty", object_define_property, 3, 0);
    def_method(rt, ctor, "defineProperties", object_define_properties, 2, 0);
    def_method(rt, ctor, "create", object_create, 2, 0);
    def_method(rt, ctor, "getOwnPropertyNames", object_get_own_names, 1, 0);
    def_method(rt, ctor, "getOwnPropertySymbols", object_get_own_names, 1, 1);
    def_method(rt, ctor, "getOwnPropertyDescriptor", object_get_own_desc, 2, 0);
    def_method(rt, ctor, "getOwnPropertyDescriptors", object_get_own_descs, 1, 0);
    def_method(rt, ctor, "getPrototypeOf", object_get_proto, 1, 0);
    def_method(rt, ctor, "setPrototypeOf", object_set_proto, 2, 0);
    def_method(rt, ctor, "is", object_is, 2, 0);
    def_method(rt, ctor, "fromEntries", object_from_entries, 1, 0);
    def_method(rt, ctor, "hasOwn", object_has_own, 2, 0);
    def_method(rt, ctor, "groupBy", object_group_by, 2, 0);
    def_method(rt, op, "hasOwnProperty", objproto_has_own, 1, 0);
    def_method(rt, op, "isPrototypeOf", objproto_is_proto_of, 1, 0);
    def_method(rt, op, "propertyIsEnumerable", objproto_prop_is_enum, 1, 0);
    def_method(rt, op, "toString", objproto_to_string, 0, 0);
    def_method(rt, op, "toLocaleString", objproto_to_locale, 0, 0);
    def_method(rt, op, "valueOf", objproto_value_of, 0, 0);
    def_method(rt, op, "__defineGetter__", objproto_define_getter, 2, 0);
    def_method(rt, op, "__defineSetter__", objproto_define_getter, 2, 1);
    JSObject *g = new_native(rt, "get __proto__", objproto_proto_get, 0, 0, false);
    JSObject *s = new_native(rt, "set __proto__", objproto_proto_set, 1, 0, false);
    def_acc(rt, op, ATOM(__proto__), mkobj(g), mkobj(s), P_C);
}

/* ---------------- Function ---------------- */
NATIVE(function_proto_noop) { return JS_UNDEF; }

static jsval build_function(js_rt *rt, int argc, jsval *argv, const char *prefix) {
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, "(");
    sb_puts(&b, prefix);
    sb_puts(&b, " anonymous(");
    for (int i = 0; i < argc - 1; i++) {
        JSString *s = to_string(rt, argv[i]);
        if (!s) {
            sb_free(&b);
            return JS_EXC;
        }
        if (i) sb_puts(&b, ",");
        sb_putstr(&b, s);
    }
    sb_puts(&b, "\n) {\n");
    if (argc > 0) {
        JSString *s = to_string(rt, argv[argc - 1]);
        if (!s) {
            sb_free(&b);
            return JS_EXC;
        }
        sb_putstr(&b, s);
    }
    sb_puts(&b, "\n})");
    JSString *src = sb_finish(&b);
    if (!src) return JS_EXC;
    push_tmp(rt, mkstr(src));
    return eval_code(rt, src, "anonymous", true);
}
NATIVE(function_ctor) {
    static const char *const pf[] = {"function", "function*", "async function", "async function*"};
    return build_function(rt, argc, argv, pf[MAGIC]);
}

NATIVE(func_call) {
    if (!is_callable(this_)) THROW_TYPE("Function.prototype.call called on non-function");
    return call_fn(rt, this_, ARG(0), argc > 0 ? argc - 1 : 0, argv + 1);
}
static int list_from_array_like(js_rt *rt, jsval a, jsval **out, uint32_t *n) {
    *out = 0;
    *n = 0;
    if (is_nullish(a)) return 0;
    if (!is_obj(a)) {
        throw_error(rt, E_TYPE, "CreateListFromArrayLike called on non-object");
        return -1;
    }
    uint64_t len;
    if (len_of(rt, a, &len) < 0) return -1;
    if (len > 100000) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    jsval *v = (jsval *)malloc((len ? len : 1) * sizeof(jsval));
    for (uint32_t i = 0; i < len; i++) {
        v[i] = get_idx(rt, a, i);
        if (v[i] == JS_EXC) {
            free(v);
            return -1;
        }
    }
    *out = v;
    *n = (uint32_t)len;
    return 0;
}
NATIVE(func_apply) {
    if (!is_callable(this_)) THROW_TYPE("Function.prototype.apply was called on %S, which is %s and not a function", val_brief(rt, this_), type_name(this_));
    jsval a = ARG(1);
    if (is_obj(a) && vobj(a)->cls == CLS_ARRAY && !(vobj(a)->oflags & OF_SLOW_ARRAY)) {
        JSObject *ao = vobj(a);
        uint32_t n = ao->u.arr.len;
        jsval *tmp = (jsval *)malloc((n ? n : 1) * sizeof(jsval));
        for (uint32_t i = 0; i < n; i++) tmp[i] = ao->u.arr.elems[i] == JS_HOLE ? JS_UNDEF : ao->u.arr.elems[i];
        jsval r = call_fn(rt, this_, ARG(0), (int)n, tmp);
        free(tmp);
        return r;
    }
    jsval *list;
    uint32_t n;
    if (list_from_array_like(rt, a, &list, &n) < 0) return JS_EXC;
    /* list values are copied onto the VM stack by call_fn before any GC can happen */
    jsval r = call_fn(rt, this_, ARG(0), (int)n, list);
    free(list);
    return r;
}
NATIVE(func_bind) {
    if (!is_callable(this_)) THROW_TYPE("Bind must be called on a function");
    JSObject *b = bound_create(rt, this_, ARG(0), argc > 1 ? argc - 1 : 0, argv + 1);
    push_tmp(rt, mkobj(b));
    double len = 0;
    int h = has_own(rt, vobj(this_), ATOM(length));
    if (h < 0) return JS_EXC;
    if (h) {
        jsval l = get_p(rt, this_, ATOM(length));
        TRY(l);
        if (is_num(l)) {
            double d = vnum(l);
            if (d == JS_INF) len = JS_INF;
            else if (d == -JS_INF) len = 0;
            else {
                d = to_integer(d) - (argc > 1 ? argc - 1 : 0);
                len = d > 0 ? d : 0;
            }
        }
    }
    jsval nm = get_p(rt, this_, ATOM(name));
    TRY(nm);
    JSString *ns = jsfmt(rt, "bound %S", is_str(nm) ? vstr(nm) : vstr(ATOM(empty)));
    add_prop(rt, b, ATOM(length), mknum(len), P_C);
    add_prop(rt, b, ATOM(name), mkstr(ns), P_C);
    return mkobj(b);
}

JSString *func_to_string(js_rt *rt, JSObject *f) {
    if (f->cls == CLS_FUNCTION) {
        FuncProto *fp = f->u.fn.fp;
        if (fp->src && fp->src_end > fp->src_start && fp->src_end <= fp->src->len)
            return str_utf8(rt, (const char *)fp->src->s8 + fp->src_start, fp->src_end - fp->src_start);
    }
    jsval nm = func_name_of(rt, f);
    return jsfmt(rt, "function %S() { [native code] }", vstr(nm));
}
NATIVE(func_to_string_n) {
    if (!is_callable(this_)) THROW_TYPE("Function.prototype.toString requires that 'this' be a Function");
    return mkstr(func_to_string(rt, vobj(this_)));
}
NATIVE(func_has_instance) {
    if (!is_callable(this_)) return JS_FALSE;
    jsval c = this_;
    for (int g = 0; vobj(c)->cls == CLS_BOUND && g < 1000; g++) c = vobj(c)->u.bound.target;
    jsval o = ARG(0);
    if (!is_obj(o)) return JS_FALSE;
    jsval p = get_p(rt, c, ATOM(prototype));
    TRY(p);
    if (!is_obj(p)) THROW_TYPE("Function has non-object prototype '%S' in instanceof check", val_brief(rt, p));
    JSObject *x = vobj(o);
    for (int g = 0; g < 100000; g++) {
        jsval pr = get_proto(rt, x);
        TRY(pr);
        if (!is_obj(pr)) return JS_FALSE;
        if (vobj(pr) == vobj(p)) return JS_TRUE;
        x = vobj(pr);
    }
    return JS_FALSE;
}

void init_function(js_rt *rt) {
    JSObject *fp = rt->intr[I_FunctionProto];
    JSObject *ctor = new_ctor(rt, "Function", function_ctor, 1, fp);
    rt->intr[I_Function] = ctor;
    def_method(rt, fp, "call", func_call, 1, 0);
    def_method(rt, fp, "apply", func_apply, 2, 0);
    def_method(rt, fp, "bind", func_bind, 1, 0);
    def_method(rt, fp, "toString", func_to_string_n, 0, 0);
    JSObject *hi = def_method_k(rt, fp, SYM(hasInstance), "[Symbol.hasInstance]", func_has_instance, 1, 0x7151);
    Prop *p = find_own(rt, fp, SYM(hasInstance));
    if (p) p->flags = 0;
    (void)hi;
    /* generator / async function constructors and prototypes */
    struct {
        int fproto, proto, ctor;
        const char *name, *tag;
        int magic;
    } kinds[] = {
        {I_GeneratorFunctionProto, I_GeneratorProto, I_GeneratorFunction, "GeneratorFunction", "GeneratorFunction", 1},
        {I_AsyncFunctionProto, -1, I_AsyncFunction, "AsyncFunction", "AsyncFunction", 2},
        {I_AsyncGeneratorFunctionProto, I_AsyncGeneratorProto, I_AsyncGeneratorFunction, "AsyncGeneratorFunction", "AsyncGeneratorFunction", 3},
    };
    for (int i = 0; i < 3; i++) {
        JSObject *pr = rt->intr[kinds[i].fproto];
        JSObject *c = new_native(rt, kinds[i].name, function_ctor, 1, kinds[i].magic, true);
        c->proto = ctor;
        materialize_fn(rt, c);
        def_prop(rt, c, ATOM(prototype), mkobj(pr), 0);
        def_prop(rt, pr, ATOM(constructor), mkobj(c), P_C);
        def_prop(rt, pr, SYM(toStringTag), mkstr(str_cstr(rt, kinds[i].tag)), P_C);
        if (kinds[i].proto >= 0) {
            def_prop(rt, pr, ATOM(prototype), mkobj(rt->intr[kinds[i].proto]), P_C);
            def_prop(rt, rt->intr[kinds[i].proto], ATOM(constructor), mkobj(pr), P_C);
        }
        rt->intr[kinds[i].ctor] = c;
    }
}

/* ---------------- Symbol ---------------- */
static JSSymbol *new_symbol(js_rt *rt, jsval desc) {
    JSSymbol *s = (JSSymbol *)gc_alloc(rt, sizeof(JSSymbol), GC_SYM);
    s->desc = desc;
    s->hash = (uint32_t)(++rt->sym_counter * 2654435761u) | 1;
    return s;
}
NATIVE(symbol_ctor) {
    if (!is_undef(rt->new_target)) THROW_TYPE("Symbol is not a constructor");
    jsval d = JS_UNDEF;
    if (!is_undef(ARG(0))) {
        JSString *s = to_string(rt, argv[0]);
        if (!s) return JS_EXC;
        d = mkstr(s);
    }
    return mksym(new_symbol(rt, d));
}
NATIVE(symbol_for) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    jsval k = atomize(rt, s);
    Prop *p = find_own(rt, rt->symbol_registry, k);
    if (p) return p->val;
    JSSymbol *sym = new_symbol(rt, k);
    sym->h.flags |= 2;
    add_prop(rt, rt->symbol_registry, k, mksym(sym), P_DEFAULT);
    return mksym(sym);
}
NATIVE(symbol_key_for) {
    jsval s = ARG(0);
    if (!is_sym(s)) THROW_TYPE("%S is not a symbol", val_brief(rt, s));
    if (vsym(s)->h.flags & 2) return vsym(s)->desc;
    return JS_UNDEF;
}
static jsval this_symbol(js_rt *rt, jsval t) {
    if (is_sym(t)) return t;
    if (is_cls(t, CLS_SYMBOL)) return vobj(t)->u.prim.v;
    return throw_error(rt, E_TYPE, "Symbol.prototype method called on incompatible receiver");
}
NATIVE(symbol_to_string) {
    jsval s = this_symbol(rt, this_);
    TRY(s);
    return key_to_display(rt, s);
}
NATIVE(symbol_value_of) { return this_symbol(rt, this_); }
NATIVE(symbol_description) {
    jsval s = this_symbol(rt, this_);
    TRY(s);
    return vsym(s)->desc;
}

void init_symbol(js_rt *rt) {
    JSObject *sp = rt->intr[I_SymbolProto];
    JSObject *ctor = new_ctor(rt, "Symbol", symbol_ctor, 0, sp);
    rt->intr[I_Symbol] = ctor;
    static const char *const names[] = {
#define X(id) #id,
        SYM_LIST(X)
#undef X
    };
    for (int i = 0; i < S__COUNT; i++) def_prop(rt, ctor, atom_c(rt, names[i]), rt->syms[i], 0);
    def_method(rt, ctor, "for", symbol_for, 1, 0);
    def_method(rt, ctor, "keyFor", symbol_key_for, 1, 0);
    def_method(rt, sp, "toString", symbol_to_string, 0, 0);
    def_method(rt, sp, "valueOf", symbol_value_of, 0, 0);
    def_getter(rt, sp, "description", symbol_description, 0);
    def_prop(rt, sp, SYM(toStringTag), mkstr(str_cstr(rt, "Symbol")), P_C);
    JSObject *tp = new_native(rt, "[Symbol.toPrimitive]", symbol_value_of, 1, 0, false);
    def_prop(rt, sp, SYM(toPrimitive), mkobj(tp), P_C);
}

/* ---------------- Error ---------------- */
static const char *const err_names[E__COUNT] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
                                                 "URIError", "EvalError", "AggregateError", "InternalError"};

NATIVE(error_ctor) {
    int type = MAGIC;
    jsval nt = rt->new_target;
    if (is_undef(nt)) nt = mkobj(rt->intr[I_Error]); /* proto lookup below uses the intrinsic for plain calls */
    JSObject *proto = is_undef(rt->new_target) ? rt->intr[I_ErrorProto + type] : proto_from_ctor(rt, nt, I_ErrorProto + type);
    if (!proto) return JS_EXC;
    JSObject *e = new_object(rt, proto, CLS_ERROR);
    push_tmp(rt, mkobj(e));
    int mi = type == E_AGGREGATE ? 1 : 0;
    jsval msg = ARG(mi);
    if (!is_undef(msg)) {
        JSString *s = to_string(rt, msg);
        if (!s) return JS_EXC;
        add_prop(rt, e, ATOM(message), mkstr(s), P_W | P_C);
    }
    jsval opts = ARG(mi + 1);
    if (is_obj(opts)) {
        int h = has_prop(rt, opts, ATOM(cause));
        if (h < 0) return JS_EXC;
        if (h) {
            jsval c = get_p(rt, opts, ATOM(cause));
            TRY(c);
            add_prop(rt, e, ATOM(cause), c, P_W | P_C);
        }
    }
    if (type == E_AGGREGATE) {
        JSObject *errs = iter_to_array(rt, ARG(0));
        if (!errs) return JS_EXC;
        add_prop(rt, e, ATOM(errors), mkobj(errs), P_W | P_C);
    }
    int line;
    JSString *file;
    JSString *st = capture_stack(rt, &line, &file);
    e->u.err.line = line;
    e->u.err.file = file;
    /* header: "<name>: <message>" using the name visible through the prototype chain */
    jsval nm = get_p(rt, mkobj(e), ATOM(name));
    if (nm == JS_EXC) {
        rt->has_exc = false;
        rt->exc = JS_UNDEF;
        nm = JS_UNDEF;
    }
    SB b;
    sb_init(&b, rt);
    if (is_str(nm))
        sb_putstr(&b, vstr(nm));
    else
        sb_puts(&b, err_names[type]);
    Prop *mp = find_own(rt, e, ATOM(message));
    if (mp && is_str(mp->val) && vstr(mp->val)->len) {
        sb_puts(&b, ": ");
        sb_putstr(&b, vstr(mp->val));
    }
    if (st) sb_putstr(&b, st);
    add_prop(rt, e, ATOM(stack), mkstr(sb_finish(&b)), P_W | P_C);
    return mkobj(e);
}

JSString *error_to_string(js_rt *rt, jsval o) {
    jsval *root = push_tmp(rt, o);
    jsval n = get_p(rt, o, ATOM(name));
    if (n == JS_EXC) {
        rt->sp = root;
        return 0;
    }
    JSString *ns = is_undef(n) ? str_cstr(rt, "Error") : to_string(rt, n);
    if (!ns) {
        rt->sp = root;
        return 0;
    }
    push_tmp(rt, mkstr(ns));
    jsval m = get_p(rt, o, ATOM(message));
    if (m == JS_EXC) {
        rt->sp = root;
        return 0;
    }
    JSString *ms = is_undef(m) ? vstr(ATOM(empty)) : to_string(rt, m);
    rt->sp = root;
    if (!ms) return 0;
    if (ns->len == 0) return ms;
    if (ms->len == 0) return ns;
    return jsfmt(rt, "%S: %S", ns, ms);
}
NATIVE(error_to_string_n) {
    if (!is_obj(this_)) THROW_TYPE("Error.prototype.toString called on non-object");
    JSString *s = error_to_string(rt, this_);
    return s ? mkstr(s) : JS_EXC;
}
NATIVE(error_capture_stack) {
    jsval o = ARG(0);
    if (!is_obj(o)) THROW_TYPE("Invalid argument");
    int line;
    JSString *file;
    JSString *st = capture_stack(rt, &line, &file);
    JSString *head = error_to_string(rt, o);
    if (!head) return JS_EXC;
    JSString *full = st ? str_concat(rt, head, st) : head;
    def_prop(rt, vobj(o), ATOM(stack), mkstr(full), P_W | P_C);
    return JS_UNDEF;
}

void init_error(js_rt *rt) {
    JSObject *base = 0;
    for (int t = 0; t < E__COUNT; t++) {
        JSObject *p = rt->intr[I_ErrorProto + t];
        JSObject *c = new_ctor(rt, err_names[t], error_ctor, t == E_AGGREGATE ? 2 : 1, p);
        c->u.nat.data = (void *)(intptr_t)t;
        def_value(rt, p, "name", mkstr(str_cstr(rt, err_names[t])));
        def_value(rt, p, "message", ATOM(empty));
        if (t == 0) {
            base = c;
            rt->intr[I_Error] = c;
            def_method(rt, p, "toString", error_to_string_n, 0, 0);
            def_method(rt, c, "captureStackTrace", error_capture_stack, 2, 0);
            def_value(rt, c, "stackTraceLimit", mknum(10));
        } else {
            c->proto = base;
        }
        if (t == E_INTERNAL) {
            /* not a standard global */
            Prop *gp = find_own(rt, rt->global, atom_c(rt, "InternalError"));
            if (gp) del_prop_at(rt, rt->global, gp);
        }
    }
}

/* ---------------- Boolean ---------------- */
NATIVE(boolean_ctor) {
    jsval b = mkbool(truthy(ARG(0)));
    if (is_undef(rt->new_target)) return b;
    JSObject *p = proto_from_ctor(rt, rt->new_target, I_BooleanProto);
    if (!p) return JS_EXC;
    JSObject *o = new_object(rt, p, CLS_BOOLEAN);
    o->u.prim.v = b;
    return mkobj(o);
}
static jsval this_bool(js_rt *rt, jsval t) {
    if (is_boolv(t)) return t;
    if (is_cls(t, CLS_BOOLEAN)) return vobj(t)->u.prim.v;
    return throw_error(rt, E_TYPE, "Boolean.prototype method called on incompatible receiver");
}
NATIVE(boolean_to_string) {
    jsval b = this_bool(rt, this_);
    TRY(b);
    return b == JS_TRUE ? ATOM(true) : ATOM(false);
}
NATIVE(boolean_value_of) { return this_bool(rt, this_); }
static void init_boolean(js_rt *rt) {
    JSObject *p = rt->intr[I_BooleanProto];
    p->u.prim.v = JS_FALSE;
    new_ctor(rt, "Boolean", boolean_ctor, 1, p);
    def_method(rt, p, "toString", boolean_to_string, 0, 0);
    def_method(rt, p, "valueOf", boolean_value_of, 0, 0);
}

/* ---------------- globals ---------------- */
NATIVE(g_parse_int) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    push_tmp(rt, mkstr(s));
    double rd;
    if (to_int(rt, ARG(1), &rd) < 0) return JS_EXC;
    int radix = (int)(int32_t)to_int32(rd);
    str_flat(rt, s);
    uint32_t i = 0, n = s->len;
    while (i < n && uc_is_space(str_at(s, i))) i++;
    bool neg = false;
    if (i < n && (str_at(s, i) == '+' || str_at(s, i) == '-')) {
        neg = str_at(s, i) == '-';
        i++;
    }
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return mknum(JS_NAN);
        if (radix != 16) strip = false;
    } else
        radix = 10;
    if (strip && i + 1 < n && str_at(s, i) == '0' && (str_at(s, i + 1) | 0x20) == 'x') {
        i += 2;
        radix = 16;
    }
    uint32_t len = n - i;
    char stackbuf[128];
    char *buf = len < sizeof stackbuf ? stackbuf : (char *)malloc(len + 1);
    uint32_t k = 0;
    for (; i < n; i++) {
        uint16_t c = str_at(s, i);
        if (c > 127) break;
        buf[k++] = (char)c;
    }
    size_t used;
    double v = parse_int_digits(buf, k, radix, &used);
    if (radix == 10 && used > 0) {
        /* parseInt reads only integer digits */
        size_t u2;
        v = js_strtod(buf, used, &u2);
    }
    if (buf != stackbuf) free(buf);
    if (used == 0) return mknum(JS_NAN);
    return mknum(neg ? -v : v);
}
NATIVE(g_parse_float) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    uint32_t i = 0, n = s->len;
    while (i < n && uc_is_space(str_at(s, i))) i++;
    uint32_t len = n - i;
    char stackbuf[256];
    char *buf = len < sizeof stackbuf ? stackbuf : (char *)malloc(len + 1);
    uint32_t k = 0;
    for (; i < n; i++) {
        uint16_t c = str_at(s, i);
        if (c > 127) break;
        buf[k++] = (char)c;
    }
    buf[k] = 0;
    double r = JS_NAN;
    const char *p = buf;
    size_t pl = k;
    bool neg = false;
    if (pl && (p[0] == '+' || p[0] == '-')) {
        neg = p[0] == '-';
        p++;
        pl--;
    }
    if (pl >= 8 && memcmp(p, "Infinity", 8) == 0)
        r = JS_INF;
    else {
        size_t used;
        double v = js_strtod(p, pl, &used);
        if (used) r = v;
    }
    if (buf != stackbuf) free(buf);
    return mknum(neg ? -r : r);
}
NATIVE(g_is_nan) {
    double d;
    if (to_number(rt, ARG(0), &d) < 0) return JS_EXC;
    return mkbool(d != d);
}
NATIVE(g_is_finite) {
    double d;
    if (to_number(rt, ARG(0), &d) < 0) return JS_EXC;
    return mkbool(js_isfinite(d));
}
NATIVE(g_eval) {
    jsval x = ARG(0);
    if (!is_str(x)) return x;
    return eval_code(rt, vstr(x), "eval", false);
}

static const char hexd[] = "0123456789ABCDEF";
static bool uri_unreserved(uint32_t c, int mode) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    if (c && strchr_safe("-_.!~*'()", c)) return true;
    if (mode == 1 && c && strchr_safe(";/?:@&=+$,#", c)) return true;
    return false;
}
NATIVE(g_encode_uri) {
    int mode = MAGIC; /* 0 component, 1 uri */
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    SB b;
    sb_init(&b, rt);
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        if (c < 128 && uri_unreserved(c, mode)) {
            sb_putc(&b, c);
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) {
            sb_free(&b);
            return throw_error(rt, E_URI, "URI malformed");
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s->len) {
                sb_free(&b);
                return throw_error(rt, E_URI, "URI malformed");
            }
            uint32_t d = str_at(s, i + 1);
            if (d < 0xDC00 || d > 0xDFFF) {
                sb_free(&b);
                return throw_error(rt, E_URI, "URI malformed");
            }
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            i++;
        }
        char u[4];
        int n = utf8_encode(c, u);
        for (int k = 0; k < n; k++) {
            sb_putc(&b, '%');
            sb_putc(&b, (uint8_t)hexd[((uint8_t)u[k]) >> 4]);
            sb_putc(&b, (uint8_t)hexd[((uint8_t)u[k]) & 15]);
        }
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}
static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}
NATIVE(g_decode_uri) {
    int mode = MAGIC; /* 0 component, 1 uri */
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    SB b;
    sb_init(&b, rt);
    uint32_t n = s->len;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c != '%') {
            sb_putc(&b, c);
            continue;
        }
        uint32_t start = i;
        if (i + 2 >= n || hexval(str_at(s, i + 1)) < 0 || hexval(str_at(s, i + 2)) < 0) goto bad;
        uint8_t b0 = (uint8_t)(hexval(str_at(s, i + 1)) * 16 + hexval(str_at(s, i + 2)));
        i += 2;
        if (b0 < 0x80) {
            if (mode == 1 && strchr_safe(";/?:@&=+$,#", b0)) {
                for (uint32_t k = start; k <= i; k++) sb_putc(&b, str_at(s, k));
            } else
                sb_putc(&b, b0);
            continue;
        }
        int len = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) goto bad;
        uint8_t u[4];
        u[0] = b0;
        for (int k = 1; k < len; k++) {
            if (i + 3 > n - 0 || str_at(s, i + 1) != '%' || i + 3 >= n + 0 || hexval(str_at(s, i + 2)) < 0 ||
                hexval(str_at(s, i + 3)) < 0)
                goto bad;
            u[k] = (uint8_t)(hexval(str_at(s, i + 2)) * 16 + hexval(str_at(s, i + 3)));
            if ((u[k] & 0xC0) != 0x80) goto bad;
            i += 3;
        }
        uint32_t cp;
        int used = utf8_decode(u, (size_t)len, &cp);
        if (used != len || cp == 0xFFFD || (cp >= 0xD800 && cp <= 0xDFFF)) goto bad;
        sb_putcp(&b, cp);
    }
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
bad:
    sb_free(&b);
    return throw_error(rt, E_URI, "URI malformed");
}
NATIVE(g_escape) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    SB b;
    sb_init(&b, rt);
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr_safe("@*_+-./", c))) {
            sb_putc(&b, c);
        } else if (c < 256) {
            sb_putc(&b, '%');
            sb_putc(&b, (uint8_t)hexd[c >> 4]);
            sb_putc(&b, (uint8_t)hexd[c & 15]);
        } else {
            sb_puts(&b, "%u");
            sb_putc(&b, (uint8_t)hexd[(c >> 12) & 15]);
            sb_putc(&b, (uint8_t)hexd[(c >> 8) & 15]);
            sb_putc(&b, (uint8_t)hexd[(c >> 4) & 15]);
            sb_putc(&b, (uint8_t)hexd[c & 15]);
        }
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}
NATIVE(g_unescape) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    SB b;
    sb_init(&b, rt);
    uint32_t n = s->len;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c == '%') {
            if (i + 5 < n && str_at(s, i + 1) == 'u') {
                int h1 = hexval(str_at(s, i + 2)), h2 = hexval(str_at(s, i + 3)), h3 = hexval(str_at(s, i + 4)),
                    h4 = hexval(str_at(s, i + 5));
                if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                    sb_putc(&b, (uint32_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4));
                    i += 5;
                    continue;
                }
            }
            if (i + 2 < n) {
                int h1 = hexval(str_at(s, i + 1)), h2 = hexval(str_at(s, i + 2));
                if (h1 >= 0 && h2 >= 0) {
                    sb_putc(&b, (uint32_t)(h1 * 16 + h2));
                    i += 2;
                    continue;
                }
            }
        }
        sb_putc(&b, c);
    }
    JSString *r = sb_finish(&b);
    return r ? mkstr(r) : JS_EXC;
}

void init_global(js_rt *rt) {
    JSObject *g = rt->global;
    def_value(rt, g, "globalThis", mkobj(g));
    def_prop(rt, g, ATOM(NaN), mknum(JS_NAN), 0);
    def_prop(rt, g, ATOM(Infinity), mknum(JS_INF), 0);
    def_prop(rt, g, ATOM(undefined), JS_UNDEF, 0);
    JSObject *pi = def_method(rt, g, "parseInt", g_parse_int, 2, 0);
    JSObject *pf = def_method(rt, g, "parseFloat", g_parse_float, 1, 0);
    def_method(rt, g, "isNaN", g_is_nan, 1, 0);
    def_method(rt, g, "isFinite", g_is_finite, 1, 0);
    rt->intr[I_Eval] = def_method(rt, g, "eval", g_eval, 1, 0);
    def_method(rt, g, "encodeURIComponent", g_encode_uri, 1, 0);
    def_method(rt, g, "encodeURI", g_encode_uri, 1, 1);
    def_method(rt, g, "decodeURIComponent", g_decode_uri, 1, 0);
    def_method(rt, g, "decodeURI", g_decode_uri, 1, 1);
    def_method(rt, g, "escape", g_escape, 1, 0);
    def_method(rt, g, "unescape", g_unescape, 1, 0);
    init_boolean(rt);
    /* Number.parseInt === parseInt */
    JSObject *num = 0;
    Prop *np = find_own(rt, g, atom_c(rt, "Number"));
    if (np) num = vobj(np->val);
    if (num) {
        def_value(rt, num, "parseInt", mkobj(pi));
        def_value(rt, num, "parseFloat", mkobj(pf));
    }
}

/* ---------------- Reflect ---------------- */
NATIVE(reflect_apply) {
    if (!is_callable(ARG(0))) THROW_TYPE("Function.prototype.apply was called on %S, which is %s and not a function", val_brief(rt, ARG(0)), type_name(ARG(0)));
    jsval *list;
    uint32_t n;
    if (!is_obj(ARG(2))) THROW_TYPE("CreateListFromArrayLike called on non-object");
    if (list_from_array_like(rt, argv[2], &list, &n) < 0) return JS_EXC;
    jsval r = call_fn(rt, argv[0], ARG(1), (int)n, list);
    free(list);
    return r;
}
NATIVE(reflect_construct) {
    if (!is_ctor(ARG(0))) THROW_TYPE("%S is not a constructor", val_brief(rt, ARG(0)));
    jsval nt = argc > 2 ? argv[2] : argv[0];
    if (!is_ctor(nt)) THROW_TYPE("%S is not a constructor", val_brief(rt, nt));
    jsval *list;
    uint32_t n;
    if (!is_obj(ARG(1))) THROW_TYPE("CreateListFromArrayLike called on non-object");
    if (list_from_array_like(rt, argv[1], &list, &n) < 0) return JS_EXC;
    jsval r = construct_fn(rt, argv[0], (int)n, list, nt);
    free(list);
    return r;
}
static JSObject *reflect_target(js_rt *rt, jsval v, const char *fn) {
    if (!is_obj(v)) {
        throw_error(rt, E_TYPE, "Reflect.%s called on non-object", fn);
        return 0;
    }
    return vobj(v);
}
NATIVE(reflect_define) {
    JSObject *o = reflect_target(rt, ARG(0), "defineProperty");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    push_tmp(rt, k);
    PropDesc d;
    if (obj_to_desc(rt, ARG(2), &d) < 0) return JS_EXC;
    int r = define_own(rt, o, k, &d, false);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_delete) {
    JSObject *o = reflect_target(rt, ARG(0), "deleteProperty");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    int r = delete_prop(rt, argv[0], k, false);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_get) {
    JSObject *o = reflect_target(rt, ARG(0), "get");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    return get_prop(rt, argv[0], k, argc > 2 ? argv[2] : argv[0]);
}
NATIVE(reflect_set) {
    JSObject *o = reflect_target(rt, ARG(0), "set");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    int r = set_prop(rt, argv[0], k, ARG(2), argc > 3 ? argv[3] : argv[0], false);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_gopd) {
    JSObject *o = reflect_target(rt, ARG(0), "getOwnPropertyDescriptor");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    PropDesc d;
    int e = get_own_desc(rt, o, k, &d);
    if (e < 0) return JS_EXC;
    return e ? desc_to_obj(rt, &d) : JS_UNDEF;
}
NATIVE(reflect_get_proto) {
    JSObject *o = reflect_target(rt, ARG(0), "getPrototypeOf");
    if (!o) return JS_EXC;
    return get_proto(rt, o);
}
NATIVE(reflect_set_proto) {
    JSObject *o = reflect_target(rt, ARG(0), "setPrototypeOf");
    if (!o) return JS_EXC;
    jsval p = ARG(1);
    if (!is_obj(p) && !is_null(p)) THROW_TYPE("Object prototype may only be an Object or null: %S", val_brief(rt, p));
    int r = set_proto(rt, o, is_obj(p) ? vobj(p) : 0, false);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_has) {
    JSObject *o = reflect_target(rt, ARG(0), "has");
    if (!o) return JS_EXC;
    jsval k = to_key(rt, ARG(1));
    TRY(k);
    int r = has_prop(rt, argv[0], k);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_is_ext) {
    JSObject *o = reflect_target(rt, ARG(0), "isExtensible");
    if (!o) return JS_EXC;
    int r = is_extensible(rt, o);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}
NATIVE(reflect_own_keys) {
    JSObject *o = reflect_target(rt, ARG(0), "ownKeys");
    if (!o) return JS_EXC;
    JSObject *k = own_keys(rt, o, KEYS_STR | KEYS_SYM);
    return k ? mkobj(k) : JS_EXC;
}
NATIVE(reflect_prevent_ext) {
    JSObject *o = reflect_target(rt, ARG(0), "preventExtensions");
    if (!o) return JS_EXC;
    int r = prevent_ext(rt, o);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}

/* ---------------- Proxy ---------------- */
static jsval proxy_trap(js_rt *rt, JSObject *p, const char *name, jsval *target) {
    if (is_null(p->u.it.b) || is_undef(p->u.it.b)) return throw_error(rt, E_TYPE, "Cannot perform '%s' on a proxy that has been revoked", name);
    *target = p->u.it.a;
    jsval t = get_c(rt, p->u.it.b, name);
    if (t == JS_EXC) return JS_EXC;
    if (is_nullish(t)) return JS_UNDEF;
    if (!is_callable(t)) return throw_error(rt, E_TYPE, "%S is not a function", val_brief(rt, t));
    return t;
}
static jsval keyval(jsval k) { return k; }

jsval proxy_get(js_rt *rt, JSObject *p, jsval key, jsval receiver) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "get", &target);
    if (t == JS_EXC) return JS_EXC;
    if (is_undef(t)) {
        rt->sp = root;
        return get_prop(rt, target, key, receiver);
    }
    jsval args[3] = {target, keyval(key), receiver};
    jsval r = call_fn(rt, t, p->u.it.b, 3, args);
    rt->sp = root;
    return r;
}
int proxy_set(js_rt *rt, JSObject *p, jsval key, jsval v, jsval receiver) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "set", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return set_prop(rt, target, key, v, receiver, false);
    }
    jsval args[4] = {target, key, v, receiver};
    jsval r = call_fn(rt, t, p->u.it.b, 4, args);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    if (!truthy(r)) {
        jsval kd = key_to_display(rt, key);
        throw_error(rt, E_TYPE, "'set' on proxy: trap returned falsish for property '%S'", vstr(kd));
        return -1;
    }
    return 1;
}
int proxy_has(js_rt *rt, JSObject *p, jsval key) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "has", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return has_prop(rt, target, key);
    }
    jsval args[2] = {target, key};
    jsval r = call_fn(rt, t, p->u.it.b, 2, args);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
int proxy_delete(js_rt *rt, JSObject *p, jsval key) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "deleteProperty", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return delete_prop(rt, target, key, false);
    }
    jsval args[2] = {target, key};
    jsval r = call_fn(rt, t, p->u.it.b, 2, args);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
int proxy_get_own_desc(js_rt *rt, JSObject *p, jsval key, PropDesc *d) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "getOwnPropertyDescriptor", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return get_own_desc(rt, vobj(target), key, d);
    }
    jsval args[2] = {target, key};
    jsval r = call_fn(rt, t, p->u.it.b, 2, args);
    if (r == JS_EXC) {
        rt->sp = root;
        return -1;
    }
    if (is_undef(r)) {
        rt->sp = root;
        return 0;
    }
    push_tmp(rt, r);
    int e = obj_to_desc(rt, r, d);
    rt->sp = root;
    if (e < 0) return -1;
    if (!(d->has & HAS_GET) && !(d->has & HAS_SET)) d->has |= HAS_VALUE | HAS_W;
    d->has |= HAS_E | HAS_C;
    return 1;
}
int proxy_define(js_rt *rt, JSObject *p, jsval key, PropDesc *d) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "defineProperty", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return define_own(rt, vobj(target), key, d, false);
    }
    jsval dobj = desc_to_obj(rt, d);
    jsval args[3] = {target, key, dobj};
    jsval r = call_fn(rt, t, p->u.it.b, 3, args);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
JSObject *proxy_own_keys(js_rt *rt, JSObject *p) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "ownKeys", &target);
    if (t == JS_EXC) return 0;
    if (is_undef(t)) {
        rt->sp = root;
        return own_keys(rt, vobj(target), KEYS_STR | KEYS_SYM);
    }
    jsval r = call_fn(rt, t, p->u.it.b, 1, &target);
    if (r == JS_EXC) {
        rt->sp = root;
        return 0;
    }
    push_tmp(rt, r);
    if (!is_obj(r)) {
        throw_error(rt, E_TYPE, "CreateListFromArrayLike called on non-object");
        rt->sp = root;
        return 0;
    }
    uint64_t len;
    if (len_of(rt, r, &len) < 0) {
        rt->sp = root;
        return 0;
    }
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    for (uint32_t i = 0; i < len; i++) {
        jsval k = get_idx(rt, r, i);
        if (k == JS_EXC) {
            rt->sp = root;
            return 0;
        }
        if (!is_str(k) && !is_sym(k)) {
            throw_error(rt, E_TYPE, "%S is not a valid property name", val_brief(rt, k));
            rt->sp = root;
            return 0;
        }
        array_push(rt, a, is_str(k) ? atomize(rt, vstr(k)) : k);
    }
    rt->sp = root;
    return a;
}
jsval proxy_get_proto(js_rt *rt, JSObject *p) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "getPrototypeOf", &target);
    if (t == JS_EXC) return JS_EXC;
    if (is_undef(t)) {
        rt->sp = root;
        return get_proto(rt, vobj(target));
    }
    jsval r = call_fn(rt, t, p->u.it.b, 1, &target);
    rt->sp = root;
    if (r == JS_EXC) return JS_EXC;
    if (!is_obj(r) && !is_null(r)) return throw_error(rt, E_TYPE, "'getPrototypeOf' on proxy: trap returned neither object nor null");
    return r;
}
int proxy_set_proto(js_rt *rt, JSObject *p, JSObject *proto) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "setPrototypeOf", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return set_proto(rt, vobj(target), proto, false);
    }
    jsval args[2] = {target, proto ? mkobj(proto) : JS_NULL};
    jsval r = call_fn(rt, t, p->u.it.b, 2, args);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
int proxy_prevent_ext(js_rt *rt, JSObject *p) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "preventExtensions", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return prevent_ext(rt, vobj(target));
    }
    jsval r = call_fn(rt, t, p->u.it.b, 1, &target);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
int proxy_is_ext(js_rt *rt, JSObject *p) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "isExtensible", &target);
    if (t == JS_EXC) return -1;
    if (is_undef(t)) {
        rt->sp = root;
        return is_extensible(rt, vobj(target));
    }
    jsval r = call_fn(rt, t, p->u.it.b, 1, &target);
    rt->sp = root;
    if (r == JS_EXC) return -1;
    return truthy(r);
}
jsval proxy_call(js_rt *rt, JSObject *p, jsval this_, int argc, jsval *argv) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "apply", &target);
    if (t == JS_EXC) return JS_EXC;
    if (is_undef(t)) {
        rt->sp = root;
        return call_fn(rt, target, this_, argc, argv);
    }
    JSObject *args = new_array_from(rt, argv, (uint32_t)argc);
    jsval a[3] = {target, this_, mkobj(args)};
    jsval r = call_fn(rt, t, p->u.it.b, 3, a);
    rt->sp = root;
    return r;
}
jsval proxy_construct(js_rt *rt, JSObject *p, int argc, jsval *argv, jsval nt) {
    jsval target;
    jsval *root = push_tmp(rt, mkobj(p));
    jsval t = proxy_trap(rt, p, "construct", &target);
    if (t == JS_EXC) return JS_EXC;
    if (is_undef(t)) {
        rt->sp = root;
        return construct_fn(rt, target, argc, argv, nt == mkobj(p) ? target : nt);
    }
    JSObject *args = new_array_from(rt, argv, (uint32_t)argc);
    jsval a[3] = {target, mkobj(args), nt};
    jsval r = call_fn(rt, t, p->u.it.b, 3, a);
    rt->sp = root;
    if (r == JS_EXC) return JS_EXC;
    if (!is_obj(r)) return throw_error(rt, E_TYPE, "proxy [[Construct]] must return an object");
    return r;
}

static jsval make_proxy(js_rt *rt, jsval target, jsval handler) {
    if (!is_obj(target) || !is_obj(handler)) return throw_error(rt, E_TYPE, "Cannot create proxy with a non-object as target or handler");
    JSObject *p = new_object(rt, 0, CLS_PROXY);
    p->u.it.a = target;
    p->u.it.b = handler;
    p->oflags |= vobj(target)->oflags & (OF_CALLABLE | OF_CTOR);
    return mkobj(p);
}
NATIVE(proxy_ctor) {
    if (is_undef(rt->new_target)) THROW_TYPE("Constructor Proxy requires 'new'");
    return make_proxy(rt, ARG(0), ARG(1));
}
NATIVE(proxy_revoke) {
    JSObject *f = vobj(rt->cur_this_fn ? mkobj(rt->cur_this_fn) : JS_UNDEF);
    (void)f;
    return JS_UNDEF;
}
NATIVE(proxy_revoke_fn) {
    JSObject *holder = (JSObject *)data;
    if (holder && is_obj(holder->u.it.a)) {
        JSObject *p = vobj(holder->u.it.a);
        p->u.it.a = JS_NULL;
        p->u.it.b = JS_NULL;
        holder->u.it.a = JS_NULL;
    }
    return JS_UNDEF;
}
NATIVE(proxy_revocable) {
    jsval p = make_proxy(rt, ARG(0), ARG(1));
    TRY(p);
    push_tmp(rt, p);
    JSObject *holder = new_object(rt, 0, CLS_INTERNAL);
    holder->u.it.a = p;
    holder->u.it.b = JS_UNDEF;
    JSObject *rf = new_native_k(rt, ATOM(empty), proxy_revoke_fn, 0, holder, false);
    /* keep holder alive through the function's property */
    add_prop(rt, rf, atom_c(rt, "%holder"), mkobj(holder), 0);
    JSObject *r = new_plain(rt);
    add_prop(rt, r, ATOM(proxy), p, P_DEFAULT);
    add_prop(rt, r, ATOM(revoke), mkobj(rf), P_DEFAULT);
    return mkobj(r);
}

void init_reflect(js_rt *rt) {
    JSObject *r = new_plain(rt);
    def_value(rt, rt->global, "Reflect", mkobj(r));
    def_prop(rt, r, SYM(toStringTag), mkstr(str_cstr(rt, "Reflect")), P_C);
    def_method(rt, r, "apply", reflect_apply, 3, 0);
    def_method(rt, r, "construct", reflect_construct, 2, 0);
    def_method(rt, r, "defineProperty", reflect_define, 3, 0);
    def_method(rt, r, "deleteProperty", reflect_delete, 2, 0);
    def_method(rt, r, "get", reflect_get, 2, 0);
    def_method(rt, r, "set", reflect_set, 3, 0);
    def_method(rt, r, "getOwnPropertyDescriptor", reflect_gopd, 2, 0);
    def_method(rt, r, "getPrototypeOf", reflect_get_proto, 1, 0);
    def_method(rt, r, "setPrototypeOf", reflect_set_proto, 2, 0);
    def_method(rt, r, "has", reflect_has, 2, 0);
    def_method(rt, r, "isExtensible", reflect_is_ext, 1, 0);
    def_method(rt, r, "ownKeys", reflect_own_keys, 1, 0);
    def_method(rt, r, "preventExtensions", reflect_prevent_ext, 1, 0);
    JSObject *pc = new_native(rt, "Proxy", proxy_ctor, 2, 0, true);
    def_value(rt, rt->global, "Proxy", mkobj(pc));
    def_method(rt, pc, "revocable", proxy_revocable, 2, 0);
    (void)proxy_revoke;
}

/* ---------------- init & GC class hooks ---------------- */
void init_builtins(js_rt *rt) {
    JSObject *op = new_object(rt, 0, CLS_OBJECT);
    rt->intr[I_ObjectProto] = op;
    JSObject *fp = new_object(rt, op, CLS_NATIVE);
    fp->oflags |= OF_CALLABLE;
    fp->u.nat.fn = function_proto_noop;
    fp->u.nat.name = ATOM(empty);
    rt->intr[I_FunctionProto] = fp;
    add_prop(rt, fp, ATOM(length), mknum(0), P_C);
    add_prop(rt, fp, ATOM(name), ATOM(empty), P_C);
    rt->global = new_object(rt, op, CLS_OBJECT);
    rt->global_lex = new_object(rt, 0, CLS_OBJECT);
    rt->symbol_registry = new_object(rt, 0, CLS_OBJECT);
    /* prototypes that other builtins refer to */
    rt->intr[I_ArrayProto] = new_object(rt, op, CLS_ARRAY);
    for (int t = 0; t < E__COUNT; t++)
        rt->intr[I_ErrorProto + t] = new_object(rt, t ? rt->intr[I_ErrorProto] : op, CLS_OBJECT);
    rt->intr[I_BooleanProto] = new_object(rt, op, CLS_BOOLEAN);
    rt->intr[I_NumberProto] = new_object(rt, op, CLS_NUMBER);
    rt->intr[I_NumberProto]->u.prim.v = mknum(0);
    rt->intr[I_StringProto] = new_object(rt, op, CLS_STRING);
    rt->intr[I_StringProto]->u.prim.v = ATOM(empty);
    rt->intr[I_SymbolProto] = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_BigIntProto] = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_IteratorProto] = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_AsyncIteratorProto] = new_object(rt, op, CLS_OBJECT);
    rt->intr[I_GeneratorFunctionProto] = new_object(rt, fp, CLS_OBJECT);
    rt->intr[I_AsyncFunctionProto] = new_object(rt, fp, CLS_OBJECT);
    rt->intr[I_AsyncGeneratorFunctionProto] = new_object(rt, fp, CLS_OBJECT);
    rt->intr[I_GeneratorProto] = new_object(rt, rt->intr[I_IteratorProto], CLS_OBJECT);
    rt->intr[I_AsyncGeneratorProto] = new_object(rt, rt->intr[I_AsyncIteratorProto], CLS_OBJECT);
    init_object(rt);
    init_function(rt);
    init_symbol(rt);
    init_error(rt);
    init_iter(rt);
    init_array(rt);
    init_string(rt);
    init_number(rt);
    init_math(rt);
    init_bigint(rt);
    init_global(rt);
    init_json(rt);
    init_regexp(rt);
    init_date(rt);
    init_promise(rt);
    init_collections(rt);
    init_typed(rt);
    init_reflect(rt);
    init_console(rt);
}

/* strchr for small constant sets without libc */
int strchr_safe(const char *s, uint32_t c) {
    for (; *s; s++)
        if ((uint8_t)*s == c) return 1;
    return 0;
}
