/* Host test driver for the AAC decoder (user/lib/media/aac*.c). Build with -DMEDIA_HOST_TEST.
 *   aac_test decode in out.raw [f32|s16] [all]   decode an MP4/M4A (first AAC track) or ADTS file; "all": every
 *                                                decoded channel, no downmix. Prints rate / channels / frames.
 *   aac_test bench in reps                       decode reps times, print x real time
 *   aac_test fuzz in seed iters                  decode mutated copies of every access unit (run under ASan)
 *   aac_test frames in                           print offset and size of every access unit
 *   aac_test seek in out.raw ms                  MP4: decode from the sample at ms (after aac_reset), s16 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../user/lib/media/aac_int.h"
#include "../../user/lib/media/mp4.h"

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

/* access units */
struct au { long off; int len; };
static struct au *aus;
static long naus;
static const uint8_t *cfg;
static int cfg_len;
static mp4 *M;
static int track = -1;

static void add_au(long off, int len) {
    static long cap;
    if (naus == cap) { cap = cap ? cap * 2 : 1024; aus = realloc(aus, cap * sizeof *aus); }
    aus[naus].off = off;
    aus[naus].len = len;
    naus++;
}

static void split(const uint8_t *d, long len) {
    M = mp4_open(d, len);
    if (M) {
        track = mp4_find(M, MP4_AUDIO);
        if (track < 0) { fprintf(stderr, "no audio track\n"); exit(1); }
        const struct mp4_track *t = mp4_track(M, track);
        cfg = t->config;
        cfg_len = t->config_len;
        struct mp4_sample s;
        for (long i = 0; mp4_sample(M, track, i, &s); i++) add_au(s.offset, (int)s.size);
        return;
    }
    long p = 0;
    while (p + 7 <= len) {
        if (d[p] != 0xff || (d[p + 1] & 0xf6) != 0xf0) { p++; continue; }
        int fl = (d[p + 3] & 3) << 11 | d[p + 4] << 3 | d[p + 5] >> 5;
        if (fl < 7) { p++; continue; }
        if (p + fl > len) fl = (int)(len - p);
        add_au(p, fl);
        p += fl;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    const char *cmd = argv[1];
    long len;
    uint8_t *data = load(argv[2], &len);
    split(data, len);
    static float fbuf[AAC_MAX_SAMPLES * AAC_MAX_CH];
    static int16_t sbuf[AAC_MAX_SAMPLES * AAC_MAX_CH];
    if (!strcmp(cmd, "elements")) {   /* per frame: offset len then type:endbit ... */
        aacdec *d = aac_open(cfg, cfg_len);
        if (!d) return 1;
        for (long i = 0; i < naus; i++) {
            aac_decode(d, data + aus[i].off, aus[i].len, sbuf);
            printf("%ld %d", aus[i].off, aus[i].len);
            for (int k = 0; k < d->dbg_n; k++) printf(" %d:%d", d->dbg_type[k], d->dbg_end[k]);
            printf("\n");
        }
        return 0;
    }
    if (!strcmp(cmd, "frames")) {
        for (long i = 0; i < naus; i++) printf("%ld %d\n", aus[i].off, aus[i].len);
        return 0;
    }
    if (!strcmp(cmd, "decode") || !strcmp(cmd, "seek")) {
        int s16 = argc > 4 && !strcmp(argv[4], "s16");
        int all = 0, strip = 0;
        static uint8_t ascbuf[64];
        for (int i = 5; i < argc; i++) {
            if (!strcmp(argv[i], "all")) all = 1;
            if (!strncmp(argv[i], "asc=", 4)) {   /* ADTS input decoded with this AudioSpecificConfig instead */
                const char *h = argv[i] + 4;
                cfg_len = 0;
                while (h[0] && h[1] && cfg_len < 64) { unsigned v; sscanf(h, "%2x", &v); ascbuf[cfg_len++] = (uint8_t)v; h += 2; }
                cfg = ascbuf;
                strip = 1;
            }
        }
        if (strip)
            for (long i = 0; i < naus; i++) {
                int hl = (data[aus[i].off + 1] & 1) ? 7 : 9;
                aus[i].off += hl;
                aus[i].len -= hl;
            }
        aacdec *d = aac_open(cfg, cfg_len);
        if (!d) { printf("open failed\n"); return 1; }
        d->all_channels = all;
        FILE *o = fopen(argv[3], "wb");
        long frames = 0, samples = 0, start = 0;
        if (!strcmp(cmd, "seek")) {
            s16 = 1;
            long ms = atol(argv[4]);
            if (!M) return 1;
            start = mp4_sample_at(M, track, ms, 0);
            /* one access unit of pre-roll (the overlap) */
            if (start > 0) start--;
            aac_reset(d);
        }
        int rate = 0, ch = 0;
        for (long i = start; i < naus; i++) {
            int n = s16 ? aac_decode(d, data + aus[i].off, aus[i].len, sbuf)
                        : aac_decode_float(d, data + aus[i].off, aus[i].len, fbuf);
            if (n < 0) { printf("error at frame %ld\n", i); continue; }
            int nc = all ? d->nch : aac_channels(d);
            if (i == start) { rate = aac_sample_rate(d); ch = nc; }
            if (nc != ch) { printf("channel count changed at frame %ld: %d -> %d\n", i, ch, nc); ch = nc; }
            if (s16) fwrite(sbuf, 2, (size_t)n * nc, o);
            else fwrite(fbuf, 4, (size_t)n * nc, o);
            frames++;
            samples += n;
        }
        fclose(o);
        int aot, sbr, ps;
        aac_info(d, &aot, &sbr, &ps);
        printf("rate %d channels %d frames %ld samples %ld aot %d sbr %d ps %d first_rate %d first_ch %d\n",
               aac_sample_rate(d), all ? d->nch : aac_channels(d), frames, samples, aot, sbr, ps, rate, ch);
        aac_close(d);
    } else if (!strcmp(cmd, "bench")) {
        int reps = atoi(argv[3]);
        double t0 = now();
        long samples = 0;
        int rate = 0;
        for (int r = 0; r < reps; r++) {
            aacdec *d = aac_open(cfg, cfg_len);
            if (!d) return 1;
            for (long i = 0; i < naus; i++) {
                int n = aac_decode(d, data + aus[i].off, aus[i].len, sbuf);
                if (n > 0) samples += n;
            }
            rate = aac_sample_rate(d);
            aac_close(d);
        }
        double t = now() - t0;
        printf("bench %.1f x realtime (%.3f s for %.1f s of audio)\n", samples / (double)rate / t, t, samples / (double)rate);
    } else if (!strcmp(cmd, "fuzz")) {
        srand((unsigned)atoi(argv[3]));
        int iters = atoi(argv[4]);
        long total = 0;
        for (int it = 0; it < iters; it++) {
            aacdec *d = aac_open(cfg, cfg_len);
            if (!d) return 1;
            for (long i = 0; i < naus; i++) {
                int l = aus[i].len;
                if (rand() % 8 == 0) l = rand() % (l + 1);
                uint8_t *buf = malloc(l ? l : 1);
                memcpy(buf, data + aus[i].off, l);
                int nm = rand() % 4;
                for (int k = 0; k < nm && l > 0; k++) {
                    int pos = rand() % l;
                    if (rand() & 1) buf[pos] ^= 1 << (rand() % 8);
                    else buf[pos] = (uint8_t)rand();
                }
                int n = aac_decode(d, buf, l, sbuf);
                if (n > AAC_MAX_SAMPLES) { fprintf(stderr, "too many samples\n"); abort(); }
                if (n > 0) total += n;
                free(buf);
                if (rand() % 50 == 0) aac_reset(d);
            }
            aac_close(d);
        }
        /* random configs */
        for (int it = 0; it < iters * 20; it++) {
            uint8_t c[8];
            for (int k = 0; k < 8; k++) c[k] = (uint8_t)rand();
            aacdec *d = aac_open(c, 1 + rand() % 8);
            if (!d) continue;
            for (long i = 0; i < naus && i < 5; i++) aac_decode(d, data + aus[i].off, aus[i].len, sbuf);
            aac_close(d);
        }
        printf("fuzz ok %ld samples\n", total);
    }
    free(data);
    return 0;
}
