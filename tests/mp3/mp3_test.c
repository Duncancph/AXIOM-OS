/* Host test driver for user/lib/audio (AXIOM MP3 decoder). Build with -DMP3_HOST_TEST.
 *   mp3_test decode in.mp3 out.raw        decode to s16le, prints stream info
 *   mp3_test bench in.mp3 reps            decode reps times, prints realtime factor
 *   mp3_test seek in.mp3 out.raw ms...    for each ms: seek, print reached ms/sample, append 8 frames of PCM
 *   mp3_test tags in.mp3                  print title / artist / album
 *   mp3_test fuzz in.mp3 seed iters       decode randomly mutated copies (run under ASan)
 *   mp3_test open in.mp3                  only print what mp3_open reports */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../user/lib/audio/mp3.h"

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

static void print_info(const struct mp3_info *i) {
    printf("info sample_rate=%d channels=%d frames_est=%ld duration_ms=%ld bitrate_kbps=%d layer=%d\n",
           i->sample_rate, i->channels, i->frames_est, i->duration_ms, i->bitrate_kbps, i->layer);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    long len;
    uint8_t *data = load(argv[2], &len);
    const char *cmd = argv[1];
    static int16_t pcm[MP3_MAX_SAMPLES_PER_FRAME * 2];

    if (!strcmp(cmd, "tags")) {
        char t[256], a[256], al[256];
        mp3_tags(data, len, t, a, al, sizeof t);
        printf("title=%s\nartist=%s\nalbum=%s\n", t, a, al);
        /* small buffers must truncate on UTF-8 boundaries */
        char s[6];
        mp3_tags(data, len, s, 0, 0, sizeof s);
        printf("short=%s\n", s);
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "fuzz")) {
        unsigned seed = (unsigned)atoi(argv[3]);
        int iters = atoi(argv[4]);
        uint8_t *buf = malloc(len + 1);
        long total = 0;
        for (int it = 0; it < iters; it++) {
            srand(seed + it);
            memcpy(buf, data, len);
            long n = len;
            int muts = 1 + rand() % 20;
            for (int m = 0; m < muts && n > 0; m++) {
                int kind = rand() % 4;
                long at = rand() % n;
                if (kind == 0) buf[at] = rand();
                else if (kind == 1) buf[at] ^= 1 << (rand() % 8);
                else if (kind == 2) n = at + 1;   /* truncate */
                else { long l = rand() % 64; if (at + l < n) memset(buf + at, rand() & 1 ? 0xff : 0, l); }
            }
            struct mp3_info info;
            mp3dec *d = mp3_open(buf, n, &info);
            char t[64], a[64], al[64];
            mp3_tags(buf, n, t, a, al, 64);
            if (!d) continue;
            int r, frames = 0;
            while ((r = mp3_decode(d, pcm)) > 0 && frames < 20000) { total += r; frames++; }
            if (r < 0) { printf("decode error %d iter %d\n", r, it); }
            if (rand() % 2) {
                mp3_seek(d, rand() % (info.duration_ms + 1000));
                for (int k = 0; k < 5 && mp3_decode(d, pcm) > 0; k++) {}
            }
            mp3_close(d);
        }
        printf("fuzz ok samples=%ld\n", total);
        free(buf);
        free(data);
        return 0;
    }

    struct mp3_info info;
    mp3dec *d = mp3_open(data, len, &info);
    if (!d) { printf("open failed\n"); free(data); return 1; }
    if (!strcmp(cmd, "open")) {
        print_info(&info);
    } else if (!strcmp(cmd, "decode")) {
        FILE *o = fopen(argv[3], "wb");
        int r;
        long total = 0;
        while ((r = mp3_decode(d, pcm)) > 0) {
            fwrite(pcm, sizeof(int16_t) * info.channels, r, o);
            total += r;
        }
        fclose(o);
        print_info(&info);
        int dl, pd;
        mp3_gapless(d, &dl, &pd);
        printf("decoded samples=%ld last=%d pos_ms=%ld delay=%d padding=%d\n", total, r, mp3_position_ms(d), dl, pd);
    } else if (!strcmp(cmd, "bench")) {
        int reps = argc > 3 ? atoi(argv[3]) : 1;
        long total = 0;
        double t0 = now();
        for (int i = 0; i < reps; i++) {
            mp3_seek(d, 0);
            int r;
            while ((r = mp3_decode(d, pcm)) > 0) total += r;
        }
        double dt = now() - t0;
        double audio = (double)total / info.sample_rate;
        printf("bench audio_s=%.3f cpu_s=%.4f realtime=%.1f\n", audio, dt, audio / dt);
    } else if (!strcmp(cmd, "seek")) {
        FILE *o = fopen(argv[3], "wb");
        for (int i = 4; i < argc; i++) {
            long ms = atol(argv[i]);
            long got = mp3_seek(d, ms);
            long smp = mp3_position_samples(d);
            int n = 0, r;
            for (int k = 0; k < 8 && (r = mp3_decode(d, pcm)) > 0; k++) {
                fwrite(pcm, sizeof(int16_t) * info.channels, r, o);
                n += r;
            }
            printf("seek req=%ld got=%ld sample=%ld n=%d\n", ms, got, smp, n);
        }
        fclose(o);
    }
    mp3_close(d);
    free(data);
    return 0;
}
