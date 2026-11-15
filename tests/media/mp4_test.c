/* Host test driver for user/lib/media/mp4.c. Build with -DMEDIA_HOST_TEST.
 *   mp4_test dump file             print tracks and every sample
 *   mp4_test seek file track ms... print mp4_sample_at(ms, 0) and (ms, 1) for each ms
 *   mp4_test fuzz file seed iters  open randomly mutated / truncated copies and walk all samples (run under ASan) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static unsigned long walk(mp4 *m, const uint8_t *d, long len) {
    unsigned long h = 0;
    for (int t = 0; t < mp4_ntracks(m); t++) {
        const struct mp4_track *tr = mp4_track(m, t);
        if (tr->config && tr->config_len) h += tr->config[0] + tr->config[tr->config_len - 1];
        struct mp4_sample s;
        for (long i = 0; mp4_sample(m, t, i, &s); i++) {
            if (s.size) h += d[s.offset] + d[s.offset + s.size - 1];
            if (s.offset < 0 || s.offset + (long)s.size > len) { fprintf(stderr, "sample outside file\n"); abort(); }
        }
        mp4_sample_at(m, t, 1234, 1);
        mp4_sample_at(m, t, 0, 0);
    }
    mp4_find(m, MP4_VIDEO);
    mp4_find(m, MP4_AUDIO);
    mp4_duration_ms(m);
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    long len;
    uint8_t *data = load(argv[2], &len);
    const char *cmd = argv[1];
    if (!strcmp(cmd, "dump")) {
        mp4 *m = mp4_open(data, len);
        if (!m) { printf("open failed\n"); return 1; }
        printf("ntracks %d duration_ms %ld find_video %d find_audio %d\n", mp4_ntracks(m), mp4_duration_ms(m),
               mp4_find(m, MP4_VIDEO), mp4_find(m, MP4_AUDIO));
        for (int t = 0; t < mp4_ntracks(m); t++) {
            const struct mp4_track *tr = mp4_track(m, t);
            printf("track %d type %d codec %s w %d h %d dw %d dh %d rate %d ch %d oti %d ts %u dur %lld n %ld cfg %d id %u shift %lld priming %lld cfghex ",
                   t, tr->type, tr->codec, tr->width, tr->height, tr->disp_w, tr->disp_h, tr->sample_rate, tr->channels,
                   tr->object_type, tr->timescale, (long long)tr->duration, tr->nsamples, tr->config_len,
                   mp4_track_id(m, t), (long long)mp4_track_shift(m, t), (long long)mp4_priming(m, t));
            for (int i = 0; i < tr->config_len && i < 64; i++) printf("%02x", tr->config[i]);
            printf("\n");
            struct mp4_sample s;
            for (long i = 0; mp4_sample(m, t, i, &s); i++)
                printf("s %d %lld %u %lld %lld %d\n", t, (long long)s.offset, s.size, (long long)s.dts, (long long)s.pts, s.keyframe);
        }
        mp4_close(m);
    } else if (!strcmp(cmd, "seek")) {
        mp4 *m = mp4_open(data, len);
        if (!m) { printf("open failed\n"); return 1; }
        int t = atoi(argv[3]);
        for (int i = 4; i < argc; i++) {
            long ms = atol(argv[i]);
            printf("seek %ld %ld %ld\n", ms, mp4_sample_at(m, t, ms, 0), mp4_sample_at(m, t, ms, 1));
        }
        mp4_close(m);
    } else if (!strcmp(cmd, "fuzz")) {
        unsigned seed = (unsigned)atoi(argv[3]);
        int iters = atoi(argv[4]);
        srand(seed);
        uint8_t *buf = malloc(len + 1);
        int opened = 0;
        unsigned long h = 0;
        /* header regions (moov / moof boxes) get most of the mutations */
        long hdr[64][2]; int nh = 0;
        for (long i = 4; i + 4 <= len && nh < 64; i++)
            if (!memcmp(data + i, "moov", 4) || !memcmp(data + i, "moof", 4)) {
                long sz = (long)data[i - 4] << 24 | data[i - 3] << 16 | data[i - 2] << 8 | data[i - 1];
                if (sz >= 8 && i - 4 + sz <= len) { hdr[nh][0] = i - 4; hdr[nh][1] = sz; nh++; }
            }
        for (int it = 0; it < iters; it++) {
            memcpy(buf, data, len);
            long l = len;
            int mode = rand() % 4;
            if (mode == 0) l = rand() % (len + 1);
            int nmut = 1 + rand() % 20;
            for (int k = 0; k < nmut && l > 0; k++) {
                long pos = rand() % l;
                if (nh && rand() % 5) {
                    int h = rand() % nh;
                    pos = hdr[h][0] + rand() % hdr[h][1];
                    if (pos >= l) pos = rand() % l;
                }
                if (rand() % 3 == 0 && pos >= 4 && pos + 4 <= l) {  /* corrupt a box size or count near a header */
                    uint32_t v = rand() % 5 == 0 ? 0xffffffffu : (uint32_t)rand() % 0x100000u;
                    buf[pos] = v >> 24; buf[pos + 1] = v >> 16; buf[pos + 2] = v >> 8; buf[pos + 3] = v;
                } else buf[pos] = (uint8_t)rand();
            }
            /* copy to an exact-size allocation so ASan sees overreads */
            uint8_t *exact = malloc(l ? l : 1);
            memcpy(exact, buf, l);
            mp4 *m = mp4_open(exact, l);
            if (m) { opened++; h += walk(m, exact, l); mp4_close(m); }
            free(exact);
        }
        printf("fuzz iters %d opened %d hash %lu\n", iters, opened, h);
        free(buf);
    }
    free(data);
    return 0;
}
