/* AXIOM JavaScript engine: Array. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

static bool dense(jsval o) {
    return is_obj(o) && vobj(o)->cls == CLS_ARRAY && !(vobj(o)->oflags & OF_SLOW_ARRAY);
}

static int has_idx(js_rt *rt, jsval o, uint64_t i) {
    if (dense(o)) {
        JSObject *a = vobj(o);
        if (i < a->u.arr.len && a->u.arr.elems[i] != JS_HOLE) return 1;
        if (!a->proto || !(a->proto->oflags & (OF_HASIDX | OF_EXOTIC)) || a->proto == rt->intr[I_ArrayProto]) {
            if (a->proto == rt->intr[I_ArrayProto] && !(rt->intr[I_ArrayProto]->oflags & OF_HASIDX) &&
                !(rt->intr[I_ObjectProto]->oflags & OF_HASIDX))
                return 0;
        }
    }
    jsval k = atomize(rt, num_to_str(rt, (double)i));
    return has_prop(rt, o, k);
}
static jsval get_i(js_rt *rt, jsval o, uint64_t i) {
    if (i < 4294967295ull) return get_idx(rt, o, (uint32_t)i);
    return get_p(rt, o, atomize(rt, num_to_str(rt, (double)i)));
}
static int set_i(js_rt *rt, jsval o, uint64_t i, jsval v) {
    if (i < 4294967295ull) return set_idx(rt, o, (uint32_t)i, v, true);
    return set_p(rt, o, atomize(rt, num_to_str(rt, (double)i)), v, true);
}
static int del_i(js_rt *rt, jsval o, uint64_t i) {
    return delete_prop(rt, o, atomize(rt, num_to_str(rt, (double)i)), true);
}
static int create_i(js_rt *rt, JSObject *o, uint64_t i, jsval v) {
    if (i < 4294967295ull) return create_data_idx(rt, o, (uint32_t)i, v);
    return create_data_prop(rt, o, atomize(rt, num_to_str(rt, (double)i)), v, true);
}
static int set_len(js_rt *rt, jsval o, uint64_t len) {
    if (dense(o) && !(vobj(o)->oflags & OF_LEN_RO)) {
        if (len <= 0xFFFFFFFFull && array_set_length(rt, vobj(o), (uint32_t)len)) return 0;
    }
    return set_p(rt, o, ATOM(length), mknum((double)len), true) < 0 ? -1 : 0;
}

static jsval species_create(js_rt *rt, jsval o, uint64_t len) {
    if (!is_array(rt, o)) {
        if (len > 4294967295ull) return throw_error(rt, E_RANGE, "Invalid array length");
        return mkobj(new_array(rt, (uint32_t)len));
    }
    if (is_obj(o) && vobj(o)->cls == CLS_ARRAY && vobj(o)->proto == rt->intr[I_ArrayProto] &&
        !find_own(rt, vobj(o), ATOM(constructor))) {
        Prop *cp = find_own(rt, rt->intr[I_ArrayProto], ATOM(constructor));
        if (cp && cp->val == mkobj(rt->intr[I_Array])) {
            if (len > 4294967295ull) return throw_error(rt, E_RANGE, "Invalid array length");
            return mkobj(new_array(rt, (uint32_t)len));
        }
    }
    jsval c = get_p(rt, o, ATOM(constructor));
    TRY(c);
    if (is_obj(c)) {
        c = get_p(rt, c, SYM(species));
        TRY(c);
        if (is_null(c)) c = JS_UNDEF;
    }
    if (is_undef(c)) {
        if (len > 4294967295ull) return throw_error(rt, E_RANGE, "Invalid array length");
        return mkobj(new_array(rt, (uint32_t)len));
    }
    if (!is_ctor(c)) THROW_TYPE("object.constructor[Symbol.species] is not a constructor");
    jsval l = mknum((double)len);
    return construct_fn(rt, c, 1, &l, c);
}

NATIVE(array_ctor) {
    jsval nt = rt->new_target;
    JSObject *proto = rt->intr[I_ArrayProto];
    if (!is_undef(nt) && nt != mkobj(rt->intr[I_Array])) {
        proto = proto_from_ctor(rt, nt, I_ArrayProto);
        if (!proto) return JS_EXC;
    }
    JSObject *a;
    if (argc == 1 && is_num(argv[0])) {
        double d = vnum(argv[0]);
        uint32_t n = to_uint32(d);
        if ((double)n != d) return throw_error(rt, E_RANGE, "Invalid array length");
        a = new_array(rt, n);
    } else
        a = new_array_from(rt, argv, (uint32_t)argc);
    a->proto = proto;
    return mkobj(a);
}
NATIVE(array_is_array) { return mkbool(is_array(rt, ARG(0))); }
NATIVE(array_of) {
    jsval c = this_;
    if (!is_ctor(c) || c == mkobj(rt->intr[I_Array])) return mkobj(new_array_from(rt, argv, (uint32_t)argc));
    jsval l = mknum(argc);
    jsval a = construct_fn(rt, c, 1, &l, c);
    TRY(a);
    push_tmp(rt, a);
    for (int i = 0; i < argc; i++)
        if (create_i(rt, vobj(a), (uint64_t)i, argv[i]) < 0) return JS_EXC;
    if (set_len(rt, a, (uint64_t)argc) < 0) return JS_EXC;
    return a;
}
NATIVE(array_from) {
    jsval items = ARG(0), mapfn = ARG(1), thisarg = ARG(2);
    bool mapping = !is_undef(mapfn);
    if (mapping && !is_callable(mapfn)) THROW_TYPE("%S is not a function", val_brief(rt, mapfn));
    jsval c = this_;
    bool use_ctor = is_ctor(c) && c != mkobj(rt->intr[I_Array]);
    if (is_nullish(items)) THROW_TYPE("%s is not iterable (cannot read property Symbol(Symbol.iterator))", is_null(items) ? "object null" : "undefined");
    jsval usingIter = get_v(rt, items, SYM(iterator));
    TRY(usingIter);
    if (!is_nullish(usingIter)) {
        jsval a = use_ctor ? construct_fn(rt, c, 0, 0, c) : mkobj(new_array(rt, 0));
        TRY(a);
        push_tmp(rt, a);
        jsval next;
        jsval it = get_iterator(rt, items, &next, false);
        TRY(it);
        push_tmp(rt, it);
        push_tmp(rt, next);
        uint64_t k = 0;
        for (;; k++) {
            jsval v;
            int r = iter_step(rt, it, next, &v);
            if (r < 0) return JS_EXC;
            if (!r) break;
            jsval *r2 = push_tmp(rt, v);
            if (mapping) {
                jsval args[2] = {v, mknum((double)k)};
                v = call_fn(rt, mapfn, thisarg, 2, args);
                if (v == JS_EXC) {
                    iter_close(rt, it, true);
                    return JS_EXC;
                }
            }
            if (create_i(rt, vobj(a), k, v) < 0) {
                iter_close(rt, it, true);
                return JS_EXC;
            }
            rt->sp = r2;
        }
        if (set_len(rt, a, k) < 0) return JS_EXC;
        return a;
    }
    jsval o = to_object(rt, items);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    jsval l = mknum((double)len);
    jsval a = use_ctor ? construct_fn(rt, c, 1, &l, c) : (len > 4294967295ull ? throw_error(rt, E_RANGE, "Invalid array length") : mkobj(new_array(rt, 0)));
    TRY(a);
    push_tmp(rt, a);
    for (uint64_t k = 0; k < len; k++) {
        jsval v = get_i(rt, o, k);
        TRY(v);
        if (mapping) {
            jsval *r2 = push_tmp(rt, v);
            jsval args[2] = {v, mknum((double)k)};
            v = call_fn(rt, mapfn, thisarg, 2, args);
            rt->sp = r2;
            TRY(v);
        }
        if (create_i(rt, vobj(a), k, v) < 0) return JS_EXC;
    }
    if (set_len(rt, a, len) < 0) return JS_EXC;
    return a;
}

/* ---------------- mutators ---------------- */
NATIVE(ap_push) {
    if (dense(this_)) {
        JSObject *a = vobj(this_);
        if ((a->oflags & OF_EXTENSIBLE) && !(a->oflags & (OF_LEN_RO | OF_FROZEN_EL | OF_SEALED_EL))) {
            if (!array_reserve(rt, a, a->u.arr.len + (uint32_t)argc)) return throw_error(rt, E_RANGE, "Invalid array length");
            for (int i = 0; i < argc; i++) a->u.arr.elems[a->u.arr.len++] = argv[i];
            return mknum(a->u.arr.len);
        }
    }
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (len + (uint64_t)argc > 9007199254740991ull) THROW_TYPE("Pushing %d elements on an array-like of length %S is disallowed", argc, num_to_str(rt, (double)len));
    for (int i = 0; i < argc; i++)
        if (set_i(rt, o, len + (uint64_t)i, argv[i]) < 0) return JS_EXC;
    if (set_len(rt, o, len + (uint64_t)argc) < 0) return JS_EXC;
    return mknum((double)(len + (uint64_t)argc));
}
NATIVE(array_pop) {
    if (dense(this_)) {
        JSObject *a = vobj(this_);
        if (!(a->oflags & (OF_LEN_RO | OF_FROZEN_EL | OF_SEALED_EL))) {
            if (a->u.arr.len == 0) return JS_UNDEF;
            jsval v = a->u.arr.elems[a->u.arr.len - 1];
            if (v != JS_HOLE) {
                a->u.arr.len--;
                return v;
            }
        }
    }
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (len == 0) {
        if (set_len(rt, o, 0) < 0) return JS_EXC;
        return JS_UNDEF;
    }
    jsval v = get_i(rt, o, len - 1);
    TRY(v);
    push_tmp(rt, v);
    if (del_i(rt, o, len - 1) < 0) return JS_EXC;
    if (set_len(rt, o, len - 1) < 0) return JS_EXC;
    return v;
}
NATIVE(array_shift) {
    if (dense(this_)) {
        JSObject *a = vobj(this_);
        if (!(a->oflags & (OF_LEN_RO | OF_FROZEN_EL | OF_SEALED_EL))) {
            uint32_t n = a->u.arr.len;
            if (n == 0) return JS_UNDEF;
            bool holes = false;
            for (uint32_t i = 0; i < n; i++)
                if (a->u.arr.elems[i] == JS_HOLE) {
                    holes = true;
                    break;
                }
            if (!holes) {
                jsval v = a->u.arr.elems[0];
                memmove(a->u.arr.elems, a->u.arr.elems + 1, (size_t)(n - 1) * sizeof(jsval));
                a->u.arr.len--;
                return v;
            }
        }
    }
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (len == 0) {
        if (set_len(rt, o, 0) < 0) return JS_EXC;
        return JS_UNDEF;
    }
    jsval first = get_i(rt, o, 0);
    TRY(first);
    push_tmp(rt, first);
    for (uint64_t k = 1; k < len; k++) {
        int h = has_idx(rt, o, k);
        if (h < 0) return JS_EXC;
        if (h) {
            jsval v = get_i(rt, o, k);
            TRY(v);
            if (set_i(rt, o, k - 1, v) < 0) return JS_EXC;
        } else if (del_i(rt, o, k - 1) < 0)
            return JS_EXC;
    }
    if (del_i(rt, o, len - 1) < 0) return JS_EXC;
    if (set_len(rt, o, len - 1) < 0) return JS_EXC;
    return first;
}
NATIVE(array_unshift) {
    if (dense(this_)) {
        JSObject *a = vobj(this_);
        if ((a->oflags & OF_EXTENSIBLE) && !(a->oflags & (OF_LEN_RO | OF_FROZEN_EL | OF_SEALED_EL))) {
            uint32_t n = a->u.arr.len;
            if (!array_reserve(rt, a, n + (uint32_t)argc)) return throw_error(rt, E_RANGE, "Invalid array length");
            memmove(a->u.arr.elems + argc, a->u.arr.elems, (size_t)n * sizeof(jsval));
            for (int i = 0; i < argc; i++) a->u.arr.elems[i] = argv[i];
            a->u.arr.len = n + (uint32_t)argc;
            return mknum(a->u.arr.len);
        }
    }
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (argc > 0) {
        for (uint64_t k = len; k > 0; k--) {
            int h = has_idx(rt, o, k - 1);
            if (h < 0) return JS_EXC;
            if (h) {
                jsval v = get_i(rt, o, k - 1);
                TRY(v);
                if (set_i(rt, o, k - 1 + (uint64_t)argc, v) < 0) return JS_EXC;
            } else if (del_i(rt, o, k - 1 + (uint64_t)argc) < 0)
                return JS_EXC;
        }
        for (int i = 0; i < argc; i++)
            if (set_i(rt, o, (uint64_t)i, argv[i]) < 0) return JS_EXC;
    }
    if (set_len(rt, o, len + (uint64_t)argc) < 0) return JS_EXC;
    return mknum((double)(len + (uint64_t)argc));
}

static int rel_index(js_rt *rt, jsval v, uint64_t len, uint64_t dflt, uint64_t *out) {
    if (is_undef(v)) {
        *out = dflt;
        return 0;
    }
    double d;
    if (to_int(rt, v, &d) < 0) return -1;
    if (d < 0) {
        d += (double)len;
        if (d < 0) d = 0;
    } else if (d > (double)len)
        d = (double)len;
    *out = (uint64_t)d;
    return 0;
}

NATIVE(array_slice) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len, start, end;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (rel_index(rt, ARG(0), len, 0, &start) < 0 || rel_index(rt, ARG(1), len, len, &end) < 0) return JS_EXC;
    uint64_t n = end > start ? end - start : 0;
    jsval a = species_create(rt, o, n);
    TRY(a);
    push_tmp(rt, a);
    if (dense(o) && dense(a) && vobj(a)->u.arr.len == n) {
        JSObject *s = vobj(o), *d = vobj(a);
        if (end <= s->u.arr.len) {
            for (uint64_t i = 0; i < n; i++) d->u.arr.elems[i] = s->u.arr.elems[start + i];
            return a;
        }
    }
    uint64_t k = 0;
    for (uint64_t i = start; i < end; i++, k++) {
        int h = has_idx(rt, o, i);
        if (h < 0) return JS_EXC;
        if (h) {
            jsval v = get_i(rt, o, i);
            TRY(v);
            if (create_i(rt, vobj(a), k, v) < 0) return JS_EXC;
        }
    }
    if (set_len(rt, a, n) < 0) return JS_EXC;
    return a;
}

NATIVE(array_splice) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len, start, dc;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (rel_index(rt, ARG(0), len, 0, &start) < 0) return JS_EXC;
    if (argc == 0)
        dc = 0;
    else if (argc == 1)
        dc = len - start;
    else {
        double d;
        if (to_int(rt, argv[1], &d) < 0) return JS_EXC;
        if (d < 0) d = 0;
        if (d > (double)(len - start)) d = (double)(len - start);
        dc = (uint64_t)d;
    }
    uint64_t ic = argc > 2 ? (uint64_t)(argc - 2) : 0;
    jsval a = species_create(rt, o, dc);
    TRY(a);
    push_tmp(rt, a);
    /* fast path */
    if (dense(o) && dense(a) && !(vobj(o)->oflags & (OF_LEN_RO | OF_FROZEN_EL | OF_SEALED_EL)) && (vobj(o)->oflags & OF_EXTENSIBLE)) {
        JSObject *s = vobj(o), *d = vobj(a);
        bool holes = false;
        for (uint32_t i = 0; i < s->u.arr.len; i++)
            if (s->u.arr.elems[i] == JS_HOLE) {
                holes = true;
                break;
            }
        if (!holes && d->u.arr.len == dc) {
            for (uint64_t i = 0; i < dc; i++) d->u.arr.elems[i] = s->u.arr.elems[start + i];
            uint32_t n = s->u.arr.len;
            uint32_t nl = (uint32_t)(n - dc + ic);
            if (!array_reserve(rt, s, nl > n ? nl : n)) return throw_error(rt, E_RANGE, "Invalid array length");
            memmove(s->u.arr.elems + start + ic, s->u.arr.elems + start + dc, (size_t)(n - start - dc) * sizeof(jsval));
            for (uint64_t i = 0; i < ic; i++) s->u.arr.elems[start + i] = argv[2 + i];
            s->u.arr.len = nl;
            return a;
        }
    }
    for (uint64_t k = 0; k < dc; k++) {
        int h = has_idx(rt, o, start + k);
        if (h < 0) return JS_EXC;
        if (h) {
            jsval v = get_i(rt, o, start + k);
            TRY(v);
            if (create_i(rt, vobj(a), k, v) < 0) return JS_EXC;
        }
    }
    if (set_len(rt, a, dc) < 0) return JS_EXC;
    if (ic < dc) {
        for (uint64_t k = start; k < len - dc; k++) {
            int h = has_idx(rt, o, k + dc);
            if (h < 0) return JS_EXC;
            if (h) {
                jsval v = get_i(rt, o, k + dc);
                TRY(v);
                if (set_i(rt, o, k + ic, v) < 0) return JS_EXC;
            } else if (del_i(rt, o, k + ic) < 0)
                return JS_EXC;
        }
        for (uint64_t k = len; k > len - dc + ic; k--)
            if (del_i(rt, o, k - 1) < 0) return JS_EXC;
    } else if (ic > dc) {
        for (uint64_t k = len - dc; k > start; k--) {
            int h = has_idx(rt, o, k + dc - 1);
            if (h < 0) return JS_EXC;
            if (h) {
                jsval v = get_i(rt, o, k + dc - 1);
                TRY(v);
                if (set_i(rt, o, k + ic - 1, v) < 0) return JS_EXC;
            } else if (del_i(rt, o, k + ic - 1) < 0)
                return JS_EXC;
        }
    }
    for (uint64_t i = 0; i < ic; i++)
        if (set_i(rt, o, start + i, argv[2 + i]) < 0) return JS_EXC;
    if (set_len(rt, o, len - dc + ic) < 0) return JS_EXC;
    return a;
}

NATIVE(array_concat) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    jsval a = species_create(rt, o, 0);
    TRY(a);
    push_tmp(rt, a);
    uint64_t n = 0;
    for (int i = -1; i < argc; i++) {
        jsval e = i < 0 ? o : argv[i];
        bool spreadable = false;
        if (is_obj(e)) {
            jsval sv = get_p(rt, e, SYM(isConcatSpreadable));
            TRY(sv);
            spreadable = is_undef(sv) ? is_array(rt, e) : truthy(sv);
        }
        if (spreadable) {
            uint64_t len;
            if (len_of(rt, e, &len) < 0) return JS_EXC;
            if (dense(e) && dense(a)) {
                JSObject *s = vobj(e), *d = vobj(a);
                if (d->u.arr.len == n && len == s->u.arr.len) {
                    if (!array_reserve(rt, d, (uint32_t)(n + len))) return throw_error(rt, E_RANGE, "Invalid array length");
                    for (uint64_t k = 0; k < len; k++) d->u.arr.elems[n + k] = s->u.arr.elems[k];
                    d->u.arr.len = (uint32_t)(n + len);
                    n += len;
                    continue;
                }
            }
            for (uint64_t k = 0; k < len; k++, n++) {
                int h = has_idx(rt, e, k);
                if (h < 0) return JS_EXC;
                if (h) {
                    jsval v = get_i(rt, e, k);
                    TRY(v);
                    if (create_i(rt, vobj(a), n, v) < 0) return JS_EXC;
                }
            }
        } else {
            if (create_i(rt, vobj(a), n, e) < 0) return JS_EXC;
            n++;
        }
    }
    if (set_len(rt, a, n) < 0) return JS_EXC;
    return a;
}

NATIVE(array_reverse) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    if (dense(o) && !(vobj(o)->oflags & OF_FROZEN_EL)) {
        JSObject *a = vobj(o);
        bool holes = false;
        for (uint32_t i = 0; i < a->u.arr.len; i++)
            if (a->u.arr.elems[i] == JS_HOLE) holes = true;
        if (!holes) {
            for (uint32_t i = 0, j = a->u.arr.len; i + 1 < j; i++, j--) {
                jsval t = a->u.arr.elems[i];
                a->u.arr.elems[i] = a->u.arr.elems[j - 1];
                a->u.arr.elems[j - 1] = t;
            }
            return o;
        }
    }
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    for (uint64_t lo = 0; lo < len / 2; lo++) {
        uint64_t hi = len - 1 - lo;
        int hl = has_idx(rt, o, lo), hh = has_idx(rt, o, hi);
        if (hl < 0 || hh < 0) return JS_EXC;
        jsval lv = JS_UNDEF, hv = JS_UNDEF;
        if (hl) {
            lv = get_i(rt, o, lo);
            TRY(lv);
        }
        jsval *r2 = push_tmp(rt, lv);
        if (hh) {
            hv = get_i(rt, o, hi);
            TRY(hv);
        }
        push_tmp(rt, hv);
        if (hh) {
            if (set_i(rt, o, lo, hv) < 0) return JS_EXC;
        } else if (del_i(rt, o, lo) < 0)
            return JS_EXC;
        if (hl) {
            if (set_i(rt, o, hi, lv) < 0) return JS_EXC;
        } else if (del_i(rt, o, hi) < 0)
            return JS_EXC;
        rt->sp = r2;
    }
    return o;
}

NATIVE(array_fill) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len, s, e;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (rel_index(rt, ARG(1), len, 0, &s) < 0 || rel_index(rt, ARG(2), len, len, &e) < 0) return JS_EXC;
    for (uint64_t i = s; i < e; i++)
        if (set_i(rt, o, i, ARG(0)) < 0) return JS_EXC;
    return o;
}

NATIVE(array_copy_within) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len, to, from, fin;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (rel_index(rt, ARG(0), len, 0, &to) < 0 || rel_index(rt, ARG(1), len, 0, &from) < 0 ||
        rel_index(rt, ARG(2), len, len, &fin) < 0)
        return JS_EXC;
    int64_t count = (int64_t)(fin > from ? fin - from : 0);
    if ((int64_t)(len - to) < count) count = (int64_t)(len - to);
    int dir = 1;
    if (from < to && to < from + (uint64_t)count) {
        dir = -1;
        from += (uint64_t)count - 1;
        to += (uint64_t)count - 1;
    }
    while (count-- > 0) {
        int h = has_idx(rt, o, from);
        if (h < 0) return JS_EXC;
        if (h) {
            jsval v = get_i(rt, o, from);
            TRY(v);
            if (set_i(rt, o, to, v) < 0) return JS_EXC;
        } else if (del_i(rt, o, to) < 0)
            return JS_EXC;
        from += (uint64_t)(int64_t)dir;
        to += (uint64_t)(int64_t)dir;
    }
    return o;
}

/* ---------------- join / toString ---------------- */
static JSObject *join_stack[64];
static int njoin;

jsval array_join(js_rt *rt, jsval o, JSString *sep) {
    for (int i = 0; i < njoin; i++)
        if (join_stack[i] == vobj(o)) return ATOM(empty);
    if (njoin >= 64 || check_stack(rt)) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    join_stack[njoin++] = vobj(o);
    SB b;
    sb_init(&b, rt);
    jsval *root = push_tmp(rt, o);
    for (uint64_t i = 0; i < len; i++) {
        if (i > 0 && sep->len) sb_putstr(&b, sep);
        jsval v = get_i(rt, o, i);
        if (v == JS_EXC) goto fail;
        if (is_nullish(v)) continue;
        if (is_str(v)) {
            sb_putstr(&b, vstr(v));
            continue;
        }
        if (is_num(v)) {
            char t[40];
            int n = num_to_cstr(vnum(v), t);
            sb_putn(&b, t, (uint32_t)n);
            continue;
        }
        push_tmp(rt, v);
        JSString *s = to_string(rt, v);
        if (!s) goto fail;
        sb_putstr(&b, s);
        rt->sp = root + 1;
        if (b.oom) break;
    }
    njoin--;
    rt->sp = root;
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
fail:
    njoin--;
    rt->sp = root;
    sb_free(&b);
    return JS_EXC;
}
NATIVE(array_join_n) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    JSString *sep = vstr(mkstr(str_cstr(rt, ",")));
    if (!is_undef(ARG(0))) {
        sep = to_string(rt, argv[0]);
        if (!sep) return JS_EXC;
    }
    push_tmp(rt, mkstr(sep));
    return array_join(rt, o, sep);
}
NATIVE(array_to_string) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    jsval j = get_p(rt, o, ATOM(join));
    TRY(j);
    if (!is_callable(j)) {
        JSString *tag = jsfmt(rt, "[object %s]", is_array(rt, o) ? "Array" : "Object");
        return mkstr(tag);
    }
    return call_fn(rt, j, o, 0, 0);
}
NATIVE(array_to_locale_string) {
    jsval o = to_object(rt, this_);
    TRY(o);
    for (int i = 0; i < njoin; i++)
        if (join_stack[i] == vobj(o)) return ATOM(empty);
    if (njoin >= 64) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    join_stack[njoin++] = vobj(o);
    SB b;
    sb_init(&b, rt);
    for (uint64_t i = 0; i < len; i++) {
        if (i > 0) sb_putc(&b, ',');
        jsval v = get_i(rt, o, i);
        if (v == JS_EXC) goto fail;
        if (is_nullish(v)) continue;
        jsval r = call_method(rt, v, atom_c(rt, "toLocaleString"), 0, 0);
        if (r == JS_EXC) goto fail;
        JSString *s = to_string(rt, r);
        if (!s) goto fail;
        sb_putstr(&b, s);
    }
    njoin--;
    {
        JSString *r = sb_finish(&b);
        return r ? mkstr(r) : JS_EXC;
    }
fail:
    njoin--;
    sb_free(&b);
    return JS_EXC;
}

/* ---------------- searching ---------------- */
NATIVE(array_index_of) {
    int m = MAGIC; /* 0 indexOf 1 lastIndexOf 2 includes */
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (len == 0) return m == 2 ? JS_FALSE : mknum(-1);
    jsval x = ARG(0);
    if (m == 1) {
        double n = (double)len - 1;
        if (argc > 1 && to_int(rt, argv[1], &n) < 0) return JS_EXC;
        int64_t k = n >= 0 ? (n > (double)(len - 1) ? (int64_t)len - 1 : (int64_t)n) : (int64_t)((double)len + n);
        for (; k >= 0; k--) {
            int h = has_idx(rt, o, (uint64_t)k);
            if (h < 0) return JS_EXC;
            if (!h) continue;
            jsval v = get_i(rt, o, (uint64_t)k);
            TRY(v);
            if (strict_eq(rt, v, x)) return mknum((double)k);
        }
        return mknum(-1);
    }
    double n = 0;
    if (to_int(rt, ARG(1), &n) < 0) return JS_EXC;
    if (n >= (double)len) return m == 2 ? JS_FALSE : mknum(-1);
    uint64_t k = n >= 0 ? (uint64_t)n : (n + (double)len < 0 ? 0 : (uint64_t)(n + (double)len));
    if (dense(o)) {
        JSObject *a = vobj(o);
        for (; k < len && k < a->u.arr.len; k++) {
            jsval v = a->u.arr.elems[k];
            if (v == JS_HOLE) {
                if (m == 2 && is_undef(x)) return JS_TRUE;
                continue;
            }
            if (m == 2 ? same_value_zero(rt, v, x) : strict_eq(rt, v, x)) return m == 2 ? JS_TRUE : mknum((double)k);
        }
        if (k >= len) return m == 2 ? JS_FALSE : mknum(-1);
    }
    for (; k < len; k++) {
        if (m != 2) {
            int h = has_idx(rt, o, k);
            if (h < 0) return JS_EXC;
            if (!h) continue;
        }
        jsval v = get_i(rt, o, k);
        TRY(v);
        if (m == 2 ? same_value_zero(rt, v, x) : strict_eq(rt, v, x)) return m == 2 ? JS_TRUE : mknum((double)k);
    }
    return m == 2 ? JS_FALSE : mknum(-1);
}

/* callback iteration: magic selects the operation */
enum { CB_FOREACH, CB_MAP, CB_FILTER, CB_SOME, CB_EVERY, CB_FIND, CB_FINDINDEX, CB_FINDLAST, CB_FINDLASTINDEX };
NATIVE(array_iterate) {
    int m = MAGIC;
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    jsval cb = ARG(0), ta = ARG(1);
    if (!is_callable(cb)) THROW_TYPE("%S is not a function", val_brief(rt, cb));
    jsval res = JS_UNDEF;
    if (m == CB_MAP) {
        res = species_create(rt, o, len);
        TRY(res);
    } else if (m == CB_FILTER) {
        res = species_create(rt, o, 0);
        TRY(res);
    }
    push_tmp(rt, res);
    uint64_t outn = 0;
    bool rev = m == CB_FINDLAST || m == CB_FINDLASTINDEX;
    bool skip_holes = m <= CB_EVERY;
    jsval *r2 = rt->sp;
    for (uint64_t j = 0; j < len; j++) {
        uint64_t k = rev ? len - 1 - j : j;
        jsval v;
        if (skip_holes) {
            if (dense(o) && k < vobj(o)->u.arr.len && vobj(o)->u.arr.elems[k] != JS_HOLE)
                v = vobj(o)->u.arr.elems[k];
            else {
                int h = has_idx(rt, o, k);
                if (h < 0) return JS_EXC;
                if (!h) continue;
                v = get_i(rt, o, k);
                TRY(v);
            }
        } else {
            v = get_i(rt, o, k);
            TRY(v);
        }
        push_tmp(rt, v);
        jsval args[3] = {v, mknum((double)k), o};
        jsval r = call_fn(rt, cb, ta, 3, args);
        TRY(r);
        switch (m) {
        case CB_MAP:
            if (create_i(rt, vobj(res), k, r) < 0) return JS_EXC;
            break;
        case CB_FILTER:
            if (truthy(r) && create_i(rt, vobj(res), outn++, v) < 0) return JS_EXC;
            break;
        case CB_SOME:
            if (truthy(r)) return JS_TRUE;
            break;
        case CB_EVERY:
            if (!truthy(r)) return JS_FALSE;
            break;
        case CB_FIND:
        case CB_FINDLAST:
            if (truthy(r)) return v;
            break;
        case CB_FINDINDEX:
        case CB_FINDLASTINDEX:
            if (truthy(r)) return mknum((double)k);
            break;
        default: break;
        }
        rt->sp = r2;
    }
    switch (m) {
    case CB_MAP:
    case CB_FILTER: return res;
    case CB_SOME: return JS_FALSE;
    case CB_EVERY: return JS_TRUE;
    case CB_FINDINDEX:
    case CB_FINDLASTINDEX: return mknum(-1);
    default: return JS_UNDEF;
    }
}

NATIVE(array_reduce) {
    bool right = MAGIC == 1;
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    jsval cb = ARG(0);
    if (!is_callable(cb)) THROW_TYPE("%S is not a function", val_brief(rt, cb));
    jsval *acc = push_tmp(rt, JS_UNDEF);
    uint64_t j = 0;
    if (argc >= 2)
        *acc = argv[1];
    else {
        bool found = false;
        for (; j < len; j++) {
            uint64_t k = right ? len - 1 - j : j;
            int h = has_idx(rt, o, k);
            if (h < 0) return JS_EXC;
            if (h) {
                *acc = get_i(rt, o, k);
                TRY(*acc);
                found = true;
                j++;
                break;
            }
        }
        if (!found) THROW_TYPE("Reduce of empty array with no initial value");
    }
    for (; j < len; j++) {
        uint64_t k = right ? len - 1 - j : j;
        int h = has_idx(rt, o, k);
        if (h < 0) return JS_EXC;
        if (!h) continue;
        jsval v = get_i(rt, o, k);
        TRY(v);
        jsval args[4] = {*acc, v, mknum((double)k), o};
        jsval r = call_fn(rt, cb, JS_UNDEF, 4, args);
        TRY(r);
        *acc = r;
    }
    return *acc;
}

static int flatten_into(js_rt *rt, JSObject *target, uint64_t *n, jsval src, uint64_t len, double depth, jsval mapfn, jsval ta) {
    if (check_stack(rt)) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    for (uint64_t k = 0; k < len; k++) {
        int h = has_idx(rt, src, k);
        if (h < 0) return -1;
        if (!h) continue;
        jsval v = get_i(rt, src, k);
        if (v == JS_EXC) return -1;
        jsval *r2 = push_tmp(rt, v);
        if (!is_undef(mapfn)) {
            jsval args[3] = {v, mknum((double)k), src};
            v = call_fn(rt, mapfn, ta, 3, args);
            if (v == JS_EXC) return -1;
            *r2 = v;
        }
        if (depth > 0 && is_array(rt, v)) {
            uint64_t l2;
            if (len_of(rt, v, &l2) < 0) return -1;
            if (flatten_into(rt, target, n, v, l2, depth - 1, JS_UNDEF, JS_UNDEF) < 0) return -1;
        } else {
            if (create_i(rt, target, (*n)++, v) < 0) return -1;
        }
        rt->sp = r2;
    }
    return 0;
}
NATIVE(array_flat) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    double depth = 1;
    jsval mapfn = JS_UNDEF;
    if (MAGIC == 1) {
        mapfn = ARG(0);
        if (!is_callable(mapfn)) THROW_TYPE("flatMap mapper function is not callable");
    } else if (!is_undef(ARG(0))) {
        if (to_int(rt, argv[0], &depth) < 0) return JS_EXC;
    }
    jsval a = species_create(rt, o, 0);
    TRY(a);
    push_tmp(rt, a);
    uint64_t n = 0;
    if (flatten_into(rt, vobj(a), &n, o, len, depth, mapfn, ARG(1)) < 0) return JS_EXC;
    return a;
}

NATIVE(array_at) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    double d;
    if (to_int(rt, ARG(0), &d) < 0) return JS_EXC;
    if (d < 0) d += (double)len;
    if (d < 0 || d >= (double)len) return JS_UNDEF;
    return get_i(rt, o, (uint64_t)d);
}

/* ---------------- sort ---------------- */
typedef struct SortCtx {
    js_rt *rt;
    jsval cmp;
    jsval *keys; /* strings when no comparator */
    bool err;
} SortCtx;

static int sort_cmp(SortCtx *c, jsval a, jsval b, uint32_t ia, uint32_t ib) {
    js_rt *rt = c->rt;
    if (c->err) return 0;
    if (is_undef(c->cmp)) {
        if (is_num(a) && is_num(b) && c->keys == 0) {
            double x = vnum(a), y = vnum(b);
            if (x == y) return 0;
        }
        JSString *sa = vstr(c->keys[ia]), *sb = vstr(c->keys[ib]);
        return str_cmp(rt, sa, sb);
    }
    jsval args[2] = {a, b};
    jsval r = call_fn(rt, c->cmp, JS_UNDEF, 2, args);
    if (r == JS_EXC) {
        c->err = true;
        return 0;
    }
    double d;
    if (to_number(rt, r, &d) < 0) {
        c->err = true;
        return 0;
    }
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* stable merge sort of v[0..n) with index array idx (sorted along) using tmp buffers */
static void merge_sort(SortCtx *c, jsval *v, uint32_t *idx, jsval *tv, uint32_t *ti, uint32_t n) {
    /* bottom-up: insertion sort runs of 8 then merge */
    for (uint32_t s = 0; s < n; s += 8) {
        uint32_t e = s + 8 < n ? s + 8 : n;
        for (uint32_t i = s + 1; i < e; i++) {
            jsval x = v[i];
            uint32_t xi = idx[i];
            uint32_t j = i;
            while (j > s && sort_cmp(c, v[j - 1], x, idx[j - 1], xi) > 0) {
                v[j] = v[j - 1];
                idx[j] = idx[j - 1];
                j--;
            }
            v[j] = x;
            idx[j] = xi;
            if (c->err) return;
        }
    }
    for (uint32_t w = 8; w < n; w *= 2) {
        for (uint32_t s = 0; s < n; s += 2 * w) {
            uint32_t m = s + w < n ? s + w : n, e = s + 2 * w < n ? s + 2 * w : n;
            uint32_t i = s, j = m, k = s;
            while (i < m && j < e) {
                if (sort_cmp(c, v[j], v[i], idx[j], idx[i]) < 0) {
                    tv[k] = v[j];
                    ti[k++] = idx[j++];
                } else {
                    tv[k] = v[i];
                    ti[k++] = idx[i++];
                }
                if (c->err) return;
            }
            while (i < m) {
                tv[k] = v[i];
                ti[k++] = idx[i++];
            }
            while (j < e) {
                tv[k] = v[j];
                ti[k++] = idx[j++];
            }
        }
        memcpy(v, tv, n * sizeof(jsval));
        memcpy(idx, ti, n * sizeof(uint32_t));
    }
}

/* sorts values of array-like o into a new dense array (holes/undefined handled) */
static JSObject *sorted_values(js_rt *rt, jsval o, uint64_t len, jsval cmp, uint64_t *nundef, bool skip_holes) {
    JSObject *vals = new_array(rt, 0);
    jsval *root = push_tmp(rt, mkobj(vals));
    *nundef = 0;
    for (uint64_t k = 0; k < len; k++) {
        if (skip_holes) {
            int h = has_idx(rt, o, k);
            if (h < 0) {
                rt->sp = root;
                return 0;
            }
            if (!h) continue;
        }
        jsval v = get_i(rt, o, k);
        if (v == JS_EXC) {
            rt->sp = root;
            return 0;
        }
        if (is_undef(v))
            (*nundef)++;
        else
            array_push(rt, vals, v);
    }
    uint32_t n = vals->u.arr.len;
    JSObject *keys = 0, *tmp = new_array(rt, n);
    push_tmp(rt, mkobj(tmp));
    SortCtx c = {rt, cmp, 0, false};
    if (is_undef(cmp)) {
        keys = new_array(rt, n);
        push_tmp(rt, mkobj(keys));
        for (uint32_t i = 0; i < n; i++) {
            JSString *s = to_string(rt, vals->u.arr.elems[i]);
            if (!s) {
                rt->sp = root;
                return 0;
            }
            keys->u.arr.elems[i] = mkstr(s);
        }
        c.keys = keys->u.arr.elems;
    }
    uint32_t *idx = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    uint32_t *ti = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    /* values live in vals (rooted); tmp buffer is a rooted array too */
    merge_sort(&c, vals->u.arr.elems, idx, tmp->u.arr.elems, ti, n);
    free(idx);
    free(ti);
    rt->sp = root;
    if (c.err) return 0;
    return vals;
}

NATIVE(array_sort) {
    jsval cmp = ARG(0);
    if (!is_undef(cmp) && !is_callable(cmp)) THROW_TYPE("The comparison function must be either a function or undefined");
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    uint64_t nundef;
    JSObject *vals = sorted_values(rt, o, len, cmp, &nundef, true);
    if (!vals) return JS_EXC;
    push_tmp(rt, mkobj(vals));
    uint64_t k = 0;
    for (uint32_t i = 0; i < vals->u.arr.len; i++, k++)
        if (set_i(rt, o, k, vals->u.arr.elems[i]) < 0) return JS_EXC;
    for (uint64_t i = 0; i < nundef; i++, k++)
        if (set_i(rt, o, k, JS_UNDEF) < 0) return JS_EXC;
    for (; k < len; k++)
        if (del_i(rt, o, k) < 0) return JS_EXC;
    return o;
}
NATIVE(array_to_sorted) {
    jsval cmp = ARG(0);
    if (!is_undef(cmp) && !is_callable(cmp)) THROW_TYPE("The comparison function must be either a function or undefined");
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    uint64_t nundef;
    JSObject *vals = sorted_values(rt, o, len, cmp, &nundef, false);
    if (!vals) return JS_EXC;
    for (uint64_t i = 0; i < nundef; i++) array_push(rt, vals, JS_UNDEF);
    return mkobj(vals);
}
NATIVE(array_to_reversed) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (len > 4294967295ull) return throw_error(rt, E_RANGE, "Invalid array length");
    JSObject *a = new_array(rt, (uint32_t)len);
    push_tmp(rt, mkobj(a));
    for (uint64_t k = 0; k < len; k++) {
        jsval v = get_i(rt, o, len - 1 - k);
        TRY(v);
        a->u.arr.elems[k] = v;
    }
    return mkobj(a);
}
NATIVE(array_with) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    double d;
    if (to_int(rt, ARG(0), &d) < 0) return JS_EXC;
    if (d < 0) d += (double)len;
    if (d < 0 || d >= (double)len) return throw_error(rt, E_RANGE, "Invalid index : %S", num_to_str(rt, d));
    JSObject *a = new_array(rt, (uint32_t)len);
    push_tmp(rt, mkobj(a));
    for (uint64_t k = 0; k < len; k++) {
        jsval v = k == (uint64_t)d ? ARG(1) : get_i(rt, o, k);
        TRY(v);
        a->u.arr.elems[k] = v;
    }
    return mkobj(a);
}
NATIVE(array_to_spliced) {
    jsval o = to_object(rt, this_);
    TRY(o);
    push_tmp(rt, o);
    uint64_t len, start, dc;
    if (len_of(rt, o, &len) < 0) return JS_EXC;
    if (rel_index(rt, ARG(0), len, 0, &start) < 0) return JS_EXC;
    if (argc == 0)
        dc = 0;
    else if (argc == 1)
        dc = len - start;
    else {
        double dd;
        if (to_int(rt, argv[1], &dd) < 0) return JS_EXC;
        if (dd < 0) dd = 0;
        if (dd > (double)(len - start)) dd = (double)(len - start);
        dc = (uint64_t)dd;
    }
    uint64_t ic = argc > 2 ? (uint64_t)(argc - 2) : 0;
    JSObject *a = new_array(rt, 0);
    push_tmp(rt, mkobj(a));
    for (uint64_t k = 0; k < start; k++) {
        jsval v = get_i(rt, o, k);
        TRY(v);
        array_push(rt, a, v);
    }
    for (uint64_t k = 0; k < ic; k++) array_push(rt, a, argv[2 + k]);
    for (uint64_t k = start + dc; k < len; k++) {
        jsval v = get_i(rt, o, k);
        TRY(v);
        array_push(rt, a, v);
    }
    return mkobj(a);
}

NATIVE(array_iter_n) {
    jsval o = to_object(rt, this_);
    TRY(o);
    return mkobj(make_array_iter(rt, o, MAGIC));
}
NATIVE(array_species) { return this_; }

void init_array(js_rt *rt) {
    JSObject *ap = rt->intr[I_ArrayProto];
    JSObject *ctor = new_ctor(rt, "Array", array_ctor, 1, ap);
    rt->intr[I_Array] = ctor;
    def_method(rt, ctor, "isArray", array_is_array, 1, 0);
    def_method(rt, ctor, "of", array_of, 0, 0);
    def_method(rt, ctor, "from", array_from, 1, 0);
    def_getter_k(rt, ctor, SYM(species), "get [Symbol.species]", array_species, 0);
    def_method(rt, ap, "push", ap_push, 1, 0);
    def_method(rt, ap, "pop", array_pop, 0, 0);
    def_method(rt, ap, "shift", array_shift, 0, 0);
    def_method(rt, ap, "unshift", array_unshift, 1, 0);
    def_method(rt, ap, "slice", array_slice, 2, 0);
    def_method(rt, ap, "splice", array_splice, 2, 0);
    def_method(rt, ap, "concat", array_concat, 1, 0);
    def_method(rt, ap, "reverse", array_reverse, 0, 0);
    def_method(rt, ap, "fill", array_fill, 1, 0);
    def_method(rt, ap, "copyWithin", array_copy_within, 2, 0);
    def_method(rt, ap, "join", array_join_n, 1, 0);
    def_method(rt, ap, "toString", array_to_string, 0, 0);
    def_method(rt, ap, "toLocaleString", array_to_locale_string, 0, 0);
    def_method(rt, ap, "indexOf", array_index_of, 1, 0);
    def_method(rt, ap, "lastIndexOf", array_index_of, 1, 1);
    def_method(rt, ap, "includes", array_index_of, 1, 2);
    def_method(rt, ap, "forEach", array_iterate, 1, CB_FOREACH);
    def_method(rt, ap, "map", array_iterate, 1, CB_MAP);
    def_method(rt, ap, "filter", array_iterate, 1, CB_FILTER);
    def_method(rt, ap, "some", array_iterate, 1, CB_SOME);
    def_method(rt, ap, "every", array_iterate, 1, CB_EVERY);
    def_method(rt, ap, "find", array_iterate, 1, CB_FIND);
    def_method(rt, ap, "findIndex", array_iterate, 1, CB_FINDINDEX);
    def_method(rt, ap, "findLast", array_iterate, 1, CB_FINDLAST);
    def_method(rt, ap, "findLastIndex", array_iterate, 1, CB_FINDLASTINDEX);
    def_method(rt, ap, "reduce", array_reduce, 1, 0);
    def_method(rt, ap, "reduceRight", array_reduce, 1, 1);
    def_method(rt, ap, "flat", array_flat, 0, 0);
    def_method(rt, ap, "flatMap", array_flat, 1, 1);
    def_method(rt, ap, "at", array_at, 1, 0);
    def_method(rt, ap, "sort", array_sort, 1, 0);
    def_method(rt, ap, "toSorted", array_to_sorted, 1, 0);
    def_method(rt, ap, "toReversed", array_to_reversed, 0, 0);
    def_method(rt, ap, "toSpliced", array_to_spliced, 2, 0);
    def_method(rt, ap, "with", array_with, 2, 0);
    def_method(rt, ap, "keys", array_iter_n, 0, 0);
    def_method(rt, ap, "entries", array_iter_n, 0, 2);
    JSObject *vals = def_method(rt, ap, "values", array_iter_n, 0, 1);
    rt->intr[I_ArrayValues] = vals;
    def_prop(rt, ap, SYM(iterator), mkobj(vals), P_W | P_C);
    JSObject *unscop = new_object(rt, 0, CLS_OBJECT);
    const char *us[] = {"at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast", "findLastIndex", "flat",
                        "flatMap", "includes", "keys", "toReversed", "toSorted", "toSpliced", "values"};
    for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) add_prop(rt, unscop, atom_c(rt, us[i]), JS_TRUE, P_DEFAULT);
    def_prop(rt, ap, SYM(unscopables), mkobj(unscop), P_C);
}
