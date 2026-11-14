# AXIOM MP3 decoder (`user/lib/audio`)

A from-scratch MPEG audio decoder for the music player. Freestanding C11: no libc, no libm. The only
external functions used are `memset memcpy memmove memcmp malloc calloc free`.

## API (`mp3.h`)

```c
mp3dec *mp3_open(const uint8_t *data, long len, struct mp3_info *info);  /* data stays owned by the caller */
int     mp3_decode(mp3dec *d, int16_t *pcm);   /* pcm: room for MP3_MAX_SAMPLES_PER_FRAME * channels */
long    mp3_seek(mp3dec *d, long ms);          /* returns the position reached (frame accurate) */
long    mp3_position_ms(mp3dec *d);
void    mp3_close(mp3dec *d);
void    mp3_tags(const uint8_t *data, long len, char *title, char *artist, char *album, int max);
/* extras */
long    mp3_position_samples(mp3dec *d);
void    mp3_gapless(mp3dec *d, int *enc_delay, int *enc_padding);
```

`mp3_decode` returns samples per channel (up to 1152), 0 at the end, <0 only for bad arguments.
Damaged data never makes it fail: bad frames are skipped (resync) or played as silence.

## Supported

- MPEG-1, MPEG-2 (LSF) and MPEG-2.5, all 9 sample rates, all bitrates, CBR / VBR / ABR, free format.
- Layer III: mono, stereo, dual channel, joint stereo with MS and intensity stereo (MPEG-1 and the
  MPEG-2 LSF variant), long / start / short / stop and mixed blocks, scfsi, preflag, subblock gain,
  all 32 Huffman tables, bit reservoir, CRC check (a frame with a bad CRC plays as silence).
- Layer II (incl. joint stereo, LSF) and Layer I.
- ID3v2 tags (any size, album art) and junk before the first frame are skipped; ID3v1, APEv2 and
  Lyrics3v2 at the end are ignored. Xing/Info and VBRI headers give the length of VBR files.
- Gapless playback: encoder delay and padding from a LAME/Lavc Info tag are trimmed (delay + 529 decoder
  samples at the start, padding at the end), exactly as ffmpeg does.
- `mp3_tags`: ID3v2.2/2.3/2.4 (Latin-1, UTF-16 with/without BOM, UTF-8, unsynchronisation, extended
  headers) and ID3v1; output is UTF-8, truncated at a character boundary.
- Seeking: frame accurate. The decoder walks the frame headers from the start (cheap: a few hops per
  frame, no data decoded), feeds the frames before the target into the bit reservoir and decodes one or
  two frames of pre-roll, so the audio after a seek is identical to a linear decode.

## Design

| file | contents |
|---|---|
| `mp3.c` | header parsing, frame sync/resync, tag skipping, Xing/VBRI/LAME tag, gapless, seek, API |
| `mp3_l3.c` | Layer III: side info, scalefactors, Huffman (multi-level lookup tables built at init), requantization, MS/intensity stereo, reorder, alias reduction, IMDCT (window folded into the cosine table) |
| `mp3_l12.c` | Layer I and II |
| `mp3_dsp.c` | math (cos/sin/sqrt/cbrt by series and Newton iteration, only at init), x^(4/3) and 2^(x/4) tables, polyphase synthesis (32-point DCT by Lee's recursive split + 512-tap window) |
| `mp3_tags.c` | ID3 parsing |
| `mp3_tables.c` | ISO 11172-3 constant tables: Huffman codes, synthesis window |
| `mp3_int.h` | internal header and the port layer: with `-DMP3_HOST_TEST` it includes libc headers, otherwise it declares the allowed functions itself |

Arithmetic is single-precision float (SSE2); tables are computed in double at the first `mp3_open`
(about 71 KB of BSS tables, built once; not thread-safe during that first call; code + const data is
about 40 KB). The decoder state (`struct mp3dec`, 40 KB including bit reservoir, overlap and synthesis
buffers) is one `calloc`. The
largest stack frame is about 3.7 KB.

Speed on the development host (`-O2`): 260x real time for 320 kbps stereo, ~480x for 128 kbps joint
stereo, >1000x for 22 kHz mono, ~650x for Layer II.

## Behaviour differences from ffmpeg (deliberate, ISO / dist10 / libmad behaviour kept)

These only matter for streams no common encoder writes; the test generator avoids them:
- a mixed block followed by a short block: ffmpeg drops the overlap of the two long subbands;
- long -> short without a start block: ffmpeg drops part of the overlap;
- MPEG-2 LSF intensity stereo: positions equal to the "illegal" maximum (`2^slen - 1`) are treated as
  not-intensity (ffmpeg treats them as positions), positions 16..30 are used (ffmpeg ignores them).
- ffmpeg cannot open free-format files at all.

## Not supported / limits

- A stream that changes channel count or sample rate in the middle: frames that do not match the first
  frame are skipped.
- MPEG-2.5 8 kHz mixed blocks follow the scalefactor-band layout (switch point at line 72); this is
  untested since no available encoder produces it.
- No MPEG-2 multichannel extension, no MPEG-4/AAC.

## Tests

`python3 tests/mp3/run.py [--quick]` (needs ffmpeg with libmp3lame/libtwolame, numpy; uses libmp3lame
and libmpg123 through ctypes when present). See the docstring at the top of that script.
