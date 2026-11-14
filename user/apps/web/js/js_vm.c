/* AXIOM JavaScript engine: bytecode interpreter, calls, generators/async plumbing, jobs. */
#include "js_int.h"
#include "js_ops.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline void wr32(uint8_t *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }

/* ---------------- line numbers / stack traces ---------------- */
int line_of(FuncProto *fp, uint32_t off) {
    if (!fp->nlines) return 0;
    uint32_t lo = 0, hi = fp->nlines / 2;
    int line = (int)fp->lines[1];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (fp->lines[mid * 2] <= off) {
            line = (int)fp->lines[mid * 2 + 1];
            lo = mid + 1;
        } else
            hi = mid;
    }
    return line;
}

static uint32_t frame_off(Frame *f) {
    uint32_t off = (uint32_t)(f->pc - f->fp->code);
    return off ? off - 1 : 0;
}

void cur_location(js_rt *rt, int *line, JSString **file) {
    *line = 0;
    *file = 0;
    if (rt->nframes > 0) {
        Frame *f = &rt->frames[rt->nframes - 1];
        *line = line_of(f->fp, frame_off(f));
        *file = f->fp->file;
    }
}

jsval func_name_of(js_rt *rt, JSObject *f) {
    if (f->cls == CLS_FUNCTION) {
        if (!(f->oflags & OF_LAZY)) {
            Prop *p = find_own(rt, f, ATOM(name));
            if (p && !(p->flags & P_ACC) && is_str(p->val)) return p->val;
        }
        return is_undef(f->u.fn.fp->name) ? ATOM(empty) : f->u.fn.fp->name;
    }
    if (f->cls == CLS_NATIVE) {
        if (!(f->oflags & OF_LAZY)) {
            Prop *p = find_own(rt, f, ATOM(name));
            if (p && !(p->flags & P_ACC) && is_str(p->val)) return p->val;
        }
        return is_str(f->u.nat.name) ? f->u.nat.name : ATOM(empty);
    }
    Prop *p = find_own(rt, f, ATOM(name));
    if (p && !(p->flags & P_ACC) && is_str(p->val)) return p->val;
    return ATOM(empty);
}

JSString *capture_stack(js_rt *rt, int *line, JSString **file) {
    *line = 0;
    *file = 0;
    if (rt->nframes == 0) return 0;
    SB b;
    sb_init(&b, rt);
    int n = 0;
    for (int i = rt->nframes - 1; i >= 0 && n < 10; i--, n++) {
        Frame *f = &rt->frames[i];
        int ln = line_of(f->fp, frame_off(f));
        if (n == 0) {
            *line = ln;
            *file = f->fp->file;
        }
        sb_puts(&b, "\n    at ");
        jsval nm = func_name_of(rt, f->func);
        bool named = is_str(nm) && vstr(nm)->len;
        if (f->fp->flags & FP_SCRIPT)
            named = false;
        else if (!named)
            sb_puts(&b, "<anonymous>");
        if (named) {
            if (f->flags & F_CONSTRUCT) sb_puts(&b, "new ");
            sb_putstr(&b, vstr(nm));
        }
        if (named || !(f->fp->flags & FP_SCRIPT)) sb_puts(&b, " (");
        if (f->fp->file) sb_putstr(&b, f->fp->file);
        sb_puts(&b, ":");
        char t[16];
        int k = num_to_cstr(ln, t);
        sb_putn(&b, t, (uint32_t)k);
        if (named || !(f->fp->flags & FP_SCRIPT)) sb_puts(&b, ")");
    }
    return sb_finish(&b);
}

/* ---------------- closures ---------------- */
JSObject *make_closure(js_rt *rt, FuncProto *fp, Env *env) {
    int pi = I_FunctionProto;
    if (fp->flags & FP_GENERATOR)
        pi = (fp->flags & FP_ASYNC) ? I_AsyncGeneratorFunctionProto : I_GeneratorFunctionProto;
    else if (fp->flags & FP_ASYNC)
        pi = I_AsyncFunctionProto;
    JSObject *f = new_object(rt, rt->intr[pi], CLS_FUNCTION);
    f->oflags |= OF_CALLABLE | OF_LAZY;
    if (!(fp->flags & (FP_ARROW | FP_METHOD | FP_GENERATOR | FP_ASYNC | FP_GETSET | FP_SCRIPT | FP_FIELD_INIT))) f->oflags |= OF_CTOR;
    if (fp->flags & FP_CLASS_CTOR) f->oflags |= OF_CTOR | OF_CLASS_CTOR;
    f->u.fn.fp = fp;
    f->u.fn.env = env;
    return f;
}

jsval make_return_cell(js_rt *rt, jsval v) {
    Cell *c = (Cell *)gc_alloc(rt, sizeof(Cell), GC_CELL);
    c->h.aux = CELL_RETURN;
    c->a = v;
    return mkptr(c);
}
bool is_return_cell(jsval v) { return is_ptr(v) && ((GCHdr *)vptr(v))->type == GC_CELL && ((GCHdr *)vptr(v))->aux == CELL_RETURN; }

static JSObject *make_arguments(js_rt *rt, jsval *argv, int argc, JSObject *func, bool strict) {
    JSObject *a = new_object(rt, rt->intr[I_ObjectProto], CLS_ARGUMENTS);
    if (argc > 0) {
        array_reserve(rt, a, (uint32_t)argc);
        memcpy(a->u.arr.elems, argv, (size_t)argc * sizeof(jsval));
        a->u.arr.len = (uint32_t)argc;
    }
    add_prop(rt, a, ATOM(length), mknum(argc), P_W | P_C);
    if (!strict) add_prop(rt, a, ATOM(callee), mkobj(func), P_W | P_C);
    add_prop(rt, a, SYM(iterator), mkobj(rt->intr[I_ArrayValues]), P_W | P_C);
    return a;
}

/* ---------------- frames ---------------- */
static JS_NOINLINE int setup_frame(js_rt *rt, JSObject *fo, jsval *argv, int argc, jsval new_target, uint32_t flags) {
    FuncProto *fp = fo->u.fn.fp;
    if (UNLIKELY(rt->nframes >= rt->max_frames)) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    int nparams = fp->nparams;
    jsval *top = argv + (argc > nparams ? argc : nparams);
    if (UNLIKELY(argv + fp->nlocals + fp->max_stack + 64 >= rt->stack_end || top >= rt->stack_end)) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    if (UNLIKELY((fp->flags & FP_CLASS_CTOR) && !(flags & F_CONSTRUCT))) {
        throw_error(rt, E_TYPE, "Class constructor %S cannot be invoked without 'new'", vstr(func_name_of(rt, fo)));
        return -1;
    }
    jsval argsobj = 0, rest = 0;
    if (UNLIKELY(fp->flags & (FP_NEEDS_ARGS | FP_HAS_REST))) {
        if (fp->flags & FP_NEEDS_ARGS) argsobj = mkobj(make_arguments(rt, argv, argc, fo, (fp->flags & FP_STRICT) != 0));
        if (fp->flags & FP_HAS_REST) {
            int n = argc > nparams ? argc - nparams : 0;
            rest = mkobj(new_array_from(rt, argv + nparams, (uint32_t)n));
        }
    }
    for (int i = argc; i < nparams; i++) argv[i] = JS_UNDEF;
    for (int i = nparams; i < fp->nlocals; i++) argv[i] = JS_UNDEF;
    if (argsobj) argv[fp->args_slot] = argsobj;
    if (rest) argv[fp->rest_slot] = rest;
    if (!(fp->flags & (FP_STRICT | FP_ARROW))) {
        jsval t = argv[-1];
        if (is_nullish(t))
            argv[-1] = mkobj(rt->global);
        else if (!is_obj(t) && t != JS_UNINIT)
            argv[-1] = to_object(rt, t);
    }
    Frame *f = &rt->frames[rt->nframes++];
    f->pc = fp->code;
    f->argv = argv;
    f->sp = argv + fp->nlocals;
    f->fp = fp;
    f->func = fo;
    f->env = fo->u.fn.env;
    f->coro = 0;
    f->new_target = new_target;
    f->argc = (uint32_t)argc;
    f->hbase = rt->nhandlers;
    f->flags = flags;
    if ((fp->flags & FP_ASYNC) && !(fp->flags & FP_GENERATOR)) {
        JSObject *co = new_object(rt, rt->intr[I_ObjectProto], CLS_ASYNC);
        CoroData *cd = (CoroData *)calloc(1, sizeof(CoroData));
        co->u.data.p = cd;
        cd->kind = 1;
        cd->state = 2;
        cd->promise = mkobj(promise_new(rt));
        f->coro = co;
    }
    rt->sp = f->sp;
    return 0;
}

static void save_coro(js_rt *rt, CoroData *cd, Frame *f, jsval *sp, uint8_t *pc) {
    jsval *base = f->argv - 2;
    uint32_t n = (uint32_t)(sp - base);
    jsval *s = (jsval *)realloc(cd->slots, (n ? n : 1) * sizeof(jsval));
    if (s) {
        cd->slots = s;
        memcpy(s, base, n * sizeof(jsval));
        cd->nslots = n;
    }
    uint32_t nh = rt->nhandlers - f->hbase;
    if (nh) {
        Handler *h = (Handler *)realloc(cd->handlers, nh * sizeof(Handler));
        if (h) {
            cd->handlers = h;
            memcpy(h, rt->handlers + f->hbase, nh * sizeof(Handler));
        }
    }
    cd->nhandlers = nh;
    rt->nhandlers = f->hbase;
    cd->fr = *f;
    cd->pc_off = (uint32_t)(pc - f->fp->code);
}

static void push_handler(js_rt *rt, Handler *h) {
    if (rt->nhandlers == rt->hcap) {
        uint32_t nc = rt->hcap ? rt->hcap * 2 : 64;
        Handler *n = (Handler *)realloc(rt->handlers, nc * sizeof(Handler));
        if (!n) return;
        rt->handlers = n;
        rt->hcap = nc;
    }
    rt->handlers[rt->nhandlers++] = *h;
}

static Frame *restore_coro(js_rt *rt, CoroData *cd) {
    jsval *base = rt->sp;
    if (base + cd->nslots + cd->fr.fp->max_stack + 64 >= rt->stack_end || rt->nframes >= rt->max_frames) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return 0;
    }
    memcpy(base, cd->slots, cd->nslots * sizeof(jsval));
    Frame *f = &rt->frames[rt->nframes++];
    *f = cd->fr;
    f->argv = base + 2;
    f->sp = base + cd->nslots;
    f->pc = f->fp->code + cd->pc_off;
    f->hbase = rt->nhandlers;
    f->flags |= F_STOP;
    for (uint32_t i = 0; i < cd->nhandlers; i++) {
        Handler h = cd->handlers[i];
        h.frame = (uint32_t)(rt->nframes - 1);
        push_handler(rt, &h);
    }
    rt->sp = f->sp;
    return f;
}

JSObject *make_generator(js_rt *rt, Frame *f, jsval *sp, uint8_t *pc, int kind) {
    bool async = kind == 2;
    JSObject *proto = 0;
    Prop *p = find_own(rt, f->func, ATOM(prototype));
    if (p && !(p->flags & P_ACC) && is_obj(p->val)) proto = vobj(p->val);
    if (!proto) proto = rt->intr[async ? I_AsyncGeneratorProto : I_GeneratorProto];
    JSObject *g = new_object(rt, proto, async ? CLS_ASYNC_GENERATOR : CLS_GENERATOR);
    CoroData *cd = (CoroData *)calloc(1, sizeof(CoroData));
    g->u.data.p = cd;
    cd->kind = async ? 2 : 0;
    cd->state = 0;
    f->coro = g;
    save_coro(rt, cd, f, sp, pc);
    return g;
}

/* ---------------- calls from C ---------------- */
static jsval call_native(js_rt *rt, JSObject *o, jsval this_, int argc, jsval *argv, jsval nt) {
    int len = o->u.nat.length;
    jsval *base = rt->sp;
    jsval *args = argv;
    int n = argc > len ? argc : len;
    if (args + n > rt->stack_end) return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    for (int i = argc; i < n; i++) args[i] = JS_UNDEF;
    rt->sp = args + n;
    jsval saved_nt = rt->new_target;
    rt->new_target = nt;
    if (o->u.nat.api) rt->native_depth++;
    jsval r = o->u.nat.fn(rt, this_, argc, args, o->u.nat.data);
    if (o->u.nat.api) rt->native_depth--;
    rt->new_target = saved_nt;
    rt->sp = base;
    if (rt->has_exc) return JS_EXC;
    if (r == JS_EXC) return throw_error(rt, E_INTERNAL, "native function failed");
    return r;
}

static jsval run(js_rt *rt, bool throwing);

jsval call_fn(js_rt *rt, jsval fv, jsval this_, int argc, const jsval *argv) {
    if (!is_callable(fv)) return throw_error(rt, E_TYPE, "%S is not a function", val_brief(rt, fv));
    if (++rt->rec_depth > JS_MAX_REC || check_stack(rt)) {
        rt->rec_depth--;
        return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    JSObject *o = vobj(fv);
    jsval *base = rt->sp;
    jsval r;
    if (base + argc + 8 >= rt->stack_end) {
        rt->rec_depth--;
        return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    base[0] = fv;
    base[1] = this_;
    for (int i = 0; i < argc; i++) base[2 + i] = argv[i];
    rt->sp = base + 2 + argc;
    switch (o->cls) {
    case CLS_NATIVE: r = call_native(rt, o, this_, argc, base + 2, JS_UNDEF); break;
    case CLS_FUNCTION:
        if (setup_frame(rt, o, base + 2, argc, JS_UNDEF, F_STOP) < 0)
            r = JS_EXC;
        else
            r = run(rt, false);
        break;
    case CLS_BOUND: {
        uint32_t nb = o->u.bound.nargs;
        jsval *a = base + 2;
        if (nb) {
            if (a + argc + nb + 8 >= rt->stack_end) {
                r = throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
                break;
            }
            memmove(a + nb, a, (size_t)argc * sizeof(jsval));
            memcpy(a, o->u.bound.args, nb * sizeof(jsval));
            rt->sp = a + argc + nb;
        }
        r = call_fn(rt, o->u.bound.target, o->u.bound.bthis, argc + (int)nb, a);
        break;
    }
    case CLS_PROXY: r = proxy_call(rt, o, this_, argc, base + 2); break;
    default: r = throw_error(rt, E_TYPE, "%S is not a function", val_brief(rt, fv));
    }
    rt->sp = base;
    rt->rec_depth--;
    return r;
}

static JSObject *ordinary_create_this(js_rt *rt, jsval nt) {
    JSObject *proto = proto_from_ctor(rt, nt, I_ObjectProto);
    if (!proto) return 0;
    return new_object(rt, proto, CLS_OBJECT);
}

jsval construct_fn(js_rt *rt, jsval fv, int argc, const jsval *argv, jsval nt) {
    if (!is_ctor(fv)) return throw_error(rt, E_TYPE, "%S is not a constructor", val_brief(rt, fv));
    if (++rt->rec_depth > JS_MAX_REC || check_stack(rt)) {
        rt->rec_depth--;
        return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    JSObject *o = vobj(fv);
    jsval *base = rt->sp;
    jsval r;
    if (base + argc + 8 >= rt->stack_end) {
        rt->rec_depth--;
        return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    base[0] = fv;
    base[1] = JS_UNDEF;
    base[2] = nt;
    for (int i = 0; i < argc; i++) base[3 + i] = argv[i];
    rt->sp = base + 3 + argc;
    jsval *a = base + 3;
    switch (o->cls) {
    case CLS_NATIVE:
        if (o->u.nat.api) {
            JSObject *t = ordinary_create_this(rt, nt);
            if (!t) {
                r = JS_EXC;
                break;
            }
            base[1] = mkobj(t);
            r = call_native(rt, o, base[1], argc, a, nt);
            if (r != JS_EXC && !is_obj(r)) r = base[1];
        } else
            r = call_native(rt, o, JS_UNDEF, argc, a, nt);
        break;
    case CLS_FUNCTION: {
        FuncProto *fp = o->u.fn.fp;
        jsval thisv = JS_UNINIT;
        if (!(fp->flags & FP_DERIVED)) {
            JSObject *t = ordinary_create_this(rt, nt);
            if (!t) {
                r = JS_EXC;
                break;
            }
            thisv = mkobj(t);
        }
        /* layout for the frame: [func this args] */
        base[1] = thisv;
        memmove(base + 2, base + 3, (size_t)argc * sizeof(jsval));
        rt->sp = base + 2 + argc;
        if (setup_frame(rt, o, base + 2, argc, nt, F_STOP | F_CONSTRUCT) < 0)
            r = JS_EXC;
        else
            r = run(rt, false);
        break;
    }
    case CLS_BOUND: {
        uint32_t nb = o->u.bound.nargs;
        if (a + argc + nb + 8 >= rt->stack_end) {
            r = throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
            break;
        }
        if (nb) {
            memmove(a + nb, a, (size_t)argc * sizeof(jsval));
            memcpy(a, o->u.bound.args, nb * sizeof(jsval));
            rt->sp = a + argc + nb;
        }
        jsval nnt = nt == fv ? o->u.bound.target : nt;
        r = construct_fn(rt, o->u.bound.target, argc + (int)nb, a, nnt);
        break;
    }
    case CLS_PROXY: r = proxy_construct(rt, o, argc, a, nt); break;
    default: r = throw_error(rt, E_TYPE, "%S is not a constructor", val_brief(rt, fv));
    }
    rt->sp = base;
    rt->rec_depth--;
    return r;
}

jsval call_method(js_rt *rt, jsval obj, jsval key, int argc, const jsval *argv) {
    jsval *root = push_tmp(rt, obj);
    jsval f = get_p(rt, obj, key);
    if (f == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    jsval r = call_fn(rt, f, obj, argc, argv);
    rt->sp = root;
    return r;
}

JSObject *bound_create(js_rt *rt, jsval target, jsval bthis, int nargs, const jsval *args) {
    JSObject *t = vobj(target);
    JSObject *b = new_object(rt, t->proto, CLS_BOUND);
    b->oflags |= OF_CALLABLE | (t->oflags & OF_CTOR);
    b->u.bound.target = target;
    b->u.bound.bthis = bthis;
    b->u.bound.nargs = (uint32_t)nargs;
    if (nargs) {
        b->u.bound.args = (jsval *)malloc((size_t)nargs * sizeof(jsval));
        memcpy(b->u.bound.args, args, (size_t)nargs * sizeof(jsval));
    }
    return b;
}

/* ---------------- interpreter helpers (slow paths) ---------------- */
static JS_NOINLINE bool poll(js_rt *rt) {
    if (rt->gc_pending) gc_collect(rt);
    if (rt->interrupt_count <= 0) {
        rt->interrupt_count = 10000;
        if (rt->host.interrupt && rt->host.interrupt(rt->host.ctx)) {
            rt->terminating = true;
            JSObject *e = make_error(rt, E_INTERNAL, str_cstr(rt, "Script execution was interrupted"));
            throw_val(rt, mkobj(e));
            return true;
        }
    }
    return false;
}

static JS_NOINLINE jsval get_field_slow(js_rt *rt, jsval o, jsval key, uint8_t *icp) {
    if (is_obj(o)) {
        JSObject *ob = vobj(o);
        if (ob->cls == CLS_ARRAY && key == ATOM(length)) return mknum(ob->u.arr.len);
        if (!(ob->oflags & OF_EXOTIC)) {
            Prop *p = find_own(rt, ob, key);
            if (p) {
                if (!(p->flags & P_ACC)) {
                    wr32(icp, (uint32_t)(p - ob->props));
                    return p->val;
                }
                Accessor *a = (Accessor *)vptr(p->val);
                if (is_undef(a->get)) return JS_UNDEF;
                return call_fn(rt, a->get, o, 0, 0);
            }
            if (ob->proto) return get_prop(rt, mkobj(ob->proto), key, o);
            return JS_UNDEF;
        }
    }
    return get_prop(rt, o, key, o);
}

static JS_NOINLINE int put_field_slow(js_rt *rt, jsval o, jsval key, jsval v, uint8_t *icp, bool strict) {
    int r = set_prop(rt, o, key, v, o, strict);
    if (r > 0 && is_obj(o)) {
        JSObject *ob = vobj(o);
        if (!(ob->oflags & OF_EXOTIC)) {
            Prop *p = find_own(rt, ob, key);
            if (p && (p->flags & (P_W | P_ACC)) == P_W) wr32(icp, (uint32_t)(p - ob->props));
        }
    }
    return r;
}

static JS_NOINLINE jsval global_get_slow(js_rt *rt, jsval key, uint8_t *icp, bool typeof_) {
    Prop *p = find_own(rt, rt->global_lex, key);
    if (p) {
        if (p->val == JS_UNINIT) return throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(key));
        if (icp) {
            wr32(icp, 0x80000000u | (uint32_t)(p - rt->global_lex->props));
            wr32(icp + 4, rt->lex_epoch);
        }
        return p->val;
    }
    JSObject *g = rt->global;
    p = find_own(rt, g, key);
    if (p) {
        if (!(p->flags & P_ACC)) {
            if (icp) {
                wr32(icp, (uint32_t)(p - g->props));
                wr32(icp + 4, rt->lex_epoch);
            }
            return p->val;
        }
        return get_prop(rt, mkobj(g), key, mkobj(g));
    }
    int h = has_prop(rt, mkobj(g), key);
    if (h < 0) return JS_EXC;
    if (!h) {
        if (typeof_) return JS_UNDEF;
        return throw_error(rt, E_REFERENCE, "%S is not defined", vstr(key));
    }
    return get_prop(rt, mkobj(g), key, mkobj(g));
}

static JS_NOINLINE int global_put_slow(js_rt *rt, jsval key, jsval v, uint8_t *icp, bool strict) {
    Prop *p = find_own(rt, rt->global_lex, key);
    if (p) {
        if (p->val == JS_UNINIT) {
            throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(key));
            return -1;
        }
        if (!(p->flags & P_W)) {
            throw_error(rt, E_TYPE, "Assignment to constant variable.");
            return -1;
        }
        p->val = v;
        wr32(icp, 0x80000000u | (uint32_t)(p - rt->global_lex->props));
        wr32(icp + 4, rt->lex_epoch);
        return 0;
    }
    JSObject *g = rt->global;
    p = find_own(rt, g, key);
    if (p && (p->flags & (P_W | P_ACC)) == P_W) {
        p->val = v;
        wr32(icp, (uint32_t)(p - g->props));
        wr32(icp + 4, rt->lex_epoch);
        return 0;
    }
    if (!p && strict) {
        int h = has_prop(rt, mkobj(g), key);
        if (h < 0) return -1;
        if (!h) {
            throw_error(rt, E_REFERENCE, "%S is not defined", vstr(key));
            return -1;
        }
    }
    return set_prop(rt, mkobj(g), key, v, mkobj(g), strict) < 0 ? -1 : 0;
}

/* ADD with ToPrimitive; operands at sp[-2], sp[-1] (rooted) */
static JS_NOINLINE jsval add_slow(js_rt *rt, jsval *sp) {
    jsval a = to_primitive(rt, sp[-2], 0);
    if (a == JS_EXC) return JS_EXC;
    sp[-2] = a;
    jsval b = to_primitive(rt, sp[-1], 0);
    if (b == JS_EXC) return JS_EXC;
    sp[-1] = b;
    if (is_str(a) || is_str(b)) {
        JSString *sa = to_string_prim(rt, a);
        if (!sa) return JS_EXC;
        sp[-2] = mkstr(sa);
        JSString *sb = to_string_prim(rt, b);
        if (!sb) return JS_EXC;
        JSString *r = str_concat(rt, sa, sb);
        if (!r) return throw_error(rt, E_RANGE, "Invalid string length");
        return mkstr(r);
    }
    if (is_big(a) || is_big(b)) return big_binop(rt, OP_ADD, a, b);
    double x, y;
    if (to_number(rt, a, &x) < 0 || to_number(rt, b, &y) < 0) return JS_EXC;
    return mknum(x + y);
}

static double num_op(int o, double x, double y) {
    switch (o) {
    case OP_SUB: return x - y;
    case OP_MUL: return x * y;
    case OP_DIV: return x / y;
    case OP_MOD: return js_fmod(x, y);
    case OP_POW: return js_pow(x, y);
    case OP_BAND: return (double)(to_int32(x) & to_int32(y));
    case OP_BOR: return (double)(to_int32(x) | to_int32(y));
    case OP_BXOR: return (double)(to_int32(x) ^ to_int32(y));
    case OP_SHL: return (double)(int32_t)((uint32_t)to_int32(x) << (to_uint32(y) & 31));
    case OP_SAR: return (double)(to_int32(x) >> (to_uint32(y) & 31));
    case OP_SHR: return (double)(to_uint32(x) >> (to_uint32(y) & 31));
    default: return 0;
    }
}

static JS_NOINLINE jsval arith_slow(js_rt *rt, int o, jsval *sp) {
    jsval a, b;
    if (to_numeric(rt, sp[-2], &a) < 0) return JS_EXC;
    sp[-2] = a;
    if (to_numeric(rt, sp[-1], &b) < 0) return JS_EXC;
    if (is_big(a) || is_big(b)) return big_binop(rt, o, a, b);
    return mknum(num_op(o, vnum(a), vnum(b)));
}

/* returns 1 true, 0 false, 2 undefined (NaN), -1 throw; computes a < b (or b < a if swap) */
static JS_NOINLINE int compare_slow(js_rt *rt, jsval *sp, bool left_first, bool swap) {
    jsval a, b;
    if (left_first) {
        a = to_primitive(rt, sp[-2], 1);
        if (a == JS_EXC) return -1;
        sp[-2] = a;
        b = to_primitive(rt, sp[-1], 1);
        if (b == JS_EXC) return -1;
        sp[-1] = b;
    } else {
        b = to_primitive(rt, sp[-1], 1);
        if (b == JS_EXC) return -1;
        sp[-1] = b;
        a = to_primitive(rt, sp[-2], 1);
        if (a == JS_EXC) return -1;
        sp[-2] = a;
    }
    if (swap) {
        jsval t = a;
        a = b;
        b = t;
    }
    if (is_str(a) && is_str(b)) return str_cmp(rt, vstr(a), vstr(b)) < 0;
    if (is_big(a) && is_str(b)) {
        jsval bb = big_from_string(rt, vstr(b), false);
        if (!is_big(bb)) return 2;
        return big_cmp((JSBig *)vptr(a), (JSBig *)vptr(bb)) < 0;
    }
    if (is_str(a) && is_big(b)) {
        jsval aa = big_from_string(rt, vstr(a), false);
        if (!is_big(aa)) return 2;
        return big_cmp((JSBig *)vptr(aa), (JSBig *)vptr(b)) < 0;
    }
    jsval na, nb;
    if (to_numeric(rt, a, &na) < 0 || to_numeric(rt, b, &nb) < 0) return -1;
    if (is_big(na) && is_big(nb)) return big_cmp((JSBig *)vptr(na), (JSBig *)vptr(nb)) < 0;
    if (is_big(na)) {
        int c = big_cmp_num((JSBig *)vptr(na), vnum(nb));
        return c == 2 ? 2 : c < 0;
    }
    if (is_big(nb)) {
        int c = big_cmp_num((JSBig *)vptr(nb), vnum(na));
        return c == 2 ? 2 : c > 0;
    }
    double x = vnum(na), y = vnum(nb);
    if (x != x || y != y) return 2;
    return x < y;
}

static JS_NOINLINE jsval instance_of(js_rt *rt, jsval o, jsval c) {
    if (!is_obj(c)) return throw_error(rt, E_TYPE, "Right-hand side of 'instanceof' is not an object");
    jsval *root = push_tmp(rt, o);
    push_tmp(rt, c);
    jsval h = get_p(rt, c, SYM(hasInstance));
    if (h == JS_EXC) {
        rt->sp = root;
        return JS_EXC;
    }
    if (!is_nullish(h) && !(is_obj(h) && vobj(h)->cls == CLS_NATIVE && vobj(h)->u.nat.data == (void *)(intptr_t)0x7151)) {
        jsval r = call_fn(rt, h, c, 1, &o);
        rt->sp = root;
        if (r == JS_EXC) return JS_EXC;
        return mkbool(truthy(r));
    }
    if (!is_callable(c)) {
        rt->sp = root;
        return throw_error(rt, E_TYPE, "Right-hand side of 'instanceof' is not callable");
    }
    for (int guard = 0; vobj(c)->cls == CLS_BOUND && guard < 1000; guard++) c = vobj(c)->u.bound.target;
    if (!is_obj(o)) {
        rt->sp = root;
        return JS_FALSE;
    }
    jsval p = get_p(rt, c, ATOM(prototype));
    rt->sp = root;
    if (p == JS_EXC) return JS_EXC;
    if (!is_obj(p)) return throw_error(rt, E_TYPE, "Function has non-object prototype '%S' in instanceof check", val_brief(rt, p));
    JSObject *po = vobj(p);
    JSObject *x = vobj(o);
    for (int guard = 0; guard < 100000; guard++) {
        jsval pr = get_proto(rt, x);
        if (pr == JS_EXC) return JS_EXC;
        if (!is_obj(pr)) return JS_FALSE;
        if (vobj(pr) == po) return JS_TRUE;
        x = vobj(pr);
    }
    return JS_FALSE;
}

static JS_NOINLINE jsval op_in(js_rt *rt, jsval *sp) {
    jsval obj = sp[-1];
    if (!is_obj(obj)) {
        JSString *ks = is_sym(sp[-2]) ? vstr(key_to_display(rt, sp[-2])) : to_string(rt, sp[-2]);
        if (!ks) return JS_EXC;
        return throw_error(rt, E_TYPE, "Cannot use 'in' operator to search for '%S' in %S", ks, val_brief(rt, obj));
    }
    jsval k = to_key(rt, sp[-2]);
    if (k == JS_EXC) return JS_EXC;
    int r = has_prop(rt, obj, k);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}

static JS_NOINLINE int define_method(js_rt *rt, jsval obj, jsval key, jsval fn, int kind, int flags) {
    JSObject *o = vobj(obj);
    JSObject *f = vobj(fn);
    if (f->cls == CLS_FUNCTION) f->u.fn.home = o;
    jsval k = key;
    if (!is_sym(k)) {
        k = to_key(rt, key);
        if (k == JS_EXC) return -1;
    }
    if (kind != 0 || (f->cls == CLS_FUNCTION && is_undef(f->u.fn.fp->name))) {
        jsval kd = key_to_display(rt, k);
        if (is_sym(k) && !(vsym(k)->h.flags & 1)) {
            JSSymbol *s = vsym(k);
            kd = is_str(s->desc) ? mkstr(jsfmt(rt, "[%S]", vstr(s->desc))) : ATOM(empty);
        }
        JSString *nm = kind == 1 ? jsfmt(rt, "get %S", vstr(kd)) : kind == 2 ? jsfmt(rt, "set %S", vstr(kd)) : vstr(kd);
        materialize_fn(rt, f);
        def_prop(rt, f, ATOM(name), mkstr(nm), P_C);
    }
    uint32_t en = (flags & 1) ? P_E : 0;
    if (kind == 0) {
        PropDesc d;
        d.value = fn;
        d.flags = (uint8_t)(P_W | P_C | en);
        d.has = HAS_VALUE | HAS_W | HAS_E | HAS_C;
        if (flags & 2) {
            def_prop(rt, o, k, fn, 0);
            return 0;
        }
        return define_own(rt, o, k, &d, true) < 0 ? -1 : 0;
    }
    PropDesc d;
    memset(&d, 0, sizeof d);
    if (kind == 1) {
        d.get = fn;
        d.has = HAS_GET;
    } else {
        d.set = fn;
        d.has = HAS_SET;
    }
    d.flags = (uint8_t)(P_C | en);
    d.has |= HAS_E | HAS_C;
    return define_own(rt, o, k, &d, true) < 0 ? -1 : 0;
}

static JS_NOINLINE int copy_data_props(js_rt *rt, jsval target, jsval src, jsval excl) {
    if (is_nullish(src)) return 0;
    jsval so = to_object(rt, src);
    if (so == JS_EXC) return -1;
    jsval *root = push_tmp(rt, so);
    JSObject *keys = own_keys(rt, vobj(so), KEYS_STR | KEYS_SYM);
    if (!keys) {
        rt->sp = root;
        return -1;
    }
    push_tmp(rt, mkobj(keys));
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jsval k = keys->u.arr.elems[i];
        if (is_obj(excl)) {
            JSObject *ex = vobj(excl);
            bool skip = false;
            for (uint32_t j = 0; j < ex->u.arr.len; j++)
                if (ex->u.arr.elems[j] == k) skip = true;
            if (skip) continue;
        }
        PropDesc d;
        int e = get_own_desc(rt, vobj(so), k, &d);
        if (e < 0) {
            rt->sp = root;
            return -1;
        }
        if (!e || !(d.flags & P_E)) continue;
        jsval v = get_p(rt, so, k);
        if (v == JS_EXC) {
            rt->sp = root;
            return -1;
        }
        if (create_data_prop(rt, vobj(target), k, v, true) < 0) {
            rt->sp = root;
            return -1;
        }
    }
    rt->sp = root;
    return 0;
}

/* throw a source-describing "is not iterable" error before iterating obj (msg: string constant, or
 * undefined for the generic message; call spread uses its own message) */
static int iter_precheck(js_rt *rt, jsval obj, jsval msg, bool call_spread) {
    if (!call_spread && !is_str(msg)) return 0;
    if (is_nullish(obj)) {
        if (call_spread) {
            throw_error(rt, E_TYPE, "%S is not iterable (cannot read property %S)", val_brief(rt, obj), val_brief(rt, obj));
            return -1;
        }
    } else {
        if (is_obj(obj) && vobj(obj)->cls == CLS_ARRAY) return 0;
        if (is_str(obj)) return 0;
        jsval *root = push_tmp(rt, obj);
        jsval m = get_v(rt, obj, SYM(iterator));
        rt->sp = root;
        if (m == JS_EXC) return -1;
        if (is_callable(m)) return 0;
        if (call_spread) {
            throw_error(rt, E_TYPE, "Spread syntax requires ...iterable[Symbol.iterator] to be a function");
            return -1;
        }
    }
    throw_error(rt, E_TYPE, "%S", vstr(msg));
    return -1;
}

static JS_NOINLINE int array_spread(js_rt *rt, jsval arr, jsval src) {
    JSObject *a = vobj(arr);
    if (is_obj(src) && vobj(src)->cls == CLS_ARRAY && !(vobj(src)->oflags & OF_SLOW_ARRAY)) {
        /* fast path when iteration is not observable */
        JSObject *s = vobj(src);
        Prop *ip = find_own(rt, s, SYM(iterator));
        Prop *pp = find_own(rt, rt->intr[I_ArrayProto], SYM(iterator));
        Prop *np = find_own(rt, rt->intr[I_ArrayIterProto], ATOM(next));
        if (!ip && s->proto == rt->intr[I_ArrayProto] && pp && pp->val == mkobj(rt->intr[I_ArrayValues]) && np &&
            np->val == mkobj(rt->intr[I_ArrayIterNext])) {
            uint32_t n = s->u.arr.len;
            if (!array_reserve(rt, a, a->u.arr.len + n)) return -1;
            for (uint32_t i = 0; i < n && i < s->u.arr.len; i++) {
                jsval v = s->u.arr.elems[i];
                a->u.arr.elems[a->u.arr.len++] = v == JS_HOLE ? JS_UNDEF : v;
            }
            return 0;
        }
    }
    jsval *root = push_tmp(rt, arr);
    push_tmp(rt, src);
    jsval next;
    jsval it = get_iterator(rt, src, &next, false);
    if (it == JS_EXC) {
        rt->sp = root;
        return -1;
    }
    push_tmp(rt, it);
    push_tmp(rt, next);
    for (;;) {
        jsval v;
        int r = iter_step(rt, it, next, &v);
        if (r < 0) {
            rt->sp = root;
            return -1;
        }
        if (r == 0) break;
        if (array_push(rt, a, v) < 0) {
            rt->sp = root;
            return -1;
        }
    }
    rt->sp = root;
    return 0;
}

/* for-in iteration state: data.p = key array */
typedef struct ForIn {
    JSObject *keys;
    uint32_t i;
} ForIn;

static JS_NOINLINE jsval for_in_init(js_rt *rt, jsval obj) {
    JSObject *it = new_object(rt, 0, CLS_FORIN_ITER);
    it->u.it.a = JS_UNDEF;
    it->u.it.b = JS_UNDEF;
    if (is_nullish(obj)) return mkobj(it);
    jsval o = to_object(rt, obj);
    if (o == JS_EXC) return JS_EXC;
    jsval *root = push_tmp(rt, mkobj(it));
    push_tmp(rt, o);
    JSObject *all = new_array(rt, 0);
    push_tmp(rt, mkobj(all));
    JSObject *cur = vobj(o);
    int depth = 0;
    while (cur && depth++ < 10000) {
        JSObject *keys = own_keys(rt, cur, KEYS_STR);
        if (!keys) {
            rt->sp = root;
            return JS_EXC;
        }
        jsval *r2 = push_tmp(rt, mkobj(keys));
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jsval k = keys->u.arr.elems[i];
            if (depth > 1) { /* skip keys shadowed by objects earlier in the chain */
                bool dup = false;
                for (uint32_t j = 0; j < all->u.arr.len; j++)
                    if (vptr(all->u.arr.elems[j]) == vptr(k)) {
                        dup = true;
                        break;
                    }
                if (dup) continue;
            }
            PropDesc d;
            int e = get_own_desc(rt, cur, k, &d);
            if (e < 0) {
                rt->sp = root;
                return JS_EXC;
            }
            if (!e) continue;
            /* non-enumerable keys are kept (as markers) only to shadow later ones */
            array_push(rt, all, (d.flags & P_E) ? k : mkptr(vptr(k)));
        }
        rt->sp = r2;
        jsval p = get_proto(rt, cur);
        if (p == JS_EXC) {
            rt->sp = root;
            return JS_EXC;
        }
        cur = is_obj(p) ? vobj(p) : 0;
    }
    it->u.it.a = mkobj(all);
    it->u.it.b = o;
    it->u.it.i = 0;
    rt->sp = root;
    return mkobj(it);
}

/* returns key, or JS_HOLE when done */
static JS_NOINLINE jsval for_in_next(js_rt *rt, jsval itv) {
    JSObject *it = vobj(itv);
    if (!is_obj(it->u.it.a)) return JS_HOLE;
    JSObject *all = vobj(it->u.it.a);
    while (it->u.it.i < all->u.arr.len) {
        jsval k = all->u.arr.elems[it->u.it.i++];
        if (is_ptr(k)) continue; /* non-enumerable shadow marker */
        /* deleted during iteration? */
        int h = has_prop(rt, it->u.it.b, k);
        if (h < 0) return JS_EXC;
        if (!h) continue;
        return k;
    }
    return JS_HOLE;
}

/* ---------------- the interpreter ---------------- */
#define ARGS_OR_EXC(x) do { if ((x) == JS_EXC) goto exception; } while (0)

static JS_NOINLINE jsval call_slow(js_rt *rt, jsval fn, jsval this_, int argc, jsval *argv) {
    return call_fn(rt, fn, this_, argc, argv);
}

static JS_NOINLINE jsval not_callable(js_rt *rt, FuncProto *fp, uint32_t desc, jsval v, bool ctor) {
    JSString *d = desc != 0xFFFFFFFFu ? vstr(fp->consts[desc]) : val_brief(rt, v);
    if (ctor) return throw_error(rt, E_TYPE, "%S is not a constructor", d);
    return throw_error(rt, E_TYPE, "%S is not a function", d);
}

/* spread an args array on the stack: [.. arr] -> [.. a0 .. an-1]; returns count or -1 */
static JS_NOINLINE int spread_args(js_rt *rt, jsval *sp) {
    JSObject *a = vobj(sp[-1]);
    uint32_t n = a->u.arr.len;
    if (sp - 1 + n + 64 >= rt->stack_end) {
        throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        jsval v = a->u.arr.elems[i];
        sp[-1 + (int)i] = v == JS_HOLE ? JS_UNDEF : v;
    }
    return (int)n;
}

static JS_NOINLINE jsval ctor_this(js_rt *rt, JSObject *fo, jsval nt) {
    if (fo->u.fn.fp->flags & FP_DERIVED) return JS_UNINIT;
    JSObject *t = ordinary_create_this(rt, nt);
    return t ? mkobj(t) : JS_EXC;
}

static JS_NOINLINE jsval do_class(js_rt *rt, FuncProto *cfp, bool heritage, jsval parent, Env *env) {
    JSObject *proto_parent = rt->intr[I_ObjectProto];
    JSObject *ctor_parent = rt->intr[I_FunctionProto];
    if (heritage) {
        if (is_null(parent)) {
            proto_parent = 0;
        } else {
            if (!is_ctor(parent)) return throw_error(rt, E_TYPE, "Class extends value %S is not a constructor or null", val_brief(rt, parent));
            jsval pp = get_p(rt, parent, ATOM(prototype));
            if (pp == JS_EXC) return JS_EXC;
            if (!is_obj(pp) && !is_null(pp)) return throw_error(rt, E_TYPE, "Class extends value does not have valid prototype property %S", val_brief(rt, pp));
            proto_parent = is_obj(pp) ? vobj(pp) : 0;
            ctor_parent = vobj(parent);
        }
    }
    JSObject *proto = new_object(rt, proto_parent, CLS_OBJECT);
    JSObject *ctor = make_closure(rt, cfp, env);
    ctor->proto = ctor_parent;
    ctor->oflags |= OF_CTOR | OF_CLASS_CTOR;
    ctor->u.fn.home = proto;
    materialize_fn(rt, ctor);
    def_prop(rt, ctor, ATOM(prototype), mkobj(proto), 0);
    def_prop(rt, proto, ATOM(constructor), mkobj(ctor), P_W | P_C);
    rt->cur_this_fn = proto; /* returned via second slot */
    return mkobj(ctor);
}

static jsval priv_missing(js_rt *rt, jsval key, const char *verb, const char *prep) {
    JSSymbol *s = vsym(key);
    if (s->h.flags & 8) return throw_error(rt, E_TYPE, "Receiver must be class %S", vstr(s->cls));
    if (s->h.flags & 4) return throw_error(rt, E_TYPE, "Receiver must be an instance of class %S", vstr(s->cls));
    return throw_error(rt, E_TYPE, "Cannot %s private member %S %s an object whose class did not declare it", verb, vstr(s->desc), prep);
}

static JS_NOINLINE jsval priv_get(js_rt *rt, jsval obj, jsval key) {
    if (is_obj(obj)) {
        Prop *p = find_own(rt, vobj(obj), key);
        if (p) {
            if (p->flags & P_ACC) {
                Accessor *a = (Accessor *)vptr(p->val);
                if (is_undef(a->get)) return throw_error(rt, E_TYPE, "'%S' was defined without a getter", vstr(vsym(key)->desc));
                return call_fn(rt, a->get, obj, 0, 0);
            }
            return p->val;
        }
    }
    return priv_missing(rt, key, "read", "from");
}
static JS_NOINLINE int priv_put(js_rt *rt, jsval obj, jsval key, jsval v) {
    if (is_obj(obj)) {
        Prop *p = find_own(rt, vobj(obj), key);
        if (p) {
            if (p->flags & P_ACC) {
                Accessor *a = (Accessor *)vptr(p->val);
                if (is_undef(a->set)) {
                    throw_error(rt, E_TYPE, "'%S' was defined without a setter", vstr(vsym(key)->desc));
                    return -1;
                }
                return call_fn(rt, a->set, obj, 1, &v) == JS_EXC ? -1 : 0;
            }
            if (!(p->flags & P_W)) {
                throw_error(rt, E_TYPE, "Private method '%S' is not writable", vstr(vsym(key)->desc));
                return -1;
            }
            p->val = v;
            return 0;
        }
    }
    priv_missing(rt, key, "write", "to");
    return -1;
}
static JS_NOINLINE int priv_def(js_rt *rt, jsval obj, jsval key, jsval v, int kind) {
    if (!is_obj(obj)) {
        throw_error(rt, E_TYPE, "Cannot define private member on non-object");
        return -1;
    }
    JSObject *o = vobj(obj);
    Prop *p = find_own(rt, o, key);
    if (kind == 2 || kind == 3) {
        if (p && (p->flags & P_ACC)) {
            Accessor *a = (Accessor *)vptr(p->val);
            if (kind == 2) a->get = v;
            else a->set = v;
            return 0;
        }
        def_acc(rt, o, key, kind == 2 ? v : JS_UNDEF, kind == 3 ? v : JS_UNDEF, 0);
        return 0;
    }
    if (p) {
        throw_error(rt, E_TYPE, "Cannot initialize %S twice on the same object", vstr(vsym(key)->desc));
        return -1;
    }
    add_prop(rt, o, key, v, kind == 0 ? P_W : 0);
    return 0;
}

static JS_NOINLINE jsval new_private(js_rt *rt, jsval name, jsval cls, int is_static) {
    JSSymbol *s = (JSSymbol *)gc_alloc(rt, sizeof(JSSymbol), GC_SYM);
    s->h.flags = 1 | (is_str(cls) ? 4 : 0) | (is_static ? 8 : 0);
    s->desc = name;
    s->cls = is_str(cls) ? cls : JS_UNDEF;
    s->hash = (uint32_t)(++rt->sym_counter * 2654435761u) | 1;
    return mksym(s);
}

static JS_NOINLINE jsval iter_value_op(js_rt *rt, jsval *slots) {
    if (is_undef(slots[1])) return JS_UNDEF;
    jsval v;
    int r = iter_step(rt, slots[0], slots[1], &v);
    if (r <= 0) {
        slots[1] = JS_UNDEF;
        return r < 0 ? JS_EXC : JS_UNDEF;
    }
    return v;
}
static JS_NOINLINE jsval iter_rest_op(js_rt *rt, jsval *slots) {
    JSObject *a = new_array(rt, 0);
    jsval *root = push_tmp(rt, mkobj(a));
    while (!is_undef(slots[1])) {
        jsval v;
        int r = iter_step(rt, slots[0], slots[1], &v);
        if (r <= 0) {
            slots[1] = JS_UNDEF;
            if (r < 0) {
                rt->sp = root;
                return JS_EXC;
            }
            break;
        }
        array_push(rt, a, v);
    }
    rt->sp = root;
    return mkobj(a);
}

static JS_NOINLINE int iter_next_op(js_rt *rt, jsval *slots, jsval *out) {
    jsval it = slots[0];
    if (is_obj(it) && vobj(it)->cls == CLS_ARRAY_ITER && slots[1] == mkobj(rt->intr[I_ArrayIterNext])) {
        bool done;
        jsval v = array_iter_next_fast(rt, vobj(it), &done);
        if (v == JS_EXC) return -1;
        if (done) return 0;
        *out = v;
        return 1;
    }
    return iter_step(rt, it, slots[1], out);
}

/* yield* step: mode 0 next 1 throw 2 return. returns 1 = done (value in *out), 0 = not done (result obj), -1 throw */
static JS_NOINLINE int delegate_op(js_rt *rt, jsval *slots, jsval recv, int mode, jsval *out) {
    jsval it = slots[0];
    jsval r;
    if (mode == 0) {
        r = call_fn(rt, slots[1], it, 1, &recv);
    } else {
        jsval m = get_p(rt, it, mode == 1 ? ATOM(throw) : ATOM(return));
        if (m == JS_EXC) return -1;
        if (is_nullish(m)) {
            if (mode == 1) {
                iter_close(rt, it, false);
                if (rt->has_exc) return -1;
                throw_error(rt, E_TYPE, "The iterator does not provide a 'throw' method");
                return -1;
            }
            throw_val(rt, make_return_cell(rt, recv));
            return -1;
        }
        r = call_fn(rt, m, it, 1, &recv);
    }
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
        jsval v = get_p(rt, r, ATOM(value));
        rt->sp = root;
        if (v == JS_EXC) return -1;
        if (mode == 2) {
            throw_val(rt, make_return_cell(rt, v));
            return -1;
        }
        *out = v;
        return 1;
    }
    rt->sp = root;
    *out = r;
    return 0;
}

static JS_NOINLINE int bind_this(js_rt *rt, Frame *f, jsval v, int hops, int idx) {
    if (hops == 0xFF) {
        if (f->argv[-1] != JS_UNINIT) {
            throw_error(rt, E_REFERENCE, "Super constructor may only be called once");
            return -1;
        }
        f->argv[-1] = v;
        return 0;
    }
    Env *e = f->env;
    while (hops-- > 0) e = e->parent;
    if (e->slots[idx] != JS_UNINIT) {
        throw_error(rt, E_REFERENCE, "Super constructor may only be called once");
        return -1;
    }
    e->slots[idx] = v;
    return 0;
}

static JS_NOINLINE jsval regexp_op(js_rt *rt, jsval pat, jsval flags) {
    return regexp_create(rt, pat, flags, rt->intr[I_RegExpProto]);
}

static JS_NOINLINE jsval typeof_global(js_rt *rt, jsval key) {
    jsval v = global_get_slow(rt, key, 0, true);
    if (v == JS_EXC) return JS_EXC;
    return typeof_val(rt, v);
}

static JS_NOINLINE int def_global(js_rt *rt, int kind, jsval key, jsval v, int isconst) {
    JSObject *g = rt->global;
    if (kind == 0) { /* var */
        if (find_own(rt, rt->global_lex, key)) {
            throw_error(rt, E_SYNTAX, "Identifier '%S' has already been declared", vstr(key));
            return -1;
        }
        int h = has_own(rt, g, key);
        if (h < 0) return -1;
        if (!h) add_prop(rt, g, key, JS_UNDEF, P_W | P_E);
        return 0;
    }
    if (kind == 1) { /* function */
        if (find_own(rt, rt->global_lex, key)) {
            throw_error(rt, E_SYNTAX, "Identifier '%S' has already been declared", vstr(key));
            return -1;
        }
        Prop *p = find_own(rt, g, key);
        if (!p || (p->flags & P_C)) {
            if (p) del_prop_at(rt, g, p);
            add_prop(rt, g, key, v, P_W | P_E);
        } else
            return set_prop(rt, mkobj(g), key, v, mkobj(g), true) < 0 ? -1 : 0;
        return 0;
    }
    if (kind == 2) { /* lexical */
        Prop *p = find_own(rt, rt->global_lex, key);
        Prop *gp = find_own(rt, g, key);
        if (p || (gp && !(gp->flags & P_C))) {
            throw_error(rt, E_SYNTAX, "Identifier '%S' has already been declared", vstr(key));
            return -1;
        }
        add_prop(rt, rt->global_lex, key, JS_UNINIT, isconst ? 0 : P_W);
        rt->lex_epoch++;
        return 0;
    }
    /* init lexical */
    Prop *p = find_own(rt, rt->global_lex, key);
    if (p) p->val = v;
    return 0;
}

static JS_NOINLINE jsval unary_slow(js_rt *rt, int o, jsval v) {
    jsval n;
    if (to_numeric(rt, v, &n) < 0) return JS_EXC;
    if (is_big(n)) {
        if (o == OP_NEG) return big_neg(rt, n);
        if (o == OP_BITNOT) return big_binop(rt, OP_BITNOT, n, n);
        if (o == OP_INC || o == OP_DEC) {
            JSBig *one = big_from_double(rt, 1);
            return big_binop(rt, o == OP_INC ? OP_ADD : OP_SUB, n, MKTAG(TAG_BIG, one));
        }
        if (o == OP_PLUS) return throw_error(rt, E_TYPE, "Cannot convert a BigInt value to a number");
        return n;
    }
    double d = vnum(n);
    switch (o) {
    case OP_NEG: return mknum(-d);
    case OP_BITNOT: return mknum(~to_int32(d));
    case OP_INC: return mknum(d + 1);
    case OP_DEC: return mknum(d - 1);
    default: return mknum(d);
    }
}

static JS_NOINLINE jsval tostring_op(js_rt *rt, jsval v) {
    JSString *s = to_string(rt, v);
    return s ? mkstr(s) : JS_EXC;
}

static JS_NOINLINE jsval get_super_ctor(js_rt *rt, jsval fn) {
    JSObject *f = vobj(fn);
    JSObject *p = f->proto;
    if (!p || !(p->oflags & OF_CTOR)) return throw_error(rt, E_TYPE, "Super constructor %S of anonymous class is not a constructor", p ? val_brief(rt, mkobj(p)) : str_cstr(rt, "null"));
    return mkobj(p);
}

static JS_NOINLINE jsval home_proto(js_rt *rt, jsval fn) {
    JSObject *f = vobj(fn);
    JSObject *h = f->cls == CLS_FUNCTION ? f->u.fn.home : 0;
    if (!h) return JS_UNDEF;
    jsval p = get_proto(rt, h);
    return p;
}

static JS_NOINLINE jsval elem_get_slow(js_rt *rt, jsval o, jsval k) {
    if (is_nullish(o)) {
        jsval kk = to_key(rt, k);
        if (kk == JS_EXC) return JS_EXC;
        jsval kd = key_to_display(rt, kk);
        return throw_error(rt, E_TYPE, "Cannot read properties of %s (reading '%S')", is_null(o) ? "null" : "undefined", vstr(kd));
    }
    return get_v(rt, o, k);
}

static JS_NOINLINE int forin_slot_init(js_rt *rt, jsval *slot, jsval obj) {
    jsval it = for_in_init(rt, obj);
    if (it == JS_EXC) return -1;
    *slot = it;
    return 0;
}

static JS_NOINLINE int iter_init_op(js_rt *rt, jsval *slots, jsval obj, int async) {
    jsval next;
    jsval it = get_iterator(rt, obj, &next, async != 0);
    if (it == JS_EXC) return -1;
    slots[0] = it;
    slots[1] = next;
    return 0;
}

static JS_NOINLINE jsval await_setup(js_rt *rt, JSObject *co, jsval v) {
    async_await(rt, co, v);
    return JS_UNDEF;
}

static JS_NOINLINE int init_fields(js_rt *rt, jsval thisv, jsval fn) {
    JSObject *f = vobj(fn);
    if (f->cls != CLS_FUNCTION || !f->u.fn.fields) return 0;
    jsval r = call_fn(rt, mkobj(f->u.fn.fields), thisv, 0, 0);
    return r == JS_EXC ? -1 : 0;
}

static JS_NOINLINE jsval obj_rest(js_rt *rt, jsval src, jsval excl) {
    JSObject *o = new_plain(rt);
    jsval *root = push_tmp(rt, mkobj(o));
    int r = copy_data_props(rt, mkobj(o), src, excl);
    rt->sp = root;
    return r < 0 ? JS_EXC : mkobj(o);
}

static JS_NOINLINE jsval set_fn_name(js_rt *rt, jsval key, jsval fn) {
    if (!is_obj(fn)) return fn;
    JSObject *f = vobj(fn);
    jsval kd;
    if (is_sym(key)) {
        JSSymbol *s = vsym(key);
        kd = is_str(s->desc) ? mkstr(jsfmt(rt, "[%S]", vstr(s->desc))) : ATOM(empty);
    } else
        kd = key;
    materialize_fn(rt, f);
    Prop *p = find_own(rt, f, ATOM(name));
    if (p && (p->flags & P_ACC)) return fn; /* static name() getter */
    def_prop(rt, f, ATOM(name), kd, P_C);
    return fn;
}

static JS_NOINLINE jsval to_propkey_op(js_rt *rt, jsval v) { return to_key(rt, v); }

static JS_NOINLINE jsval delete_op(js_rt *rt, jsval o, jsval k, bool strict) {
    if (is_nullish(o)) return throw_error(rt, E_TYPE, "Cannot convert undefined or null to object");
    jsval *root = push_tmp(rt, o);
    jsval kk = to_key(rt, k);
    rt->sp = root;
    if (kk == JS_EXC) return JS_EXC;
    int r = delete_prop(rt, o, kk, strict);
    if (r < 0) return JS_EXC;
    return mkbool(r);
}

#define OPLEN(n) (1 + (n))

static jsval run(js_rt *rt, bool throwing) {
    static void *const labels[OP__COUNT] = {
#define X(n, sz) &&L_##n,
        OPCODES(X)
#undef X
    };
    Frame *f = &rt->frames[rt->nframes - 1];
    uint8_t *pc;
    jsval *sp, *loc;
    jsval *K;
    jsval v;
    int nargs;
    jsval callee;
#define LOAD() (pc = f->pc, sp = f->sp, loc = f->argv, K = f->fp->consts)
#define SAVE() (f->pc = pc, f->sp = sp, rt->sp = sp)
#define NEXT goto *labels[*pc]
#define CASE(n) L_##n:
#define POLL()                                                                  \
    do {                                                                        \
        if (UNLIKELY(--rt->interrupt_count <= 0 || rt->gc_pending)) {           \
            SAVE();                                                             \
            if (poll(rt)) goto exception;                                       \
        }                                                                       \
    } while (0)
#define STRICT ((f->fp->flags & FP_STRICT) != 0)
    LOAD();
    if (throwing) goto exception;
    NEXT;

    CASE(NOP) pc++; NEXT;
    CASE(DEBUGGER) pc++; NEXT;
    CASE(PUSH_UNDEF) *sp++ = JS_UNDEF; pc++; NEXT;
    CASE(PUSH_NULL) *sp++ = JS_NULL; pc++; NEXT;
    CASE(PUSH_TRUE) *sp++ = JS_TRUE; pc++; NEXT;
    CASE(PUSH_FALSE) *sp++ = JS_FALSE; pc++; NEXT;
    CASE(PUSH_UNINIT) *sp++ = JS_UNINIT; pc++; NEXT;
    CASE(PUSH_I8) *sp++ = mknum((int8_t)pc[1]); pc += 2; NEXT;
    CASE(PUSH_I32) *sp++ = mknum((int32_t)rd32(pc + 1)); pc += 5; NEXT;
    CASE(PUSH_CONST) *sp++ = K[rd32(pc + 1)]; pc += 5; NEXT;
    CASE(PUSH_THIS) *sp++ = loc[-1]; pc++; NEXT;
    CASE(PUSH_THIS_CHK)
        v = loc[-1];
        if (UNLIKELY(v == JS_UNINIT)) {
            SAVE();
            throw_error(rt, E_REFERENCE, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
            goto exception;
        }
        *sp++ = v;
        pc++;
        NEXT;
    CASE(PUSH_NEW_TARGET) *sp++ = f->new_target; pc++; NEXT;
    CASE(PUSH_FUNC) *sp++ = mkobj(f->func); pc++; NEXT;
    CASE(POP) sp--; pc++; NEXT;
    CASE(DUP) sp[0] = sp[-1]; sp++; pc++; NEXT;
    CASE(DUP2) sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; pc++; NEXT;
    CASE(SWAP) v = sp[-1]; sp[-1] = sp[-2]; sp[-2] = v; pc++; NEXT;
    CASE(ROT3R) v = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = v; pc++; NEXT;
    CASE(ROT3L) v = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = v; pc++; NEXT;
    CASE(ROT4R) v = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = sp[-4]; sp[-4] = v; pc++; NEXT;
    CASE(NIP) sp[-2] = sp[-1]; sp--; pc++; NEXT;
    CASE(OVER) sp[0] = sp[-2]; sp++; pc++; NEXT;

    CASE(GET_LOC) *sp++ = loc[rd16(pc + 1)]; pc += 3; NEXT;
    CASE(PUT_LOC) loc[rd16(pc + 1)] = *--sp; pc += 3; NEXT;
    CASE(SET_LOC) loc[rd16(pc + 1)] = sp[-1]; pc += 3; NEXT;
    CASE(GET_LOC_CHK)
        v = loc[rd16(pc + 1)];
        if (UNLIKELY(v == JS_UNINIT)) {
            SAVE();
            throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(K[rd32(pc + 3)]));
            goto exception;
        }
        *sp++ = v;
        pc += 7;
        NEXT;
    CASE(SET_LOC_CHK)
        if (UNLIKELY(loc[rd16(pc + 1)] == JS_UNINIT)) {
            SAVE();
            throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(K[rd32(pc + 3)]));
            goto exception;
        }
        loc[rd16(pc + 1)] = sp[-1];
        pc += 7;
        NEXT;
    CASE(INC_LOC) {
        jsval *p = &loc[rd16(pc + 1)];
        if (LIKELY(is_num(*p))) {
            *p = mknum(vnum(*p) + 1);
        } else {
            SAVE();
            v = unary_slow(rt, OP_INC, *p);
            if (v == JS_EXC) goto exception;
            *p = v;
        }
        pc += 3;
        NEXT;
    }
    CASE(GET_ENV) {
        Env *e = f->env;
        for (int h = pc[1]; h > 0; h--) e = e->parent;
        *sp++ = e->slots[rd16(pc + 2)];
        pc += 4;
        NEXT;
    }
    CASE(PUT_ENV) {
        Env *e = f->env;
        for (int h = pc[1]; h > 0; h--) e = e->parent;
        e->slots[rd16(pc + 2)] = *--sp;
        pc += 4;
        NEXT;
    }
    CASE(SET_ENV) {
        Env *e = f->env;
        for (int h = pc[1]; h > 0; h--) e = e->parent;
        e->slots[rd16(pc + 2)] = sp[-1];
        pc += 4;
        NEXT;
    }
    CASE(GET_ENV_CHK) {
        Env *e = f->env;
        for (int h = pc[1]; h > 0; h--) e = e->parent;
        v = e->slots[rd16(pc + 2)];
        if (UNLIKELY(v == JS_UNINIT)) {
            SAVE();
            throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(K[rd32(pc + 4)]));
            goto exception;
        }
        *sp++ = v;
        pc += 8;
        NEXT;
    }
    CASE(SET_ENV_CHK) {
        Env *e = f->env;
        for (int h = pc[1]; h > 0; h--) e = e->parent;
        jsval *p = &e->slots[rd16(pc + 2)];
        if (UNLIKELY(*p == JS_UNINIT)) {
            SAVE();
            throw_error(rt, E_REFERENCE, "Cannot access '%S' before initialization", vstr(K[rd32(pc + 4)]));
            goto exception;
        }
        *p = sp[-1];
        pc += 8;
        NEXT;
    }
    CASE(GET_GLOBAL) {
        jsval key = K[rd32(pc + 1)];
        uint32_t ic = rd32(pc + 5);
        if (ic != 0xFFFFFFFFu && rd32(pc + 9) == rt->lex_epoch) {
            JSObject *o = (ic & 0x80000000u) ? rt->global_lex : rt->global;
            uint32_t ix = ic & 0x7FFFFFFFu;
            if (ix < o->nprops && o->props[ix].key == key && !(o->props[ix].flags & P_ACC) && o->props[ix].val != JS_UNINIT) {
                *sp++ = o->props[ix].val;
                pc += 13;
                NEXT;
            }
        }
        SAVE();
        v = global_get_slow(rt, key, pc + 5, false);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 13;
        NEXT;
    }
    CASE(TYPEOF_GLOBAL)
        SAVE();
        v = typeof_global(rt, K[rd32(pc + 1)]);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 5;
        NEXT;
    CASE(PUT_GLOBAL)
    CASE(SET_GLOBAL) {
        jsval key = K[rd32(pc + 1)];
        uint32_t ic = rd32(pc + 5);
        bool keep = *pc == OP_SET_GLOBAL;
        if (ic != 0xFFFFFFFFu && rd32(pc + 9) == rt->lex_epoch) {
            JSObject *o = (ic & 0x80000000u) ? rt->global_lex : rt->global;
            uint32_t ix = ic & 0x7FFFFFFFu;
            if (ix < o->nprops && o->props[ix].key == key && (o->props[ix].flags & (P_W | P_ACC)) == P_W &&
                o->props[ix].val != JS_UNINIT) {
                o->props[ix].val = sp[-1];
                if (!keep) sp--;
                pc += 13;
                NEXT;
            }
        }
        SAVE();
        if (global_put_slow(rt, key, sp[-1], pc + 5, STRICT) < 0) goto exception;
        if (!keep) sp--;
        pc += 13;
        NEXT;
    }
    CASE(DEF_GLOBAL_VAR)
        SAVE();
        if (def_global(rt, 0, K[rd32(pc + 1)], JS_UNDEF, 0) < 0) goto exception;
        pc += 5;
        NEXT;
    CASE(DEF_GLOBAL_FUNC)
        SAVE();
        if (def_global(rt, 1, K[rd32(pc + 1)], sp[-1], 0) < 0) goto exception;
        sp--;
        pc += 5;
        NEXT;
    CASE(DEF_GLOBAL_LEX)
        SAVE();
        if (def_global(rt, 2, K[rd32(pc + 1)], JS_UNDEF, pc[5]) < 0) goto exception;
        pc += 6;
        NEXT;
    CASE(INIT_GLOBAL_LEX)
        def_global(rt, 3, K[rd32(pc + 1)], sp[-1], 0);
        sp--;
        pc += 5;
        NEXT;
    CASE(DELETE_GLOBAL) {
        SAVE();
        jsval key = K[rd32(pc + 1)];
        if (find_own(rt, rt->global_lex, key))
            v = JS_FALSE;
        else {
            int dr = delete_prop(rt, mkobj(rt->global), key, false);
            if (dr < 0) goto exception;
            v = mkbool(dr);
        }
        *sp++ = v;
        pc += 5;
        NEXT;
    }
    CASE(PUSH_ENV) {
        uint32_t n = rd16(pc + 1);
        Env *e = (Env *)gc_alloc(rt, sizeof(Env) + n * sizeof(jsval), GC_ENV);
        e->parent = f->env;
        e->n = n;
        for (uint32_t i = 0; i < n; i++) e->slots[i] = JS_UNDEF;
        f->env = e;
        pc += 3;
        NEXT;
    }
    CASE(POP_ENV) f->env = f->env->parent; pc++; NEXT;
    CASE(COPY_ENV) {
        Env *o = f->env;
        Env *e = (Env *)gc_alloc(rt, sizeof(Env) + o->n * sizeof(jsval), GC_ENV);
        e->parent = o->parent;
        e->n = o->n;
        memcpy(e->slots, o->slots, o->n * sizeof(jsval));
        f->env = e;
        pc++;
        NEXT;
    }

    CASE(GET_FIELD) {
        jsval key = K[rd32(pc + 1)];
        jsval o = sp[-1];
        if (LIKELY(is_obj(o))) {
            JSObject *ob = vobj(o);
            uint32_t ic = rd32(pc + 5);
            if (ic < ob->nprops) {
                Prop *p = &ob->props[ic];
                if (p->key == key && !(p->flags & P_ACC) && !(ob->oflags & OF_EXOTIC)) {
                    sp[-1] = p->val;
                    pc += 9;
                    NEXT;
                }
            }
            if (ob->cls == CLS_ARRAY && key == rt->atoms[A_length]) {
                sp[-1] = mknum(ob->u.arr.len);
                pc += 9;
                NEXT;
            }
        } else if (is_str(o) && key == rt->atoms[A_length]) {
            sp[-1] = mknum(vstr(o)->len);
            pc += 9;
            NEXT;
        }
        SAVE();
        v = get_field_slow(rt, o, key, pc + 5);
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        pc += 9;
        NEXT;
    }
    CASE(GET_METHOD) {
        jsval key = K[rd32(pc + 1)];
        jsval o = sp[-1];
        if (LIKELY(is_obj(o))) {
            JSObject *ob = vobj(o);
            uint32_t ic = rd32(pc + 5);
            if (ic < ob->nprops) {
                Prop *p = &ob->props[ic];
                if (p->key == key && !(p->flags & P_ACC) && !(ob->oflags & OF_EXOTIC)) {
                    sp[-1] = p->val;
                    sp[0] = o;
                    sp++;
                    pc += 9;
                    NEXT;
                }
            }
        }
        SAVE();
        v = get_field_slow(rt, o, key, pc + 5);
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        sp[0] = o;
        sp++;
        pc += 9;
        NEXT;
    }
    CASE(PUT_FIELD) {
        jsval key = K[rd32(pc + 1)];
        jsval o = sp[-2];
        if (LIKELY(is_obj(o))) {
            JSObject *ob = vobj(o);
            uint32_t ic = rd32(pc + 5);
            if (ic < ob->nprops) {
                Prop *p = &ob->props[ic];
                if (p->key == key && (p->flags & (P_W | P_ACC)) == P_W && !(ob->oflags & OF_EXOTIC)) {
                    p->val = sp[-1];
                    sp[-2] = sp[-1];
                    sp--;
                    pc += 9;
                    NEXT;
                }
            }
        }
        SAVE();
        if (put_field_slow(rt, o, key, sp[-1], pc + 5, STRICT) < 0) goto exception;
        sp[-2] = sp[-1];
        sp--;
        pc += 9;
        NEXT;
    }
    CASE(GET_ELEM) {
        jsval o = sp[-2], k = sp[-1];
        if (is_obj(o) && is_num(k)) {
            JSObject *ob = vobj(o);
            double d = vnum(k);
            if (ob->cls == CLS_ARRAY && !(ob->oflags & OF_SLOW_ARRAY) && d >= 0 && d < ob->u.arr.len) {
                uint32_t i = (uint32_t)d;
                if ((double)i == d && ob->u.arr.elems[i] != JS_HOLE) {
                    sp[-2] = ob->u.arr.elems[i];
                    sp--;
                    pc++;
                    NEXT;
                }
            }
        } else if (is_str(o) && is_num(k)) {
            JSString *s = vstr(o);
            double d = vnum(k);
            if (d >= 0 && d < s->len && (double)(uint32_t)d == d) {
                str_flat(rt, s);
                sp[-2] = mkstr(str_char(rt, str_at(s, (uint32_t)d)));
                sp--;
                pc++;
                NEXT;
            }
        }
        SAVE();
        v = elem_get_slow(rt, o, k);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    }
    CASE(GET_ELEM_METHOD) {
        jsval o = sp[-2], k = sp[-1];
        SAVE();
        v = elem_get_slow(rt, o, k);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp[-1] = o;
        pc++;
        NEXT;
    }
    CASE(PUT_ELEM) {
        jsval o = sp[-3], k = sp[-2];
        if (is_obj(o) && is_num(k)) {
            JSObject *ob = vobj(o);
            double d = vnum(k);
            if (ob->cls == CLS_ARRAY && !(ob->oflags & (OF_SLOW_ARRAY | OF_FROZEN_EL)) && d >= 0 && d < ob->u.arr.len) {
                uint32_t i = (uint32_t)d;
                if ((double)i == d && ob->u.arr.elems[i] != JS_HOLE) {
                    ob->u.arr.elems[i] = sp[-1];
                    sp[-3] = sp[-1];
                    sp -= 2;
                    pc++;
                    NEXT;
                }
            }
        }
        SAVE();
        if (is_nullish(o)) {
            jsval kk = to_key(rt, k);
            if (kk == JS_EXC) goto exception;
            set_prop(rt, o, kk, sp[-1], o, true);
            goto exception;
        }
        if (set_v(rt, o, k, sp[-1], STRICT) < 0) goto exception;
        sp[-3] = sp[-1];
        sp -= 2;
        pc++;
        NEXT;
    }
    CASE(DELETE_FIELD)
        SAVE();
        v = delete_op(rt, sp[-1], K[rd32(pc + 1)], STRICT);
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        pc += 5;
        NEXT;
    CASE(DELETE_ELEM)
        SAVE();
        v = delete_op(rt, sp[-2], sp[-1], STRICT);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    CASE(HOME_PROTO)
        SAVE();
        v = home_proto(rt, sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        pc++;
        NEXT;
    CASE(GET_SUPER_FIELD)
        SAVE();
        if (!is_obj(sp[-2])) {
            throw_error(rt, E_TYPE, "Cannot read properties of %S (reading '%S')", val_brief(rt, sp[-2]), vstr(K[rd32(pc + 1)]));
            goto exception;
        }
        v = get_prop(rt, sp[-2], K[rd32(pc + 1)], sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc += 5;
        NEXT;
    CASE(GET_SUPER_METHOD)
        SAVE();
        if (!is_obj(sp[-2])) {
            throw_error(rt, E_TYPE, "Cannot read properties of %S (reading '%S')", val_brief(rt, sp[-2]), vstr(K[rd32(pc + 1)]));
            goto exception;
        }
        v = get_prop(rt, sp[-2], K[rd32(pc + 1)], sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        pc += 5;
        NEXT;
    CASE(PUT_SUPER_FIELD)
        SAVE();
        if (set_prop(rt, sp[-3], K[rd32(pc + 1)], sp[-1], sp[-2], true) < 0) goto exception;
        sp[-3] = sp[-1];
        sp -= 2;
        pc += 5;
        NEXT;
    CASE(GET_SUPER_ELEM) {
        SAVE();
        jsval k = to_key(rt, sp[-1]);
        if (k == JS_EXC) goto exception;
        v = get_prop(rt, sp[-3], k, sp[-2]);
        if (v == JS_EXC) goto exception;
        sp[-3] = v;
        sp -= 2;
        pc++;
        NEXT;
    }
    CASE(PUT_SUPER_ELEM) {
        SAVE();
        jsval k = to_key(rt, sp[-2]);
        if (k == JS_EXC) goto exception;
        if (set_prop(rt, sp[-4], k, sp[-1], sp[-3], true) < 0) goto exception;
        sp[-4] = sp[-1];
        sp -= 3;
        pc++;
        NEXT;
    }
    CASE(GET_PRIVATE)
        SAVE();
        v = priv_get(rt, sp[-2], sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    CASE(GET_PRIVATE_METHOD) pc++; NEXT;
    CASE(PUT_PRIVATE)
        SAVE();
        if (priv_put(rt, sp[-3], sp[-2], sp[-1]) < 0) goto exception;
        sp[-3] = sp[-1];
        sp -= 2;
        pc++;
        NEXT;
    CASE(HAS_PRIVATE)
        if (!is_obj(sp[-1])) {
            SAVE();
            throw_error(rt, E_TYPE, "Cannot use 'in' operator to search for '%S' in %S", vstr(vsym(sp[-2])->desc), val_brief(rt, sp[-1]));
            goto exception;
        }
        v = mkbool(find_own(rt, vobj(sp[-1]), sp[-2]) != 0);
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    CASE(DEF_PRIVATE)
        SAVE();
        if (priv_def(rt, sp[-3], sp[-2], sp[-1], pc[1]) < 0) goto exception;
        sp -= 3;
        pc += 2;
        NEXT;
    CASE(IN)
        SAVE();
        v = op_in(rt, sp);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    CASE(INSTANCEOF)
        SAVE();
        v = instance_of(rt, sp[-2], sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc++;
        NEXT;
    CASE(NEW_OBJECT) *sp++ = mkobj(new_plain(rt)); pc++; NEXT;
    CASE(NEW_ARRAY) *sp++ = mkobj(new_array(rt, 0)); pc++; NEXT;
    CASE(DEFINE_FIELD) {
        JSObject *o = vobj(sp[-2]);
        jsval key = K[rd32(pc + 1)];
        if (!(o->oflags & OF_EXOTIC) && o->nprops < 8 && !find_own(rt, o, key))
            add_prop(rt, o, key, sp[-1], P_DEFAULT);
        else {
            SAVE();
            if (create_data_prop(rt, o, key, sp[-1], true) < 0) goto exception;
        }
        sp--;
        pc += 5;
        NEXT;
    }
    CASE(DEFINE_ELEM) {
        SAVE();
        jsval k = to_key(rt, sp[-2]);
        if (k == JS_EXC) goto exception;
        if (!is_obj(sp[-3])) {
            throw_error(rt, E_TYPE, "Cannot define property on non-object");
            goto exception;
        }
        if (create_data_prop(rt, vobj(sp[-3]), k, sp[-1], true) < 0) goto exception;
        sp -= 2;
        pc++;
        NEXT;
    }
    CASE(DEFINE_METHOD)
        SAVE();
        if (define_method(rt, sp[-3], sp[-2], sp[-1], pc[1], pc[2]) < 0) goto exception;
        sp -= 2;
        pc += 3;
        NEXT;
    CASE(SET_PROTO_LIT)
        if (is_obj(sp[-1]) || is_null(sp[-1])) vobj(sp[-2])->proto = is_obj(sp[-1]) ? vobj(sp[-1]) : 0;
        sp--;
        pc++;
        NEXT;
    CASE(COPY_DATA_PROPS)
        SAVE();
        if (copy_data_props(rt, sp[-2], sp[-1], JS_UNDEF) < 0) goto exception;
        sp--;
        pc++;
        NEXT;
    CASE(OBJ_REST)
        SAVE();
        v = obj_rest(rt, sp[-2], sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-2] = v;
        sp--;
        pc += 3;
        NEXT;
    CASE(ARRAY_APPEND) {
        JSObject *a = vobj(sp[-2]);
        if (LIKELY(a->u.arr.len < a->u.arr.cap))
            a->u.arr.elems[a->u.arr.len++] = sp[-1];
        else
            array_push(rt, a, sp[-1]);
        sp--;
        pc++;
        NEXT;
    }
    CASE(ARRAY_HOLE) {
        JSObject *a = vobj(sp[-1]);
        array_push(rt, a, JS_HOLE);
        pc++;
        NEXT;
    }
    CASE(ARRAY_SPREAD) {
        SAVE();
        uint32_t di = rd32(pc + 1);
        if (!(is_obj(sp[-1]) && vobj(sp[-1])->cls == CLS_ARRAY) &&
            iter_precheck(rt, sp[-1], di >= 0xFFFFFFFEu ? JS_UNDEF : K[di], di == 0xFFFFFFFEu) < 0)
            goto exception;
        if (array_spread(rt, sp[-2], sp[-1]) < 0) goto exception;
        sp--;
        pc += 5;
        NEXT;
    }
    CASE(SET_FUNC_NAME)
        SAVE();
        set_fn_name(rt, sp[-2], sp[-1]);
        pc += 2;
        NEXT;
    CASE(TO_PROPKEY)
        if (!is_num(sp[-1]) || !(vnum(sp[-1]) >= 0 && vnum(sp[-1]) < 4294967295.0 && vnum(sp[-1]) == (uint32_t)vnum(sp[-1]))) {
            SAVE();
            v = to_propkey_op(rt, sp[-1]);
            if (v == JS_EXC) goto exception;
            sp[-1] = v;
        }
        pc++;
        NEXT;

    CASE(ADD) {
        jsval a = sp[-2], b = sp[-1];
        if (LIKELY(is_num(a) && is_num(b))) {
            sp[-2] = mknum(vnum(a) + vnum(b));
        } else if (is_str(a) && is_str(b)) {
            JSString *s = str_concat(rt, vstr(a), vstr(b));
            if (!s) {
                SAVE();
                throw_error(rt, E_RANGE, "Invalid string length");
                goto exception;
            }
            sp[-2] = mkstr(s);
        } else {
            SAVE();
            v = add_slow(rt, sp);
            if (v == JS_EXC) goto exception;
            sp[-2] = v;
        }
        sp--;
        pc++;
        NEXT;
    }
#define ARITH(NAME, EXPR)                                         \
    CASE(NAME) {                                                  \
        jsval a = sp[-2], b = sp[-1];                             \
        if (LIKELY(is_num(a) && is_num(b))) {                     \
            double x = vnum(a), y = vnum(b);                      \
            sp[-2] = mknum(EXPR);                                 \
        } else {                                                  \
            SAVE();                                               \
            v = arith_slow(rt, OP_##NAME, sp);                    \
            if (v == JS_EXC) goto exception;                      \
            sp[-2] = v;                                           \
        }                                                         \
        sp--;                                                     \
        pc++;                                                     \
        NEXT;                                                     \
    }
    ARITH(SUB, x - y)
    ARITH(MUL, x * y)
    ARITH(DIV, x / y)
    ARITH(MOD, (x >= 0 && y > 0 && x < 2147483648.0 && y < 2147483648.0 && x == (int32_t)x && y == (int32_t)y) ? (double)((int32_t)x % (int32_t)y) : js_fmod(x, y))
    ARITH(POW, js_pow(x, y))
    ARITH(BAND, (double)(to_int32(x) & to_int32(y)))
    ARITH(BOR, (double)(to_int32(x) | to_int32(y)))
    ARITH(BXOR, (double)(to_int32(x) ^ to_int32(y)))
    ARITH(SHL, (double)(int32_t)((uint32_t)to_int32(x) << (to_uint32(y) & 31)))
    ARITH(SAR, (double)(to_int32(x) >> (to_uint32(y) & 31)))
    ARITH(SHR, (double)(to_uint32(x) >> (to_uint32(y) & 31)))
#undef ARITH
#define CMP(NAME, OPX, LEFT_FIRST, SWAP, NEG)                                       \
    CASE(NAME) {                                                                    \
        jsval a = sp[-2], b = sp[-1];                                               \
        if (LIKELY(is_num(a) && is_num(b))) {                                       \
            sp[-2] = mkbool(vnum(a) OPX vnum(b));                                   \
        } else if (is_str(a) && is_str(b)) {                                        \
            sp[-2] = mkbool(str_cmp(rt, vstr(a), vstr(b)) OPX 0);                   \
        } else {                                                                    \
            SAVE();                                                                 \
            int cr = compare_slow(rt, sp, LEFT_FIRST, SWAP);                        \
            if (cr < 0) goto exception;                                             \
            sp[-2] = cr == 2 ? JS_FALSE : mkbool(NEG ? !cr : cr);                   \
        }                                                                           \
        sp--;                                                                       \
        pc++;                                                                       \
        NEXT;                                                                       \
    }
    CMP(LT, <, true, false, false)
    CMP(GT, >, true, true, false)
    CMP(LE, <=, true, true, true)
    CMP(GE, >=, true, false, true)
#undef CMP
    CASE(EQ)
    CASE(NE) {
        jsval a = sp[-2], b = sp[-1];
        int eq;
        if (is_num(a) && is_num(b))
            eq = vnum(a) == vnum(b);
        else if (a == b && !is_num(a))
            eq = 1;
        else {
            SAVE();
            eq = loose_eq(rt, a, b);
            if (eq < 0) goto exception;
        }
        sp[-2] = mkbool(*pc == OP_EQ ? eq : !eq);
        sp--;
        pc++;
        NEXT;
    }
    CASE(SEQ)
    CASE(SNE) {
        jsval a = sp[-2], b = sp[-1];
        bool eq;
        if (is_num(a) && is_num(b))
            eq = vnum(a) == vnum(b);
        else if (is_str(a) && is_str(b))
            eq = str_eq(rt, vstr(a), vstr(b));
        else if (is_big(a) && is_big(b))
            eq = big_cmp((JSBig *)vptr(a), (JSBig *)vptr(b)) == 0;
        else
            eq = a == b;
        sp[-2] = mkbool(*pc == OP_SEQ ? eq : !eq);
        sp--;
        pc++;
        NEXT;
    }
    CASE(TYPEOF) sp[-1] = typeof_val(rt, sp[-1]); pc++; NEXT;
    CASE(NOT) sp[-1] = mkbool(!truthy(sp[-1])); pc++; NEXT;
    CASE(NEG)
    CASE(PLUS)
    CASE(BITNOT)
    CASE(INC)
    CASE(DEC)
    CASE(TO_NUMERIC) {
        jsval a = sp[-1];
        int o = *pc;
        if (LIKELY(is_num(a))) {
            double d = vnum(a);
            switch (o) {
            case OP_NEG: d = -d; break;
            case OP_BITNOT: d = (double)~to_int32(d); break;
            case OP_INC: d += 1; break;
            case OP_DEC: d -= 1; break;
            default: break;
            }
            sp[-1] = mknum(d);
        } else {
            SAVE();
            v = unary_slow(rt, o == OP_TO_NUMERIC ? OP_NOP : o, a);
            if (v == JS_EXC) goto exception;
            sp[-1] = v;
        }
        pc++;
        NEXT;
    }
    CASE(TO_STRING)
        if (!is_str(sp[-1])) {
            SAVE();
            v = tostring_op(rt, sp[-1]);
            if (v == JS_EXC) goto exception;
            sp[-1] = v;
        }
        pc++;
        NEXT;

    CASE(JMP) {
        int32_t off = (int32_t)rd32(pc + 1);
        pc += 5 + off;
        if (off < 0) POLL();
        NEXT;
    }
    CASE(JF) {
        int32_t off = (int32_t)rd32(pc + 1);
        jsval c = *--sp;
        bool t = c == JS_TRUE ? true : c == JS_FALSE ? false : truthy(c);
        pc += 5;
        if (!t) {
            pc += off;
            if (off < 0) POLL();
        }
        NEXT;
    }
    CASE(JT) {
        int32_t off = (int32_t)rd32(pc + 1);
        jsval c = *--sp;
        bool t = c == JS_TRUE ? true : c == JS_FALSE ? false : truthy(c);
        pc += 5;
        if (t) {
            pc += off;
            if (off < 0) POLL();
        }
        NEXT;
    }
    CASE(JF_KEEP)
        if (!truthy(sp[-1])) pc += 5 + (int32_t)rd32(pc + 1);
        else {
            sp--;
            pc += 5;
        }
        NEXT;
    CASE(JT_KEEP)
        if (truthy(sp[-1])) pc += 5 + (int32_t)rd32(pc + 1);
        else {
            sp--;
            pc += 5;
        }
        NEXT;
    CASE(JNN_KEEP)
        if (!is_nullish(sp[-1])) pc += 5 + (int32_t)rd32(pc + 1);
        else {
            sp--;
            pc += 5;
        }
        NEXT;
    CASE(JNU_KEEP)
        if (!is_undef(sp[-1])) pc += 5 + (int32_t)rd32(pc + 1);
        else {
            sp--;
            pc += 5;
        }
        NEXT;
    CASE(OPT_CHK)
        if (is_nullish(sp[-1 - pc[1]])) {
            sp -= pc[2] & 0x7F;
            *sp++ = (pc[2] & 0x80) ? JS_TRUE : JS_UNDEF;
            pc += 7 + (int32_t)rd32(pc + 3);
        } else
            pc += 7;
        NEXT;

    CASE(CALL) {
        nargs = rd16(pc + 1);
        callee = sp[-nargs - 2];
        pc += 7;
    do_call:
        if (LIKELY(is_obj(callee))) {
            JSObject *o = vobj(callee);
            if (LIKELY(o->cls == CLS_FUNCTION)) {
                SAVE();
                f->sp = sp - nargs - 2;
                if (setup_frame(rt, o, sp - nargs, nargs, JS_UNDEF, 0) < 0) goto exception;
                f = &rt->frames[rt->nframes - 1];
                LOAD();
                POLL();
                NEXT;
            }
            if (o->cls == CLS_NATIVE) {
                SAVE();
                v = call_native(rt, o, sp[-nargs - 1], nargs, sp - nargs, JS_UNDEF);
                if (v == JS_EXC) goto exception;
                sp -= nargs + 1;
                sp[-1] = v;
                NEXT;
            }
            if (o->oflags & OF_CALLABLE) {
                SAVE();
                v = call_slow(rt, callee, sp[-nargs - 1], nargs, sp - nargs);
                if (v == JS_EXC) goto exception;
                sp -= nargs + 1;
                sp[-1] = v;
                NEXT;
            }
        }
        SAVE();
        {
            uint8_t *ip = pc - 7;
            uint32_t desc = *ip == OP_CALL ? rd32(ip + 3) : rd32(pc - 4);
            not_callable(rt, f->fp, desc, callee, false);
        }
        goto exception;
    }
    CASE(CALL_ARRAY) {
        SAVE();
        nargs = spread_args(rt, sp);
        if (nargs < 0) goto exception;
        sp += nargs - 1;
        callee = sp[-nargs - 2];
        pc += 5;
        if (!is_callable(callee)) {
            SAVE();
            not_callable(rt, f->fp, rd32(pc - 4), callee, false);
            goto exception;
        }
        goto do_call;
    }
    CASE(NEW)
    CASE(NEW_ARRAY_ARGS)
    CASE(SUPER_CALL)
    CASE(SUPER_CALL_ARRAY) {
        int o_ = *pc;
        int argc;
        jsval nt;
        uint32_t desc = 0xFFFFFFFFu;
        if (o_ == OP_NEW_ARRAY_ARGS || o_ == OP_SUPER_CALL_ARRAY) {
            SAVE();
            argc = spread_args(rt, sp);
            if (argc < 0) goto exception;
            sp += argc - 1;
            if (o_ == OP_NEW_ARRAY_ARGS) desc = rd32(pc + 1);
            pc += o_ == OP_NEW_ARRAY_ARGS ? 5 : 1;
        } else {
            argc = rd16(pc + 1);
            if (o_ == OP_NEW) desc = rd32(pc + 3);
            pc += o_ == OP_NEW ? 7 : 3;
        }
        jsval ctor = sp[-argc - 2];
        bool is_super = o_ == OP_SUPER_CALL || o_ == OP_SUPER_CALL_ARRAY;
        nt = is_super ? sp[-argc - 1] : ctor;
        if (!is_ctor(ctor)) {
            SAVE();
            not_callable(rt, f->fp, desc, ctor, true);
            goto exception;
        }
        JSObject *o = vobj(ctor);
        SAVE();
        if (o->cls == CLS_FUNCTION) {
            v = ctor_this(rt, o, nt);
            if (v == JS_EXC) goto exception;
            sp[-argc - 1] = v;
            f->sp = sp - argc - 2;
            if (setup_frame(rt, o, sp - argc, argc, nt, F_CONSTRUCT) < 0) goto exception;
            f = &rt->frames[rt->nframes - 1];
            LOAD();
            POLL();
            NEXT;
        }
        v = construct_fn(rt, ctor, argc, sp - argc, nt);
        if (v == JS_EXC) goto exception;
        sp -= argc + 1;
        sp[-1] = v;
        NEXT;
    }
    CASE(GET_SUPER_CTOR)
        SAVE();
        v = get_super_ctor(rt, sp[-1]);
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        pc++;
        NEXT;
    CASE(BIND_THIS)
        SAVE();
        if (bind_this(rt, f, sp[-1], pc[1], rd16(pc + 2)) < 0) goto exception;
        pc += 4;
        NEXT;
    CASE(INIT_FIELDS)
        SAVE();
        if (init_fields(rt, sp[-2], sp[-1]) < 0) goto exception;
        sp--;
        pc++;
        NEXT;
    CASE(RETURN_UNDEF)
        *sp++ = JS_UNDEF;
        /* fallthrough */
    CASE(RETURN) {
        v = sp[-1];
    do_return:
        if (f->flags & F_CONSTRUCT) {
            if (!is_obj(v)) {
                if (f->fp->flags & FP_DERIVED) {
                    if (!is_undef(v) && v != JS_UNINIT) {
                        SAVE();
                        throw_error(rt, E_TYPE, "Derived constructors may only return object or undefined");
                        goto exception;
                    }
                    v = loc[-1];
                    if (v == JS_UNINIT) {
                        SAVE();
                        throw_error(rt, E_REFERENCE, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
                        goto exception;
                    }
                } else
                    v = loc[-1];
            }
        }
        if (UNLIKELY(f->coro != 0)) {
            JSObject *co = f->coro;
            CoroData *cd = (CoroData *)co->u.data.p;
            cd->state = 3;
            if (co->cls == CLS_ASYNC) {
                SAVE();
                rt->sp = sp + 1;
                *sp = v;
                promise_resolve(rt, vobj(cd->promise), v);
                v = cd->promise;
            }
            rt->coro_status = 0;
        }
        rt->nhandlers = f->hbase;
        jsval *dst = f->argv - 2;
        if (f->flags & F_STOP) {
            rt->nframes--;
            rt->sp = dst;
            return v;
        }
        rt->nframes--;
        f = &rt->frames[rt->nframes - 1];
        LOAD();
        sp = dst;
        *sp++ = v;
        NEXT;
    }
    CASE(THROW)
        SAVE();
        throw_val(rt, sp[-1]);
        goto exception;
    CASE(RETHROW)
        SAVE();
        rt->exc = sp[-1];
        rt->has_exc = true;
        goto exception;
    CASE(THROW_REF)
        SAVE();
        throw_error(rt, E_REFERENCE, "%S is not defined", vstr(K[rd32(pc + 1)]));
        goto exception;
    CASE(THROW_CONST)
        SAVE();
        throw_error(rt, E_TYPE, "Assignment to constant variable.");
        goto exception;
    CASE(THROW_ERR)
        SAVE();
        throw_error(rt, pc[1], "%S", vstr(K[rd32(pc + 2)]));
        goto exception;
    CASE(TRY) {
        Handler h;
        h.frame = (uint32_t)(rt->nframes - 1);
        h.kind = pc[1];
        h.pc_off = (uint32_t)(pc + 6 + (int32_t)rd32(pc + 2) - f->fp->code);
        h.sp_off = (uint32_t)(sp - loc);
        h.env = f->env;
        push_handler(rt, &h);
        pc += 6;
        NEXT;
    }
    CASE(TRY_END) rt->nhandlers--; pc++; NEXT;
    CASE(GOSUB) {
        int32_t off = (int32_t)rd32(pc + 1);
        pc += 5;
        *sp++ = mknum((double)(pc - f->fp->code));
        pc += off;
        NEXT;
    }
    CASE(RET) {
        double a = vnum(*--sp);
        pc = f->fp->code + (uint32_t)a;
        NEXT;
    }
    CASE(CLOSURE)
        *sp++ = mkobj(make_closure(rt, (FuncProto *)vptr(K[rd32(pc + 1)]), f->env));
        pc += 5;
        NEXT;
    CASE(CLASS) {
        SAVE();
        bool her = pc[5] != 0;
        jsval parent = her ? sp[-1] : JS_UNDEF;
        v = do_class(rt, (FuncProto *)vptr(K[rd32(pc + 1)]), her, parent, f->env);
        if (v == JS_EXC) goto exception;
        if (her) sp--;
        *sp++ = v;
        *sp++ = mkobj(rt->cur_this_fn);
        rt->cur_this_fn = 0;
        pc += 6;
        NEXT;
    }
    CASE(SET_FIELDS) {
        JSObject *fn = vobj(sp[-1]);
        fn->u.fn.home = vobj(sp[-2]);
        vobj(sp[-3])->u.fn.fields = fn;
        sp--;
        pc++;
        NEXT;
    }
    CASE(NEW_PRIVATE)
        *sp++ = new_private(rt, K[rd32(pc + 1)], rd32(pc + 5) == 0xFFFFFFFFu ? JS_UNDEF : K[rd32(pc + 5)], pc[9]);
        pc += 10;
        NEXT;
    CASE(SET_HOME) {
        JSObject *fn = vobj(sp[-1]);
        if (fn->cls == CLS_FUNCTION) fn->u.fn.home = vobj(sp[-2]);
        sp[-2] = sp[-1];
        sp--;
        pc++;
        NEXT;
    }
    CASE(TEMPLATE) pc += 5; NEXT;
    CASE(REGEXP)
        SAVE();
        v = regexp_op(rt, K[rd32(pc + 1)], K[rd32(pc + 5)]);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 9;
        NEXT;
    CASE(FOR_IN_INIT)
        SAVE();
        if (forin_slot_init(rt, &loc[rd16(pc + 1)], sp[-1]) < 0) goto exception;
        sp--;
        pc += 3;
        NEXT;
    CASE(FOR_IN_NEXT)
        SAVE();
        v = for_in_next(rt, loc[rd16(pc + 1)]);
        if (v == JS_EXC) goto exception;
        if (v == JS_HOLE) {
            pc += 7 + (int32_t)rd32(pc + 3);
        } else {
            *sp++ = v;
            pc += 7;
        }
        POLL();
        NEXT;
    CASE(ITER_INIT)
        SAVE();
        rt->coro_status = 0;
        if (rd32(pc + 4) != 0xFFFFFFFFu && iter_precheck(rt, sp[-1], K[rd32(pc + 4)], false) < 0) goto exception;
        if (iter_init_op(rt, &loc[rd16(pc + 1)], sp[-1], pc[3]) < 0) goto exception;
        sp--;
        pc += 8;
        NEXT;
    CASE(ITER_NEXT) {
        SAVE();
        int ir = iter_next_op(rt, &loc[rd16(pc + 1)], &v);
        if (ir < 0) goto exception;
        if (ir == 0) {
            loc[rd16(pc + 1) + 1] = JS_UNDEF;
            pc += 7 + (int32_t)rd32(pc + 3);
        } else {
            *sp++ = v;
            pc += 7;
        }
        POLL();
        NEXT;
    }
    CASE(ITER_CALL_NEXT) {
        SAVE();
        jsval *sl = &loc[rd16(pc + 1)];
        v = call_fn(rt, sl[1], sl[0], 0, 0);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 3;
        POLL();
        NEXT;
    }
    CASE(ITER_RES) {
        jsval res = sp[-1];
        SAVE();
        if (!is_obj(res)) {
            throw_error(rt, E_TYPE, "Iterator result %S is not an object", val_brief(rt, res));
            goto exception;
        }
        jsval d = get_p(rt, res, ATOM(done));
        if (d == JS_EXC) goto exception;
        if (truthy(d)) {
            sp--;
            pc += 5 + (int32_t)rd32(pc + 1);
            NEXT;
        }
        v = get_p(rt, res, ATOM(value));
        if (v == JS_EXC) goto exception;
        sp[-1] = v;
        pc += 5;
        NEXT;
    }
    CASE(ITER_CLOSE) {
        SAVE();
        jsval *sl = &loc[rd16(pc + 1)];
        if (!is_undef(sl[1])) {
            sl[1] = JS_UNDEF;
            iter_close(rt, sl[0], false);
            if (rt->has_exc) goto exception;
        }
        pc += 3;
        NEXT;
    }
    CASE(ITER_CLOSE_QUIET) {
        SAVE();
        jsval *sl = &loc[rd16(pc + 1)];
        if (!is_undef(sl[1])) {
            sl[1] = JS_UNDEF;
            if (!rt->terminating) {
                rt->sp = sp;
                iter_close(rt, sl[0], true);
            }
        }
        pc += 3;
        NEXT;
    }
    CASE(ITER_CLOSE_IF) {
        jsval *sl = &loc[rd16(pc + 1)];
        if (!is_undef(sl[1])) {
            SAVE();
            sl[1] = JS_UNDEF;
            iter_close(rt, sl[0], false);
            if (rt->has_exc) goto exception;
        }
        pc += 3;
        NEXT;
    }
    CASE(ITER_VALUE)
        SAVE();
        v = iter_value_op(rt, &loc[rd16(pc + 1)]);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 3;
        NEXT;
    CASE(ITER_REST)
        SAVE();
        v = iter_rest_op(rt, &loc[rd16(pc + 1)]);
        if (v == JS_EXC) goto exception;
        *sp++ = v;
        pc += 3;
        NEXT;
    CASE(MAP_ARGS) {
        jsval av = loc[f->fp->args_slot];
        int pi = pc[1];
        if (is_obj(av) && pi < (int)vobj(av)->u.arr.len) {
            JSObject *ao = vobj(av);
            if (!ao->u.arr.pmap) {
                int n = f->fp->nparams;
                ao->u.arr.pmap = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(n + 1));
                if (ao->u.arr.pmap) {
                    ao->u.arr.pmap[0] = (uint16_t)n;
                    for (int i = 0; i < n; i++) ao->u.arr.pmap[1 + i] = 0xFFFF;
                }
                ao->u.arr.menv = f->env;
            }
            if (ao->u.arr.pmap && pi < ao->u.arr.pmap[0]) ao->u.arr.pmap[1 + pi] = rd16(pc + 2);
        }
        pc += 4;
        NEXT;
    }
    CASE(REQ_COERCIBLE)
        if (is_nullish(sp[-1])) {
            SAVE();
            JSString *vb = val_brief(rt, sp[-1]);
            uint32_t ni = rd32(pc + 1), di = rd32(pc + 5);
            JSString *ds = di != 0xFFFFFFFFu ? vstr(K[di]) : vb;
            if (ni != 0xFFFFFFFFu && pc[9])
                throw_error(rt, E_TYPE, "Cannot read properties of %S (reading '%S')", vb, vstr(K[ni]));
            else if (ni != 0xFFFFFFFFu)
                throw_error(rt, E_TYPE, "Cannot destructure property '%S' of '%S' as it is %S.", vstr(K[ni]), ds, vb);
            else
                throw_error(rt, E_TYPE, "Cannot destructure '%S' as it is %S.", ds, vb);
            goto exception;
        }
        pc += 10;
        NEXT;

    CASE(INITIAL_YIELD) {
        pc++;
        SAVE();
        int kind = (f->fp->flags & FP_ASYNC) ? 2 : 0;
        JSObject *g = make_generator(rt, f, sp, pc, kind);
        v = mkobj(g);
        jsval *dst = f->argv - 2;
        if (f->flags & F_STOP) {
            rt->nframes--;
            rt->sp = dst;
            return v;
        }
        rt->nframes--;
        f = &rt->frames[rt->nframes - 1];
        LOAD();
        sp = dst;
        *sp++ = v;
        NEXT;
    }
    CASE(YIELD)
    CASE(YIELD_RAW) {
        bool raw = *pc == OP_YIELD_RAW;
        v = *--sp;
        pc++;
        SAVE();
        JSObject *co = f->coro;
        CoroData *cd = (CoroData *)co->u.data.p;
        jsval *dst = f->argv - 2;
        save_coro(rt, cd, f, sp, pc);
        cd->state = 1;
        cd->sp_off = raw ? 1 : 0; /* delegate flag */
        rt->nframes--;
        rt->sp = dst;
        rt->coro_status = 1;
        return v;
    }
    CASE(AWAIT) {
        v = *--sp;
        pc++;
        SAVE();
        JSObject *co = f->coro;
        CoroData *cd = (CoroData *)co->u.data.p;
        jsval *dst = f->argv - 2;
        bool stop = (f->flags & F_STOP) != 0;
        save_coro(rt, cd, f, sp, pc);
        cd->state = 4;
        cd->sp_off = 0;
        rt->nframes--;
        dst[0] = mkobj(co);
        dst[1] = v;
        rt->sp = dst + 2;
        await_setup(rt, co, v);
        if (stop) {
            rt->sp = dst;
            rt->coro_status = 2;
            return JS_UNDEF;
        }
        /* first await of an async function called from bytecode: return its promise */
        v = cd->promise;
        f = &rt->frames[rt->nframes - 1];
        LOAD();
        sp = dst;
        *sp++ = v;
        NEXT;
    }
    CASE(DELEGATE) {
        SAVE();
        int mode = (int)vnum(sp[-1]);
        jsval recv = sp[-2];
        int dr = delegate_op(rt, &loc[rd16(pc + 1)], recv, mode, &v);
        if (dr < 0) goto exception;
        sp -= 2;
        *sp++ = v;
        if (dr == 1)
            pc += 7 + (int32_t)rd32(pc + 3);
        else
            pc += 7;
        NEXT;
    }

exception:
    for (;;) {
        f = &rt->frames[rt->nframes - 1];
        while (rt->nhandlers > f->hbase) {
            Handler *h = &rt->handlers[--rt->nhandlers];
            if (rt->terminating) continue;
            if (h->kind == 0 && is_return_cell(rt->exc)) continue;
            loc = f->argv;
            sp = loc + h->sp_off;
            f->env = h->env;
            pc = f->fp->code + h->pc_off;
            K = f->fp->consts;
            *sp++ = rt->exc;
            rt->exc = JS_UNDEF;
            rt->has_exc = false;
            NEXT;
        }
        /* no handler in this frame */
        if (f->coro && !rt->terminating) {
            JSObject *co = f->coro;
            CoroData *cd = (CoroData *)co->u.data.p;
            if (is_return_cell(rt->exc)) {
                v = ((Cell *)vptr(rt->exc))->a;
                rt->exc = JS_UNDEF;
                rt->has_exc = false;
                loc = f->argv;
                sp = f->sp;
                goto do_return;
            }
            if (co->cls == CLS_ASYNC) {
                jsval e = rt->exc;
                rt->exc = JS_UNDEF;
                rt->has_exc = false;
                cd->state = 3;
                jsval *dst = f->argv - 2;
                dst[0] = e;
                rt->sp = dst + 1;
                promise_reject(rt, vobj(cd->promise), e);
                v = cd->promise;
                rt->nhandlers = f->hbase;
                rt->coro_status = 0;
                if (f->flags & F_STOP) {
                    rt->nframes--;
                    rt->sp = dst;
                    return v;
                }
                rt->nframes--;
                f = &rt->frames[rt->nframes - 1];
                LOAD();
                sp = dst;
                *sp++ = v;
                NEXT;
            }
            cd->state = 3;
        }
        rt->nhandlers = f->hbase;
        bool stop = (f->flags & F_STOP) != 0;
        jsval *dst = f->argv - 2;
        rt->nframes--;
        if (stop) {
            rt->sp = dst;
            return JS_EXC;
        }
        if (rt->nframes == 0) return JS_EXC;
    }
}

/* ---------------- coroutines ---------------- */
jsval coro_resume(js_rt *rt, JSObject *co, int mode, jsval v) {
    CoroData *cd = (CoroData *)co->u.data.p;
    if (cd->state == 2) return throw_error(rt, E_TYPE, "Generator is already running");
    if (cd->state == 3) {
        rt->coro_status = 0;
        if (mode == 1) return throw_val(rt, v);
        return mode == 2 ? v : JS_UNDEF;
    }
    if (cd->state == 0 && mode != 0) {
        cd->state = 3;
        rt->coro_status = 0;
        if (mode == 1) return throw_val(rt, v);
        return v;
    }
    if (++rt->rec_depth > JS_MAX_REC || check_stack(rt)) {
        rt->rec_depth--;
        return throw_error(rt, E_RANGE, "Maximum call stack size exceeded");
    }
    jsval *base = rt->sp;
    /* keep the coroutine object and value rooted below the frame */
    base[0] = mkobj(co);
    base[1] = v;
    rt->sp = base + 2;
    bool was_started = cd->state != 0;
    bool delegate = cd->sp_off == 1;
    Frame *f = restore_coro(rt, cd);
    if (!f) {
        rt->sp = base;
        rt->rec_depth--;
        return JS_EXC;
    }
    cd->state = 2;
    bool throwing = false;
    if (was_started) {
        if (delegate) {
            *f->sp++ = v;
            *f->sp++ = mknum(mode);
        } else if (mode == 0)
            *f->sp++ = v;
        else if (mode == 1) {
            rt->exc = v;
            rt->has_exc = true;
            throwing = true;
        } else {
            rt->exc = make_return_cell(rt, v);
            rt->has_exc = true;
            throwing = true;
        }
    }
    rt->sp = f->sp;
    rt->coro_status = 0;
    jsval r = run(rt, throwing);
    if (cd->state == 2) cd->state = 3;
    rt->sp = base;
    rt->rec_depth--;
    if (r == JS_EXC) {
        cd->state = 3;
        rt->coro_status = 0;
    }
    return r;
}

/* ---------------- scripts ---------------- */
jsval run_script(js_rt *rt, FuncProto *fp) {
    JSObject *fn = make_closure(rt, fp, 0);
    jsval *base = rt->sp;
    base[0] = mkobj(fn);
    base[1] = mkobj(rt->global);
    rt->sp = base + 2;
    if (setup_frame(rt, fn, base + 2, 0, JS_UNDEF, F_STOP) < 0) {
        rt->sp = base;
        return JS_EXC;
    }
    jsval r = run(rt, false);
    rt->sp = base;
    return r;
}

jsval eval_code(js_rt *rt, JSString *src, const char *file, bool is_fn_ctor) {
    size_t n;
    char *u = str_to_utf8(rt, src, &n);
    if (!u) return throw_error(rt, E_RANGE, "out of memory");
    JSString *err = 0;
    FuncProto *fp = compile_script(rt, u, n, file, 0, &err);
    free(u);
    if (!fp) {
        return throw_val(rt, mkobj(make_error(rt, rt->compile_errtype == E_RANGE ? E_RANGE : E_SYNTAX, err)));
    }
    (void)is_fn_ctor;
    return run_script(rt, fp);
}

/* ---------------- jobs ---------------- */
void enqueue_job(js_rt *rt, int kind, jsval fn, jsval a, jsval b, jsval c) {
    if ((rt->jtail + 1) % rt->jcap == rt->jhead) {
        uint32_t nc = rt->jcap * 2;
        Job *n = (Job *)malloc(nc * sizeof(Job));
        if (!n) return;
        uint32_t k = 0;
        for (uint32_t i = rt->jhead; i != rt->jtail; i = (i + 1) % rt->jcap) n[k++] = rt->jobs[i];
        free(rt->jobs);
        rt->jobs = n;
        rt->jhead = 0;
        rt->jtail = k;
        rt->jcap = nc;
    }
    Job *j = &rt->jobs[rt->jtail];
    j->kind = kind;
    j->fn = fn;
    j->a = a;
    j->b = b;
    j->c = c;
    rt->jtail = (rt->jtail + 1) % rt->jcap;
}

int run_jobs(js_rt *rt) {
    int n = 0;
    while (rt->jhead != rt->jtail) {
        Job j = rt->jobs[rt->jhead];
        rt->jhead = (rt->jhead + 1) % rt->jcap;
        jsval *root = rt->sp;
        push_tmp(rt, j.fn);
        push_tmp(rt, j.a);
        push_tmp(rt, j.b);
        push_tmp(rt, j.c);
        job_trampoline(rt, &j);
        rt->sp = root;
        n++;
        if (rt->terminating) break;
        gc_maybe(rt);
    }
    return n;
}

void report_exception(js_rt *rt, const char *prefix) {
    jsval e = rt->exc;
    rt->exc = JS_UNDEF;
    rt->has_exc = false;
    if (!rt->host.console) return;
    jsval *root = push_tmp(rt, e);
    JSString *s = 0;
    if (is_obj(e) && vobj(e)->cls == CLS_ERROR) {
        jsval st = get_p(rt, e, ATOM(stack));
        if (st != JS_EXC && is_str(st)) s = vstr(st);
    }
    if (rt->has_exc) {
        rt->has_exc = false;
        rt->exc = JS_UNDEF;
    }
    if (!s) s = inspect_value(rt, e, 1);
    if (rt->has_exc) {
        rt->has_exc = false;
        rt->exc = JS_UNDEF;
        s = str_cstr(rt, "exception");
    }
    SB b;
    sb_init(&b, rt);
    sb_puts(&b, prefix);
    if (s) sb_putstr(&b, s);
    JSString *m = sb_finish(&b);
    char *u = m ? str_to_utf8(rt, m, 0) : 0;
    if (u) {
        rt->host.console(rt->host.ctx, 3, u);
        free(u);
    }
    rt->sp = root;
}
