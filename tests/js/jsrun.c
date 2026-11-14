/* Host test driver for the AXIOM JavaScript engine: jsrun file.js
 * console.log/info -> stdout, console.warn/error -> stderr. Exit code 1 on uncaught error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "../../user/apps/web/js/js.h"

static uint64_t now_ms(void *ctx) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    (void)ctx;
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
static uint64_t mono_ms(void *ctx) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    (void)ctx;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static uint64_t deadline;
static bool interrupt(void *ctx) { return deadline && mono_ms(ctx) > deadline; }
static void console_out(void *ctx, int level, const char *s) {
    (void)ctx;
    FILE *f = level >= 2 ? stderr : stdout;
    fputs(s, f);
    fputc('\n', f);
}
static void rnd(void *ctx, void *buf, int n) {
    (void)ctx;
    unsigned char *b = buf;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)rand();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: jsrun file.js [more.js ...]\n");
        return 2;
    }
    const char *t = getenv("JSRUN_TIMEOUT_MS");
    srand((unsigned)time(0));
    struct js_host host = {0, now_ms, mono_ms, interrupt, console_out, rnd};
    js_rt *rt = js_new(&host);
    int rc = 0;
    for (int a = 1; a < argc && !rc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            perror(argv[a]);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)n + 1);
        if (fread(src, 1, (size_t)n, f) != (size_t)n) n = 0;
        src[n] = 0;
        fclose(f);
        if (t) deadline = mono_ms(0) + (uint64_t)atol(t);
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        jsval r;
        if (js_eval(rt, src, n, name, &r) < 0) {
            fflush(stdout);
            fprintf(stderr, "Uncaught %s\n", js_error_text(rt));
            rc = 1;
        }
        while (js_run_jobs(rt) > 0) {}
        free(src);
    }
    fflush(stdout);
    if (getenv("JSRUN_GC_STATS")) {
        js_gc(rt);
        fprintf(stderr, "heap after gc: %ld bytes\n", js_heap_bytes(rt));
    }
    js_free(rt);
    return rc;
}
