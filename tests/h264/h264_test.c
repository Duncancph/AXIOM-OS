/* Host test driver for user/lib/video (AXIOM H.264 decoder). Build with -DH264_HOST_TEST.
 *   h264_test annexb in.h264 out.yuv [chunk]   decode an Annex B stream fed in chunks of `chunk` bytes
 *                                                (0 = one access unit per call, default), write yuv420p
 *   h264_test samples in.smp out.yuv            decode a sample file (avcC + length-prefixed samples, see run.py)
 *   h264_test seek in.smp out.yuv k             decode samples 0..k-1, h264_reset, then decode from sample k
 *   h264_test bench in.smp reps                 decode reps times without output, print fps
 *   h264_test fuzz in.smp seed iters            decode randomly damaged copies (run under ASan/UBSan)
 *   h264_test fuzzab in.h264 seed iters         same for an Annex B stream fed in random chunks
 *   h264_test info in.smp                       print frame count / size / pts list
 * Sample file: "H264SMP1", u32 avcC length, avcC, then per sample u32 length, i64 pts, u8 keyframe, data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../user/lib/video/h264.h"

static uint8_t *load(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len ? *len : 1);
    if (fread(p, 1, *len, f) != (size_t)*len) { perror("read"); exit(2); }
    fclose(f);
    return p;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct sample { const uint8_t *data; int len; int64_t pts; int key; };
struct smp { const uint8_t *avcc; int avcc_len; struct sample *s; int n; };

static int parse_smp(const uint8_t *p, long len, struct smp *m) {
    if (len < 12 || memcmp(p, "H264SMP1", 8)) return -1;
    long pos = 8;
    uint32_t al = p[pos] | p[pos + 1] << 8 | p[pos + 2] << 16 | (uint32_t)p[pos + 3] << 24;
    pos += 4;
    m->avcc = p + pos;
    m->avcc_len = (int)al;
    pos += al;
    m->s = 0; m->n = 0;
    int cap = 0;
    while (pos + 13 <= len) {
        uint32_t l = p[pos] | p[pos + 1] << 8 | p[pos + 2] << 16 | (uint32_t)p[pos + 3] << 24;
        int64_t pts = 0;
        for (int i = 7; i >= 0; i--) pts = pts << 8 | p[pos + 4 + i];
        int key = p[pos + 12];
        pos += 13;
        if (pos + l > (uint32_t)len) break;
        if (m->n == cap) { cap = cap ? cap * 2 : 256; m->s = realloc(m->s, cap * sizeof(struct sample)); }
        m->s[m->n].data = p + pos; m->s[m->n].len = (int)l; m->s[m->n].pts = pts; m->s[m->n].key = key;
        m->n++;
        pos += l;
    }
    return 0;
}

static long frames_out;
static void write_frame(FILE *o, const struct h264_frame *f) {
    frames_out++;
    if (!o) return;
    for (int y = 0; y < f->height; y++) fwrite(f->y + (long)y * f->stride_y, 1, f->width, o);
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    for (int y = 0; y < ch; y++) fwrite(f->u + (long)y * f->stride_uv, 1, cw, o);
    for (int y = 0; y < ch; y++) fwrite(f->v + (long)y * f->stride_uv, 1, cw, o);
}

static int drain(h264dec *d, FILE *o, int print_pts) {
    struct h264_frame f;
    int n = 0;
    while (h264_get_frame(d, &f)) {
        if (print_pts) printf("frame pts=%lld key=%d %dx%d range=%d matrix=%d\n", (long long)f.pts, f.keyframe, f.width, f.height, f.full_range, f.matrix);
        write_frame(o, &f);
        n++;
    }
    return n;
}

/* splits an Annex B stream at access unit boundaries (AUD / SPS / PPS / SEI / first slice with first_mb 0) */
static int next_au(const uint8_t *p, long len, long pos) {
    int seen_slice = 0;
    long i = pos;
    while (i + 3 < len) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int t = p[i + 3] & 31;
            if (seen_slice && (t == 9 || t == 7 || t == 8 || t == 6 || ((t == 1 || t == 5) && i + 4 < len && (p[i + 4] & 0x80)))) {
                long s = i;
                while (s > pos && p[s - 1] == 0) s--;
                return (int)(s - pos);
            }
            if (t == 1 || t == 5) seen_slice = 1;
            i += 3;
            continue;
        }
        i++;
    }
    return (int)(len - pos);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    long len;
    uint8_t *data = load(argv[2], &len);
    const char *mode = argv[1];
    if (!strcmp(mode, "annexb")) {
        FILE *o = argc > 3 ? fopen(argv[3], "wb") : 0;
        long chunk = argc > 4 ? atol(argv[4]) : 0;
        h264dec *d = h264_open(0, 0);
        long pos = 0;
        int64_t pts = 0;
        int errs = 0;
        while (pos < len) {
            long n = chunk > 0 ? (len - pos < chunk ? len - pos : chunk) : next_au(data, len, pos);
            if (n <= 0) n = len - pos;
            if (h264_decode(d, data + pos, (int)n, pts++) < 0) { if (!errs++) fprintf(stderr, "error: %s\n", h264_error(d)); }
            pos += n;
            drain(d, o, 0);
        }
        h264_flush(d);
        drain(d, o, 0);
        printf("frames %ld concealed_mbs %ld errors %d\n", frames_out, h264_concealed_mbs(d), errs);
        h264_close(d);
        if (o) fclose(o);
        free(data);
        return 0;
    }
    if (!strcmp(mode, "fuzzab")) {
        unsigned seed = (unsigned)atoi(argv[3]);
        int iters = atoi(argv[4]);
        srand(seed);
        uint8_t *buf = malloc(len + 1);
        for (int it = 0; it < iters; it++) {
            memcpy(buf, data, len);
            int nm = 1 + rand() % 40;
            for (int j = 0; j < nm && len > 0; j++) {
                long at = rand() % len;
                int kind = rand() % 4;
                if (kind == 0) buf[at] ^= 1 << (rand() % 8);
                else if (kind == 1) buf[at] = rand();
                else if (kind == 2) { int run = rand() % 200; for (long q = at; q < len && q < at + run; q++) buf[q] = rand(); }
                else { int run = rand() % 16; for (long q = at; q < len && q < at + run; q++) buf[q] = 0; }
            }
            long l = rand() % 4 ? len : rand() % (len + 1);
            h264dec *d = h264_open(0, 0);
            long pos = 0;
            while (pos < l) {
                long n = 1 + rand() % 3000;
                if (n > l - pos) n = l - pos;
                h264_decode(d, buf + pos, (int)n, pos);
                drain(d, 0, 0);
                pos += n;
            }
            h264_flush(d);
            drain(d, 0, 0);
            h264_close(d);
        }
        free(buf);
        free(data);
        printf("fuzz ok %d iterations\n", iters);
        return 0;
    }
    struct smp m;
    if (parse_smp(data, len, &m) < 0) { fprintf(stderr, "bad sample file\n"); return 2; }
    if (!strcmp(mode, "samples") || !strcmp(mode, "seek") || !strcmp(mode, "info")) {
        int info = !strcmp(mode, "info");
        FILE *o = (!info && argc > 3) ? fopen(argv[3], "wb") : 0;
        int k = !strcmp(mode, "seek") ? atoi(argv[4]) : -1;
        h264dec *d = h264_open(m.avcc, m.avcc_len);
        int errs = 0;
        for (int i = 0; i < m.n; i++) {
            if (i == k) {
                drain(d, 0, 0);
                h264_reset(d);
                frames_out = 0;
            }
            if (h264_decode(d, m.s[i].data, m.s[i].len, m.s[i].pts) < 0) { if (!errs++) fprintf(stderr, "error: %s\n", h264_error(d)); }
            drain(d, (k < 0 || i >= k) ? o : 0, info);
        }
        h264_flush(d);
        drain(d, o, info);
        int w, h, r;
        if (h264_info(d, &w, &h, &r)) printf("size %dx%d reorder %d\n", w, h, r);
        printf("frames %ld concealed_mbs %ld errors %d\n", frames_out, h264_concealed_mbs(d), errs);
        h264_close(d);
        if (o) fclose(o);
        free(m.s);
        free(data);
        return 0;
    }
    if (!strcmp(mode, "bench")) {
        int reps = argc > 3 ? atoi(argv[3]) : 3;
        double best = 1e9;
        long frames = 0;
        for (int r = 0; r < reps; r++) {
            frames_out = 0;
            double t0 = now();
            h264dec *d = h264_open(m.avcc, m.avcc_len);
            for (int i = 0; i < m.n; i++) { h264_decode(d, m.s[i].data, m.s[i].len, m.s[i].pts); drain(d, 0, 0); }
            h264_flush(d);
            drain(d, 0, 0);
            h264_close(d);
            double t = now() - t0;
            if (t < best) best = t;
            frames = frames_out;
        }
        printf("bench frames %ld best %.3f s fps %.1f\n", frames, best, frames / best);
        free(m.s);
        free(data);
        return 0;
    }
    if (!strcmp(mode, "fuzz")) {
        unsigned seed = (unsigned)atoi(argv[3]);
        int iters = atoi(argv[4]);
        srand(seed);
        for (int it = 0; it < iters; it++) {
            /* copy and damage: flip bits, overwrite runs, truncate samples, drop samples */
            int kind = rand() % 4;
            h264dec *d = h264_open(m.avcc, m.avcc_len);
            if (it % 7 == 6) { h264_close(d); d = h264_open(m.avcc, rand() % (m.avcc_len + 1)); }
            for (int i = 0; i < m.n; i++) {
                int l = m.s[i].len;
                uint8_t *buf = malloc(l + 1);
                memcpy(buf, m.s[i].data, l);
                if (rand() % 3 == 0 && l > 0) {
                    int nm = 1 + rand() % 8;
                    for (int j = 0; j < nm; j++) {
                        int at = rand() % l;
                        if (kind == 0) buf[at] ^= 1 << (rand() % 8);
                        else if (kind == 1) buf[at] = rand();
                        else if (kind == 2) { int run = rand() % 64; for (int q = at; q < l && q < at + run; q++) buf[q] = rand(); }
                        else { int run = rand() % 32; for (int q = at; q < l && q < at + run; q++) buf[q] = 0; }
                    }
                }
                if (rand() % 20 == 0) l = rand() % (l + 1);
                if (rand() % 50 == 0) { free(buf); continue; }
                h264_decode(d, buf, l, m.s[i].pts);
                drain(d, 0, 0);
                if (rand() % 100 == 0) h264_reset(d);
                free(buf);
            }
            h264_flush(d);
            drain(d, 0, 0);
            h264_close(d);
        }
        printf("fuzz ok %d iterations\n", iters);
        free(m.s);
        free(data);
        return 0;
    }
    fprintf(stderr, "unknown mode\n");
    return 2;
}
