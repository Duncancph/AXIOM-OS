#pragma once
#include <stdint.h>
typedef struct aacdec aacdec;
#define AAC_MAX_SAMPLES 2048   /* per channel per call (1024 for AAC-LC; 2048 with SBR) */
/* config: AudioSpecificConfig (from the MP4 esds). For ADTS streams pass config=0, len=0 (headers are in-band). */
aacdec *aac_open(const uint8_t *config, int len);
/* decode one access unit (one MP4 sample, or one ADTS frame incl. header) into interleaved int16 (stereo: L R L R; mono: one channel).
   Returns samples per channel written, 0 if nothing yet, <0 on error (damaged frames: output silence and return the normal count, don't fail). */
int aac_decode(aacdec *d, const uint8_t *data, int len, int16_t *pcm);
int aac_sample_rate(aacdec *d);   /* output rate (doubles with SBR) */
int aac_channels(aacdec *d);      /* output channels: 1 or 2 (downmix 5.1 etc. to stereo) */
void aac_reset(aacdec *d);        /* after seeking */
void aac_close(aacdec *d);

/* ---- extras */
/* Same as aac_decode, but float output in [-1, 1] (not clipped). */
int aac_decode_float(aacdec *d, const uint8_t *data, int len, float *pcm);
/* Implicit signalling: a plain AAC-LC config (or ADTS header) may carry SBR / PS data that is only seen in
   the first frames. aac_sample_rate / aac_channels can change once, after the first aac_decode that finds
   SBR (rate doubles) or PS (mono becomes stereo); query them after the first call that returned > 0. */
/* Audio object type of the core (2 = LC, 1 = Main, 4 = LTP), 1 if SBR is active, 1 if PS is active. */
void aac_info(aacdec *d, int *object_type, int *sbr, int *ps);
