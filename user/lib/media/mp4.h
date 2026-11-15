#pragma once
#include <stdint.h>
typedef struct mp4 mp4;
enum { MP4_VIDEO = 1, MP4_AUDIO = 2 };
struct mp4_track {
    int type;                    /* MP4_VIDEO, MP4_AUDIO, or 0 for others */
    char codec[5];               /* sample entry fourcc: "avc1", "avc3", "mp4a", "hev1", "vp09", "av01", "Opus"... */
    int width, height;           /* video: from the sample entry (tkhd display size in disp_w/disp_h) */
    int disp_w, disp_h;
    int sample_rate, channels;   /* audio */
    const uint8_t *config; int config_len;  /* avc1: avcC payload; mp4a: AudioSpecificConfig from esds (DecoderSpecificInfo) */
    int object_type;             /* mp4a: esds objectTypeIndication (0x40 = AAC) */
    uint32_t timescale;
    int64_t duration;            /* in timescale units, 0 if unknown */
    long nsamples;
};
struct mp4_sample { int64_t offset; uint32_t size; int64_t dts, pts; /* timescale units, pts = dts + ctts offset (edit list applied, see below) */ int keyframe; };
/* data: the whole file in memory (not copied; keep it alive). Returns 0 if it isn't an MP4/MOV/M4A/M4V file it can read. */
mp4 *mp4_open(const uint8_t *data, int64_t len);
int mp4_ntracks(mp4 *m);
const struct mp4_track *mp4_track(mp4 *m, int i);
int mp4_find(mp4 *m, int type);          /* first track of that type (prefer ones whose codec we can decode: avc1/avc3, mp4a), -1 if none */
int mp4_sample(mp4 *m, int track, long i, struct mp4_sample *s);   /* 1 if sample i exists */
long mp4_sample_at(mp4 *m, int track, int64_t pts_ms, int keyframe_only);   /* index of the sample at/just before that time (for seeking) */
int64_t mp4_ms(mp4 *m, int track, int64_t t);   /* convert timescale units to milliseconds */
long mp4_duration_ms(mp4 *m);
void mp4_close(mp4 *m);

/* ---- extras */
/* Track ID from tkhd, and the edit-list shift that was added to every dts/pts (timescale units). */
uint32_t mp4_track_id(mp4 *m, int track);
int64_t mp4_track_shift(mp4 *m, int track);
/* Duration of sample i in timescale units (0 if unknown / no such sample). */
int64_t mp4_sample_duration(mp4 *m, int track, long i);
/* Encoder delay in samples from the edit list (audio priming, e.g. 1024 or 2112 for AAC): output that a
   decoder produces for pts < 0 should be dropped. 0 if none. */
int64_t mp4_priming(mp4 *m, int track);
