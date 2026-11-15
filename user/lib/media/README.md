# AXIOM media library (`user/lib/media`)

From-scratch MP4 / ISO BMFF demuxer and AAC audio decoder (AAC-LC, Main, LTP, HE-AAC v1 (SBR) and HE-AAC v2
(Parametric Stereo)) for the Video app and the web browser. Freestanding C11 (gnu11): no libc, no libm. The
only external functions used are `malloc calloc realloc free memset memcpy memmove memcmp` (declared in
`media_port.h`; with `-DMEDIA_HOST_TEST` the libc headers are used instead, for the host tests).

Target build (warning-free with `-Werror`; `nm -u` of the linked library lists only the functions above):

    gcc -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -m64 -mcmodel=large \
        -msse2 -mfpmath=sse -fno-tree-loop-distribute-patterns -Wall -Wextra -Wno-unused-parameter -c file.c

for `mp4.c aac.c aac_dsp.c aac_sbr.c aac_ps.c aac_tables.c`. No threads, no global state other than read-only
tables that are built on the first `aac_open` (not thread-safe during that first call). Largest stack frame
about 12 KB (`apply_ltp`, AAC-LTP only); everything else is below 9 KB.

## MP4 demuxer (`mp4.h`)

```c
mp4 *m = mp4_open(data, len);            /* whole file in memory, not copied */
int v = mp4_find(m, MP4_VIDEO), a = mp4_find(m, MP4_AUDIO);
const struct mp4_track *t = mp4_track(m, a);   /* codec, sizes, rate/channels, config (avcC / AudioSpecificConfig) */
struct mp4_sample s;
for (long i = 0; mp4_sample(m, a, i, &s); i++) ... data + s.offset, s.size, s.dts, s.pts, s.keyframe ...
long i = mp4_sample_at(m, v, ms, 1);     /* seek: last keyframe with pts <= ms */
```

Supported:
- `ftyp`/`moov`/`mdat` in any order (moov at the end or after `+faststart`), MOV/QuickTime files (`wave`
  boxes, sound sample description versions 0/1/2), 64-bit box sizes, size 0 (to end of file), `uuid` and
  unknown boxes, truncated files (boxes are clamped to their parent).
- Sample tables: `stsd` (first entry; `avc1`/`avc3` with `avcC`, `mp4a` with `esds` (full ES_Descriptor ->
  DecoderConfigDescriptor -> DecoderSpecificInfo tree, objectTypeIndication), also `hvcC`, `vpcC`, `av1C`,
  `dOps`, `dfLa` as `config`), `stts`, `ctts` (v0 and v1, signed), `stsc`, `stsz`/`stz2` (4/8/16 bit),
  `stco`/`co64`, `stss` (none = all keyframes), `tkhd` (track ID, display size), `mdhd` (timescale,
  duration, v0/v1), `hdlr`, `mvhd`.
- Movie fragments: `mvex`/`trex` defaults, `moof`/`traf`/`tfhd` (base data offset, default-base-is-moof,
  implicit base = end of the previous track fragment's data, all default fields), `tfdt` (v0/v1; without it
  the decode time continues from the previous fragment), `trun` (data offset, first-sample flags, per-sample
  duration/size/flags/composition offset, v1 signed offsets), several `trun`s per `traf`, several tracks per
  `moof` or separate `moof`s, a non-empty `moov` followed by fragments.
- Timestamps (timescale units): edit lists are applied like ffmpeg (and players): an initial empty edit
  delays the track; the first real edit's media time is subtracted; for video the first presented frame
  inside the edit gets pts = (empty edit duration), which also covers stream copies cut mid-GOP (`-ss`
  with `-c copy`). With negative composition offsets (ctts v1 / `negative_cts_offsets`) dts is shifted down
  so that dts <= pts, as in ffmpeg. `mp4_priming()` (extra) returns the audio edit-list start (encoder delay,
  e.g. 1024 or 2112 for AAC): decoded audio before pts 0 should be dropped.
- `mp4_ms()` converts timescale units to milliseconds (floor, 64-bit safe); `mp4_duration_ms()` is the
  longest audio/video track (mdhd duration, or the sum of the sample durations for fragmented files).
- Damaged files: every read is bounds-checked; tables that are too short or inconsistent give fewer
  samples; a sample whose data would lie outside the file is clipped to the file (size 0 if it starts
  outside), so `data + offset .. + size` is always readable. Sample counts are limited by the table sizes and
  the file size (so a damaged count cannot allocate gigabytes).

Memory: one flat index of 32 bytes per sample (a 2-hour 30 fps video with 48 kHz AAC: ~11 MB).

Not supported: `cmov` (compressed movie headers), multiple sample descriptions per track (the first one is
used for all samples), edit lists with several non-empty edits (only the first is used), `sidx`/`mfra`
(not needed: fragments are found by scanning the top-level boxes), encrypted tracks (`encv`/`enca` are
reported with that fourcc).

Behaviour difference from ffmpeg (deliberate): in *fragmented* files with negative composition offsets
(`trun` v1, e.g. `-movflags cmaf`) ffmpeg adds the largest negative offset to every pts; we keep
pts = dts + offset as the standard and browsers do (the test accepts this constant shift).

## AAC decoder (`aac.h`)

```c
aacdec *d = aac_open(track->config, track->config_len);   /* or aac_open(0, 0) for ADTS */
int16_t pcm[AAC_MAX_SAMPLES * 2];
int n = aac_decode(d, data + s.offset, s.size, pcm);      /* samples per channel, interleaved */
int rate = aac_sample_rate(d), ch = aac_channels(d);      /* ask after the first call that returned > 0 */
aac_reset(d);                                             /* after seeking */
```

Query `aac_sample_rate` / `aac_channels` after the first successful `aac_decode`: with implicit SBR / PS
signalling (a plain AAC-LC config or ADTS header) the decoder only knows after the first frame that the
output rate doubles (SBR) or that mono becomes stereo. Like ffmpeg, mono HE-AAC is always output as stereo
(PS may appear at any time; without PS both channels are equal). Extras: `aac_decode_float` (float output,
not clipped), `aac_info` (core object type, SBR, PS).

Supported:
- AAC-LC (object type 2, also MPEG-2 LC), AAC Main (type 1, backward-adaptive prediction incl. predictor
  reset groups, bit-exact 16-bit float rounding) and AAC-LTP (type 4, long term prediction with the forward
  MDCT, TNS on the prediction).
- All 13 sample rates (7.35..96 kHz) and explicit (escape) rates, AudioSpecificConfig with
  `channelConfiguration` 1..7, 11, 12, 14 or a program_config_element (in the config or in-band in ADTS),
  ADTS (with or without CRC; the header can change channel layout / rate mid-stream).
- ICS: long/start/short/stop windows, window grouping, sine and KBD windows with shape switching, section
  data, scalefactors (incl. noise and intensity positions), all 11 spectral codebooks + escapes, pulse
  data, TNS (all orders, both directions, coefficient compression), M/S stereo (per band / all), intensity
  stereo (both codebooks, with M/S sign inversion), PNS (same noise generator as ffmpeg, so the output is
  comparable), IMDCT (FFT-based) + overlap-add.
- Channel elements: SCE, CPE, LFE, CCE (parsed and skipped, coupling is not applied), DSE and FIL skipped
  (FIL carries SBR), PCE.
- Multichannel: up to 10 decoded channels, downmixed to stereo with the ITU-R BS.775 coefficients ffmpeg/mpv
  use (`L = FL + 0.7071 C + 0.7071 SL/BL`, back centre 0.5 to both, LFE dropped, no normalisation;
  int16 output saturates).
- HE-AAC (SBR, ISO 14496-3 4.6.18): explicit (object type 5 / 29, or the 0x2b7/0x548 sync extension in an
  LC config) and implicit signalling (SBR found in the first frame), all SBR rates (16..96 kHz output),
  SCE and CPE (coupled and independent), all four frame classes, delta coding in time and frequency, both
  amplitude resolutions, header changes (frequency table resets), noise floors, inverse filtering levels,
  additional sinusoids, limiter bands/gains, interpolated or band-averaged energy estimation, gain
  smoothing. Elements without SBR data (and LFE) pass through the QMF banks (pure upsampling) like ffmpeg.
- HE-AAC v2 (Parametric Stereo, 8.6.4): 10/20/34 bands, IID (coarse/fine), ICC (mixing A and B), IPD/OPD
  with phase smoothing, both frame classes, 0..4 envelopes, hybrid analysis/synthesis filterbanks,
  decorrelation with transient attenuation.
- Damaged data: every bitstream read is bounds-checked (the access unit is copied into a padded buffer);
  a frame that fails to parse produces silence (overlap cleared) and the normal sample count; damaged SBR
  data switches SBR to plain upsampling until the next header, like ffmpeg.

Arithmetic is single precision (SSE2), transcendental functions are implemented locally (series / Newton)
and only used to build tables (plus a few values per SBR header). Tables: ~165 KB BSS built at the first
`aac_open`, ~45 KB of constant tables (Huffman codebooks, scalefactor bands, SBR/PS Huffman tables, QMF
window, SBR noise table). Decoder state: 20 KB per channel, +280 KB per SBR element (+134 KB for PS).

Not supported / limits:
- 960-sample frames (`frameLengthFlag`, DAB+), the ER object types (LD/ELD/ER-LC/BSAC), SSR, USAC/xHE-AAC,
  "downsampled SBR" (SBR at the core rate): such configs are refused by `aac_open`.
- Coupling channel elements are skipped (their effect on the target channels is not applied; no common
  encoder writes them).
- ADTS frames with several raw data blocks: only the first block is decoded.
- Dynamic range control and matrix-mixdown metadata are ignored.
- PNS in M/S bands uses independent noise in both channels (ffmpeg does the same; the standard asks for
  the same noise vector).
- AAC-LTP: two ffmpeg quirks are not copied (they only matter for channel pairs without a common window):
  ffmpeg applies the second channel's prediction only when the first channel of the pair also has predictor
  data, and keeps a stale `ltp_data_present` when a later frame of the second channel has no predictor data.

Speed on the development host (one core, -O2): AAC-LC 44.1 kHz stereo 128 kbps ~900x real time, 320 kbps
~700x, 5.1 384 kbps ~300x; HE-AAC stereo 44.1/48 kHz ~85-135x, HE-AAC v2 ~80-135x (the SBR QMF banks are
computed directly, not with an FFT).

## Design

| file | contents |
|---|---|
| `mp4.c` | box parser, sample tables, fragments, edit lists, seeking |
| `aac.c` | AudioSpecificConfig / ADTS / PCE, channel element parsing, Huffman spectral decoding, M/S, intensity, PNS, TNS, Main prediction, LTP, filterbank, channel mapping and downmix, API |
| `aac_dsp.c` | math helpers (cos/sin/sqrt/exp/log/pow without libm), x^(4/3) and 2^(x/4) tables, sine/KBD windows, radix-2 FFT, IMDCT/MDCT via DCT-IV, two-level Huffman lookup tables |
| `aac_sbr.c` | SBR: header/grid/envelope/noise parsing, frequency band tables, QMF analysis/synthesis, HF generator, HF adjustment |
| `aac_ps.c` | Parametric Stereo |
| `aac_tables.c` | constant tables of ISO/IEC 14496-3 (generated, do not edit by hand) |
| `aac_int.h`, `media_port.h` | internal declarations, bit reader, port layer |

## Tests

`python3 tests/media/run.py [--quick] [--out DIR] [--only demux,fuzz,aac,sbr,speed]` (needs ffmpeg/ffprobe
with libx264 and the native aac encoder, numpy; `--quick` takes under a minute and compares with the -O2 build
instead of the ASan one, except for the damaged-stream and fuzz tests). It exits non-zero on any
failure. It builds the host drivers (`tests/media/mp4_test.c`, `aac_test.c`) with ASan+UBSan and with -O2,
compiles every source with the target flags above (`-Werror`, `nm -u`, stack usage) and then:

- demuxer: ~35 files muxed by ffmpeg (moov at end / faststart, fragmented with default-base-moof, implicit
  base, separate moofs, every frame, frag_duration, dash, cmaf, B-frames, negative cts offsets, audio-only,
  video-only, four tracks, odd timescales, edit lists from -itsoffset and -ss stream copies, MOV, HEVC,
  MPEG-4 Part 2 + MP3, VP9 + Opus, 5.1 AAC) plus rewrites ffmpeg never writes (co64, stz2, 64-bit mdat
  size, uuid/unknown boxes): every track and every sample (offset, size, dts, pts, keyframe) is compared
  with `ffprobe -show_packets`, seeking with the same rule on ffprobe's packet list; then a mutation fuzz
  pass under ASan/UBSan (mutations concentrated on moov/moof).
- AAC: ~60 streams from ffmpeg's encoder (all rates, mono..7.1, 16..320 kbps and VBR, sines, sweeps, noise,
  chords, music-like notes with drum bursts, clicks for short windows, silence, near-full-scale noise;
  fast/twoloop/anmr coders, PNS/IS/MS/TNS on/off, PCE, Main prediction), as ADTS and in MP4, decoded with
  ffmpeg's float decoder: SNR >= 70 dB per stream on all channels before the downmix, the int16 output
  within 1 LSB of ffmpeg's and the stereo downmix equal to ffmpeg's `-ac 2`. AAC-LTP streams get random LTP
  data injected (`ltpgen.py`), since ffmpeg's encoder never uses LTP. Seeking/reset, damaged streams.
- HE-AAC / HE-AACv2: no HE-AAC encoder is installed, so `sbrgen.py`/`psgen.py` add random but valid SBR and
  PS payloads to LC cores from ffmpeg (every frame class, coupling, header changes, all tools; checked with
  ffmpeg to be error-free), decoded by ffmpeg as the reference, through ADTS (implicit signalling), MP4,
  and explicit AudioSpecificConfigs (object type 5/29, sync extensions). Noise-like sources must reach
  70 dB. For tonal sources SBR's LPC inverse filter is ill-conditioned: rounding differences are amplified
  (computing just that filter in double instead of float changes our own output by ~60 dB SNR), so those
  streams are checked against a 35 dB bound and reported. ffmpeg's SBR decoder reads uninitialised memory on
  some streams (its output changes from run to run, sometimes to NaN); the test decodes such streams several
  times and uses the run that matches.
- AAC fuzz (mutated access units and random configs under ASan/UBSan, LC/Main/LTP/multichannel/SBR/PS) and
  decode speed.
