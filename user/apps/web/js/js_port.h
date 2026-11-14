/* AXIOM JavaScript engine: all platform dependencies live here.
 * Target (default): freestanding; only memset memcpy memmove memcmp strlen strcmp strncmp
 * malloc free realloc calloc are used. Host test build: -DJS_HOST_TEST uses the normal libc. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef JS_HOST_TEST
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
typedef jmp_buf js_jmp_buf;
#define JS_SETJMP(b) setjmp(b)
#define JS_LONGJMP(b) longjmp(b, 1)
/* sanitizers make C frames much bigger */
#ifndef JS_C_STACK_LIMIT
#define JS_C_STACK_LIMIT (1536 * 1024)
#endif
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void *malloc(size_t n);
void free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t m);
typedef void *js_jmp_buf[5];
#define JS_SETJMP(b) __builtin_setjmp(b)
#define JS_LONGJMP(b) __builtin_longjmp(b, 1)
#endif

/* how much C stack (bytes, measured from the outermost API call) the engine may use before it
 * throws RangeError / SyntaxError instead of recursing further. The AXIOM user stack is 256 KiB. */
#ifndef JS_C_STACK_LIMIT
#define JS_C_STACK_LIMIT (192 * 1024)
#endif

static inline double js_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
