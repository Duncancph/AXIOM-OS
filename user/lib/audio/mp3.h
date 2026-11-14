/* AXIOM MP3 decoder: MPEG-1/2/2.5 audio, Layers I, II and III.
 * Freestanding (no libc / libm); see README.md in this directory. */
#pragma once
#include <stdint.h>

struct mp3_info { int sample_rate, channels; long frames_est; long duration_ms; int bitrate_kbps; int layer; };
typedef struct mp3dec mp3dec;

/* Most samples per channel that one mp3_decode call can return. */
#define MP3_MAX_SAMPLES_PER_FRAME 1152

/* data: the whole file in memory (it is not copied; keep it alive). Skips ID3v2 tags and junk before the first frame,
   reads Xing/Info/VBRI headers for VBR length. Returns 0 if it isn't a playable MPEG audio file. */
mp3dec *mp3_open(const uint8_t *data, long len, struct mp3_info *info);
/* decode the next frame into interleaved 16-bit PCM (stereo files: L R L R...; mono: one channel).
   Returns the number of samples PER CHANNEL written (up to 1152), 0 at the end, <0 on an unrecoverable error.
   Damaged frames are skipped (resync), not fatal. */
int mp3_decode(mp3dec *d, int16_t *pcm);
/* jump to about ms milliseconds from the start (frame-accurate is enough); returns the position actually reached in ms */
long mp3_seek(mp3dec *d, long ms);
long mp3_position_ms(mp3dec *d);
void mp3_close(mp3dec *d);
/* ID3v2/ID3v1 title, artist, album if present (UTF-8, empty strings if not) */
void mp3_tags(const uint8_t *data, long len, char *title, char *artist, char *album, int max);

/* Extras. */
/* Position in samples per channel from the start of the output (what mp3_position_ms is derived from). */
long mp3_position_samples(mp3dec *d);
/* Encoder delay / padding read from a LAME/Lavc Info tag (0 if none); these are trimmed from the output. */
void mp3_gapless(mp3dec *d, int *enc_delay, int *enc_padding);
