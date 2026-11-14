/* Host-side driver for user/lib/img.
 *   test_img FILE            decode, print "w h fnv1a-hash" (or "ERR n msg")
 *   test_img FILE OUT.rgba   also dump raw RGBA bytes
 *   test_img batch           binary request/response protocol on stdin/stdout (used by test_img.py)
 * Request:  op(1) arg(u32) len(u32) data[len]
 * Response: status(i64) extra(i64) n(u32) data[n]
 * Ops: D decode, P probe, Z zlib, G gunzip, R inflate_raw (arg = outmax), S gzip_size,
 *      E/e png_encode RGBA/opaque (arg = width, data = LE 0xAARRGGBB words) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../user/lib/img/img.h"

static long live;
static void *t_alloc(void *c, size_t n) { (void)c; void *p = malloc(n); if (p) live++; return p; }
static void t_free(void *c, void *p) { (void)c; if (p) { live--; free(p); } }
static const struct img_alloc A = {0, t_alloc, t_free};

static void to_rgba(const struct image *im, uint8_t *o) {
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        uint32_t p = im->px[i];
        o[i * 4] = p >> 16; o[i * 4 + 1] = p >> 8; o[i * 4 + 2] = p; o[i * 4 + 3] = p >> 24;
    }
}

static int rd(void *b, size_t n) { return fread(b, 1, n, stdin) == n; }
static void resp(int64_t st, int64_t ex, const void *d, uint32_t n) {
    fwrite(&st, 8, 1, stdout); fwrite(&ex, 8, 1, stdout); fwrite(&n, 4, 1, stdout);
    if (n) fwrite(d, 1, n, stdout);
    fflush(stdout);
}

static int batch(void) {
    for (;;) {
        uint8_t op;
        uint32_t arg, len;
        if (!rd(&op, 1)) return 0;
        if (!rd(&arg, 4) || !rd(&len, 4)) return 1;
        uint8_t *d = malloc(len ? len : 1);
        if (!rd(d, len)) return 1;
        long before = live;
        if (op == 'D') {
            struct image im;
            int r = img_decode(d, len, &im, &A);
            if (r == 0) {
                size_t n = (size_t)im.w * im.h * 4;
                uint8_t *o = malloc(n + 8);
                memcpy(o, &im.w, 4); memcpy(o + 4, &im.h, 4);
                to_rgba(&im, o + 8);
                t_free(0, im.px);
                resp(live != before ? -1000 : 0, 0, o, (uint32_t)(n + 8));
                free(o);
            } else {
                if (im.px || im.w || im.h) r = -1001;
                resp(live != before ? -1000 : r, 0, 0, 0);
            }
        } else if (op == 'P') {
            int w = 0, h = 0;
            int r = img_probe(d, len, &w, &h);
            resp(r, (int64_t)w << 32 | (uint32_t)h, 0, 0);
        } else if (op == 'Z' || op == 'G' || op == 'R') {
            uint8_t *o = malloc(arg ? arg : 1);
            size_t used = 0;
            long r = op == 'Z' ? zlib_decompress(d, len, o, arg, &A)
                   : op == 'G' ? gunzip(d, len, o, arg, &A)
                               : inflate_raw(d, len, o, arg, &used, &A);
            if (live != before) r = -1000;
            resp(r, (int64_t)used, o, r > 0 ? (uint32_t)r : 0);
            free(o);
        } else if (op == 'S') {
            resp(gzip_size(d, len), 0, 0, 0);
        } else if (op == 'E' || op == 'e') {
            int w = (int)arg, h = w ? (int)(len / 4 / w) : 0;
            size_t cap = png_encode_bound(w, h);
            uint8_t *o = malloc(cap ? cap : 1);
            long r = png_encode((const uint32_t *)d, w, h, op == 'e', o, cap);
            resp(r, (int64_t)cap, o, r > 0 ? (uint32_t)r : 0);
            free(o);
        } else {
            resp(-999, 0, 0, 0);
        }
        free(d);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "batch")) return batch();
    if (argc < 2) { fprintf(stderr, "usage: %s FILE [OUT.rgba] | batch\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n ? n : 1);
    if (fread(d, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    struct image im;
    int r = img_decode(d, n, &im, &A);
    if (r) { printf("ERR %d %s\n", r, img_strerror(r)); return 1; }
    uint8_t *o = malloc((size_t)im.w * im.h * 4);
    to_rgba(&im, o);
    uint64_t hsh = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)im.w * im.h * 4; i++) hsh = (hsh ^ o[i]) * 1099511628211ull;
    printf("%d %d %016llx\n", im.w, im.h, (unsigned long long)hsh);
    if (argc >= 3) {
        FILE *g = fopen(argv[2], "wb");
        fwrite(o, 1, (size_t)im.w * im.h * 4, g);
        fclose(g);
    }
    t_free(0, im.px);
    free(o);
    free(d);
    return live != 0;
}
