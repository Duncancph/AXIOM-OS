# AXIOM H.264 decoder (`user/lib/video`)

A from-scratch H.264 / AVC (ITU-T H.264, ISO/IEC 14496-10) video decoder for the Video app and the web
browser. Freestanding C11 with SSE2: no libc, no libm. The only external functions used are
`malloc calloc realloc free memset memcpy memmove` (`memcmp` is allowed but not needed).
Output is bit-exact with ffmpeg's H.264 decoder on every stream of the test corpus.

## API (`h264.h`)

```c
h264dec *h264_open(const uint8_t *extradata, int len);   /* avcC payload, or 0/0 for an Annex B byte stream */
int  h264_decode(h264dec *d, const uint8_t *data, int len, int64_t pts);   /* one MP4 sample / any Annex B chunk */
int  h264_get_frame(h264dec *d, struct h264_frame *f);   /* next picture in display order (call until 0) */
void h264_flush(h264dec *d);                             /* end of stream: release the remaining pictures */
void h264_reset(h264dec *d);                             /* seeking: forget everything, restart at a keyframe */
void h264_close(h264dec *d);
const char *h264_error(h264dec *d);
/* extras */
int  h264_info(h264dec *d, int *width, int *height, int *reorder);
long h264_concealed_mbs(h264dec *d);
```

Typical use (MP4):

```c
h264dec *d = h264_open(avcc, avcc_len);
for each sample: {
    if (h264_decode(d, sample, size, pts) < 0) show(h264_error(d));   /* unsupported stream */
    struct h264_frame f;
    while (h264_get_frame(d, &f)) display(&f);    /* planes valid until the next decode/flush/close */
}
h264_flush(d);
while (h264_get_frame(d, &f)) display(&f);
h264_close(d);
```

* `h264_frame` gives 8-bit Y, U, V planes (4:2:0) of the cropped picture, the pts of the access unit that
  produced it, `keyframe` (IDR), and the VUI `video_full_range_flag` / `matrix_coefficients`
  (2 = unspecified when the stream has no colour description) for the YUV -> RGB conversion.
* `h264_decode` returns <0 only for streams the decoder cannot decode (see *Not supported*); the reason is
  in `h264_error`. Damaged data never makes it fail: lost or corrupt slices are concealed and decoding
  continues. A bad `avcC` makes every `h264_decode` call fail with "invalid avcC extradata".
* avcC mode: each call must be one access unit (an MP4 sample); the picture is finished at the end of the
  call. Length sizes 1, 2 and 4 are supported; in-band SPS/PPS (avc3) also work.
* Annex B mode: chunks may be cut anywhere; NAL units are assembled across calls. A NAL unit is decoded when
  the next start code arrives, so a picture appears one call later than in avcC mode (or at `h264_flush`).
  The pts of a picture is the pts of the chunk in which its first slice starts.
* Seeking: call `h264_reset`, then feed from a sync sample. Decoding starts at the first IDR picture, I
  picture, or picture with a recovery point SEI. Leading pictures of an open GOP (displayed before the I
  picture and referencing pictures before it) are dropped; after a recovery point in an intra-refresh
  stream, output is suppressed until `recovery_frame_cnt` frames later, so only complete pictures are shown.
  Non-keyframes fed after a reset are skipped.

## Supported

* Profiles: Constrained Baseline, Baseline (without FMO/ASO), Main, Extended (without SP/SI and data
  partitioning), High, and the 8-bit 4:2:0 subset of High 10 / High 4:2:2 / High 4:4:4 (Predictive) —
  including the lossless transform-bypass mode x264 uses for `qp=0` / `crf=0`.
* SPS / PPS with scaling matrices (flat, default, explicit, fall-back rules A and B), VUI (cropping, colour
  description, `max_num_reorder_frames` / `max_dec_frame_buffering`), any number of SPS/PPS, changes of
  resolution mid-stream.
* CAVLC and CABAC; I, P and B slices; multiple slices per picture (any slice structure, deblocking across or
  not across slice boundaries, `disable_deblocking_filter_idc` 0/1/2).
* All intra modes: 4x4 (9), 8x8 (9, with reference sample filtering), 16x16 (4), chroma (4), constrained
  intra prediction, I_PCM.
* Inter prediction: all partitions down to 4x4, quarter-pel luma (6-tap) and eighth-pel chroma, motion
  vectors pointing anywhere outside the picture, up to 32 references per list, P_Skip, B_Skip / B_Direct
  with spatial and temporal direct (with and without `direct_8x8_inference`), weighted prediction
  (explicit for P and B, implicit for B), long-term references.
* 4x4 and 8x8 transforms, Intra16x16 / chroma DC transforms, dequantisation with scaling matrices.
* Deblocking filter (bS 0..4, alpha/beta offsets, chroma QP offsets).
* Reference picture marking: sliding window and all MMCO operations (1..6), gaps in `frame_num` (with
  "non-existing" frames), reference list initialisation and `ref_pic_list_modification`.
* POC types 0, 1 and 2; output in display order with a reorder window from `max_num_reorder_frames` (or
  the DPB size of the level when the stream does not say, growing it if the stream reorders more than it
  announced), IDR / MMCO 5 boundaries, frame cropping.

## Not supported (rejected with an error, never a crash)

* Interlaced coding: field pictures (PAFF) and MBAFF. Progressive frames of a stream with
  `frame_mbs_only_flag = 0` decode normally; interlaced slices fail with "unsupported: interlaced ...".
* Chroma formats other than 4:2:0 (monochrome, 4:2:2, 4:4:4) and bit depths above 8.
* Slice groups (FMO), arbitrary slice order (only as a consequence of FMO), redundant pictures (ignored, the
  primary picture is used), data partitioning (NAL types 2-4), SP/SI slices.
* SVC / MVC extensions (NAL types 14, 15, 20 are ignored, so the base layer / base view decodes).

## Design

| file | contents |
|---|---|
| `h264.h` | public API |
| `h264_int.h` | internal header: port layer (`-DH264_HOST_TEST` includes libc headers, otherwise the allowed functions are declared here), bit reader, CABAC engine (inline), structures |
| `h264.c` | API, NAL unit parsing (Annex B start codes and avcC length prefixes, emulation prevention), SEI (recovery point), picture buffer pool, output order |
| `h264_ps.c` | SPS / PPS / VUI / scaling lists, dequantisation tables |
| `h264_slice.c` | slice header, picture start / finish, POC, frame_num gaps, reference lists and modification, marking (sliding window, MMCO), implicit weights, slice data loop, concealment trigger |
| `h264_cavlc.c` | CAVLC macroblock layer: mb_type, prediction syntax, coded_block_pattern, residual (coeff_token / level / total_zeros / run_before through two-level lookup tables) |
| `h264_cabac.c` | CABAC: engine start, context initialisation, all syntax elements with their context selection, residual (significance map + levels) |
| `h264_mvpred.c` | neighbour context cache, motion vector prediction, P_Skip, spatial / temporal direct |
| `h264_recon.c` | macroblock reconstruction (intra prediction + residual, motion compensation + residual, lossless bypass), per-MB bookkeeping, concealment of lost MBs |
| `h264_intra.c` | intra prediction |
| `h264_dsp.c` | inverse transforms, DC transforms, luma/chroma interpolation, averaging and weighted prediction (SSE2) |
| `h264_deblock.c` | deblocking: boundary strength, SSE2 edge filters (16 pixels at a time, vertical edges through 16x8 transposes), padding of reference pictures |
| `h264_tables.c` | constant tables of the standard: CABAC context init (m, n) for all 460 frame contexts, rangeTabLPS, state transitions, zigzag scans, default scaling lists, alpha/beta/tc0, CBP mappings |

Decoding is macroblock by macroblock: parse (CAVLC or CABAC) into a small per-MB state, using an 8x5
neighbour cache (left / top / top-right / top-left context of motion vectors, references, mvds,
coefficient counts and intra modes) that is filled from per-picture arrays, then reconstruct directly into
the picture, then store the MB's data back. Pictures carry their motion vectors (per 4x4) and reference
indices / reference picture ids (per 8x8) for direct prediction and deblocking. The deblocking filter runs
over the whole picture once all its slices are decoded (intra prediction needs the unfiltered samples), then
reference pictures get 32-pixel (luma) / 16-pixel (chroma) edge padding so that motion compensation reads
directly from the picture; blocks reaching further out go through a small edge-emulation buffer.

Each picture buffer is one allocation (planes with padding + motion data, about 1.5 x the frame size plus
padding); the pool grows on demand up to 40 pictures (normally references + reorder window + 2). Per
decoder state is about 80 KB (one `calloc`) plus 128 bytes per macroblock. The largest stack frame is under 3 KB.
Tables that are computed (CAVLC lookup tables, 512-byte CABAC range table) are built at the first
`h264_open` (not thread-safe during that first call). SIMD is SSE2 only (no SSSE3/SSE4); compiling with
`-DH264_NO_SIMD` selects the plain C versions of every SIMD routine (same output).

Error handling: every syntax element is range checked; bit readers return zeros past the end of a NAL;
reference indices, mvd sizes, coefficient counts and levels are bounded; missing references are replaced by
the nearest available picture (or grey); macroblocks of lost or broken slices are copied from the previous
reference picture; frame_num gaps create concealment frames. The code is fuzzed under ASan/UBSan.

## Performance

Single core, decoding only (no output), measured by `run.py` on the development host (Xeon @ 2.1 GHz);
"target" = the objects compiled with the exact AXIOM flags (`-mcmodel=large -fno-pic ...`):

| clip (25 fps source, x264) | bitrate | host `-O2` | target flags |
|---|---|---|---|
| 1280x720 High, CRF 21, 3 B-frames, 4 refs, rotating test pattern + noise | 3.5 Mbit/s | 198 fps | 190 fps |
| 1280x720 High, CRF 21, mandelbrot zoom (detailed, all-B-heavy) | 6.5 Mbit/s | 151 fps | 147 fps |
| 1280x720 Constrained Baseline (CAVLC, no B) | 3.7 Mbit/s | 344 fps | 330 fps |
| 1920x1080 High, CRF 21, 3 B-frames, 4 refs | 7.2 Mbit/s | 89 fps | 83 fps |
| 1920x1080 High, CRF 18, colour bars scrolling | 0.4 Mbit/s | 255 fps | 240 fps |

Timings vary by about +-5 % between runs. The time is spread over CABAC residual decoding (~25-35 % on
high-bitrate streams), motion compensation, deblocking and per-macroblock bookkeeping; MC, inverse
transforms, weighted prediction and the deblocking filter use SSE2.

## Tests

```
python3 tests/h264/run.py            # full run (~2.5 minutes with an empty corpus cache on 4 cores, ~1.5 cached)
python3 tests/h264/run.py --quick    # subset: ~60 s from scratch, ~50 s with the corpus cached
options: --out DIR (default /tmp/axiom-h264test), --keep, --only NAME_SUBSTRING
```

`run.py` builds `tests/h264/h264_test.c` with the decoder for the host (ASan+UBSan, -O2, and a plain-C
`-DH264_NO_SIMD` ASan build), compiles every file with the AXIOM target flags and `-Werror`, checks
`nm -u` for allowed symbols only and reports stack usage. It then generates a corpus with ffmpeg/libx264 and
compares the decoder's yuv420p output byte for byte with `ffmpeg -f rawvideo -pix_fmt yuv420p`:

* 86 streams (full run; 30 in `--quick`): Constrained Baseline / Main / High, CAVLC and CABAC, B-frames
  (up to 16, b-pyramid normal/strict), direct spatial / temporal / auto, `weightp=1/2`, implicit weighted
  bi-prediction, `ref=8`, 8x8 transform, `cqm=jvt` and a custom `cqmfile` (exercising the fall-back
  rules), `slices=4`, `slice-max-size`, intra-only, intra refresh, open GOP, short / long GOPs,
  `tune` film/animation/grain/stillimage/psnr/ssim/fastdecode/zerolatency, constrained intra, deblocking
  offsets / off, chroma QP offsets, crf 0 / qp 0 (lossless), crf 5 / 45 / qp 51, odd sizes with cropping
  (854x480, 1366x768, 642x362, 200x120, 48x34, 16x16, 1920x1080), all presets ultrafast..veryslow for
  each profile, and sources testsrc2, mandelbrot, smptehdbars scrolling, noise, rotation + noise,
  cellauto, life, gradients.
* every stream through both input paths: Annex B (one access unit per call, and odd chunk sizes) and MP4
  samples via avcC (`mp4samples.py` extracts them).
* streams libx264 cannot produce, made by rewriting x264 output or synthesised (`synth.py`), compared with
  ffmpeg decoding the same bytes: POC type 1, long-term references + MMCO 4/6 + `ref_pic_list_modification`
  with long-term pic nums, short-term list modification (CAVLC and CABAC), frame_num gaps with
  `gaps_in_frame_num_allowed_flag`, I_PCM macroblocks in CAVLC and CABAC (with CABAC re-initialisation).
* seeking: `h264_reset` + decode from keyframes (closed and open GOP: must equal the reference from that
  picture on), from non-keyframes (must resume exactly at the next keyframe), from intra-refresh recovery
  points (must output only exact pictures); resolution changes mid-stream; truncated files; a lost slice
  (must be exact again from the next IDR); interlaced / 4:2:2 / 4:4:4 / 10-bit streams must be rejected
  cleanly; a mutation fuzz pass (bit flips, random bytes, zeroed runs, truncated and dropped samples,
  random resets, broken avcC, random Annex B chunking) under ASan+UBSan.
* speed of 720p and 1080p clips with both the host and the AXIOM target flags.

Current result: all 86 corpus streams bit-exact through all input paths (172+ decodes, plus chunked Annex B
feeding), all rewritten / synthetic streams bit-exact with ffmpeg, 16 seek, 20 truncation and 3 lost-slice
tests, 4 unsupported formats rejected, no sanitizer findings in the fuzz pass (and in an extra run of 1500
damaged decodes over 15 streams).
