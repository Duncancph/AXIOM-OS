#!/usr/bin/env python3
"""Tests the AXIOM H.264 decoder (user/lib/video) against ffmpeg's decoder.

Usage: python3 tests/h264/run.py [--quick] [--keep] [--out DIR] [--only SUBSTR]

1. Builds tests/h264/h264_test.c + user/lib/video/*.c for the host twice (ASan+UBSan for correctness,
   -O2 for speed), and compiles the decoder with the freestanding target flags (warnings are errors,
   `nm -u` must list only the allowed libc symbols, reports per-function stack use).
2. Generates a corpus with ffmpeg + libx264 from lavfi test sources (testsrc2, mandelbrot, smptehdbars with
   motion, noise, cellauto, ...): Constrained Baseline / Main / High, CAVLC and CABAC, B-frames, b-pyramid,
   direct spatial/temporal, weighted prediction (weightp=1/2, weightb implicit), many refs, 8x8 transform,
   custom scaling matrices (cqm=jvt and a cqmfile exercising the fall-back rules), multiple slices,
   intra-only, intra refresh, odd sizes with cropping, lossless (transform bypass), crf 0..51, long / short /
   open GOPs, tune zerolatency/fastdecode/..., presets ultrafast..veryslow, constrained intra, POC types 0/1/2.
3. Decodes every stream both as an Annex B byte stream (one access unit per call, and in odd-sized chunks)
   and as MP4 samples through the avcC path, and requires the yuv420p output to be bit-exact with
   `ffmpeg -f rawvideo -pix_fmt yuv420p`.
4. Seeking (h264_reset + feed from a keyframe, incl. open GOP), resolution changes, truncated files, lost
   NAL units (must recover exactly at the next IDR), unsupported streams (must fail cleanly), a mutation
   fuzz pass under ASan+UBSan, and decode speed for 720p / 1080p clips.
Exits non-zero on any failure."""
import hashlib, os, random, shutil, struct, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp4samples
import synth

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "user", "lib", "video")
QUICK = "--quick" in sys.argv
KEEP = "--keep" in sys.argv
OUT = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(tempfile.gettempdir(), "axiom-h264test")
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
CORPUS = os.path.join(OUT, "corpus")
os.makedirs(CORPUS, exist_ok=True)
FAILS = []
BASES = {}   # case name -> corpus path prefix
JOBS = max(1, min(8, os.cpu_count() or 1))
R = random.Random(1234)


def fail(msg):
    FAILS.append(msg)
    print("FAIL:", msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)


# ---------------------------------------------------------------- build
CSRC = sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".c"))
DRIVER = os.path.join(ROOT, "tests", "h264", "h264_test.c")
ASAN = os.path.join(OUT, "h264_test_asan")
FAST = os.path.join(OUT, "h264_test")
NOSIMD = os.path.join(OUT, "h264_test_nosimd")
TARGETBIN = os.path.join(OUT, "h264_test_target")
TARGET_FLAGS = ("-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -m64 "
                "-mcmodel=large -msse2 -mfpmath=sse -fno-tree-loop-distribute-patterns -Wall -Wextra "
                "-Wno-unused-parameter").split()
ALLOWED = {"memset", "memcpy", "memmove", "memcmp", "malloc", "free", "realloc", "calloc"}


def build():
    print("== build", flush=True)
    r = run(["gcc", "-std=gnu11", "-O1", "-g", "-DH264_HOST_TEST", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Wno-unused-parameter", "-o", ASAN, DRIVER] + CSRC)
    if r.returncode: print(r.stderr); sys.exit("asan build failed")
    r = run(["gcc", "-std=gnu11", "-O2", "-DH264_HOST_TEST", "-o", FAST, DRIVER] + CSRC)
    if r.returncode: print(r.stderr); sys.exit("host build failed")
    r = run(["gcc", "-std=gnu11", "-O1", "-g", "-DH264_HOST_TEST", "-DH264_NO_SIMD", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Wno-unused-parameter", "-o", NOSIMD, DRIVER] + CSRC)
    if r.returncode: print(r.stderr); sys.exit("plain C (H264_NO_SIMD) build failed")
    objs, stack = [], []
    tdir = os.path.join(OUT, "target")
    os.makedirs(tdir, exist_ok=True)
    for c in CSRC:
        o = os.path.join(tdir, os.path.basename(c)[:-2] + ".o")
        r = run(["gcc"] + TARGET_FLAGS + ["-Werror", "-fstack-usage", "-c", c, "-o", o], cwd=tdir)
        if r.returncode or r.stderr.strip():
            fail("target compile %s: %s" % (os.path.basename(c), r.stderr.strip()))
            continue
        objs.append(o)
        su = o[:-2] + ".su"
        if os.path.exists(su):
            for line in open(su):
                parts = line.rstrip("\n").split("\t")
                stack.append((int(parts[1]), parts[0].split(":")[-1], parts[2]))
    if objs:
        r = run(["nm", "-u"] + objs)
        undef = {l.split()[-1] for l in r.stdout.splitlines() if l.strip() and l.split()[0] == "U"}
        defined = set()
        for l in run(["nm", "--defined-only"] + objs).stdout.splitlines():
            p = l.split()
            if len(p) == 3: defined.add(p[2])
        ext = undef - defined
        bad = ext - ALLOWED
        print("target compile: %d files, warning-free; external symbols: %s" % (len(objs), " ".join(sorted(ext))))
        if bad: fail("disallowed external symbols: %s" % sorted(bad))
        stack.sort(reverse=True)
        print("largest stack frames (bytes):", ", ".join("%s=%d%s" % (f, n, "" if k == "static" else "(" + k + ")") for n, f, k in stack[:6]))
        if stack and stack[0][0] > 16384: fail("stack frame over 16 KiB: %s" % (stack[0],))
        # the decoder objects exactly as compiled for AXIOM, linked with the host driver (for speed numbers)
        drv = os.path.join(tdir, "driver.o")
        r = run(["gcc", "-O2", "-fno-pie", "-c", DRIVER, "-o", drv])
        r2 = run(["gcc", "-no-pie", "-o", TARGETBIN, drv] + objs) if not r.returncode else r
        if r2.returncode: print("note: could not link the target objects for benchmarking:", r2.stderr.strip()[-200:])


# ---------------------------------------------------------------- corpus
CQMFILE = os.path.join(CORPUS, "custom.cqm")


def write_cqm():
    def mat(n, base, slope):
        return ",".join(str(min(255, base + slope * ((i % n) + (i // n)))) for i in range(n * n))
    y4i, y4p = mat(4, 8, 5), mat(4, 12, 3)
    with open(CQMFILE, "w") as f:
        f.write("INTRA4X4_LUMA = %s\n" % y4i)
        f.write("INTRA4X4_CHROMAU = %s\n" % y4i)          # equal to the previous list: fall-back rule
        f.write("INTRA4X4_CHROMAV = %s\n" % mat(4, 10, 4))
        f.write("INTER4X4_LUMA = %s\n" % y4p)
        f.write("INTER4X4_CHROMAU = %s\n" % mat(4, 14, 2))
        f.write("INTER4X4_CHROMAV = %s\n" % mat(4, 14, 2))
        f.write("INTRA8X8_LUMA = %s\n" % mat(8, 6, 2))
        f.write("INTER8X8_LUMA = %s\n" % mat(8, 9, 2))


SOURCES = {
    "testsrc2": "testsrc2=size={s}:rate={r}",
    "mandel": "mandelbrot=size={s}:rate={r}:end_scale=0.01:maxiter=200",
    "bars": "smptehdbars=size={s}:rate={r},scroll=horizontal=0.003:vertical=0.001",
    "noise": "testsrc2=size={s}:rate={r},noise=alls=25:allf=t+u",
    "rot": "testsrc2=size={s}:rate={r},rotate=a=0.15*t:fillcolor=gray,noise=alls=6:allf=t+u",
    "cells": "cellauto=size={s}:rate={r}:rule=110",
    "life": "life=size={s}:rate={r}:mold=10:ratio=0.2:death_color=#406080:life_color=#e0c020",
    "grad": "gradients=size={s}:rate={r}:speed=0.03:n=5,noise=alls=4:allf=t",
}

# name, source, size, frames, ffmpeg encoder args, x264-params, quick?
CASES = [
    ("cbp_cavlc", "testsrc2", "352x288", 40, ["-profile:v", "baseline"], "keyint=15", True),
    ("cbp_ultrafast", "rot", "320x240", 30, ["-profile:v", "baseline", "-preset", "ultrafast"], "", True),
    ("cbp_slices", "bars", "352x288", 24, ["-profile:v", "baseline"], "slices=4:ref=3", True),
    ("cbp_zerolatency", "noise", "320x240", 30, ["-profile:v", "baseline", "-tune", "zerolatency"], "", False),
    ("cbp_cavlc_noise_hq", "noise", "320x240", 20, ["-profile:v", "baseline", "-crf", "5"], "", False),
    ("cbp_constrained_intra", "rot", "320x240", 24, ["-profile:v", "baseline"], "constrained-intra=1:keyint=10", True),
    ("main_cabac_b", "testsrc2", "352x288", 40, ["-profile:v", "main"], "bframes=3:ref=4", True),
    ("main_cavlc_b", "rot", "352x288", 30, ["-profile:v", "main"], "bframes=2:cabac=0:ref=3", True),
    ("main_temporal", "mandel", "320x240", 30, ["-profile:v", "main"], "bframes=3:direct=temporal:b-pyramid=none", True),
    ("main_spatial_pyr", "rot", "320x240", 30, ["-profile:v", "main"], "bframes=3:direct=spatial:b-pyramid=normal:ref=5", False),
    ("main_weightb", "grad", "320x240", 30, ["-profile:v", "main"], "bframes=3:weightb=1:direct=temporal", True),
    ("main_weightp", "grad", "320x240", 30, ["-profile:v", "main"], "bframes=0:weightp=2:ref=4", True),
    ("high_8x8", "rot", "352x288", 30, ["-profile:v", "high"], "8x8dct=1:bframes=3", True),
    ("high_cqm_jvt", "mandel", "352x288", 24, ["-profile:v", "high"], "cqm=jvt:bframes=2", True),
    ("high_cqmfile", "rot", "352x288", 24, ["-profile:v", "high"], "cqmfile=" + CQMFILE + ":bframes=2", True),
    ("high_cqmfile_cavlc", "noise", "320x240", 16, ["-profile:v", "high"], "cqmfile=" + CQMFILE + ":cabac=0:bframes=1", False),
    ("high_weightp2", "grad", "352x288", 30, ["-profile:v", "high"], "weightp=2:bframes=3:ref=6", True),
    ("high_weightp1", "rot", "320x240", 24, ["-profile:v", "high"], "weightp=1:bframes=2", False),
    ("high_pyr_strict", "testsrc2", "320x240", 40, ["-profile:v", "high"], "b-pyramid=strict:bframes=3:ref=4", False),
    ("high_temporal_ref8", "rot", "352x288", 40, ["-profile:v", "high"], "direct=temporal:ref=8:bframes=3:b-pyramid=normal", True),
    ("high_spatial_ref8", "mandel", "320x240", 40, ["-profile:v", "high"], "direct=spatial:ref=8:bframes=3", False),
    ("high_auto_b16", "bars", "320x240", 50, ["-profile:v", "high"], "direct=auto:bframes=16:b-adapt=2:ref=4", False),
    ("high_slices4", "rot", "352x288", 24, ["-profile:v", "high"], "slices=4:bframes=2:ref=3", True),
    ("high_slicesize", "noise", "352x288", 16, ["-profile:v", "high"], "slice-max-size=1500:bframes=2", False),
    ("high_intra_only", "mandel", "352x288", 10, ["-profile:v", "high"], "keyint=1", True),
    ("high_intra_refresh", "rot", "320x240", 50, ["-profile:v", "high"], "intra-refresh=1:keyint=12:bframes=0", True),
    ("high_open_gop", "testsrc2", "320x240", 60, ["-profile:v", "high"], "open-gop=1:keyint=20:bframes=3", True),
    ("high_short_gop", "rot", "320x240", 30, ["-profile:v", "high"], "keyint=4:bframes=2", False),
    ("high_long_gop", "bars", "320x240", 80, ["-profile:v", "high"], "keyint=300:min-keyint=300:scenecut=0", False),
    ("high_noise_crf0", "noise", "176x144", 8, ["-crf", "0"], "", True),
    ("high_lossless_qp0", "rot", "176x144", 10, [], "qp=0:bframes=2", True),
    ("high_crf5", "noise", "320x240", 10, ["-profile:v", "high", "-crf", "5"], "", False),
    ("high_crf45", "rot", "352x288", 30, ["-profile:v", "high", "-crf", "45"], "", False),
    ("high_qp51", "testsrc2", "320x240", 20, ["-profile:v", "high"], "qp=51:bframes=2", True),
    ("high_qp10_noise", "noise", "352x288", 8, ["-profile:v", "high"], "qp=10:bframes=1", False),
    ("high_deblock_off", "rot", "320x240", 20, ["-profile:v", "high"], "deblock=-6,6:bframes=2", False),
    ("high_deblock_strong", "rot", "320x240", 20, ["-profile:v", "high"], "deblock=6,-6:bframes=2", False),
    ("high_no_deblock", "noise", "320x240", 16, ["-profile:v", "high"], "no-deblock=1", False),
    ("high_chroma_qp", "mandel", "320x240", 20, ["-profile:v", "high"], "chroma-qp-offset=7:bframes=2", False),
    ("high_chroma_qp_neg", "rot", "320x240", 20, ["-profile:v", "high"], "chroma-qp-offset=-9:bframes=2", False),
    ("size_854x480", "rot", "854x480", 12, ["-profile:v", "high"], "bframes=2", True),
    ("size_1366x768", "bars", "1366x768", 6, ["-profile:v", "main"], "bframes=2", False),
    ("size_642x362", "mandel", "642x362", 10, ["-profile:v", "high"], "bframes=2", False),
    ("size_200x120", "noise", "200x120", 12, ["-profile:v", "baseline"], "", True),
    ("size_16x16", "testsrc2", "16x16", 12, ["-profile:v", "high"], "bframes=2", False),
    ("size_48x34", "rot", "48x34", 12, ["-profile:v", "main"], "bframes=1", True),
    ("size_1920x1080", "rot", "1920x1080", 6, ["-profile:v", "high"], "bframes=2", False),
    ("cells_cavlc", "cells", "320x240", 20, ["-profile:v", "baseline"], "", False),
    ("life_cabac", "life", "320x240", 20, ["-profile:v", "high"], "bframes=2", False),
    ("mbtree_off_aq", "rot", "320x240", 24, ["-profile:v", "high"], "mbtree=0:aq-mode=2:bframes=2", False),
    ("partitions_all", "rot", "352x288", 30, ["-profile:v", "high"], "partitions=all:subme=9:bframes=3:ref=4:direct=temporal", True),
    ("partitions_all_spatial", "mandel", "352x288", 30, ["-profile:v", "main"], "partitions=all:subme=10:trellis=2:bframes=3:ref=6", False),
]
for prof in ("baseline", "main", "high"):
    for preset in ("ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"):
        q = (prof, preset) in (("baseline", "veryfast"), ("main", "slow"), ("high", "veryslow"), ("high", "ultrafast"))
        CASES.append(("preset_%s_%s" % (prof, preset), "rot" if preset[0] in "uvm" else "mandel", "320x240", 20,
                      ["-profile:v", prof, "-preset", preset], "", q))
for tune in ("film", "animation", "grain", "stillimage", "psnr", "ssim", "fastdecode"):
    CASES.append(("tune_%s" % tune, "rot", "320x240", 20, ["-profile:v", "high", "-tune", tune], "", tune == "grain"))

if QUICK:
    CASES = [c for c in CASES if c[6]]
if ONLY:
    CASES = [c for c in CASES if ONLY in c[0]]


def dims(size):
    w, h = size.split("x")
    return int(w), int(h)


def frame_bytes(w, h):
    return w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)


def case_base(case):
    name, src, size, n, args, params, _ = case
    h = hashlib.sha1(repr((src, SOURCES[src], size, n, args, params)).encode()).hexdigest()[:8]
    return os.path.join(CORPUS, "%s-%s" % (name, h))


def encode(case):
    name, src, size, n, args, params, _ = case
    base = case_base(case)
    mp4 = base + ".mp4"
    if not os.path.exists(mp4):
        filt = SOURCES[src].format(s=size, r=25)
        cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", filt, "-frames:v", str(n), "-pix_fmt", "yuv420p",
               "-c:v", "libx264"] + args
        if params: cmd += ["-x264-params", params]
        r = run(cmd + [mp4 + ".tmp.mp4"])
        if r.returncode:
            return "encode failed: " + r.stderr.strip()[-300:]
        os.rename(mp4 + ".tmp.mp4", mp4)
    if not os.path.exists(base + ".h264"):
        r = run(["ffmpeg", "-v", "error", "-y", "-i", mp4, "-c:v", "copy", "-bsf:v", "h264_mp4toannexb", "-f", "h264", base + ".h264"])
        if r.returncode: return "annexb extract failed"
    if not os.path.exists(base + ".smp"):
        mp4samples.write_samples(mp4, base + ".smp")
    if not os.path.exists(base + ".ref.yuv"):
        r = run(["ffmpeg", "-v", "error", "-y", "-i", mp4, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", base + ".ref.yuv"])
        if r.returncode: return "reference decode failed"
    return None


def stream_dims(path):
    r = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path])
    w, h = r.stdout.strip().split(",")[:2]
    return int(w), int(h)


def compare(out, ref, w, h):
    """returns (equal, n_out, n_ref, first differing frame)"""
    fb = frame_bytes(w, h)
    a = open(out, "rb").read() if os.path.exists(out) else b""
    b = open(ref, "rb").read()
    na, nb = len(a) // fb, len(b) // fb
    first = None
    for i in range(min(na, nb)):
        if a[i * fb:(i + 1) * fb] != b[i * fb:(i + 1) * fb]:
            first = i
            break
    return a == b, na, nb, first


def decode(binary, mode, inp, out, *extra):
    r = run([binary, mode, inp, out] + [str(x) for x in extra])
    return r


def check_case(case):
    name = case[0]
    msg = encode(case)
    if msg: return [name + ": " + msg], name + ": skipped"
    base = case_base(case)
    BASES[name] = base
    w, h = stream_dims(base + ".mp4")
    fails = []
    big = w * h > 700 * 400
    binary = FAST if big else ASAN
    results = []
    runs = [("annexb", base + ".h264", ()), ("samples", base + ".smp", ())]
    if not QUICK or name in ("main_cabac_b", "cbp_slices"):
        runs.append(("annexb", base + ".h264", (R.choice([7, 333, 4096]),)))
    for mode, inp, extra in runs:
        out = base + ".%s%s.yuv" % (mode, "".join("_%d" % e for e in extra))
        r = decode(binary, mode, inp, out, *extra)
        if r.returncode:
            fails.append("%s %s%s: decoder exited %d: %s" % (name, mode, extra, r.returncode, r.stderr.strip()[-800:]))
            continue
        if "errors 0" not in r.stdout or "concealed_mbs 0" not in r.stdout:
            fails.append("%s %s%s: decoder reported problems: %s %s" % (name, mode, extra, r.stdout.strip(), r.stderr.strip()[-200:]))
        eq, na, nb, first = compare(out, base + ".ref.yuv", w, h)
        if not eq:
            fails.append("%s %s%s: output differs (frames %d vs ref %d, first differing frame %s)" % (name, mode, extra, na, nb, first))
        results.append(eq)
        if not KEEP: os.remove(out)
    return fails, "%-26s %4dx%-4d %3d frames  %s" % (name, w, h, os.path.getsize(base + ".ref.yuv") // frame_bytes(w, h),
                                                    "bit-exact" if results and all(results) and not fails else "MISMATCH")


# ---------------------------------------------------------------- extra tests
def keyframes_and_display(smp_path_mp4):
    avcc, samples = mp4samples.read_track(open(smp_path_mp4, "rb").read())
    order = sorted(range(len(samples)), key=lambda i: samples[i][1])
    disp = {idx: k for k, idx in enumerate(order)}
    keys = [i for i, s in enumerate(samples) if s[2]]
    return keys, disp, len(samples)


def test_seek(name):
    base = BASES[name]
    w, h = stream_dims(base + ".mp4")
    keys, disp, n = keyframes_and_display(base + ".mp4")
    fb = frame_bytes(w, h)
    ref = open(base + ".ref.yuv", "rb").read()
    ok = 0
    for k in keys[1:3] + ([keys[0]] if keys else []):
        out = base + ".seek.yuv"
        r = decode(ASAN, "seek", base + ".smp", out, k)
        if r.returncode: fail("seek %s to %d: exit %d %s" % (name, k, r.returncode, r.stderr[-500:])); continue
        got = open(out, "rb").read()
        start = disp[k]
        if got != ref[start * fb:]:
            fail("seek %s to sample %d (display %d): output differs (%d frames, expected %d)" % (name, k, start, len(got) // fb, len(ref) // fb - start))
        else: ok += 1
        os.remove(out)
    # seeking to a non-keyframe: frames must resume exactly at the next keyframe
    if len(keys) > 1:
        k = keys[1] - 2
        out = base + ".seek.yuv"
        r = decode(ASAN, "seek", base + ".smp", out, k)
        got = open(out, "rb").read() if r.returncode == 0 else b""
        start = disp[keys[1]]
        if r.returncode or got != ref[start * fb:]:
            fail("seek %s to non-keyframe %d: expected resume at keyframe %d" % (name, k, keys[1]))
        else: ok += 1
        os.remove(out)
    return ok


def test_seek_gdr(name):
    """intra refresh: the MP4 sync samples are P pictures with a recovery point SEI. After h264_reset + feeding
    from one, the decoder must output nothing incomplete and be exact from the recovery point on."""
    base = BASES[name]
    w, h = stream_dims(base + ".mp4")
    keys, disp, n = keyframes_and_display(base + ".mp4")
    fb = frame_bytes(w, h)
    ref = open(base + ".ref.yuv", "rb").read()
    ok = 0
    for i, k in enumerate(keys[1:4]):
        nxt = keys[i + 2] if i + 2 < len(keys) else n
        out = base + ".seek.yuv"
        r = decode(ASAN, "seek", base + ".smp", out, k)
        got = open(out, "rb").read() if r.returncode == 0 else b""
        if r.returncode or not got or got != ref[len(ref) - len(got):] or len(got) // fb < (n - nxt):
            fail("seek (intra refresh) %s to sample %d: %d frames, exact suffix: %s" % (name, k, len(got) // fb, got == ref[len(ref) - len(got):]))
        else: ok += 1
        if os.path.exists(out): os.remove(out)
    return ok


def test_truncated(name):
    base = BASES[name]
    w, h = stream_dims(base + ".mp4")
    data = open(base + ".h264", "rb").read()
    fb = frame_bytes(w, h)
    ref = open(base + ".ref.yuv", "rb").read()
    nref = len(ref) // fb
    n = 0
    for frac in (0.13, 0.37, 0.61, 0.89, 0.995):
        cut = int(len(data) * frac)
        p = base + ".trunc.h264"
        open(p, "wb").write(data[:cut])
        out = base + ".trunc.yuv"
        r = decode(ASAN, "annexb", p, out)
        if r.returncode: fail("truncated %s at %d: exit %d %s" % (name, cut, r.returncode, r.stderr[-500:])); continue
        got = open(out, "rb").read()
        ng = len(got) // fb
        same = 0
        while same < ng and got[same * fb:(same + 1) * fb] == ref[same * fb:(same + 1) * fb]: same += 1
        # every picture completely contained must be exact, except the few around the cut (reordering)
        aus = data[:cut].count(b"\x00\x00\x01\x09") or data[:cut].count(b"\x00\x00\x01")
        if ng > nref or same + 6 < min(ng, int(nref * frac) - 2):
            fail("truncated %s at %.0f%%: %d frames out, %d exact (ref %d)" % (name, frac * 100, ng, same, nref))
        n += 1
        os.remove(p); os.remove(out)
    return n


def test_lost_nal(name):
    """drop one NAL unit of a P picture: the decoder must conceal, keep going and be exact again after the next IDR"""
    base = BASES[name]
    w, h = stream_dims(base + ".mp4")
    data = open(base + ".h264", "rb").read()
    starts = []
    i = data.find(b"\x00\x00\x01")
    while i >= 0:
        starts.append(i + 3)
        i = data.find(b"\x00\x00\x01", i + 3)
    nals = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        while e > s and data[e - 1] == 0: e -= 1
        nals.append((s, e))
    slices = [(s, e) for s, e in nals if data[s] & 31 == 1]
    idrs = [k for k, (s, e) in enumerate(nals) if data[s] & 31 == 5]
    if len(idrs) < 2 or not slices: return 0
    # remove a non-IDR slice between the first two IDRs
    cand = [k for k in range(idrs[0] + 1, idrs[1]) if data[nals[k][0]] & 31 == 1]
    k = cand[len(cand) // 2]
    s, e = nals[k]
    damaged = data[:s - 3] + data[e:]
    p = base + ".lost.h264"
    open(p, "wb").write(damaged)
    out = base + ".lost.yuv"
    r = decode(ASAN, "annexb", p, out)
    ok = 0
    if r.returncode:
        fail("lost NAL %s: exit %d %s" % (name, r.returncode, r.stderr[-500:]))
    else:
        fb = frame_bytes(w, h)
        keys, disp, n = keyframes_and_display(base + ".mp4")
        ref = open(base + ".ref.yuv", "rb").read()
        got = open(out, "rb").read()
        idr_disp = disp[keys[1]]
        # count of output frames may differ by one at most (a lost non-reference picture)
        tail = len(ref) - idr_disp * fb
        if len(got) < tail or got[len(got) - tail:] != ref[idr_disp * fb:]:
            fail("lost NAL %s: output after the next IDR is not exact" % name)
        else: ok = 1
    os.remove(p)
    if os.path.exists(out): os.remove(out)
    return ok


def test_resolution_change():
    if "cbp_cavlc" not in BASES or "main_cabac_b" not in BASES:
        return
    a, b = BASES["cbp_cavlc"], BASES["main_cabac_b"]
    c = os.path.join(CORPUS, "reschange")
    parts = [a, BASES["size_200x120"], b] if "size_200x120" in BASES else [a, b]
    open(c + ".h264", "wb").write(b"".join(open(p + ".h264", "rb").read() for p in parts))
    out = c + ".yuv"
    r = decode(ASAN, "annexb", c + ".h264", out)
    want = b"".join(open(p + ".ref.yuv", "rb").read() for p in parts)
    got = open(out, "rb").read() if os.path.exists(out) else b""
    if r.returncode or got != want:
        fail("resolution change: output differs (%d vs %d bytes) %s" % (len(got), len(want), r.stderr[-300:]))
    else:
        print("resolution change (%s): bit-exact" % " -> ".join(os.path.basename(p) for p in parts))


# -- POC type 1: rewrite the SPS of a stream without B-frames (POC type 2) into the equivalent POC type 1
class Bits:
    def __init__(self, data): self.d, self.p = data, 0
    def u(self, n):
        v = 0
        for _ in range(n):
            v = v << 1 | (self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1
            self.p += 1
        return v
    def ue(self):
        z = 0
        while self.u(1) == 0: z += 1
        return (1 << z) - 1 + self.u(z)
    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


class W:
    def __init__(self): self.b = []
    def u(self, n, v): self.b += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0); self.u(n, v)
    def se(self, v): self.ue(2 * v - 1 if v > 0 else -2 * v)


def unescape(b):
    out, z = bytearray(), 0
    for c in b:
        if z >= 2 and c == 3: z = 0; continue
        z = z + 1 if c == 0 else 0
        out.append(c)
    return bytes(out)


def escape(b):
    out, z = bytearray(), 0
    for c in b:
        if z >= 2 and c <= 3: out.append(3); z = 0
        out.append(c)
        z = z + 1 if c == 0 else 0
    return bytes(out)


def rewrite_sps_poc1(nal):
    rb = unescape(nal[1:])
    r = Bits(rb)
    w = W()
    prof = r.u(8); w.u(8, prof); w.u(8, r.u(8)); w.u(8, r.u(8)); w.ue(r.ue())
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = r.ue(); w.ue(cf)
        if cf == 3: w.u(1, r.u(1))
        w.ue(r.ue()); w.ue(r.ue()); w.u(1, r.u(1))
        if r.u(1): raise ValueError("scaling lists not handled")
        w.u(1, 0)
    w.ue(r.ue())                    # log2_max_frame_num_minus4
    pt = r.ue()
    if pt != 2: raise ValueError("expected POC type 2")
    w.ue(1); w.u(1, 1); w.se(0); w.se(0); w.ue(1); w.se(2)
    # copy the rest bit by bit (including the stop bit)
    total = len(rb) * 8
    while r.p < total: w.u(1, r.u(1))
    bits = w.b + [0] * (-len(w.b) % 8)
    out = bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
    return nal[:1] + escape(out)


def test_poc1():
    case = ("poc_type1_src", "rot", "320x240", 30, ["-profile:v", "high"], "bframes=0:ref=3", True)
    if encode(case): fail("poc1: encode failed"); return
    base = case_base(case)
    data = open(base + ".h264", "rb").read()
    out, i = bytearray(), 0
    pos = [m for m in range(len(data) - 3) if data[m:m + 3] == b"\x00\x00\x01"]
    for k, s in enumerate(pos):
        e = pos[k + 1] if k + 1 < len(pos) else len(data)
        nal = data[s + 3:e]
        while nal and nal[-1] == 0: nal = nal[:-1]
        if nal[0] & 31 == 7: nal = rewrite_sps_poc1(nal)
        out += b"\x00\x00\x00\x01" + nal
    p = base + ".poc1.h264"
    open(p, "wb").write(out)
    refp = base + ".poc1.ref.yuv"
    run(["ffmpeg", "-v", "error", "-y", "-i", p, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", refp])
    o = base + ".poc1.yuv"
    r = decode(ASAN, "annexb", p, o)
    w, h = dims(case[2])
    eq, na, nb, first = compare(o, refp, w, h)
    r2 = run(["ffprobe", "-v", "error", "-show_entries", "stream=profile", p])
    if r.returncode or not eq or open(refp, "rb").read() != open(base + ".ref.yuv", "rb").read():
        fail("POC type 1 stream: mismatch (%d vs %d frames, first diff %s) %s" % (na, nb, first, r.stderr[-300:]))
    else:
        print("POC type 1 (rewritten SPS): bit-exact, %d frames" % na)


# -- slice header rewriting (streams without B-frames, POC type 2): long-term references / MMCO / list
#    modification, and frame_num gaps. ffmpeg decodes the rewritten stream as the reference.
def split_nals(data):
    pos = [m for m in range(len(data) - 3) if data[m:m + 3] == b"\x00\x00\x01"]
    out = []
    for k, s_ in enumerate(pos):
        e = pos[k + 1] if k + 1 < len(pos) else len(data)
        nal = data[s_ + 3:e]
        while nal and nal[-1] == 0: nal = nal[:-1]
        out.append(nal)
    return out


def bits_to_bytes(bits):
    bits = bits + [0] * (-len(bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def parse_sps_min(rb):
    r = Bits(rb)
    prof = r.u(8); r.u(16); r.ue()
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = r.ue()
        if cf == 3: r.u(1)
        r.ue(); r.ue(); r.u(1)
        if r.u(1): raise ValueError("scaling lists")
    lmfn = r.ue() + 4
    pt = r.ue()
    if pt != 2: raise ValueError("POC type 2 expected")
    gaps_pos_before = None
    r.ue()                       # max_num_ref_frames
    gaps_pos = r.p
    return lmfn, gaps_pos


def parse_pps_min(rb):
    r = Bits(rb)
    r.ue(); r.ue()
    cabac = r.u(1); r.u(1)
    if r.ue(): raise ValueError("slice groups")
    r.ue(); r.ue()
    wp = r.u(1); r.u(2); r.se(); r.se(); r.se()
    dfc = r.u(1); r.u(1); red = r.u(1)
    if wp or red: raise ValueError("weighted pred / redundant pictures")
    return cabac, dfc


def rewrite_slice(nal, lmfn, cabac, dfc, longterm):
    ref_idc, typ = (nal[0] >> 5) & 3, nal[0] & 31
    rb = unescape(nal[1:])
    r, w = Bits(rb), W()
    w.ue(r.ue())
    st = r.ue(); w.ue(st)
    w.ue(r.ue())
    w.u(lmfn, r.u(lmfn))
    if typ == 5: w.ue(r.ue())
    sty = st % 5
    if sty == 0:
        if r.u(1): r.ue()
        w.u(1, 1); w.ue(0)                       # num_ref_idx_active_override: 1 reference
        if r.u(1):
            while True:
                idc = r.ue()
                if idc == 3: break
                r.ue()
        if longterm:
            w.u(1, 1); w.ue(2); w.ue(0); w.ue(3)  # ref_pic_list_modification: long_term_pic_num 0
        else:
            w.u(1, 1); w.ue(0); w.ue(0); w.ue(3)  # abs_diff_pic_num_minus1 = 0 (the previous frame)
    if ref_idc:
        if typ == 5:
            w.u(1, r.u(1)); r.u(1); w.u(1, 1 if longterm else 0)
        else:
            if r.u(1):
                while True:
                    op = r.ue()
                    if op == 0: break
                    if op in (1, 3, 2, 4, 6): r.ue()
                    if op == 3: r.ue()
            if longterm:
                w.u(1, 1); w.ue(4); w.ue(1); w.ue(6); w.ue(0); w.ue(0)   # MMCO 4 (max idx 0), MMCO 6 (idx 0), end
            else:
                w.u(1, 0)
    if cabac and sty != 2: w.ue(r.ue())
    w.se(r.se())
    if dfc:
        idc = r.ue(); w.ue(idc)
        if idc != 1: w.se(r.se()); w.se(r.se())
    if cabac:
        while r.p % 8: r.u(1)
        while len(w.b) % 8: w.u(1, 1)
        out = bits_to_bytes(w.b) + rb[r.p // 8:]
    else:
        total = len(rb) * 8
        rest = [(rb[i >> 3] >> (7 - (i & 7))) & 1 for i in range(r.p, total)]
        out = bits_to_bytes(w.b + rest)
    return nal[:1] + escape(out)


def set_sps_gaps(nal):
    rb = bytearray(unescape(nal[1:]))
    lmfn, gp = parse_sps_min(bytes(rb))
    rb[gp >> 3] |= 0x80 >> (gp & 7)
    return nal[:1] + escape(bytes(rb))


def rewritten_test(label, case, transform):
    if encode(case): fail(label + ": encode failed"); return
    base = case_base(case)
    nals = split_nals(open(base + ".h264", "rb").read())
    lmfn = cabac = dfc = None
    out = bytearray()
    try:
        for i, nal in enumerate(nals):
            t = nal[0] & 31
            if t == 7: lmfn = parse_sps_min(unescape(nal[1:]))[0]
            if t == 8: cabac, dfc = parse_pps_min(unescape(nal[1:]))
            nal = transform(i, nal, lmfn, cabac, dfc)
            if nal: out += b"\x00\x00\x00\x01" + nal
    except ValueError as e:
        fail("%s: cannot rewrite stream: %s" % (label, e)); return
    p = base + "." + label + ".h264"
    open(p, "wb").write(out)
    refp = base + "." + label + ".ref.yuv"
    run(["ffmpeg", "-v", "error", "-y", "-i", p, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", refp])
    o = base + "." + label + ".yuv"
    r = decode(ASAN, "annexb", p, o)
    w, h = dims(case[2])
    eq, na, nb, first = compare(o, refp, w, h)
    if r.returncode or not eq:
        fail("%s: mismatch with ffmpeg (%d vs %d frames, first diff %s) %s" % (label, na, nb, first, r.stderr[-300:]))
        return None
    print("%s: bit-exact with ffmpeg, %d frames" % (label, na))
    return refp


def test_rewritten():
    for cab in (0, 1):
        case = ("lt_src%d" % cab, "rot", "320x240", 30, ["-profile:v", "main" if cab else "baseline"],
                "bframes=0:ref=1:weightp=0:cabac=%d" % cab, True)
        for lt in (1, 0):
            label = ("longterm_mmco" if lt else "list_modification") + ("_cabac" if cab else "_cavlc")
            def tr(i, nal, lmfn, cabac, dfc, lt=lt):
                if nal[0] & 31 in (1, 5): return rewrite_slice(nal, lmfn, cabac, dfc, lt)
                return nal
            refp = rewritten_test(label, case, tr)
            if refp and open(refp, "rb").read() != open(case_base(case) + ".ref.yuv", "rb").read():
                fail("%s: rewritten stream does not decode to the original pictures" % label)
    # frame_num gaps: gaps_in_frame_num_allowed + dropped reference pictures ("non-existing" frames)
    case = ("gap_src", "rot", "320x240", 40, ["-profile:v", "baseline"], "bframes=0:ref=3:keyint=100", True)
    drop = {9, 10, 17, 25, 26, 27}
    state = {"slice": -1}
    def tr(i, nal, lmfn, cabac, dfc):
        t = nal[0] & 31
        if t == 7: return set_sps_gaps(nal)
        if t in (1, 5):
            state["slice"] += 1
            if state["slice"] in drop: return None
        return nal
    rewritten_test("frame_num_gaps", case, tr)


# -- generic copying parsers for x264 output (no scaling lists, frame coding): rewrite single fields
class Copier:
    def __init__(self, rb):
        self.r, self.w = Bits(rb), W()
        self.rb = rb
    def u(self, n, new=None):
        v = self.r.u(n); self.w.u(n, v if new is None else new); return v
    def ue(self, new=None):
        v = self.r.ue(); self.w.ue(v if new is None else new); return v
    def se(self, new=None):
        v = self.r.se(); self.w.se(v if new is None else new); return v
    def rest_bits(self):
        total = len(self.rb) * 8
        return self.w.b + [(self.rb[i >> 3] >> (7 - (i & 7))) & 1 for i in range(self.r.p, total)]


def copy_sps(rb, direct8x8=None):
    c = Copier(rb)
    prof = c.u(8); c.u(8); c.u(8); c.ue()
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = c.ue()
        if cf == 3: c.u(1)
        c.ue(); c.ue(); c.u(1)
        if c.u(1): raise ValueError("scaling lists")
    info = {"lmfn": c.ue() + 4}
    info["poc"] = c.ue()
    if info["poc"] == 0: info["lpoc"] = c.ue() + 4
    elif info["poc"] == 1: raise ValueError("POC type 1")
    c.ue(); c.u(1); c.ue(); c.ue()
    fmo = c.u(1)
    if not fmo: c.u(1)
    c.u(1, direct8x8)
    return info, bits_to_bytes(c.rest_bits())


def copy_pps(rb, bipred=None):
    c = Copier(rb)
    c.ue(); c.ue()
    info = {"cabac": c.u(1), "bfpo": c.u(1)}
    if c.ue(): raise ValueError("slice groups")
    info["nref"] = [c.ue() + 1, c.ue() + 1]
    info["wp"] = c.u(1)
    info["bipred"] = c.u(2, bipred)
    if bipred is not None: info["bipred"] = bipred
    c.se(); c.se(); c.se()
    info["dfc"] = c.u(1); c.u(1)
    if c.u(1): raise ValueError("redundant_pic_cnt")
    return info, bits_to_bytes(c.rest_bits())


def copy_slice(nal, sps, pps, inject_bweights=None):
    ref_idc, typ = (nal[0] >> 5) & 3, nal[0] & 31
    rb = unescape(nal[1:])
    c = Copier(rb)
    c.ue(); st = c.ue() % 5; c.ue()
    c.u(sps["lmfn"])
    if typ == 5: c.ue()
    if sps["poc"] == 0:
        c.u(sps["lpoc"])
        if pps["bfpo"]: c.se()
    if st == 1: c.u(1)
    nref = list(pps["nref"])
    if st in (0, 1):
        if c.u(1):
            nref[0] = c.ue() + 1
            if st == 1: nref[1] = c.ue() + 1
        for li in range(2 if st == 1 else 1):
            if c.u(1):
                while True:
                    idc = c.ue()
                    if idc == 3: break
                    c.ue()
    if (pps["wp"] and st == 0):
        lw = c.ue(); cw = c.ue()
        for i in range(nref[0]):
            if c.u(1): c.se(); c.se()
            if c.u(1):
                for j in range(4): c.se()
    if st == 1 and inject_bweights:
        R2 = inject_bweights
        c.w.ue(5); c.w.ue(4)
        for li in range(2):
            for i in range(nref[li]):
                f = R2.random() < 0.7
                c.w.u(1, f)
                if f: c.w.se(32 + R2.randint(-12, 12)); c.w.se(R2.randint(-10, 10))
                f = R2.random() < 0.5
                c.w.u(1, f)
                if f:
                    for j in range(2): c.w.se(16 + R2.randint(-6, 6)); c.w.se(R2.randint(-8, 8))
    if ref_idc:
        if typ == 5: c.u(1); c.u(1)
        elif c.u(1):
            while True:
                op = c.ue()
                if op == 0: break
                if op in (1, 2, 3, 4, 6): c.ue()
                if op == 3: c.ue()
    if pps["cabac"] and st != 2: c.ue()
    c.se()
    if pps["dfc"]:
        idc = c.ue()
        if idc != 1: c.se(); c.se()
    if pps["cabac"]:
        while c.r.p % 8: c.r.u(1)
        while len(c.w.b) % 8: c.w.u(1, 1)
        out = bits_to_bytes(c.w.b) + rb[c.r.p // 8:]
    else:
        out = bits_to_bytes(c.rest_bits())
    return nal[:1] + escape(out)


def test_rewritten_b():
    # direct_8x8_inference_flag = 0 (direct motion per 4x4), spatial and temporal; no 8x8 transform so that
    # the syntax does not change
    for direct in ("spatial", "temporal"):
        case = ("noinfer_src_" + direct, "rot", "320x240", 30, ["-profile:v", "main"],
                "bframes=3:b-pyramid=normal:ref=3:direct=%s:weightp=0:partitions=all:subme=9" % direct, True)
        st = {}
        def tr(i, nal, lmfn, cabac, dfc):
            t = nal[0] & 31
            if t == 7:
                info, rb = copy_sps(unescape(nal[1:]), direct8x8=0)
                st["sps"] = info
                return nal[:1] + escape(rb)
            if t == 8:
                st["pps"] = copy_pps(unescape(nal[1:]))[0]
            return nal
        rewritten_test_generic("direct_8x8_inference_0_" + direct, case, tr)
    # explicit weighted bi-prediction (weighted_bipred_idc = 1) with random weights / offsets
    case = ("bweight_src", "rot", "320x240", 30, ["-profile:v", "high"], "bframes=3:b-pyramid=normal:ref=3:weightp=2", True)
    st = {"R": random.Random(77)}
    def tr2(i, nal, lmfn, cabac, dfc):
        t = nal[0] & 31
        if t == 7:
            st["sps"] = copy_sps(unescape(nal[1:]))[0]
        if t == 8:
            info, rb = copy_pps(unescape(nal[1:]), bipred=1)
            st["pps"] = info
            return nal[:1] + escape(rb)
        if t in (1, 5):
            return copy_slice(nal, st["sps"], st["pps"], inject_bweights=st["R"])
        return nal
    rewritten_test_generic("explicit_weighted_bipred", case, tr2)


def rewritten_test_generic(label, case, transform):
    if encode(case): fail(label + ": encode failed"); return
    base = case_base(case)
    nals = split_nals(open(base + ".h264", "rb").read())
    out = bytearray()
    try:
        for i, nal in enumerate(nals):
            nal = transform(i, nal, None, None, None)
            if nal: out += b"\x00\x00\x00\x01" + nal
    except (ValueError, KeyError) as e:
        fail("%s: cannot rewrite stream: %s" % (label, e)); return
    p = base + "." + label + ".h264"
    open(p, "wb").write(out)
    refp = base + "." + label + ".ref.yuv"
    run(["ffmpeg", "-v", "error", "-y", "-i", p, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", refp])
    o = base + "." + label + ".yuv"
    r = decode(ASAN, "annexb", p, o)
    w, h = dims(case[2])
    eq, na, nb, first = compare(o, refp, w, h)
    if r.returncode or not eq or na == 0:
        fail("%s: mismatch with ffmpeg (%d vs %d frames, first diff %s) %s" % (label, na, nb, first, r.stderr[-300:]))
        return
    changed = open(refp, "rb").read() != open(base + ".ref.yuv", "rb").read()
    print("%s: bit-exact with ffmpeg, %d frames (%s)" % (label, na, "pictures differ from the original stream" if changed else "same pictures as the original"))


def test_pcm():
    n = 0
    for cabac in (0, 1):
        for wm, hm, seed in ((11, 9, 1), (20, 15, 2), (3, 2, 3)):
            p = os.path.join(CORPUS, "pcm_%d_%dx%d.h264" % (cabac, wm, hm))
            open(p, "wb").write(synth.make_pcm_stream(wm, hm, 4, cabac, seed))
            refp = p[:-5] + ".ref.yuv"
            run(["ffmpeg", "-v", "error", "-y", "-i", p, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", refp])
            o = p[:-5] + ".yuv"
            r = decode(ASAN, "annexb", p, o)
            eq, na, nb, first = compare(o, refp, wm * 16, hm * 16)
            if r.returncode or not eq or na != 4:
                fail("I_PCM %s %dx%d: mismatch (%d vs %d frames, first diff %s) %s" % ("CABAC" if cabac else "CAVLC", wm, hm, na, nb, first, r.stderr[-300:]))
            else: n += 1
    print("I_PCM synthetic streams (CAVLC + CABAC): %d/6 bit-exact" % n)


def test_unsupported():
    cases = [
        ("unsup_interlaced", "rot", "320x240", 6, ["-profile:v", "high", "-flags", "+ildct+ilme"], "interlaced=1", False),
        ("unsup_422", "rot", "320x240", 4, ["-profile:v", "high422", "-pix_fmt", "yuv422p"], "", False),
        ("unsup_444", "rot", "320x240", 4, ["-profile:v", "high444", "-pix_fmt", "yuv444p"], "", False),
        ("unsup_10bit", "rot", "320x240", 4, ["-profile:v", "high10", "-pix_fmt", "yuv420p10le"], "", False),
    ]
    for c in cases:
        name, src, size, n, args, params, _ = c
        base = os.path.join(CORPUS, name)
        cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", SOURCES[src].format(s=size, r=25), "-frames:v", str(n),
               "-c:v", "libx264"] + args
        if params: cmd += ["-x264-params", params]
        # pix_fmt must come from args (overrides)
        r = run(cmd + ["-bsf:v", "h264_mp4toannexb", "-f", "h264", base + ".h264"])
        if r.returncode:
            print("unsupported %s: could not encode (%s), skipped" % (name, r.stderr.strip()[-120:]))
            continue
        r = decode(ASAN, "annexb", base + ".h264", base + ".yuv")
        if r.returncode:
            fail("unsupported %s: crashed: %s" % (name, r.stderr[-500:]))
        elif "error:" not in r.stderr or "frames 0" not in r.stdout:
            fail("unsupported %s: not rejected (%s %s)" % (name, r.stdout.strip(), r.stderr.strip()))
        else:
            print("unsupported %-18s rejected: %s" % (name, r.stderr.strip().splitlines()[0]))


def test_fuzz():
    names = ["cbp_cavlc", "main_cabac_b", "high_cqmfile", "high_weightp2", "high_slices4", "high_lossless_qp0",
             "cbp_constrained_intra", "main_temporal", "high_8x8", "size_48x34"]
    names = [n for n in names if n in BASES]
    iters = 3 if QUICK else 12
    jobs = []
    for i, n in enumerate(names):
        jobs.append(([ASAN, "fuzz", BASES[n] + ".smp", str(1000 + i), str(iters)], n))
        jobs.append(([ASAN, "fuzzab", BASES[n] + ".h264", str(2000 + i), str(max(2, iters // 2))], n + "(annexb)"))
    def one(j):
        cmd, n = j
        r = run(cmd)
        return n, r
    total = 0
    with ThreadPoolExecutor(JOBS) as ex:
        for n, r in ex.map(one, jobs):
            if r.returncode or "fuzz ok" not in r.stdout:
                fail("fuzz %s: exit %d: %s" % (n, r.returncode, r.stderr[-1500:]))
            else:
                total += int(r.stdout.split()[2])
    print("fuzz: %d damaged decodes of %d streams under ASan+UBSan, no crashes" % (total, len(names)))


def test_speed():
    clips = [("speed_720p_rot", "rot", "1280x720", 150 if not QUICK else 60, ["-profile:v", "high", "-crf", "21"], "bframes=3:ref=4"),
             ("speed_720p_mandel", "mandel", "1280x720", 150 if not QUICK else 60, ["-profile:v", "high", "-crf", "21"], ""),
             ("speed_1080p_rot", "rot", "1920x1080", 120 if not QUICK else 40, ["-profile:v", "high", "-crf", "21"], "bframes=3:ref=4")]
    if not QUICK:
        clips.append(("speed_720p_baseline", "rot", "1280x720", 150, ["-profile:v", "baseline", "-crf", "21"], ""))
        clips.append(("speed_1080p_bars", "bars", "1920x1080", 120, ["-profile:v", "high", "-crf", "18"], "bframes=3"))
    for name, src, size, n, args, params in clips:
        case = (name, src, size, n, args, params, True)
        msg = encode(case)
        if msg: fail(name + ": " + msg); continue
        base = case_base(case)
        if True:
            r = decode(FAST, "samples", base + ".smp", base + ".yuv")
            w, h = dims(size)
            eq, na, nb, first = compare(base + ".yuv", base + ".ref.yuv", w, h)
            if not eq: fail("%s: output differs (first frame %s)" % (name, first))
            os.remove(base + ".yuv")
        r = run([FAST, "bench", base + ".smp", "3"])
        kbps = os.path.getsize(base + ".h264") * 8 / (n / 25.0) / 1000   # 25 fps clips
        fps = float(r.stdout.split("fps")[-1]) if "fps" in r.stdout else 0
        tfps = 0
        if os.path.exists(TARGETBIN):
            r = run([TARGETBIN, "bench", base + ".smp", "3"])
            tfps = float(r.stdout.split("fps")[-1]) if "fps" in r.stdout else 0
        print("speed %-20s %s %5.0f kbit/s: %6.1f fps host -O2, %6.1f fps with the AXIOM target flags (one core)" % (name, size, kbps, fps, tfps))


def main():
    t0 = time.time()
    build()
    if FAILS and any("target compile" in f for f in FAILS): pass
    write_cqm()
    print("== corpus (%d streams, %d jobs)" % (len(CASES), JOBS), flush=True)
    with ThreadPoolExecutor(JOBS) as ex:
        for fails, line in ex.map(check_case, CASES):
            print(line, flush=True)
            for f in fails: fail(f)
    if ONLY:
        print("done in %.0f s" % (time.time() - t0))
        sys.exit(1 if FAILS else 0)
    print("== plain C build (-DH264_NO_SIMD)", flush=True)
    nos = 0
    for n in ("main_cabac_b", "high_8x8", "high_weightp2", "main_weightb", "cbp_slices", "high_lossless_qp0"):
        if n not in BASES: continue
        base = BASES[n]
        w, h = stream_dims(base + ".mp4")
        r = decode(NOSIMD, "samples", base + ".smp", base + ".nosimd.yuv")
        eq, na, nb, first = compare(base + ".nosimd.yuv", base + ".ref.yuv", w, h)
        if r.returncode or not eq: fail("plain C build, %s: mismatch (first frame %s) %s" % (n, first, r.stderr[-300:]))
        else: nos += 1
        if os.path.exists(base + ".nosimd.yuv"): os.remove(base + ".nosimd.yuv")
    print("plain C build: %d streams bit-exact" % nos)
    print("== seeking / damage", flush=True)
    seeks = 0
    for n in ("cbp_cavlc", "high_open_gop", "high_short_gop", "main_cabac_b"):
        if n in BASES: seeks += test_seek(n)
    if "high_intra_refresh" in BASES: seeks += test_seek_gdr("high_intra_refresh")
    print("seek tests passed: %d" % seeks)
    tr = 0
    for n in ("cbp_cavlc", "main_cabac_b", "high_slices4", "high_open_gop"):
        if n in BASES: tr += test_truncated(n)
    print("truncated-file tests passed: %d" % tr)
    lost = 0
    for n in ("cbp_cavlc", "high_short_gop", "cbp_constrained_intra", "high_open_gop"):
        if n in BASES: lost += test_lost_nal(n)
    print("lost-NAL recovery tests passed: %d" % lost)
    test_resolution_change()
    test_poc1()
    test_rewritten()
    test_pcm()
    test_rewritten_b()
    test_unsupported()
    print("== fuzz", flush=True)
    test_fuzz()
    print("== speed", flush=True)
    test_speed()
    print("done in %.0f s" % (time.time() - t0))
    if FAILS:
        print("%d FAILURE(S)" % len(FAILS))
        for f in FAILS: print("  " + f)
        sys.exit(1)
    print("ALL PASSED")


if __name__ == "__main__":
    main()
