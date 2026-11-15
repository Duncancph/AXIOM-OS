#pragma once
#include <stdint.h>
typedef struct h264dec h264dec;
struct h264_frame {
    const uint8_t *y, *u, *v;      /* planes, valid until the next h264_decode/h264_flush/h264_close call */
    int stride_y, stride_uv;       /* bytes per row */
    int width, height;             /* after cropping (display size); chroma is (width+1)/2 x (height+1)/2 */
    int64_t pts;                   /* the pts passed in with the access unit that produced this picture */
    int keyframe;                  /* IDR */
    int full_range;                /* VUI video_full_range_flag */
    int matrix;                    /* VUI matrix_coefficients (1 = BT.709, 6 = BT.601, 2 = unspecified) */
};
/* extradata: the 'avcC' box payload from MP4 (AVCDecoderConfigurationRecord) - then input is length-prefixed NAL units.
   Pass extradata=0, len=0 for an Annex B byte stream (start codes) - then SPS/PPS come in-band. */
h264dec *h264_open(const uint8_t *extradata, int len);
/* Feed one access unit (one MP4 sample, or for Annex B any chunk of the stream) with its presentation time.
   Returns 0 on success, <0 on an unrecoverable error (bad/unsupported stream). */
int h264_decode(h264dec *d, const uint8_t *data, int len, int64_t pts);
/* Get the next picture in display order, if one is ready: returns 1 and fills *f, else 0. Call repeatedly after each h264_decode. */
int h264_get_frame(h264dec *d, struct h264_frame *f);
/* End of stream: makes all remaining pictures available to h264_get_frame. */
void h264_flush(h264dec *d);
/* Seeking: forget all pictures and references; decoding restarts at the next IDR/keyframe you feed. */
void h264_reset(h264dec *d);
void h264_close(h264dec *d);
/* For error messages: why the last call failed ("" if it didn't). */
const char *h264_error(h264dec *d);

/* ---- extras */
/* Stream information once the first SPS has been seen (0 if none yet): coded display size and the
   number of pictures the decoder may hold back for reordering. */
int h264_info(h264dec *d, int *width, int *height, int *reorder);
/* Count of macroblocks concealed (lost / corrupt) since h264_open. */
long h264_concealed_mbs(h264dec *d);
