/* AXIOM MP3 decoder: internal declarations shared by the decoder sources. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mp3.h"

/* ---- port layer: the only external functions used are the ones below. */
#ifdef MP3_HOST_TEST
#include <stdlib.h>
#include <string.h>
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void free(void *p);
#endif

/* ---- frame header */
struct mp3_hdr {
    uint32_t raw;
    int lsf;          /* 0: MPEG-1, 1: MPEG-2 or MPEG-2.5 */
    int mpeg25;
    int layer;        /* 1..3 */
    int crc;          /* 1 if a 16-bit CRC follows the header */
    int br_index, sr_index, sr_table; /* sr_table: 0..8 = 44.1 48 32 22.05 24 16 11.025 12 8 kHz */
    int padding, mode, mode_ext;
    int channels;
    int sample_rate;
    int bitrate;      /* kbps, 0 = free format */
    int size;         /* frame size in bytes, including header (0 if free format and unknown) */
    int spf;          /* samples per frame */
};

/* Parses 4 header bytes; free_size is the stream's free-format frame size without padding (0 if unknown).
   Returns 1 if the header is valid. */
int mp3_parse_header(const uint8_t *p, struct mp3_hdr *h, int free_size);

/* ---- tables (mp3_tables.c) */
struct mp3_hufsrc { const uint16_t *code; const uint8_t *len; int side, linbits; };
extern const struct mp3_hufsrc mp3_hufsrc[34];
extern const int32_t mp3_window_d[257];

double mp3_cos(double x);
double mp3_sin(double x);
double mp3_sqrt(double x);

/* ---- shared math tables (mp3_dsp.c), built once by mp3_dsp_init() */
void mp3_dsp_init(void);
extern float mp3_pow43[8207];        /* i^(4/3) */
float mp3_pow2q(int e);              /* 2^(e/4) for -1000 < e < 400 */

/* ---- bit reader. The buffer must have at least 8 readable bytes after the last position read. */
struct mp3_bits { const uint8_t *buf; long pos, end; };   /* positions in bits */
static inline uint32_t mp3_peek32(const struct mp3_bits *b) {
    const uint8_t *p = b->buf + (b->pos >> 3);
    uint64_t v = (uint64_t)p[0] << 32 | (uint64_t)p[1] << 24 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 8 | p[4];
    return (uint32_t)(v >> (8 - (b->pos & 7)));
}
static inline uint32_t mp3_getbits(struct mp3_bits *b, int n) {   /* n = 0..32 */
    if (n == 0) return 0;
    uint32_t v = mp3_peek32(b) >> (32 - n);
    b->pos += n;
    return v;
}

/* ---- polyphase synthesis */
struct mp3_synth {
    float v[2][16][64];  /* last 16 V vectors per channel */
    int slot;            /* index of the newest V vector */
};
void mp3_synth_reset(struct mp3_synth *s);
/* One time slot: 32 subband samples -> 32 PCM samples written to out[0], out[stride], ... */
void mp3_synth32(struct mp3_synth *s, int ch, const float *sb, int16_t *out, int stride);

/* ---- Layer III state */
#define MP3_RES_SIZE 4096
struct mp3_l3 {
    uint8_t res[MP3_RES_SIZE + 64];  /* bit reservoir + current main data (+ zero padding) */
    int res_len;
    float overlap[2][32][18];
    float xr[2][576];
    float tmp[576];
    int ival[576];
    uint8_t scf_prev[2][40];         /* granule 0 scalefactors (MPEG-1 scfsi) */
};

/* ---- Layer I/II state */
struct mp3_l12 {
    float sb[2][36][32];             /* dequantized subband samples */
};

struct mp3dec {
    const uint8_t *data;
    long len, end;            /* end: data length without trailing tags */
    long first;               /* offset of the first audio frame */
    long pos;                 /* offset of the next frame to decode */
    struct mp3_info info;
    uint32_t match_mask, match_val; /* fields every frame of the stream must share */
    int free_size;            /* free-format frame size without padding (0: not free format) */
    int spf, lsf;
    long xing_frames;         /* frames counted by an Xing/VBRI header (0 if none) */
    int enc_delay, enc_padding, gapless;
    long skip;                /* output samples still to drop at the start */
    long out_limit;           /* total output samples (-1: unlimited) */
    long out_pos;             /* output samples delivered so far (output timeline) */
    long frame_no;            /* index of the next frame (0 = first audio frame) */
    int res_ok;               /* the reservoir is continuous */
    int insync;               /* d->pos is right after a decoded frame */
    int vbr;                  /* Xing (not Info) or VBRI header present */
    struct mp3_synth synth;
    struct mp3_l3 l3;
    struct mp3_l12 l12;
    int16_t scratch[2 * 1152];
};

long mp3_skip_id3v2(const uint8_t *data, long len);

/* Decodes one frame (header h at p, frame fully inside the buffer) into pcm (interleaved, out_ch channels).
   Returns samples per channel. decode=0 only feeds the bit reservoir (used for seeking). */
int mp3_l3_frame(mp3dec *d, const struct mp3_hdr *h, const uint8_t *p, int16_t *pcm, int out_ch, int decode);
int mp3_l12_frame(mp3dec *d, const struct mp3_hdr *h, const uint8_t *p, int16_t *pcm, int out_ch);
void mp3_l3_reset(mp3dec *d);
