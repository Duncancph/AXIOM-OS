/* AXIOM JavaScript engine: public interface (used by the Web app's DOM layer, user/apps/web/dom.c).
 * Values are NaN-boxed 64-bit words. Strings cross this interface as UTF-8. All functions taking a js_rt
 * may run the garbage collector unless noted; keep values you hold across calls in rooted slots (js_root). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct js_rt js_rt;
typedef uint64_t jsval;

/* ---- the host (the browser) ---- */
struct js_host {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);                        /* wall clock, ms since 1970 (Date.now) */
    uint64_t (*mono_ms)(void *ctx);                       /* monotonic ms (performance.now) */
    bool (*interrupt)(void *ctx);                         /* polled now and then: true stops the running script */
    void (*console)(void *ctx, int level, const char *utf8);   /* 0 log, 1 info, 2 warn, 3 error */
    void (*random)(void *ctx, void *buf, int n);          /* random bytes (Math.random, crypto) */
};

js_rt *js_new(const struct js_host *host);
void   js_free(js_rt *rt);
void   js_check_soon(js_rt *rt);   /* ask the host's interrupt hook at the next step (after slow native work) */

/* ---- running code ---- */
/* Run a script in the global scope. Returns 0, or -1 if it threw (see js_error_text). */
int  js_eval(js_rt *rt, const char *src, long len, const char *filename, jsval *result);
/* Call a function. Returns 0, or -1 if it threw. */
int  js_call(js_rt *rt, jsval fn, jsval this_, int argc, const jsval *argv, jsval *result);
/* Run pending promise reactions / microtasks; returns how many ran. Call after every task. */
int  js_run_jobs(js_rt *rt);
/* After -1: the exception as text ("TypeError: x is not a function (page.js:12)"). */
const char *js_error_text(js_rt *rt);
jsval js_exception(js_rt *rt);                           /* the thrown value itself */

/* ---- values ---- */
jsval js_undefined(void);
jsval js_null(void);
jsval js_bool(bool b);
jsval js_num(double d);
jsval js_int(int64_t i);
jsval js_str(js_rt *rt, const char *utf8, long len);    /* len < 0: strlen */
bool js_is_undefined(jsval v);
bool js_is_null(jsval v);
bool js_is_bool(jsval v);
bool js_is_number(jsval v);
bool js_is_string(jsval v);
bool js_is_object(jsval v);                               /* includes functions and arrays */
bool js_is_function(jsval v);
const uint8_t *js_bytes(js_rt *rt, jsval v, long *len);
jsval js_new_arraybuffer(js_rt *rt, const void *p, long n);   /* a copy of the bytes (undefined if out of memory) */   /* ArrayBuffer / typed array / DataView contents, 0 if not one */
bool js_is_array(jsval v);
bool js_truthy(jsval v);
double js_to_number(js_rt *rt, jsval v);                  /* ToNumber (may call valueOf) */
int64_t js_to_int(js_rt *rt, jsval v);
/* ToString into out as UTF-8 (NUL-terminated if it fits); returns the full length */
long js_to_string(js_rt *rt, jsval v, char *out, long max);
/* ToString as a newly allocated UTF-8 string (free() it) */
char *js_to_cstring(js_rt *rt, jsval v);
bool js_strict_equals(jsval a, jsval b);

/* ---- objects ---- */
jsval js_global(js_rt *rt);
jsval js_object(js_rt *rt);
jsval js_array(js_rt *rt);
jsval js_get(js_rt *rt, jsval obj, const char *key);                 /* runs getters; undefined if missing */
void  js_set(js_rt *rt, jsval obj, const char *key, jsval v);
jsval js_get_index(js_rt *rt, jsval obj, uint32_t i);
void  js_set_index(js_rt *rt, jsval obj, uint32_t i, jsval v);
long  js_length(js_rt *rt, jsval obj);                                 /* the "length" property as a number */
void  js_define(js_rt *rt, jsval obj, const char *key, jsval v, bool enumerable);   /* own data property, writable+configurable */
/* an accessor property implemented in C */
typedef jsval (*js_native)(js_rt *rt, jsval this_, int argc, jsval *argv, void *data);
void  js_define_accessor(js_rt *rt, jsval obj, const char *key, js_native getter, js_native setter, void *data);
jsval js_function(js_rt *rt, const char *name, js_native fn, int length, void *data);
/* call from a js_native to throw: return js_throw(...) */
jsval js_throw(js_rt *rt, const char *error_type, const char *message);   /* "TypeError", "Error", ... */
jsval js_throw_value(js_rt *rt, jsval v);
jsval js_new_error(js_rt *rt, const char *error_type, const char *message);
jsval js_get_prototype(js_rt *rt, jsval obj);
void  js_set_prototype(js_rt *rt, jsval obj, jsval proto);

/* ---- host objects: JS objects that wrap a C pointer (DOM nodes, events, ...) ----
 * They behave like ordinary objects (properties can be added); the class may also intercept
 * property access before the normal lookup. */
struct js_class {
    const char *name;                                     /* Object.prototype.toString shows [object <name>] */
    /* return true and set *out if handled (else the normal lookup continues) */
    bool (*get)(js_rt *rt, jsval obj, void *ptr, const char *key, jsval *out);
    /* return true if handled (else an ordinary own property is created/updated) */
    bool (*set)(js_rt *rt, jsval obj, void *ptr, const char *key, jsval v);
    /* called during GC marking: call js_mark for every jsval the C side keeps for this object */
    void (*trace)(js_rt *rt, void *ptr);
    void (*finalize)(void *ptr);                          /* the object was collected */
};
jsval js_host_object(js_rt *rt, const struct js_class *cls, void *ptr, jsval proto);
void *js_host_ptr(jsval obj, const struct js_class *cls);           /* 0 if obj isn't of that class */
void  js_mark(js_rt *rt, jsval v);                                  /* only inside a trace callback */

/* ---- keeping values alive between calls ---- */
void js_root(js_rt *rt, jsval *slot);                     /* the slot is scanned by the GC until unrooted */
void js_unroot(js_rt *rt, jsval *slot);
void js_gc(js_rt *rt);
long js_heap_bytes(js_rt *rt);

/* ---- additions (engine extensions; everything above is unchanged) ---- */
/* inside a js_native: true if it was invoked with `new` (this_ is then a fresh object whose
 * prototype is newTarget.prototype; returning an object replaces it) */
bool js_is_constructing(js_rt *rt);
/* true while an exception is pending (e.g. after js_get ran a throwing getter inside a native) */
bool js_has_exception(js_rt *rt);
/* `new ctor(...argv)`. Returns 0, or -1 if it threw. */
int  js_construct(js_rt *rt, jsval ctor, int argc, const jsval *argv, jsval *result);
