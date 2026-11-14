/* AXIOM JavaScript engine: JSON, console and node-style value inspection. */
#include "js_int.h"

#define THROW_TYPE(...) return throw_error(rt, E_TYPE, __VA_ARGS__)

/* ---------------- JSON.parse ---------------- */
typedef struct JP {
    js_rt *rt;
    JSString *s;
    uint32_t i, n;
} JP;

static jsval jp_error(JP *p, const char *what) {
    js_rt *rt = p->rt;
    uint32_t pos = p->i;
    uint32_t line = 1, col = 1;
    for (uint32_t k = 0; k < pos && k < p->n; k++) {
        if (str_at(p->s, k) == '\n') {
            line++;
            col = 1;
        } else
            col++;
    }
    if (pos >= p->n && !what) return throw_error(rt, E_SYNTAX, "Unexpected end of JSON input");
    if (!what) {
        uint32_t c = str_at(p->s, pos);
        JSString *ctx;
        if (p->n <= 40)
            ctx = jsfmt(rt, "\"%S\" is not valid JSON", p->s);
        else {
            uint32_t a = pos > 10 ? pos - 10 : 0, b = pos + 10 < p->n ? pos + 10 : p->n;
            ctx = jsfmt(rt, "%s\"%S\"%s is not valid JSON", a > 0 ? "..." : "", str_sub(rt, p->s, a, b), b < p->n ? "..." : "");
        }
        SB sb;
        sb_init(&sb, rt);
        sb_puts(&sb, "Unexpected token '");
        sb_putc(&sb, c);
        sb_puts(&sb, "', ");
        sb_putstr(&sb, ctx);
        JSString *m = sb_finish(&sb);
        return throw_error(rt, E_SYNTAX, "%S", m);
    }
    return throw_error(rt, E_SYNTAX, "%s in JSON at position %u (line %u column %u)", what, pos, line, col);
}
static void jp_ws(JP *p) {
    while (p->i < p->n) {
        uint16_t c = str_at(p->s, p->i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->i++;
        else
            break;
    }
}
static jsval jp_value(JP *p, int depth);

static jsval jp_string(JP *p) {
    js_rt *rt = p->rt;
    p->i++; /* opening quote */
    uint32_t start = p->i;
    while (p->i < p->n) {
        uint16_t c = str_at(p->s, p->i);
        if (c == '"') {
            JSString *r = str_sub(rt, p->s, start, p->i);
            p->i++;
            return mkstr(r);
        }
        if (c == '\\' || c < 0x20) break;
        p->i++;
    }
    SB b;
    sb_init(&b, rt);
    sb_putsub(&b, p->s, start, p->i);
    while (p->i < p->n) {
        uint16_t c = str_at(p->s, p->i);
        if (c == '"') {
            p->i++;
            JSString *r = sb_finish(&b);
            return r ? mkstr(r) : JS_EXC;
        }
        if (c < 0x20) {
            sb_free(&b);
            return jp_error(p, "Bad control character in string literal");
        }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            uint16_t e = str_at(p->s, p->i);
            switch (e) {
            case '"': sb_putc(&b, '"'); break;
            case '\\': sb_putc(&b, '\\'); break;
            case '/': sb_putc(&b, '/'); break;
            case 'b': sb_putc(&b, '\b'); break;
            case 'f': sb_putc(&b, '\f'); break;
            case 'n': sb_putc(&b, '\n'); break;
            case 'r': sb_putc(&b, '\r'); break;
            case 't': sb_putc(&b, '\t'); break;
            case 'u': {
                uint32_t v = 0;
                for (int k = 1; k <= 4; k++) {
                    uint16_t h = p->i + (uint32_t)k < p->n ? str_at(p->s, p->i + (uint32_t)k) : 0;
                    int d = (h >= '0' && h <= '9') ? h - '0' : ((h | 0x20) >= 'a' && (h | 0x20) <= 'f') ? (h | 0x20) - 'a' + 10 : -1;
                    if (d < 0) {
                        p->i += (uint32_t)k;
                        sb_free(&b);
                        return jp_error(p, "Bad Unicode escape");
                    }
                    v = v * 16 + (uint32_t)d;
                }
                p->i += 4;
                sb_putc(&b, v);
                break;
            }
            default: sb_free(&b); return jp_error(p, "Bad escaped character");
            }
            p->i++;
            continue;
        }
        sb_putc(&b, c);
        p->i++;
    }
    sb_free(&b);
    return jp_error(p, "Unterminated string");
}

static jsval jp_number(JP *p) {
    uint32_t start = p->i;
    char buf[400];
    int k = 0;
    if (str_at(p->s, p->i) == '-') {
        buf[k++] = '-';
        p->i++;
        if (p->i >= p->n || str_at(p->s, p->i) < '0' || str_at(p->s, p->i) > '9') return jp_error(p, "No number after minus sign");
    }
    if (str_at(p->s, p->i) == '0') {
        buf[k++] = '0';
        p->i++;
    } else
        while (p->i < p->n && str_at(p->s, p->i) >= '0' && str_at(p->s, p->i) <= '9') {
            if (k < 390) buf[k++] = (char)str_at(p->s, p->i);
            p->i++;
        }
    if (p->i < p->n && str_at(p->s, p->i) == '.') {
        buf[k++] = '.';
        p->i++;
        if (p->i >= p->n || str_at(p->s, p->i) < '0' || str_at(p->s, p->i) > '9') return jp_error(p, "Unterminated fractional number");
        while (p->i < p->n && str_at(p->s, p->i) >= '0' && str_at(p->s, p->i) <= '9') {
            if (k < 390) buf[k++] = (char)str_at(p->s, p->i);
            p->i++;
        }
    }
    if (p->i < p->n && (str_at(p->s, p->i) | 0x20) == 'e') {
        buf[k++] = 'e';
        p->i++;
        if (p->i < p->n && (str_at(p->s, p->i) == '+' || str_at(p->s, p->i) == '-')) buf[k++] = (char)str_at(p->s, p->i++);
        if (p->i >= p->n || str_at(p->s, p->i) < '0' || str_at(p->s, p->i) > '9') return jp_error(p, "Exponent part is missing a number");
        while (p->i < p->n && str_at(p->s, p->i) >= '0' && str_at(p->s, p->i) <= '9') {
            if (k < 398) buf[k++] = (char)str_at(p->s, p->i);
            p->i++;
        }
    }
    (void)start;
    size_t used;
    bool neg = buf[0] == '-';
    double d = js_strtod(buf + (neg ? 1 : 0), (size_t)(k - (neg ? 1 : 0)), &used);
    return mknum(neg ? -d : d);
}

static bool jp_lit(JP *p, const char *w) {
    /* on mismatch p->i is left at the offending character (or the end) for the error message */
    for (; *w; w++, p->i++)
        if (p->i >= p->n || str_at(p->s, p->i) != (uint8_t)*w) return false;
    return true;
}

static jsval jp_value(JP *p, int depth) {
    js_rt *rt = p->rt;
    if (depth > 5000 || check_stack(rt)) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    jp_ws(p);
    if (p->i >= p->n) return jp_error(p, 0);
    uint16_t c = str_at(p->s, p->i);
    if (c == '{') {
        p->i++;
        JSObject *o = new_plain(rt);
        jsval *root = push_tmp(rt, mkobj(o));
        jp_ws(p);
        if (p->i < p->n && str_at(p->s, p->i) == '}') {
            p->i++;
            rt->sp = root;
            return mkobj(o);
        }
        for (bool first = true;; first = false) {
            jp_ws(p);
            if (p->i >= p->n || str_at(p->s, p->i) != '"')
                return jp_error(p, first ? "Expected property name or '}'" : "Expected double-quoted property name");
            jsval k = jp_string(p);
            TRY(k);
            k = atomize(rt, vstr(k));
            push_tmp(rt, k);
            jp_ws(p);
            if (p->i >= p->n || str_at(p->s, p->i) != ':') return jp_error(p, "Expected ':' after property name");
            p->i++;
            jsval v = jp_value(p, depth + 1);
            TRY(v);
            if (k == ATOM(__proto__) || o->nprops >= 8)
                create_data_prop(rt, o, k, v, false);
            else {
                Prop *pp = find_own(rt, o, k);
                if (pp)
                    pp->val = v;
                else
                    add_prop(rt, o, k, v, P_DEFAULT);
            }
            rt->sp = root + 1;
            jp_ws(p);
            if (p->i >= p->n) return jp_error(p, "Expected ',' or '}' after property value");
            c = str_at(p->s, p->i);
            if (c == ',') {
                p->i++;
                continue;
            }
            if (c == '}') {
                p->i++;
                break;
            }
            return jp_error(p, "Expected ',' or '}' after property value");
        }
        rt->sp = root;
        return mkobj(o);
    }
    if (c == '[') {
        p->i++;
        JSObject *a = new_array(rt, 0);
        jsval *root = push_tmp(rt, mkobj(a));
        jp_ws(p);
        if (p->i < p->n && str_at(p->s, p->i) == ']') {
            p->i++;
            rt->sp = root;
            return mkobj(a);
        }
        for (;;) {
            jsval v = jp_value(p, depth + 1);
            TRY(v);
            array_push(rt, a, v);
            jp_ws(p);
            if (p->i >= p->n) return jp_error(p, "Expected ',' or ']' after array element");
            c = str_at(p->s, p->i);
            if (c == ',') {
                p->i++;
                continue;
            }
            if (c == ']') {
                p->i++;
                break;
            }
            return jp_error(p, "Expected ',' or ']' after array element");
        }
        rt->sp = root;
        return mkobj(a);
    }
    if (c == '"') return jp_string(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p);
    if (c == 't' && jp_lit(p, "true")) return JS_TRUE;
    if (c == 'f' && jp_lit(p, "false")) return JS_FALSE;
    if (c == 'n' && jp_lit(p, "null")) return JS_NULL;
    return jp_error(p, 0);
}

static jsval internalize(js_rt *rt, jsval holder, jsval key, jsval reviver, int depth) {
    if (depth > 5000 || check_stack(rt)) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    jsval val = get_p(rt, holder, key);
    TRY(val);
    jsval *root = push_tmp(rt, val);
    if (is_obj(val)) {
        if (is_array(rt, val)) {
            uint64_t len;
            if (len_of(rt, val, &len) < 0) return JS_EXC;
            for (uint64_t i = 0; i < len; i++) {
                jsval k = atomize(rt, num_to_str(rt, (double)i));
                jsval nv = internalize(rt, val, k, reviver, depth + 1);
                TRY(nv);
                if (is_undef(nv)) {
                    if (delete_prop(rt, val, k, false) < 0) return JS_EXC;
                } else if (create_data_prop(rt, vobj(val), k, nv, false) < 0)
                    return JS_EXC;
            }
        } else {
            JSObject *keys = own_keys(rt, vobj(val), KEYS_STR | KEYS_ENUM_ONLY);
            if (!keys) return JS_EXC;
            push_tmp(rt, mkobj(keys));
            for (uint32_t i = 0; i < keys->u.arr.len; i++) {
                jsval k = keys->u.arr.elems[i];
                jsval nv = internalize(rt, val, k, reviver, depth + 1);
                TRY(nv);
                if (is_undef(nv)) {
                    if (delete_prop(rt, val, k, false) < 0) return JS_EXC;
                } else if (create_data_prop(rt, vobj(val), k, nv, false) < 0)
                    return JS_EXC;
            }
        }
    }
    jsval args[2] = {key, val};
    jsval r = call_fn(rt, reviver, holder, 2, args);
    rt->sp = root;
    return r;
}

NATIVE(json_parse) {
    JSString *s = to_string(rt, ARG(0));
    if (!s) return JS_EXC;
    str_flat(rt, s);
    push_tmp(rt, mkstr(s));
    JP p = {rt, s, 0, s->len};
    jsval v = jp_value(&p, 0);
    TRY(v);
    jp_ws(&p);
    if (p.i < p.n) return jp_error(&p, "Unexpected non-whitespace character after JSON");
    if (is_callable(ARG(1))) {
        push_tmp(rt, v);
        JSObject *root = new_plain(rt);
        push_tmp(rt, mkobj(root));
        add_prop(rt, root, ATOM(empty), v, P_DEFAULT);
        return internalize(rt, mkobj(root), ATOM(empty), argv[1], 0);
    }
    return v;
}

/* ---------------- JSON.stringify ---------------- */
typedef struct JS {
    js_rt *rt;
    SB b;
    jsval replacer;
    JSObject *plist;
    JSString *gap;
    JSString *indent;
    JSObject *stack; /* array of objects being serialized */
    JSObject *keystack;
} JSt;

static void quote(SB *b, JSString *s) {
    static const char hex[] = "0123456789abcdef";
    sb_putc(b, '"');
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = str_at(s, i);
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len) {
                    uint32_t d = str_at(s, i + 1);
                    if (d >= 0xDC00 && d <= 0xDFFF) {
                        sb_putc(b, c);
                        sb_putc(b, d);
                        i++;
                        break;
                    }
                }
                sb_puts(b, "\\u");
                sb_putc(b, (uint8_t)hex[(c >> 12) & 15]);
                sb_putc(b, (uint8_t)hex[(c >> 8) & 15]);
                sb_putc(b, (uint8_t)hex[(c >> 4) & 15]);
                sb_putc(b, (uint8_t)hex[c & 15]);
            } else
                sb_putc(b, c);
        }
    }
    sb_putc(b, '"');
}

static jsval circular_error(JSt *j, JSObject *o) {
    js_rt *rt = j->rt;
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, "Converting circular structure to JSON");
    uint32_t start = 0;
    for (uint32_t i = 0; i < j->stack->u.arr.len; i++)
        if (vobj(j->stack->u.arr.elems[i]) == o) start = i;
    for (uint32_t i = start; i < j->stack->u.arr.len; i++) {
        JSObject *x = vobj(j->stack->u.arr.elems[i]);
        jsval cn = JS_UNDEF;
        jsval c = get_p(rt, mkobj(x), ATOM(constructor));
        if (c != JS_EXC && is_obj(c)) cn = func_name_of(rt, vobj(c));
        rt->has_exc = false;
        JSString *cns = is_str(cn) && vstr(cn)->len ? vstr(cn) : str_cstr(rt, "Object");
        if (i == start) {
            sb_puts(&b, "\n    --> starting at object with constructor '");
            sb_putstr(&b, cns);
            sb_puts(&b, "'");
        } else {
            sb_puts(&b, "\n    |     ");
            jsval k = j->keystack->u.arr.elems[i];
            if (key_index(rt, k) >= 0) {
                sb_puts(&b, "index ");
                sb_putstr(&b, vstr(k));
            } else {
                sb_puts(&b, "property '");
                sb_putstr(&b, vstr(key_to_display(rt, k)));
                sb_puts(&b, "'");
            }
            sb_puts(&b, " -> object with constructor '");
            sb_putstr(&b, cns);
            sb_puts(&b, "'");
        }
    }
    jsval k = j->keystack->u.arr.elems[j->keystack->u.arr.len - 1];
    if (key_index(rt, k) >= 0) {
        sb_puts(&b, "\n    --- index ");
        sb_putstr(&b, vstr(k));
        sb_puts(&b, " closes the circle");
    } else {
        sb_puts(&b, "\n    --- property '");
        sb_putstr(&b, vstr(key_to_display(rt, k)));
        sb_puts(&b, "' closes the circle");
    }
    JSString *m = sb_finish(&b);
    return throw_error(rt, E_TYPE, "%S", m);
}

static int ser(JSt *j, jsval holder, jsval key, jsval val);

static int ser_obj(JSt *j, jsval val) {
    js_rt *rt = j->rt;
    JSObject *o = vobj(val);
    for (uint32_t i = 0; i < j->stack->u.arr.len; i++)
        if (vobj(j->stack->u.arr.elems[i]) == o) {
            circular_error(j, o);
            return -1;
        }
    if (j->stack->u.arr.len > 3000 || check_stack(rt)) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    array_push(rt, j->stack, val);
    JSString *stepback = j->indent;
    if (j->gap->len) j->indent = str_concat(rt, j->indent, j->gap);
    jsval *root = push_tmp(rt, mkstr(j->indent));
    push_tmp(rt, mkstr(stepback));
    bool arr = is_array(rt, val);
    int r = 0;
    if (arr) {
        uint64_t len;
        if (len_of(rt, val, &len) < 0) goto fail;
        sb_putc(&j->b, '[');
        for (uint64_t i = 0; i < len; i++) {
            if (i) sb_putc(&j->b, ',');
            if (j->gap->len) {
                sb_putc(&j->b, '\n');
                sb_putstr(&j->b, j->indent);
            }
            jsval k = atomize(rt, num_to_str(rt, (double)i));
            jsval v = get_idx(rt, val, (uint32_t)i);
            if (v == JS_EXC) goto fail;
            uint32_t before = j->b.len;
            array_push(rt, j->keystack, k);
            int sr = ser(j, val, k, v);
            j->keystack->u.arr.len--;
            if (sr < 0) goto fail;
            if (sr == 0) {
                j->b.len = before;
                sb_puts(&j->b, "null");
            }
        }
        if (len && j->gap->len) {
            sb_putc(&j->b, '\n');
            sb_putstr(&j->b, stepback);
        }
        sb_putc(&j->b, ']');
    } else {
        JSObject *keys = j->plist;
        if (!keys) {
            keys = own_keys(rt, o, KEYS_STR | KEYS_ENUM_ONLY);
            if (!keys) goto fail;
        }
        push_tmp(rt, mkobj(keys));
        sb_putc(&j->b, '{');
        bool any = false;
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jsval k = keys->u.arr.elems[i];
            jsval v;
            Prop *p = (o->oflags & OF_EXOTIC) ? 0 : find_own(rt, o, k);
            if (p && !(p->flags & P_ACC))
                v = p->val;
            else
                v = get_p(rt, val, k);
            if (v == JS_EXC) goto fail;
            uint32_t before = j->b.len;
            if (any) sb_putc(&j->b, ',');
            if (j->gap->len) {
                sb_putc(&j->b, '\n');
                sb_putstr(&j->b, j->indent);
            }
            quote(&j->b, vstr(k));
            sb_putc(&j->b, ':');
            if (j->gap->len) sb_putc(&j->b, ' ');
            array_push(rt, j->keystack, k);
            int sr = ser(j, val, k, v);
            j->keystack->u.arr.len--;
            if (sr < 0) goto fail;
            if (sr == 0)
                j->b.len = before;
            else
                any = true;
        }
        if (any && j->gap->len) {
            sb_putc(&j->b, '\n');
            sb_putstr(&j->b, stepback);
        }
        sb_putc(&j->b, '}');
    }
    j->stack->u.arr.len--;
    j->indent = stepback;
    rt->sp = root;
    return 1;
fail:
    r = -1;
    j->stack->u.arr.len--;
    j->indent = stepback;
    rt->sp = root;
    return r;
}

/* returns 1 written, 0 undefined (nothing written), -1 error */
static int ser(JSt *j, jsval holder, jsval key, jsval val) {
    js_rt *rt = j->rt;
    jsval *root = push_tmp(rt, val);
    if (is_obj(val) || is_big(val)) {
        jsval tj = get_p(rt, val, ATOM(toJSON));
        if (tj == JS_EXC) goto fail;
        if (is_callable(tj)) {
            val = call_fn(rt, tj, val, 1, &key);
            if (val == JS_EXC) goto fail;
            *root = val;
        }
    }
    if (!is_undef(j->replacer)) {
        jsval args[2] = {key, val};
        val = call_fn(rt, j->replacer, holder, 2, args);
        if (val == JS_EXC) goto fail;
        *root = val;
    }
    if (is_obj(val)) {
        JSObject *o = vobj(val);
        if (o->cls == CLS_NUMBER) {
            double d;
            if (to_number(rt, val, &d) < 0) goto fail;
            val = mknum(d);
        } else if (o->cls == CLS_STRING) {
            JSString *s = to_string(rt, val);
            if (!s) goto fail;
            val = mkstr(s);
        } else if (o->cls == CLS_BOOLEAN || o->cls == CLS_BIGINT)
            val = o->u.prim.v;
    }
    rt->sp = root;
    if (is_null(val)) {
        sb_puts(&j->b, "null");
        return 1;
    }
    if (val == JS_TRUE) {
        sb_puts(&j->b, "true");
        return 1;
    }
    if (val == JS_FALSE) {
        sb_puts(&j->b, "false");
        return 1;
    }
    if (is_str(val)) {
        quote(&j->b, str_flat(rt, vstr(val)));
        return 1;
    }
    if (is_num(val)) {
        double d = vnum(val);
        if (!js_isfinite(d))
            sb_puts(&j->b, "null");
        else {
            char t[40];
            int n = num_to_cstr(d, t);
            sb_putn(&j->b, t, (uint32_t)n);
        }
        return 1;
    }
    if (is_big(val)) {
        throw_error(rt, E_TYPE, "Do not know how to serialize a BigInt");
        return -1;
    }
    if (is_obj(val) && !(vobj(val)->oflags & OF_CALLABLE)) {
        push_tmp(rt, val);
        int r = ser_obj(j, val);
        rt->sp = root;
        return r;
    }
    return 0;
fail:
    rt->sp = root;
    return -1;
}

jsval json_stringify(js_rt *rt, jsval v, jsval replacer, jsval space) {
    JSt j;
    memset(&j, 0, sizeof j);
    j.rt = rt;
    j.replacer = JS_UNDEF;
    jsval *root = push_tmp(rt, v);
    if (is_callable(replacer))
        j.replacer = replacer;
    else if (is_array(rt, replacer)) {
        JSObject *pl = new_array(rt, 0);
        push_tmp(rt, mkobj(pl));
        uint64_t len;
        if (len_of(rt, replacer, &len) < 0) return JS_EXC;
        for (uint64_t i = 0; i < len; i++) {
            jsval e = get_idx(rt, replacer, (uint32_t)i);
            if (e == JS_EXC) return JS_EXC;
            jsval item = JS_UNDEF;
            if (is_str(e) || is_num(e) || is_cls(e, CLS_STRING) || is_cls(e, CLS_NUMBER)) {
                JSString *s = to_string(rt, e);
                if (!s) return JS_EXC;
                item = atomize(rt, s);
            }
            if (is_undef(item)) continue;
            bool dup = false;
            for (uint32_t k = 0; k < pl->u.arr.len; k++)
                if (pl->u.arr.elems[k] == item) dup = true;
            if (!dup) array_push(rt, pl, item);
        }
        j.plist = pl;
    }
    push_tmp(rt, j.replacer);
    if (is_cls(space, CLS_NUMBER)) {
        double d;
        if (to_number(rt, space, &d) < 0) return JS_EXC;
        space = mknum(d);
    } else if (is_cls(space, CLS_STRING)) {
        JSString *s = to_string(rt, space);
        if (!s) return JS_EXC;
        space = mkstr(s);
    }
    if (is_num(space)) {
        double d = to_integer(vnum(space));
        int n = d > 10 ? 10 : d < 0 ? 0 : (int)d;
        char sp[11] = "          ";
        sp[n] = 0;
        j.gap = str_cstr(rt, sp);
    } else if (is_str(space)) {
        JSString *s = vstr(space);
        j.gap = s->len > 10 ? str_sub(rt, s, 0, 10) : s;
    } else
        j.gap = vstr(ATOM(empty));
    push_tmp(rt, mkstr(j.gap));
    j.indent = vstr(ATOM(empty));
    j.stack = new_array(rt, 0);
    push_tmp(rt, mkobj(j.stack));
    j.keystack = new_array(rt, 0);
    push_tmp(rt, mkobj(j.keystack));
    JSObject *wrapper = new_plain(rt);
    push_tmp(rt, mkobj(wrapper));
    add_prop(rt, wrapper, ATOM(empty), v, P_DEFAULT);
    sb_init(&j.b, rt);
    array_push(rt, j.keystack, ATOM(empty));
    int r = ser(&j, mkobj(wrapper), ATOM(empty), v);
    rt->sp = root;
    if (r < 0) {
        sb_free(&j.b);
        return JS_EXC;
    }
    if (r == 0) {
        sb_free(&j.b);
        return JS_UNDEF;
    }
    JSString *s = sb_finish(&j.b);
    return s ? mkstr(s) : JS_EXC;
}
NATIVE(json_stringify_n) { return json_stringify(rt, ARG(0), ARG(1), ARG(2)); }

void init_json(js_rt *rt) {
    JSObject *j = new_plain(rt);
    def_value(rt, rt->global, "JSON", mkobj(j));
    def_method(rt, j, "parse", json_parse, 2, 0);
    def_method(rt, j, "stringify", json_stringify_n, 3, 0);
    def_prop(rt, j, SYM(toStringTag), mkstr(str_cstr(rt, "JSON")), P_C);
}

/* ---------------- inspect (node util.inspect subset) ---------------- */
typedef struct IC {
    js_rt *rt;
    int depth;        /* max depth (2) */
    int indent;       /* indentationLvl */
    int cur_depth;    /* ctx.currentDepth */
    JSObject *seen;   /* stack of objects */
    JSObject *circ;   /* objects referenced circularly (index = id-1) */
    bool err;
} IC;

static JSString *ins(IC *c, jsval v, int rec, bool top);

static bool ident_key(js_rt *rt, JSString *s) {
    str_flat(rt, s);
    if (s->len == 0) return false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t ch = str_at(s, i);
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || (i > 0 && ch >= '0' && ch <= '9');
        if (!ok) return false;
    }
    return true;
}

static void str_escape(SB *b, JSString *s) {
    static const char hex[] = "0123456789ABCDEF";
    uint16_t q = '\'';
    bool has_s = false, has_d = false, has_b = false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t ch = str_at(s, i);
        if (ch == '\'') has_s = true;
        if (ch == '"') has_d = true;
        if (ch == '`') has_b = true;
    }
    if (has_s) {
        if (!has_d)
            q = '"';
        else if (!has_b)
            q = '`';
    }
    sb_putc(b, q);
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t ch = str_at(s, i);
        if (ch == q || ch == '\\') {
            sb_putc(b, '\\');
            sb_putc(b, ch);
        } else if (ch == '\n')
            sb_puts(b, "\\n");
        else if (ch == '\t')
            sb_puts(b, "\\t");
        else if (ch == '\r')
            sb_puts(b, "\\r");
        else if (ch == '\b')
            sb_puts(b, "\\b");
        else if (ch == '\f')
            sb_puts(b, "\\f");
        else if (ch == '\v')
            sb_puts(b, "\\v");
        else if (ch < 0x20 || ch == 0x7F) {
            sb_puts(b, "\\x");
            sb_putc(b, (uint8_t)hex[ch >> 4]);
            sb_putc(b, (uint8_t)hex[ch & 15]);
        } else if (ch >= 0xD800 && ch <= 0xDFFF) {
            bool pair = ch <= 0xDBFF && i + 1 < s->len && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF;
            if (pair) {
                sb_putc(b, ch);
                sb_putc(b, str_at(s, ++i));
            } else {
                sb_puts(b, "\\u");
                sb_putc(b, (uint8_t)"0123456789abcdef"[(ch >> 12) & 15]);
                sb_putc(b, (uint8_t)"0123456789abcdef"[(ch >> 8) & 15]);
                sb_putc(b, (uint8_t)"0123456789abcdef"[(ch >> 4) & 15]);
                sb_putc(b, (uint8_t)"0123456789abcdef"[ch & 15]);
            }
        } else
            sb_putc(b, ch);
    }
    sb_putc(b, q);
}

static JSString *fmt_number(js_rt *rt, double d) {
    if (d == 0 && js_signbit(d)) return str_cstr(rt, "-0");
    return num_to_str(rt, d);
}

static jsval ctor_name(js_rt *rt, JSObject *o, bool *nullproto) {
    *nullproto = false;
    JSObject *p = o;
    int guard = 0;
    while (p && guard++ < 1000) {
        if (p->cls == CLS_PROXY) break;
        Prop *cp = find_own(rt, p, ATOM(constructor));
        if (cp && !(cp->flags & P_ACC) && is_callable(cp->val)) {
            jsval n = func_name_of(rt, vobj(cp->val));
            if (is_str(n) && vstr(n)->len) return n;
        }
        p = p->proto;
    }
    if (!o->proto) *nullproto = true;
    return JS_UNDEF;
}

static JSString *join_entries(IC *c, JSObject *out, const char *sep_in) {
    js_rt *rt = c->rt;
    SB b;
    sb_init(&b, rt);
    for (uint32_t i = 0; i < out->u.arr.len; i++) {
        if (i) sb_pututf8(&b, sep_in, strlen(sep_in));
        sb_putstr(&b, vstr(out->u.arr.elems[i]));
    }
    return sb_finish(&b);
}

static bool contains_nl(JSString *s) {
    for (uint32_t i = 0; i < s->len; i++)
        if (str_at(s, i) == '\n') return true;
    return false;
}

static JSObject *group_array(IC *c, JSObject *out, bool has_more, JSObject *value) {
    js_rt *rt = c->rt;
    uint32_t total = 0, maxlen = 0;
    uint32_t outlen = out->u.arr.len - (has_more ? 1 : 0);
    const uint32_t sepsp = 2;
    uint32_t *dl = (uint32_t *)malloc((outlen ? outlen : 1) * 4);
    for (uint32_t i = 0; i < outlen; i++) {
        uint32_t len = vstr(out->u.arr.elems[i])->len;
        dl[i] = len;
        total += len + sepsp;
        if (maxlen < len) maxlen = len;
    }
    uint32_t actual = maxlen + sepsp;
    JSObject *res = out;
    if (actual * 3 + (uint32_t)c->indent < 80 && ((double)total / actual > 5 || maxlen <= 6)) {
        double avg_bias = js_sqrt(actual - (double)total / out->u.arr.len);
        double biased = actual - 3 - avg_bias;
        if (biased < 1) biased = 1;
        double c1 = js_round(js_sqrt(2.5 * biased * outlen) / biased);
        double c2 = js_floor((80.0 - c->indent) / actual);
        double cols = c1;
        if (c2 < cols) cols = c2;
        if (12 < cols) cols = 12;
        if (15 < cols) cols = 15;
        int columns = (int)cols;
        if (columns > 1) {
            uint32_t *mll = (uint32_t *)malloc((size_t)columns * 4);
            for (int i = 0; i < columns; i++) {
                uint32_t ll = 0;
                for (uint32_t k = (uint32_t)i; k < out->u.arr.len; k += (uint32_t)columns)
                    if (k < outlen && dl[k] > ll) ll = dl[k];
                mll[i] = ll + sepsp;
            }
            bool pad_start = true;
            if (value) {
                for (uint32_t i = 0; i < out->u.arr.len && i < value->u.arr.len; i++) {
                    jsval e = value->cls == CLS_ARRAY ? value->u.arr.elems[i] : JS_UNDEF;
                    if (!is_num(e) && !is_big(e)) {
                        pad_start = false;
                        break;
                    }
                }
            } else
                pad_start = false;
            res = new_array(rt, 0);
            push_tmp(rt, mkobj(res));
            for (uint32_t i = 0; i < outlen; i += (uint32_t)columns) {
                uint32_t mx = i + (uint32_t)columns < outlen ? i + (uint32_t)columns : outlen;
                SB b;
                sb_init(&b, rt);
                uint32_t j = i;
                for (; j < mx - 1; j++) {
                    JSString *s = vstr(out->u.arr.elems[j]);
                    uint32_t pad = mll[j - i];
                    uint32_t w = s->len + 2;
                    if (pad_start)
                        for (uint32_t k = w; k < pad; k++) sb_putc(&b, ' ');
                    sb_putstr(&b, s);
                    sb_puts(&b, ", ");
                    if (!pad_start)
                        for (uint32_t k = w; k < pad; k++) sb_putc(&b, ' ');
                }
                JSString *s = vstr(out->u.arr.elems[j]);
                if (pad_start) {
                    uint32_t pad = mll[j - i] - sepsp;
                    for (uint32_t k = s->len; k < pad; k++) sb_putc(&b, ' ');
                }
                sb_putstr(&b, s);
                array_push(rt, res, mkstr(sb_finish(&b)));
            }
            if (has_more) array_push(rt, res, out->u.arr.elems[outlen]);
            free(mll);
        }
    }
    free(dl);
    return res;
}

static JSString *reduce_single(IC *c, JSObject *out, JSString *base, const char *b0, const char *b1, int rec, bool is_array_type,
                               bool has_more, JSObject *value) {
    js_rt *rt = c->rt;
    uint32_t entries = out->u.arr.len;
    if (entries == 0) return base->len ? jsfmt(rt, "%S %s%s", base, b0, b1) : jsfmt(rt, "%s%s", b0, b1);
    if (is_array_type && entries > 6) out = group_array(c, out, has_more, value);
    if (c->cur_depth - rec < 3 && entries == out->u.arr.len) {
        uint32_t start = out->u.arr.len + (uint32_t)c->indent + (uint32_t)strlen(b0) + base->len + 10;
        uint32_t total = out->u.arr.len + start;
        bool below = total + out->u.arr.len <= 80;
        for (uint32_t i = 0; below && i < out->u.arr.len; i++) {
            total += vstr(out->u.arr.elems[i])->len;
            if (total > 80) below = false;
        }
        if (below && !contains_nl(base)) {
            JSString *joined = join_entries(c, out, ", ");
            if (!contains_nl(joined)) {
                if (base->len) return jsfmt(rt, "%S %s %S %s", base, b0, joined, b1);
                return jsfmt(rt, "%s %S %s", b0, joined, b1);
            }
        }
    }
    SB ind;
    sb_init(&ind, rt);
    sb_putc(&ind, '\n');
    for (int i = 0; i < c->indent; i++) sb_putc(&ind, ' ');
    JSString *indent = sb_finish(&ind);
    SB sep;
    sb_init(&sep, rt);
    sb_putc(&sep, ',');
    sb_putstr(&sep, indent);
    sb_puts(&sep, "  ");
    JSString *seps = sb_finish(&sep);
    SB b;
    sb_init(&b, rt);
    if (base->len) {
        sb_putstr(&b, base);
        sb_putc(&b, ' ');
    }
    sb_puts(&b, b0);
    sb_putstr(&b, indent);
    sb_puts(&b, "  ");
    for (uint32_t i = 0; i < out->u.arr.len; i++) {
        if (i) sb_putstr(&b, seps);
        sb_putstr(&b, vstr(out->u.arr.elems[i]));
    }
    sb_putstr(&b, indent);
    sb_puts(&b, b1);
    return sb_finish(&b);
}

static JSString *fmt_key(IC *c, jsval k) {
    js_rt *rt = c->rt;
    if (is_sym(k)) return jsfmt(rt, "[%S]", vstr(key_to_display(rt, k)));
    JSString *s = vstr(k);
    if (ident_key(rt, s)) return s;
    SB b;
    sb_init(&b, rt);
    str_escape(&b, s);
    return sb_finish(&b);
}

static JSString *fmt_prop(IC *c, JSObject *o, jsval k, int rec, bool array_like) {
    js_rt *rt = c->rt;
    PropDesc d;
    int e = get_own_desc(rt, o, k, &d);
    if (e < 0) {
        rt->has_exc = false;
        rt->exc = JS_UNDEF;
        return str_cstr(rt, "?");
    }
    JSString *vs;
    if (d.has & (HAS_GET | HAS_SET)) {
        bool g = !is_undef(d.get), s = !is_undef(d.set);
        vs = str_cstr(rt, g && s ? "[Getter/Setter]" : g ? "[Getter]" : s ? "[Setter]" : "undefined");
    } else {
        c->indent += 2;
        vs = ins(c, d.value, rec, false);
        c->indent -= 2;
    }
    if (array_like) return vs;
    return jsfmt(rt, "%S: %S", fmt_key(c, k), vs);
}

static const char *func_kind(JSObject *f) {
    if (f->cls == CLS_FUNCTION) {
        uint32_t fl = f->u.fn.fp->flags;
        if ((fl & FP_GENERATOR) && (fl & FP_ASYNC)) return "AsyncGeneratorFunction";
        if (fl & FP_GENERATOR) return "GeneratorFunction";
        if (fl & FP_ASYNC) return "AsyncFunction";
    }
    return "Function";
}

static JSString *ins_obj(IC *c, JSObject *o, int rec) {
    js_rt *rt = c->rt;
    bool nullproto;
    jsval cn = ctor_name(rt, o, &nullproto);
    JSString *cname = is_str(cn) ? vstr(cn) : 0;
    jsval tagv = JS_UNDEF;
    if (o->cls != CLS_PROXY) {
        Prop *tp = 0;
        for (JSObject *p = o; p && !tp; p = p->proto) tp = find_own(rt, p, SYM(toStringTag));
        if (tp && !(tp->flags & P_ACC) && is_str(tp->val) && vstr(tp->val)->len) tagv = tp->val;
    }
    JSString *base = vstr(ATOM(empty));
    const char *b0 = "{", *b1 = "}";
    bool arr = o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS || o->cls == CLS_TYPEDARRAY;
    bool callable = (o->oflags & OF_CALLABLE) != 0;
    JSObject *keys = own_keys(rt, o, KEYS_STR | KEYS_SYM | KEYS_ENUM_ONLY);
    if (!keys) {
        rt->has_exc = false;
        rt->exc = JS_UNDEF;
        keys = new_array(rt, 0);
    }
    jsval *root = push_tmp(rt, mkobj(keys));
    uint32_t extra_keys = 0;
    JSObject *out = new_array(rt, 0);
    push_tmp(rt, mkobj(out));
    /* prefix */
    SB pre;
    sb_init(&pre, rt);
    bool plain_object = false;
    if (callable) {
        jsval nm = func_name_of(rt, o);
        if (o->oflags & OF_CLASS_CTOR) {
            sb_puts(&pre, "[class ");
            if (is_str(nm) && vstr(nm)->len)
                sb_putstr(&pre, vstr(nm));
            else
                sb_puts(&pre, "(anonymous)");
            if (o->proto && o->proto != rt->intr[I_FunctionProto]) {
                jsval pn = func_name_of(rt, o->proto);
                sb_puts(&pre, " extends ");
                if (is_str(pn) && vstr(pn)->len) sb_putstr(&pre, vstr(pn));
            }
            sb_puts(&pre, "]");
        } else {
            sb_puts(&pre, "[");
            sb_puts(&pre, func_kind(o));
            if (is_str(nm) && vstr(nm)->len) {
                sb_puts(&pre, ": ");
                sb_putstr(&pre, vstr(nm));
            } else
                sb_puts(&pre, " (anonymous)");
            sb_puts(&pre, "]");
        }
        base = sb_finish(&pre);
        /* skip non-enumerable 'prototype' etc.: keys are enumerable only */
        if (keys->u.arr.len == 0) {
            rt->sp = root;
            return base;
        }
    } else {
        if (arr) {
            b0 = "[";
            b1 = "]";
            if (o->cls == CLS_ARGUMENTS)
                sb_puts(&pre, "[Arguments] ");
            else if (o->cls == CLS_TYPEDARRAY) {
                sb_putstr(&pre, cname ? cname : vstr(ATOM(empty)));
                sb_puts(&pre, "(");
                sb_putstr(&pre, num_to_str(rt, ta_length(o)));
                sb_puts(&pre, ") ");
            } else if (cname && !str_eq(rt, cname, str_cstr(rt, "Array"))) {
                sb_putstr(&pre, cname);
                sb_puts(&pre, "(");
                sb_putstr(&pre, num_to_str(rt, o->u.arr.len));
                sb_puts(&pre, ") ");
            } else if (nullproto) {
                sb_puts(&pre, "[Array(");
                sb_putstr(&pre, num_to_str(rt, o->u.arr.len));
                sb_puts(&pre, "): null prototype] ");
            }
        } else if (o->cls == CLS_MAP || o->cls == CLS_SET) {
            sb_puts(&pre, o->cls == CLS_MAP ? "Map(" : "Set(");
            jsval sz = get_p(rt, mkobj(o), ATOM(size));
            if (sz == JS_EXC) {
                rt->has_exc = false;
                sz = mknum(0);
            }
            sb_putstr(&pre, num_to_str(rt, is_num(sz) ? vnum(sz) : 0));
            sb_puts(&pre, ") ");
        } else if (nullproto) {
            sb_puts(&pre, "[Object: null prototype] ");
        } else if (cname && !str_eq(rt, cname, str_cstr(rt, "Object"))) {
            sb_putstr(&pre, cname);
            sb_puts(&pre, " ");
        } else
            plain_object = true;
        if (is_str(tagv) && (!cname || !str_eq(rt, cname, vstr(tagv)))) {
            if (plain_object) sb_puts(&pre, "Object ");
            sb_puts(&pre, "[");
            sb_putstr(&pre, vstr(tagv));
            sb_puts(&pre, "] ");
        }
        JSString *ps = sb_finish(&pre);
        /* strip trailing space into braces */
        if (ps->len) {
            if (arr && ps->len >= 1) {
                b0 = "[";
            }
            JSString *t = str_sub(rt, ps, 0, ps->len - 1);
            b0 = 0;
            base = t;
        }
        if (!b0) b0 = arr ? "[" : "{";
        if (arr) b1 = "]";
    }
    if (o->cls == CLS_ERROR) {
        jsval st = get_p(rt, mkobj(o), ATOM(stack));
        if (st == JS_EXC) {
            rt->has_exc = false;
            st = JS_UNDEF;
        }
        JSString *s = is_str(st) ? vstr(st) : error_to_string(rt, mkobj(o));
        if (!s) {
            rt->has_exc = false;
            s = str_cstr(rt, "Error");
        }
        uint32_t nk = 0;
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jsval k = keys->u.arr.elems[i];
            if (k == ATOM(stack) || k == ATOM(message)) continue;
            nk++;
        }
        if (rec > c->depth || nk == 0) {
            rt->sp = root;
            if (rec > c->depth || !is_str(st)) return jsfmt(rt, "[%S]", s);
            return s;
        }
        base = s;
    }
    if (o->cls == CLS_DATE || o->cls == CLS_REGEXP || o->cls == CLS_NUMBER || o->cls == CLS_STRING || o->cls == CLS_BOOLEAN ||
        o->cls == CLS_SYMBOL || o->cls == CLS_BIGINT) {
        SB b;
        sb_init(&b, rt);
        if (o->cls == CLS_DATE) {
            jsval r = call_method(rt, mkobj(o), atom_c(rt, "toISOString"), 0, 0);
            if (r == JS_EXC || !is_str(r)) {
                rt->has_exc = false;
                rt->exc = JS_UNDEF;
                sb_puts(&b, "Invalid Date");
            } else
                sb_putstr(&b, vstr(r));
        } else if (o->cls == CLS_REGEXP) {
            jsval r = call_method(rt, mkobj(o), ATOM(toString), 0, 0);
            if (r != JS_EXC && is_str(r)) sb_putstr(&b, vstr(r));
            rt->has_exc = false;
        } else {
            const char *t = o->cls == CLS_NUMBER ? "Number" : o->cls == CLS_STRING ? "String" : o->cls == CLS_BOOLEAN ? "Boolean" : o->cls == CLS_SYMBOL ? "Symbol" : "BigInt";
            sb_puts(&b, "[");
            sb_puts(&b, t);
            sb_puts(&b, ": ");
            sb_putstr(&b, ins(c, o->u.prim.v, rec, false));
            sb_puts(&b, "]");
        }
        JSString *s = sb_finish(&b);
        uint32_t nk = keys->u.arr.len;
        if (o->cls == CLS_STRING) nk = 0;
        if (nk == 0) {
            rt->sp = root;
            return s;
        }
        base = s;
    }
    /* empty */
    bool has_entries = keys->u.arr.len > 0 || ((o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) && o->u.arr.len > 0) ||
                       o->cls == CLS_MAP || o->cls == CLS_SET || o->cls == CLS_PROMISE || o->cls == CLS_TYPEDARRAY ||
                       o->cls == CLS_WEAKMAP || o->cls == CLS_WEAKSET;
    if (!has_entries && !(o->cls == CLS_ARRAY && (o->oflags & OF_SLOW_ARRAY))) {
        rt->sp = root;
        if (arr) return base->len ? jsfmt(rt, "%S []", base) : str_cstr(rt, "[]");
        if (callable) return base;
        return base->len ? jsfmt(rt, "%S {}", base) : str_cstr(rt, "{}");
    }
    if (rec > c->depth) {
        rt->sp = root;
        if (arr) return jsfmt(rt, "[%s]", cname ? "Array" : "Array");
        return jsfmt(rt, "[%S]", cname ? cname : nullproto ? str_cstr(rt, "Object: null prototype") : str_cstr(rt, "Object"));
    }
    rec++;
    array_push(rt, c->seen, mkobj(o));
    c->cur_depth = rec;
    bool has_more = false;
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS || o->cls == CLS_TYPEDARRAY) {
        uint32_t len = o->cls == CLS_TYPEDARRAY ? ta_length(o) : o->u.arr.len;
        uint32_t shown = 0, i = 0;
        while (i < len && shown < 100) {
            jsval v;
            bool hole;
            if (o->cls == CLS_TYPEDARRAY) {
                hole = !ta_get(rt, o, i, &v);
            } else if (o->oflags & OF_SLOW_ARRAY) {
                Prop *p = find_own(rt, o, atomize(rt, num_to_str(rt, i)));
                hole = !p;
                v = p ? p->val : JS_UNDEF;
            } else {
                v = o->u.arr.elems[i];
                hole = v == JS_HOLE;
                if (!hole && o->cls == CLS_ARGUMENTS && o->u.arr.pmap) v = get_idx(rt, mkobj(o), i);
            }
            if (hole) {
                uint32_t j = i;
                while (j < len) {
                    bool h2;
                    if (o->oflags & OF_SLOW_ARRAY)
                        h2 = !find_own(rt, o, atomize(rt, num_to_str(rt, j)));
                    else
                        h2 = o->u.arr.elems[j] == JS_HOLE;
                    if (!h2) break;
                    j++;
                }
                uint32_t n = j - i;
                array_push(rt, out, mkstr(jsfmt(rt, "<%u empty item%s>", n, n > 1 ? "s" : "")));
                i = j;
                shown++;
                continue;
            }
            c->indent += 2;
            JSString *s = ins(c, v, rec, false);
            c->indent -= 2;
            array_push(rt, out, mkstr(s));
            i++;
            shown++;
        }
        if (i < len) {
            uint32_t rem = len - i;
            array_push(rt, out, mkstr(jsfmt(rt, "... %u more item%s", rem, rem > 1 ? "s" : "")));
            has_more = true;
        }
        for (uint32_t k = 0; k < keys->u.arr.len; k++) {
            jsval key = keys->u.arr.elems[k];
            if (key_index(rt, key) >= 0) continue;
            array_push(rt, out, mkstr(fmt_prop(c, o, key, rec, false)));
            extra_keys++;
        }
    } else if (o->cls == CLS_MAP || o->cls == CLS_SET) {
        jsval it = call_method(rt, mkobj(o), ATOM(entries), 0, 0);
        if (it != JS_EXC) {
            push_tmp(rt, it);
            jsval next = get_p(rt, it, ATOM(next));
            push_tmp(rt, next);
            jsval e;
            while (iter_step(rt, it, next, &e) > 0) {
                jsval *r2 = push_tmp(rt, e);
                jsval k = get_idx(rt, e, 0), v = get_idx(rt, e, 1);
                c->indent += 2;
                JSString *ks = ins(c, k, rec, false);
                if (o->cls == CLS_MAP) {
                    push_tmp(rt, mkstr(ks));
                    JSString *vs = ins(c, v, rec, false);
                    ks = jsfmt(rt, "%S => %S", ks, vs);
                }
                c->indent -= 2;
                rt->sp = r2;
                array_push(rt, out, mkstr(ks));
            }
        }
        rt->has_exc = false;
        for (uint32_t k = 0; k < keys->u.arr.len; k++) array_push(rt, out, mkstr(fmt_prop(c, o, keys->u.arr.elems[k], rec, false)));
    } else if (o->cls == CLS_WEAKMAP || o->cls == CLS_WEAKSET) {
        array_push(rt, out, mkstr(str_cstr(rt, "<items unknown>")));
    } else if (o->cls == CLS_PROMISE) {
        int st = promise_state(o);
        jsval res = o->u.data.v;
        if (st == 0)
            array_push(rt, out, mkstr(str_cstr(rt, "<pending>")));
        else {
            c->indent += 2;
            JSString *s = ins(c, res, rec, false);
            c->indent -= 2;
            array_push(rt, out, mkstr(st == 2 ? jsfmt(rt, "<rejected> %S", s) : s));
        }
        for (uint32_t k = 0; k < keys->u.arr.len; k++) array_push(rt, out, mkstr(fmt_prop(c, o, keys->u.arr.elems[k], rec, false)));
    } else {
        for (uint32_t k = 0; k < keys->u.arr.len; k++) {
            jsval key = keys->u.arr.elems[k];
            if (o->cls == CLS_ERROR && (key == ATOM(stack) || key == ATOM(message))) continue;
            array_push(rt, out, mkstr(fmt_prop(c, o, key, rec, false)));
        }
    }
    c->seen->u.arr.len--;
    JSString *res = reduce_single(c, out, base, b0, b1, rec, arr && extra_keys == 0 && o->cls != CLS_TYPEDARRAY ? true : arr, has_more,
                                  o->cls == CLS_ARRAY && !(o->oflags & OF_SLOW_ARRAY) ? o : 0);
    /* circular reference marker */
    for (uint32_t i = 0; i < c->circ->u.arr.len; i++)
        if (vobj(c->circ->u.arr.elems[i]) == o) {
            res = jsfmt(rt, "<ref *%u> %S", i + 1, res);
            break;
        }
    rt->sp = root;
    return res;
}

static JSString *ins(IC *c, jsval v, int rec, bool top) {
    js_rt *rt = c->rt;
    if (check_stack(rt)) return str_cstr(rt, "[Object]");
    if (is_str(v)) {
        if (top) return vstr(v);
        SB b;
        sb_init(&b, rt);
        str_escape(&b, str_flat(rt, vstr(v)));
        return sb_finish(&b);
    }
    if (is_num(v)) return fmt_number(rt, vnum(v));
    if (is_big(v)) return jsfmt(rt, "%Sn", big_to_string(rt, (JSBig *)vptr(v), 10));
    if (is_sym(v)) return vstr(key_to_display(rt, v));
    if (!is_obj(v)) return to_string_prim(rt, v);
    JSObject *o = vobj(v);
    for (uint32_t i = 0; i < c->seen->u.arr.len; i++)
        if (vobj(c->seen->u.arr.elems[i]) == o) {
            uint32_t id = 0;
            for (uint32_t k = 0; k < c->circ->u.arr.len; k++)
                if (vobj(c->circ->u.arr.elems[k]) == o) id = k + 1;
            if (!id) {
                array_push(rt, c->circ, v);
                id = c->circ->u.arr.len;
            }
            return jsfmt(rt, "[Circular *%u]", id);
        }
    if (o->cls == CLS_PROXY) {
        jsval t = o->u.it.a;
        if (!is_obj(t)) return str_cstr(rt, "<Revoked Proxy>");
        return ins(c, t, rec, top);
    }
    return ins_obj(c, o, rec);
}

JSString *inspect_value(js_rt *rt, jsval v, int top) {
    IC c;
    memset(&c, 0, sizeof c);
    c.rt = rt;
    c.depth = 2;
    jsval *root = push_tmp(rt, v);
    c.seen = new_array(rt, 0);
    push_tmp(rt, mkobj(c.seen));
    c.circ = new_array(rt, 0);
    push_tmp(rt, mkobj(c.circ));
    bool saved_exc = rt->has_exc;
    jsval saved = rt->exc;
    push_tmp(rt, saved);
    rt->has_exc = false;
    JSString *s = ins(&c, v, 0, top != 0);
    rt->has_exc = saved_exc;
    rt->exc = saved;
    rt->sp = root;
    return s ? s : str_cstr(rt, "");
}

/* ---------------- console ---------------- */
#define group_indent (rt->group_indent)
#define console_counts (rt->intr[I_ConsoleCounts])

static void emit(js_rt *rt, int level, JSString *s) {
    if (!rt->host.console) return;
    if (group_indent > 0) {
        SB b;
        sb_init(&b, rt);
        str_flat(rt, s);
        for (int i = 0; i < group_indent; i++) sb_putc(&b, ' ');
        for (uint32_t i = 0; i < s->len; i++) {
            uint16_t ch = str_at(s, i);
            sb_putc(&b, ch);
            if (ch == '\n')
                for (int k = 0; k < group_indent; k++) sb_putc(&b, ' ');
        }
        s = sb_finish(&b);
    }
    char *u = str_to_utf8(rt, s, 0);
    if (u) {
        rt->host.console(rt->host.ctx, level, u);
        free(u);
    }
}

static JSString *format_args(js_rt *rt, int argc, jsval *argv) {
    SB b;
    sb_init(&b, rt);
    int ai = 0;
    if (argc > 0 && is_str(argv[0])) {
        JSString *f = str_flat(rt, vstr(argv[0]));
        ai = 1;
        for (uint32_t i = 0; i < f->len; i++) {
            uint16_t ch = str_at(f, i);
            if (ch == '%' && i + 1 < f->len) {
                uint16_t t = str_at(f, i + 1);
                if (t == '%') {
                    sb_putc(&b, '%');
                    i++;
                    continue;
                }
                if (ai < argc && (t == 's' || t == 'd' || t == 'i' || t == 'f' || t == 'o' || t == 'O' || t == 'j' || t == 'c')) {
                    jsval a = argv[ai++];
                    i++;
                    JSString *s = 0;
                    switch (t) {
                    case 's':
                        if (is_str(a)) s = vstr(a);
                        else if (is_big(a)) s = jsfmt(rt, "%Sn", big_to_string(rt, (JSBig *)vptr(a), 10));
                        else if (is_sym(a)) s = vstr(key_to_display(rt, a));
                        else if (is_obj(a)) s = inspect_value(rt, a, 0);
                        else if (is_num(a)) s = fmt_number(rt, vnum(a));
                        else s = to_string_prim(rt, a);
                        break;
                    case 'd':
                    case 'i':
                    case 'f': {
                        double d;
                        if (is_obj(a) || is_sym(a))
                            d = JS_NAN;
                        else if (to_number(rt, a, &d) < 0) {
                            rt->has_exc = false;
                            d = JS_NAN;
                        }
                        if (t == 'i') d = js_trunc(d);
                        s = fmt_number(rt, d);
                        break;
                    }
                    case 'j': {
                        jsval r = json_stringify(rt, a, JS_UNDEF, JS_UNDEF);
                        if (r == JS_EXC) {
                            rt->has_exc = false;
                            s = str_cstr(rt, "[Circular]");
                        } else
                            s = is_str(r) ? vstr(r) : str_cstr(rt, "undefined");
                        break;
                    }
                    case 'c': s = vstr(ATOM(empty)); break;
                    default: s = inspect_value(rt, a, 0); break;
                    }
                    sb_putstr(&b, s);
                    continue;
                }
            }
            sb_putc(&b, ch);
        }
    }
    for (; ai < argc; ai++) {
        if (b.len || ai > 0) sb_putc(&b, ' ');
        sb_putstr(&b, inspect_value(rt, argv[ai], 1));
    }
    return sb_finish(&b);
}

NATIVE(console_log) {
    int level = MAGIC;
    JSString *s = format_args(rt, argc, argv);
    if (!s) return JS_EXC;
    emit(rt, level, s);
    return JS_UNDEF;
}
NATIVE(console_assert) {
    if (truthy(ARG(0))) return JS_UNDEF;
    JSString *s = format_args(rt, argc > 1 ? argc - 1 : 0, argv + 1);
    if (!s) return JS_EXC;
    emit(rt, 3, s->len ? jsfmt(rt, "Assertion failed: %S", s) : str_cstr(rt, "Assertion failed"));
    return JS_UNDEF;
}
NATIVE(console_group) {
    if (MAGIC == 1) {
        if (group_indent >= 2) group_indent -= 2;
        return JS_UNDEF;
    }
    if (argc > 0) {
        JSString *s = format_args(rt, argc, argv);
        if (!s) return JS_EXC;
        emit(rt, 0, s);
    }
    group_indent += 2;
    return JS_UNDEF;
}
NATIVE(console_count) {
    JSString *label = is_undef(ARG(0)) ? str_cstr(rt, "default") : to_string(rt, argv[0]);
    if (!label) return JS_EXC;
    jsval k = atomize(rt, label);
    Prop *p = find_own(rt, console_counts, k);
    double n = 0;
    if (MAGIC == 1) {
        if (p) p->val = mknum(0);
        return JS_UNDEF;
    }
    if (p) {
        n = vnum(p->val) + 1;
        p->val = mknum(n);
    } else {
        n = 1;
        add_prop(rt, console_counts, k, mknum(1), P_DEFAULT);
    }
    emit(rt, 0, jsfmt(rt, "%S: %S", label, num_to_str(rt, n)));
    return JS_UNDEF;
}
NATIVE(console_time) {
    JSString *label = is_undef(ARG(0)) ? str_cstr(rt, "default") : to_string(rt, argv[0]);
    if (!label) return JS_EXC;
    jsval k = atomize(rt, jsfmt(rt, "time:%S", label));
    double now = rt->host.mono_ms ? (double)rt->host.mono_ms(rt->host.ctx) : 0;
    Prop *p = find_own(rt, console_counts, k);
    if (MAGIC == 0) {
        if (p)
            p->val = mknum(now);
        else
            add_prop(rt, console_counts, k, mknum(now), P_DEFAULT);
        return JS_UNDEF;
    }
    if (!p) {
        emit(rt, 2, jsfmt(rt, "Warning: No such label '%S' for console.time%s()", label, MAGIC == 1 ? "End" : "Log"));
        return JS_UNDEF;
    }
    double el = now - vnum(p->val);
    char buf[64];
    int n = num_to_fixed(el, 3, buf);
    buf[n] = 0;
    emit(rt, 0, jsfmt(rt, "%S: %sms", label, buf));
    if (MAGIC == 1) del_prop_at(rt, console_counts, p);
    return JS_UNDEF;
}
NATIVE(console_trace) {
    JSString *s = format_args(rt, argc, argv);
    if (!s) return JS_EXC;
    int line;
    JSString *file;
    JSString *st = capture_stack(rt, &line, &file);
    emit(rt, 3, jsfmt(rt, "Trace%s%S%S", s->len ? ": " : "", s, st ? st : vstr(ATOM(empty))));
    return JS_UNDEF;
}

void init_console(js_rt *rt) {
    JSObject *c = new_plain(rt);
    def_value(rt, rt->global, "console", mkobj(c));
    def_method(rt, c, "log", console_log, 0, 0);
    def_method(rt, c, "info", console_log, 0, 1);
    def_method(rt, c, "debug", console_log, 0, 0);
    def_method(rt, c, "warn", console_log, 0, 2);
    def_method(rt, c, "error", console_log, 0, 3);
    def_method(rt, c, "dir", console_log, 0, 0);
    def_method(rt, c, "table", console_log, 0, 0);
    def_method(rt, c, "trace", console_trace, 0, 0);
    def_method(rt, c, "assert", console_assert, 0, 0);
    def_method(rt, c, "group", console_group, 0, 0);
    def_method(rt, c, "groupCollapsed", console_group, 0, 0);
    def_method(rt, c, "groupEnd", console_group, 0, 1);
    def_method(rt, c, "count", console_count, 0, 0);
    def_method(rt, c, "countReset", console_count, 0, 1);
    def_method(rt, c, "time", console_time, 0, 0);
    def_method(rt, c, "timeEnd", console_time, 0, 1);
    def_method(rt, c, "timeLog", console_time, 0, 2);
    console_counts = new_object(rt, 0, CLS_OBJECT);
    add_prop(rt, c, atom_c(rt, "%counts"), mkobj(console_counts), 0);
    group_indent = 0;
}
