#!/usr/bin/env python3
"""Tests the AXIOM MP3 decoder (user/lib/audio) against ffmpeg's decoder.

Usage: python3 tests/mp3/run.py [--quick] [--keep] [--out DIR]

1. Builds tests/mp3/mp3_test.c + user/lib/audio/*.c for the host twice (ASan+UBSan for correctness,
   -O2 for speed), and compiles the decoder with the freestanding target flags (warnings are errors,
   `nm -u` must list only the allowed libc symbols, reports per-function stack use).
2. Synthesizes audio (sweeps, noise, chords, clicks, silence) and encodes MP3s with ffmpeg/libmp3lame
   (MPEG-1/2/2.5, all sample rates, mono/stereo/joint, CBR 8..320 kbps, VBR, ABR, ID3v2.3/2.4 with album art,
   ID3v1, no Xing tag), extra files through libmp3lame's C API (CRC-protected frames, forced short blocks,
   no bit reservoir, dual channel, forced MS, free format), Layer II files (ffmpeg mp2 encoder, twolame incl.
   intensity joint stereo), synthetic Layer I streams, synthetic Layer III streams exercising mixed blocks,
   intensity stereo and scfsi, and damaged files (truncated, garbage inserted/overwritten, junk before).
3. Decodes every file with ffmpeg (-f s16le) and with the decoder; prints max |diff| and SNR per file.
   Valid files must reach SNR >= 60 dB and the same length. Damaged files must not crash and must decode
   the intact parts. Also: seeking accuracy, ID3 tags, a mutation fuzz pass under ASan, decode speed.
Exits non-zero on any failure."""
import ctypes, ctypes.util, os, random, re, subprocess, sys, tempfile, threading, time, wave
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "user", "lib", "audio")
QUICK = "--quick" in sys.argv
OUT = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(tempfile.gettempdir(), "axiom-mp3test")
CORPUS = os.path.join(OUT, "corpus")
os.makedirs(CORPUS, exist_ok=True)
MIN_SNR = 60.0
FAILS = []
R = random.Random(1234)
NP = np.random.default_rng(1234)


def fail(msg):
    FAILS.append(msg)
    print("FAIL:", msg)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)


# ---------------------------------------------------------------- build
CSRC = sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".c"))
DRIVER = os.path.join(ROOT, "tests", "mp3", "mp3_test.c")
ASAN = os.path.join(OUT, "mp3_test_asan")
FAST = os.path.join(OUT, "mp3_test")
TARGET_FLAGS = ("-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -m64 "
                "-mcmodel=large -msse2 -mfpmath=sse -fno-tree-loop-distribute-patterns -Wall -Wextra "
                "-Wno-unused-parameter").split()
ALLOWED = {"memset", "memcpy", "memmove", "memcmp", "strlen", "strcmp", "strncmp", "malloc", "free", "realloc", "calloc"}


def build():
    print("== build")
    r = run(["gcc", "-std=gnu11", "-O1", "-g", "-DMP3_HOST_TEST", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Wno-unused-parameter", "-o", ASAN, DRIVER] + CSRC)
    if r.returncode: print(r.stderr); sys.exit("asan build failed")
    r = run(["gcc", "-std=gnu11", "-O2", "-DMP3_HOST_TEST", "-o", FAST, DRIVER] + CSRC)
    if r.returncode: print(r.stderr); sys.exit("host build failed")
    objs, undef, stack = [], set(), []
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
        r2 = run(["nm", "--defined-only"] + objs)
        for l in r2.stdout.splitlines():
            p = l.split()
            if len(p) == 3: defined.add(p[2])
        ext = undef - defined
        bad = ext - ALLOWED
        print("target compile: %d files, warning-free; external symbols: %s" % (len(objs), " ".join(sorted(ext))))
        if bad: fail("disallowed external symbols: %s" % sorted(bad))
        stack.sort(reverse=True)
        print("largest stack frames (bytes):", ", ".join("%s=%d%s" % (f, n, "" if k == "static" else "(" + k + ")") for n, f, k in stack[:6]))
        if stack and stack[0][0] > 16384: fail("stack frame over 16 KiB: %s" % (stack[0],))


# ---------------------------------------------------------------- signals
def sig(kind, sr, ch, dur):
    n = int(sr * dur)
    t = np.arange(n) / sr
    if kind == "sweep":
        f0, f1 = 30.0, 0.45 * sr
        k = np.log(f1 / f0) / dur
        ph = 2 * np.pi * f0 * (np.exp(k * t) - 1) / k
        L = 0.5 * np.sin(ph)
        R_ = 0.4 * np.sin(ph * 0.97 + 1.0)
    elif kind == "noise":
        a = NP.standard_normal(n)
        b = NP.standard_normal(n)
        env = 0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t)
        L = 0.18 * env * a
        R_ = 0.18 * env * (0.7 * a + 0.3 * b)
    elif kind == "chord":
        L = np.zeros(n); R_ = np.zeros(n)
        notes = [220, 277.18, 329.63, 440, 293.66, 369.99, 196, 246.94, 392, 523.25]
        step = int(0.5 * sr)
        for i, s in enumerate(range(0, n, step)):
            seg = np.arange(n - s) / sr
            env = np.exp(-seg * 3.0) * np.minimum(1, seg * 2000)
            for j in range(3):
                f = notes[(i + 2 * j) % len(notes)]
                for hmn in range(1, 8):
                    if f * hmn >= sr / 2: break
                    v = env * np.sin(2 * np.pi * f * hmn * seg) * 0.12 / hmn
                    pan = (j + 1) / 4
                    L[s:] += v * (1 - pan)
                    R_[s:] += v * pan
    elif kind == "clicks":
        L = 0.05 * np.sin(2 * np.pi * 150 * t)
        R_ = 0.05 * np.sin(2 * np.pi * 190 * t)
        for s in range(int(0.05 * sr), n, int(0.23 * sr)):
            m = min(n, s + int(0.004 * sr))
            burst = NP.standard_normal(m - s) * 0.7 * np.exp(-np.arange(m - s) / (0.0008 * sr))
            L[s:m] += burst
            R_[s:m] -= burst * 0.8
    elif kind == "silence":
        L = np.zeros(n); R_ = np.zeros(n)
    else:
        raise ValueError(kind)
    x = np.stack([L, R_], 1) if ch == 2 else ((L + R_) * 0.5)[:, None]
    return np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16)


def write_wav(path, pcm, sr):
    with wave.open(path, "wb") as w:
        w.setnchannels(pcm.shape[1]); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(pcm.tobytes())


DUR = 2.5 if QUICK else 4.0


_WAV_LOCK = threading.Lock()


def wav_for(kind, sr, ch, dur=DUR):
    p = os.path.join(CORPUS, "src_%s_%d_%d_%g.wav" % (kind, sr, ch, dur))
    with _WAV_LOCK:   # encoder jobs run in threads
        if not os.path.exists(p):
            write_wav(p + ".tmp", sig(kind, sr, ch, dur), sr)
            os.replace(p + ".tmp", p)
    return p


# ---------------------------------------------------------------- test corpus
CASES = []   # (name, path, kind) kind: valid | truncated | garbage


def ff_encode(name, kind, sr, ch, args, ext="mp3", extra_in=None, pre=None, dur=DUR):
    path = os.path.join(CORPUS, name + "." + ext)
    src = wav_for(kind, sr, ch, dur)
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", src]
    if extra_in: cmd += extra_in
    cmd += args + [path]
    r = run(cmd)
    if r.returncode:
        fail("ffmpeg encode %s: %s" % (name, r.stderr.strip()[-300:]))
        return None
    return path


def lame_encode(name, kind, sr, ch, brate=128, mode=1, crc=0, short=0, ms=0, nores=0, free=0, vbr=None):
    """Encodes through libmp3lame's C API (features ffmpeg's wrapper does not expose). No Xing tag."""
    lib = ctypes.CDLL(ctypes.util.find_library("mp3lame") or "libmp3lame.so.0")
    lib.lame_init.restype = ctypes.c_void_p
    g = ctypes.c_void_p(lib.lame_init())
    lib.lame_set_in_samplerate(g, sr); lib.lame_set_out_samplerate(g, sr)
    lib.lame_set_num_channels(g, ch)
    lib.lame_set_mode(g, 3 if ch == 1 else mode)
    lib.lame_set_bWriteVbrTag(g, 0)
    if vbr is not None:
        lib.lame_set_VBR(g, 4); lib.lame_set_VBR_q(g, vbr)
    else:
        lib.lame_set_brate(g, brate)
    if crc: lib.lame_set_error_protection(g, 1)
    if short: lib.lame_set_force_short_blocks(g, 1)
    if ms: lib.lame_set_force_ms(g, 1)
    if nores: lib.lame_set_disable_reservoir(g, 1)
    if free: lib.lame_set_free_format(g, 1)
    if lib.lame_init_params(g) < 0:
        fail("lame_init_params %s" % name)
        return None
    pcm = sig(kind, sr, ch, DUR)
    L = np.ascontiguousarray(pcm[:, 0]); Rr = np.ascontiguousarray(pcm[:, -1])
    buf = ctypes.create_string_buffer(len(L) * 2 + 20000)
    out = b""
    step = 4608
    for s in range(0, len(L), step):
        l = L[s:s + step]; r = Rr[s:s + step]
        n = lib.lame_encode_buffer(g, l.ctypes.data_as(ctypes.c_void_p), r.ctypes.data_as(ctypes.c_void_p),
                                   len(l), buf, len(buf))
        if n < 0: fail("lame_encode_buffer %s: %d" % (name, n)); return None
        out += buf.raw[:n]
    n = lib.lame_encode_flush(g, buf, len(buf))
    out += buf.raw[:max(n, 0)]
    lib.lame_close(g)
    path = os.path.join(CORPUS, name + ".mp3")
    open(path, "wb").write(out)
    return path


def add(name, path, kind="valid"):
    if path: CASES.append((name, path, kind))


def make_corpus():
    print("== generating corpus in", CORPUS)
    jobs = []
    M1 = [(44100, "s44"), (48000, "s48"), (32000, "s32")]
    # name, sr, ch, joint, rate (int kbps or 'qN' VBR), signal
    plan = [
        # MPEG-1
        (44100, 1, 0, 32, "sweep"), (44100, 2, 1, 40, "noise"), (44100, 1, 0, 56, "chord"), (44100, 2, 1, 64, "chord"),
        (44100, 2, 1, 128, "chord"), (44100, 2, 0, 128, "noise"), (44100, 2, 1, 192, "noise"), (44100, 2, 0, 256, "chord"),
        (44100, 2, 1, 320, "sweep"), (44100, 2, 0, 320, "clicks"), (44100, 2, 1, 128, "silence"),
        (48000, 1, 0, 48, "noise"), (48000, 2, 1, 96, "sweep"), (48000, 2, 0, 160, "chord"), (48000, 2, 1, 224, "clicks"),
        (48000, 2, 1, 320, "noise"), (32000, 2, 1, 32, "chord"), (32000, 1, 0, 80, "clicks"), (32000, 2, 0, 112, "sweep"),
        (32000, 2, 1, 256, "noise"),
        (44100, 2, 1, "q0", "chord"), (44100, 2, 0, "q2", "noise"), (44100, 2, 1, "q4", "sweep"), (44100, 1, 0, "q6", "chord"),
        (44100, 2, 1, "q9", "clicks"), (44100, 2, 1, "q4", "silence"), (48000, 2, 1, "q3", "noise"), (32000, 2, 1, "q5", "chord"),
        # MPEG-2
        (22050, 1, 0, 8, "sweep"), (22050, 2, 1, 32, "chord"), (22050, 2, 1, 64, "noise"), (22050, 2, 0, 160, "sweep"),
        (22050, 2, 1, "q2", "chord"), (22050, 1, 0, "q7", "clicks"), (24000, 1, 0, 24, "chord"), (24000, 2, 1, 96, "noise"),
        (24000, 2, 0, 144, "clicks"), (24000, 2, 1, "q4", "sweep"), (16000, 2, 1, 16, "chord"), (16000, 1, 0, 48, "noise"),
        (16000, 2, 1, 128, "sweep"), (16000, 2, 1, "q5", "noise"),
        # MPEG-2.5
        (11025, 1, 0, 8, "chord"), (11025, 2, 1, 32, "noise"), (11025, 2, 0, 64, "sweep"), (11025, 2, 1, "q4", "chord"),
        (12000, 2, 1, 16, "clicks"), (12000, 1, 0, 64, "sweep"), (12000, 2, 1, "q6", "noise"), (8000, 1, 0, 8, "noise"),
        (8000, 2, 1, 24, "chord"), (8000, 2, 0, 64, "clicks"), (8000, 2, 1, "q3", "sweep"),
    ]
    if QUICK: plan = plan[::3]
    for sr, ch, joint, rate, kind in plan:
        mode = "mono" if ch == 1 else ("joint" if joint else "stereo")
        rs = ("cbr%d" % rate) if isinstance(rate, int) else ("vbr" + rate)
        name = "l3_%d_%s_%s_%s" % (sr, mode, rs, kind)
        args = ["-c:a", "libmp3lame"]
        args += ["-b:a", "%dk" % rate] if isinstance(rate, int) else ["-q:a", rate[1:]]
        if ch == 2: args += ["-joint_stereo", str(joint)]
        jobs.append((name, kind, sr, ch, args))
    # ABR and no-Xing
    jobs.append(("l3_44100_joint_abr160_chord", "chord", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "160k", "-abr", "1"]))
    jobs.append(("l3_44100_joint_cbr128_noxing_noise", "noise", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0"]))
    # Layer II (ffmpeg's native encoder, and twolame)
    l2 = [(48000, 2, "mp2", 192, "chord"), (48000, 1, "mp2", 64, "noise"), (44100, 2, "mp2", 128, "sweep"),
          (44100, 2, "mp2", 384, "clicks"), (32000, 1, "mp2", 48, "chord"), (32000, 2, "mp2", 64, "noise"),
          (44100, 2, "mp2", 64, "clicks"), (24000, 2, "mp2", 96, "sweep"), (22050, 1, "mp2", 32, "chord"),
          (16000, 2, "mp2", 64, "noise"), (48000, 2, "libtwolame", 256, "noise"), (44100, 2, "libtwolame", 96, "sweep"),
          (22050, 2, "libtwolame", 64, "clicks"), (48000, 1, "libtwolame", 32, "chord")]
    if QUICK: l2 = l2[::2]
    for sr, ch, enc, rate, kind in l2:
        name = "l2_%d_%s_%s%d_%s" % (sr, "mono" if ch == 1 else "stereo", "tl" if enc == "libtwolame" else "ff", rate, kind)
        jobs.append((name, kind, sr, ch, ["-c:a", enc, "-b:a", "%dk" % rate, "-f", "mp2"]))
    jobs.append(("l2_44100_joint_tl128_chord", "chord", 44100, 2, ["-c:a", "libtwolame", "-b:a", "128k", "-mode", "joint_stereo", "-f", "mp2"]))
    jobs.append(("l2_32000_joint_tl64_noise", "noise", 32000, 2, ["-c:a", "libtwolame", "-b:a", "64k", "-mode", "joint_stereo", "-f", "mp2"]))

    def do(j):
        name, kind, sr, ch, args = j
        return name, ff_encode(name, kind, sr, ch, args, ext="mp2" if name.startswith("l2") else "mp3")
    with ThreadPoolExecutor(4) as ex:
        for name, p in ex.map(do, jobs):
            add(name, p)

    # tags / album art
    cover = os.path.join(CORPUS, "cover.png")
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240", "-frames:v", "1", cover])
    meta = ["-metadata", "title=Test Title", "-metadata", "artist=The Artist", "-metadata", "album=Some Album"]
    add("tag_id3v23_art", ff_encode("tag_id3v23_art", "chord", 44100, 2, ["-map", "0", "-map", "1", "-c:a", "libmp3lame", "-b:a", "128k",
        "-c:v", "copy", "-id3v2_version", "3", "-metadata:s:v", "comment=Cover (front)"] + meta, extra_in=["-i", cover]))
    meta_u = ["-metadata", "title=Ünïcode – 日本語 🎵", "-metadata", "artist=Björk", "-metadata", "album=Ålbum"]
    add("tag_id3v24_art_utf8", ff_encode("tag_id3v24_art_utf8", "noise", 48000, 2, ["-map", "0", "-map", "1", "-c:a", "libmp3lame",
        "-q:a", "3", "-c:v", "copy", "-id3v2_version", "4"] + meta_u, extra_in=["-i", cover]))
    add("tag_id3v23_utf16", ff_encode("tag_id3v23_utf16", "sweep", 22050, 1, ["-c:a", "libmp3lame", "-b:a", "48k",
        "-id3v2_version", "3"] + meta_u))
    def id3v1(t, a, al):
        f = lambda x: x.encode("latin-1")[:30].ljust(30, b"\0")
        return b"TAG" + f(t) + f(a) + f(al) + b"2024" + bytes(30) + bytes([12])
    p = ff_encode("tag_id3v1_only", "chord", 32000, 2, ["-c:a", "libmp3lame", "-b:a", "96k", "-id3v2_version", "0"])
    if p: open(p, "ab").write(id3v1("Caf\xe9 Title", "The Artist   ", "Some Album")); add("tag_id3v1_only", p)
    p = ff_encode("tag_id3v24_and_v1", "clicks", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "160k", "-id3v2_version", "4"] + meta)
    if p: open(p, "ab").write(id3v1("Other", "Other", "")); add("tag_id3v24_and_v1", p)

    # libmp3lame C API: CRC, forced short blocks, no reservoir, dual channel, forced MS, free format
    add("lame_crc_44100_joint_128_chord", lame_encode("lame_crc_44100_joint_128_chord", "chord", 44100, 2, 128, crc=1))
    add("lame_crc_22050_mono_48_noise", lame_encode("lame_crc_22050_mono_48_noise", "noise", 22050, 1, 48, crc=1))
    add("lame_crc_8000_joint_vbr_sweep", lame_encode("lame_crc_8000_joint_vbr_sweep", "sweep", 8000, 2, crc=1, vbr=4))
    add("lame_short_48000_joint_192_chord", lame_encode("lame_short_48000_joint_192_chord", "chord", 48000, 2, 192, short=1))
    add("lame_short_22050_stereo_64_noise", lame_encode("lame_short_22050_stereo_64_noise", "noise", 22050, 2, 64, mode=0, short=1))
    add("lame_short_8000_mono_16_sweep", lame_encode("lame_short_8000_mono_16_sweep", "sweep", 8000, 1, 16, short=1))
    add("lame_short_11025_joint_32_clicks", lame_encode("lame_short_11025_joint_32_clicks", "clicks", 11025, 2, 32, short=1))
    add("lame_nores_44100_joint_128_clicks", lame_encode("lame_nores_44100_joint_128_clicks", "clicks", 44100, 2, 128, nores=1))
    add("lame_dual_44100_192_chord", lame_encode("lame_dual_44100_192_chord", "chord", 44100, 2, 192, mode=2))
    add("lame_forcems_32000_96_noise", lame_encode("lame_forcems_32000_96_noise", "noise", 32000, 2, 96, ms=1))
    add("lame_free_44100_joint_96_chord", lame_encode("lame_free_44100_joint_96_chord", "chord", 44100, 2, 96, free=1))

    # synthetic streams (features no available encoder produces)
    import synth_streams
    for name, data in synth_streams.make_all(R, quick=QUICK):
        p = os.path.join(CORPUS, name)
        open(p, "wb").write(data)
        add(name.rsplit(".", 1)[0], p)

    # damaged files
    base = os.path.join(CORPUS, "l3_44100_joint_cbr128_chord.mp3")
    if not os.path.exists(base):
        base = ff_encode("l3_44100_joint_cbr128_chord", "chord", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k"])
    vb = ff_encode("dmg_src_vbr", "noise", 44100, 2, ["-c:a", "libmp3lame", "-q:a", "2", "-write_xing", "0"])
    data = open(base, "rb").read()
    vdata = open(vb, "rb").read()
    def put(name, b, kind):
        p = os.path.join(CORPUS, name + ".mp3"); open(p, "wb").write(b); add(name, p, kind)
    put("dmg_truncated", data[: int(len(data) * 0.6) + 77], "truncated")
    put("dmg_truncated_vbr", vdata[: int(len(vdata) * 0.45) + 13], "truncated")
    mid = len(vdata) // 2
    put("dmg_garbage_inserted", vdata[:mid] + bytes(R.randrange(256) for _ in range(3000)) + vdata[mid:], "garbage")
    m2 = len(data) // 2
    put("dmg_garbage_overwrite", data[:m2] + bytes(R.randrange(256) for _ in range(1500)) + data[m2 + 1500:], "garbage")
    put("dmg_zeros_inserted", vdata[:mid + 333] + bytes(4096) + vdata[mid + 333:], "garbage")
    put("dmg_junk_before", bytes(R.randrange(256) for _ in range(5000)) + vdata, "junk")


# ---------------------------------------------------------------- compare
def ffdecode(path, raw):
    # ffmpeg's default Layer I/II decoders are fixed point; use its float decoders like for Layer III
    dec = {".mp2": ["-c:a", "mp2float"], ".mp1": ["-c:a", "mp1float"]}.get(os.path.splitext(path)[1], [])
    r = run(["ffmpeg", "-v", "error", "-y"] + dec + ["-i", path, "-f", "s16le", "-acodec", "pcm_s16le", raw])
    return r.returncode == 0 or os.path.exists(raw)


_M123 = None


def mpg123_decode(path):
    """Second reference decoder (libmpg123 via ctypes; gapless trimming off). None if unavailable."""
    global _M123
    try:
        if _M123 is None:
            _M123 = ctypes.CDLL(ctypes.util.find_library("mpg123") or "libmpg123.so.0")
            _M123.mpg123_new.restype = ctypes.c_void_p
            _M123.mpg123_init()
        L = _M123
        err = ctypes.c_int()
        h = ctypes.c_void_p(L.mpg123_new(None, ctypes.byref(err)))
        L.mpg123_param(h, 13, ctypes.c_long(0x40), ctypes.c_double(0))   # MPG123_REMOVE_FLAGS, MPG123_GAPLESS
        L.mpg123_param(h, 12, ctypes.c_long(0x20), ctypes.c_double(0))   # MPG123_ADD_FLAGS, MPG123_QUIET
        if L.mpg123_open(h, path.encode()) != 0: return None
        buf = ctypes.create_string_buffer(1 << 16); done = ctypes.c_size_t(); out = []
        while True:
            r = L.mpg123_read(h, buf, len(buf), ctypes.byref(done))
            out.append(buf.raw[:done.value])
            if r not in (0, -10, -11): break
        L.mpg123_close(h); L.mpg123_delete(h)
        return np.frombuffer(b"".join(out), np.int16)
    except OSError:
        return None


def mydecode(exe, path, raw):
    r = run([exe, "decode", path, raw], timeout=600)
    info = {}
    for line in r.stdout.splitlines():
        for k, v in re.findall(r"(\w+)=(-?\d+)", line): info[k] = int(v)
    return r, info


def snr(ref, out):
    n = min(len(ref), len(out))
    if n == 0: return float("nan"), 0
    a = ref[:n].astype(np.float64); b = out[:n].astype(np.float64)
    d = a - b
    md = int(np.abs(d).max())
    e = (d * d).sum()
    s = (a * a).sum()
    if e == 0: return float("inf"), md
    if s == 0: return -float("inf"), md
    return 10 * np.log10(s / e), md


def best_lag(ref, out, ch, maxlag=2400):
    """Finds the lag (in samples per channel) that aligns out with ref; out[i+lag] ~ ref[i]."""
    a = ref.reshape(-1, ch)[:, 0].astype(np.float64)
    b = out.reshape(-1, ch)[:, 0].astype(np.float64)
    n = min(len(a), len(b), 48000 * 4)
    if n < 100: return 0
    best, bl = -1, 0
    seg = a[maxlag:n - maxlag] if n > 3 * maxlag else a[: n // 2]
    off = maxlag if n > 3 * maxlag else 0
    for lag in range(-min(maxlag, off), min(maxlag, len(b) - off - len(seg)) + 1):
        c = np.dot(seg, b[off + lag: off + lag + len(seg)])
        if c > best: best, bl = c, lag
    return bl


RESULTS = []


def check_case(case):
    name, path, kind = case
    ref_raw = os.path.join(OUT, name + ".ref.raw")
    out_raw = os.path.join(OUT, name + ".out.raw")
    ffdecode(path, ref_raw)
    r, info = mydecode(ASAN, path, out_raw)
    res = {"name": name, "kind": kind, "info": info}
    if r.returncode != 0 or "runtime error" in r.stderr or "AddressSanitizer" in r.stderr:
        res["error"] = "decoder exit %d %s" % (r.returncode, (r.stdout + r.stderr)[-400:])
        return res
    ch = info.get("channels", 1)
    ref = np.fromfile(ref_raw, np.int16) if os.path.exists(ref_raw) else np.zeros(0, np.int16)
    out = np.fromfile(out_raw, np.int16)
    res["len_ref"], res["len_out"] = len(ref) // max(ch, 1), len(out) // max(ch, 1)
    if kind == "valid" and not (snr(ref, out)[0] >= MIN_SNR and len(ref) == len(out)):
        # ffmpeg cannot open free-format files and deviates from ISO in a few corner cases (README.md);
        # libmpg123 then serves as the reference
        m = mpg123_decode(path)
        if m is not None and len(m) == len(out) and snr(m, out)[0] >= MIN_SNR:
            ref = m
            res["refname"] = "mpg123"
            res["len_ref"] = len(ref) // max(ch, 1)
    if kind in ("valid", "junk"):
        s, md = snr(ref, out)
        lag = 0
        if s < MIN_SNR and kind == "junk":
            lag = best_lag(ref, out, ch)
            if lag > 0: s, md = snr(ref, out[lag * ch:])
            elif lag < 0: s, md = snr(ref[-lag * ch:], out)
        res.update(snr=s, maxdiff=md, lag=lag)
        if not (s >= MIN_SNR): res["error"] = "SNR %.1f dB < %g" % (s, MIN_SNR)
        elif kind == "valid" and len(ref) != len(out): res["error"] = "length %d != ffmpeg %d" % (res["len_out"], res["len_ref"])
    elif kind == "truncated":
        n = min(len(ref), len(out)) - 1152 * 2 * ch
        s, md = snr(ref[:n], out[:n])
        res.update(snr=s, maxdiff=md, note="prefix of %d samples" % (n // ch))
        if not (s >= MIN_SNR): res["error"] = "prefix SNR %.1f" % s
        if len(out) < 0.9 * len(ref): res["error"] = "decoded too little (%d vs %d)" % (len(out), len(ref))
    else:  # garbage: the part before the damage must match, the end must match again after resync
        n = int(min(len(ref), len(out)) * 0.4)
        s, md = snr(ref[:n], out[:n])
        sr = info.get("sample_rate", 44100)
        spf = 384 if info.get("layer") == 1 else 576 if (info.get("layer") == 3 and sr < 32000) else 1152
        R2, O2 = ref.reshape(-1, ch), out.reshape(-1, ch)
        st, lag = -float("inf"), 0
        for k in range(-8, 9):   # resync shifts the timeline by whole frames
            L = k * spf
            a_off, b_off = (L, 0) if L > 0 else (0, -L)   # samples dropped from the end of ref / out
            m = min(sr, len(R2) - a_off, len(O2) - b_off)
            if m <= 0: continue
            v, _ = snr(R2[len(R2) - a_off - m:len(R2) - a_off].ravel(), O2[len(O2) - b_off - m:len(O2) - b_off].ravel())
            if v > st: st, lag = v, L
        res.update(snr=s, maxdiff=md, note="head SNR; tail SNR after resync %s dB (offset %d), length %d vs ffmpeg %d"
                   % (fmt_snr(st), lag, len(out) // ch, len(ref) // ch))
        if not (s >= MIN_SNR): res["error"] = "head SNR %.1f" % s
        if len(out) < 0.85 * len(ref): res["error"] = "decoded too little after damage (%d vs %d)" % (len(out), len(ref))
        if not (st >= 40): res["error"] = "no resync: tail SNR %.1f" % st
    return res


def fmt_snr(s):
    if s == float("inf"): return "exact"
    if s != s: return "n/a"
    return "%.1f" % s


def compare_all():
    print("== decoding %d files with ffmpeg and the decoder (ASan build)" % len(CASES))
    with ThreadPoolExecutor(4) as ex:
        res = list(ex.map(check_case, CASES))
    print("%-44s %-9s %6s %5s %4s %9s %8s %s" % ("file", "kind", "rate", "ch", "kbps", "samples", "maxdiff", "SNR dB"))
    for r in res:
        i = r["info"]
        line = "%-44s %-9s %6s %5s %4s %9s %8s %s" % (r["name"][:44], r["kind"], i.get("sample_rate", "-"), i.get("channels", "-"),
                                                       i.get("bitrate_kbps", "-"), r.get("len_out", "-"), r.get("maxdiff", "-"),
                                                       fmt_snr(r.get("snr", float("nan"))))
        if r.get("lag"): line += " (lag %d)" % r["lag"]
        if r.get("refname"): line += " (vs %s)" % r["refname"]
        if r.get("note"): line += "  [%s]" % r["note"]
        print(line)
        if "error" in r: fail("%s: %s" % (r["name"], r["error"]))
    RESULTS.extend(res)
    vals = [r["snr"] for r in res if r["kind"] == "valid" and "snr" in r and r["snr"] != float("inf")]
    if vals: print("valid files: min SNR %.1f dB, median %.1f dB, %d exact" % (min(vals), float(np.median(vals)),
               sum(1 for r in res if r.get("snr") == float("inf"))))


# ---------------------------------------------------------------- seek / tags / info / fuzz / speed
def test_info():
    print("== stream info (vs ffprobe / ffmpeg decode length)")
    names = ["l3_44100_joint_cbr128_chord", "l3_22050_mono_cbr8_sweep", "l3_44100_joint_vbrq0_chord", "l3_8000_mono_cbr8_noise",
             "l3_44100_joint_cbr128_noxing_noise", "l2_48000_stereo_ff192_chord", "tag_id3v24_art_utf8", "synth_l1_48000_joint"]
    for name in names:
        c = [x for x in CASES if x[0] == name]
        if not c: continue
        p = c[0][1]
        r = run([FAST, "open", p])
        i = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", r.stdout))
        pr = run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate,channels,bit_rate,codec_name",
                  "-of", "default=nw=1", p]).stdout
        f = dict(l.split("=", 1) for l in pr.splitlines() if "=" in l)
        ref = [x for x in RESULTS if x["name"] == name]
        exp_ms = ref[0]["len_ref"] * 1000 // int(f["sample_rate"]) if ref else None
        layer = {"mp3": 3, "mp2": 2, "mp1": 1}.get(f.get("codec_name"), 0)
        ok = i.get("sample_rate") == int(f["sample_rate"]) and i.get("channels") == int(f["channels"]) and i.get("layer") == layer
        fb = int(f["bit_rate"]) // 1000 if f.get("bit_rate", "N/A").isdigit() else None
        if fb: ok = ok and abs(i.get("bitrate_kbps", 0) - fb) <= max(2, fb // 50)
        if exp_ms is not None: ok = ok and abs(i.get("duration_ms", 0) - exp_ms) <= 30
        print("%-36s %s  [ffprobe %s Hz %s ch %s kbps; ffmpeg length %s ms] %s" % (name, r.stdout.strip(), f.get("sample_rate"),
              f.get("channels"), fb, exp_ms, "ok" if ok else "MISMATCH"))
        if not ok: fail("info %s" % name)
    # things that are not MPEG audio must be rejected
    junk = {"random": bytes(R.randrange(256) for _ in range(20000)), "zeros": bytes(5000), "png": open(os.path.join(CORPUS, "cover.png"), "rb").read(),
            "wav": open(wav_for("chord", 44100, 2), "rb").read()[:200000], "id3_only": b"ID3\x04\x00\x00\x00\x00\x01\x00" + bytes(128), "empty": b""}
    for k, v in junk.items():
        p = os.path.join(CORPUS, "notmp3_" + k)
        open(p, "wb").write(v)
        r = run([ASAN, "open", p])
        ok = r.returncode == 1 and "open failed" in r.stdout
        print("%-36s %s" % ("not MPEG audio: " + k, "rejected" if ok else "ACCEPTED/CRASH " + (r.stdout + r.stderr)[-200:]))
        if not ok: fail("mp3_open accepted %s" % k)


def test_seek():
    print("== seek")
    files = ["l3_44100_joint_cbr128_chord", "l3_44100_joint_vbrq0_chord", "l3_22050_joint_vbrq2_chord",
             "l3_8000_mono_cbr8_noise", "l3_44100_joint_cbr128_noxing_noise", "l2_48000_stereo_ff192_chord",
             "l2_22050_mono_ff32_chord", "synth_l1_48000_joint", "lame_free_44100_joint_96_chord",
             "lame_short_48000_joint_192_chord", "lame_crc_8000_joint_vbr_sweep"]
    for name in files:
        c = [x for x in CASES if x[0] == name]
        if not c: continue
        p = c[0][1]
        full = os.path.join(OUT, name + ".out.raw")
        if not os.path.exists(full): mydecode(FAST, p, full)
        r = run([FAST, "open", p])
        info = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", r.stdout))
        sr, ch, layer = info["sample_rate"], info["channels"], info["layer"]
        spf = 384 if layer == 1 else 576 if (layer == 3 and sr < 32000) else 1152
        ref = np.fromfile(full, np.int16)
        dur = info["duration_ms"]
        reqs = [0, 1, 777, dur // 3, dur // 2 + 13, dur - 300, 250, dur + 5000]
        sk = os.path.join(OUT, name + ".seek.raw")
        r = run([ASAN, "seek", p, sk] + [str(x) for x in reqs])
        if r.returncode: fail("seek crashed %s: %s" % (name, r.stderr[-300:])); continue
        got = np.fromfile(sk, np.int16)
        off, worst, worst_snr = 0, 0, float("inf")
        for line in r.stdout.splitlines():
            m = re.match(r"seek req=(-?\d+) got=(-?\d+) sample=(\d+) n=(\d+)", line)
            req, gms, smp, n = map(int, m.groups())
            target = min(max(req, 0), dur)
            err_ms = abs(gms - target)
            frame_ms = spf * 1000.0 / sr
            if err_ms > frame_ms + 1: fail("%s seek %d -> %d ms (more than a frame off)" % (name, req, gms))
            worst = max(worst, err_ms)
            seg = got[off: off + n * ch]
            off += n * ch
            want = ref[smp * ch: smp * ch + n * ch]
            if n and len(want) == len(seg):
                s, _ = snr(want, seg)
                worst_snr = min(worst_snr, s)
                if s < MIN_SNR: fail("%s: audio after seek to %d differs from linear decode (SNR %.1f)" % (name, req, s))
            elif n:
                fail("%s: seek to %d returned %d samples, linear decode has %d" % (name, req, n, len(want) // ch))
        print("%-36s %d seeks, max error %d ms (frame %.1f ms), audio after seek vs linear decode: min SNR %s" %
              (name, len(reqs), worst, spf * 1000.0 / sr, fmt_snr(worst_snr)))


def test_tags():
    print("== tags")
    exp = {"tag_id3v23_art": ("Test Title", "The Artist", "Some Album"),
           "tag_id3v24_art_utf8": ("Ünïcode – 日本語 🎵", "Björk", "Ålbum"),
           "tag_id3v23_utf16": ("Ünïcode – 日本語 🎵", "Björk", "Ålbum"),
           "tag_id3v1_only": ("Caf\xe9 Title", "The Artist", "Some Album"),
           "tag_id3v24_and_v1": ("Test Title", "The Artist", "Some Album"),
           "l3_44100_joint_cbr128_chord": ("", "", "")}
    for name, (t, a, al) in exp.items():
        p = os.path.join(CORPUS, name + ".mp3")
        if not os.path.exists(p): continue
        r = subprocess.run([ASAN, "tags", p], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = r.stdout.decode("utf-8", "replace")
        got = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
        ok = r.returncode == 0 and got.get("title") == t and got.get("artist") == a and got.get("album") == al
        short = got.get("short", "")
        ok = ok and short == t.encode()[:5].decode("utf-8", "ignore")
        # ID3v2.3/2.4 vs v1 versions
        hdr = open(p, "rb").read(4)
        ver = "ID3v2.%d" % hdr[3] if hdr[:3] == b"ID3" else "ID3v1"
        print("%-28s %-8s title=%r artist=%r album=%r short=%r %s" % (name, ver, got.get("title"), got.get("artist"), got.get("album"), short, "ok" if ok else "MISMATCH"))
        if not ok: fail("tags %s" % name)


def test_fuzz():
    print("== mutation fuzz (ASan)")
    files = ["l3_44100_joint_cbr128_chord", "l3_22050_joint_vbrq2_chord", "lame_crc_44100_joint_128_chord",
             "l2_48000_stereo_ff192_chord", "tag_id3v24_art_utf8", "synth_l3_mpeg1_mixed_is", "synth_l3_mpeg2_is",
             "synth_l2_24000_joint_lsf", "synth_l1_48000_joint", "lame_free_44100_joint_96_chord", "l3_8000_joint_vbrq3_sweep"]
    iters = 30 if QUICK else 120
    for name in files:
        c = [x for x in CASES if x[0] == name]
        if not c: continue
        r = run([ASAN, "fuzz", c[0][1], "7", str(iters)], timeout=1200)
        ok = r.returncode == 0 and "fuzz ok" in r.stdout
        print("%-36s %d mutated copies: %s" % (name, iters, "ok" if ok else "FAILED"))
        if not ok: fail("fuzz %s: %s" % (name, (r.stdout + r.stderr)[-600:]))


def test_speed():
    print("== speed (-O2 host build)")
    long_files = []
    for kind, sr, ch, args, label in [("chord", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k"], "44.1k joint 128k"),
                                      ("noise", 44100, 2, ["-c:a", "libmp3lame", "-b:a", "320k", "-joint_stereo", "0"], "44.1k stereo 320k"),
                                      ("clicks", 48000, 2, ["-c:a", "libmp3lame", "-q:a", "0"], "48k VBR q0"),
                                      ("sweep", 22050, 1, ["-c:a", "libmp3lame", "-b:a", "64k"], "22.05k mono 64k"),
                                      ("chord", 48000, 2, ["-c:a", "mp2", "-b:a", "256k", "-f", "mp2"], "48k Layer II 256k")]:
        p = ff_encode("speed_%s_%d_%d_%s" % (kind, sr, ch, args[-1]), kind, sr, ch, args, dur=60.0,
                      ext="mp2" if "mp2" in args else "mp3")
        long_files.append((label, p))
    for label, p in long_files:
        r = run([FAST, "bench", p, "3"])
        m = re.search(r"realtime=([\d.]+)", r.stdout)
        t0 = time.time()
        run(["ffmpeg", "-v", "error", "-threads", "1", "-i", p, "-f", "null", "-"])
        tf = time.time() - t0
        rt = float(m.group(1)) if m else 0
        print("%-20s %s   (ffmpeg incl. startup: %.0fx)" % (label, r.stdout.strip(), 60.0 / tf))
        if rt < 50: fail("speed %s: %.1fx real time" % (label, rt))


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.dont_write_bytecode = True   # keep tests/mp3 free of __pycache__
    build()
    make_corpus()
    compare_all()
    test_info()
    test_seek()
    test_tags()
    test_fuzz()
    test_speed()
    print()
    if FAILS:
        print("%d FAILURE(S):" % len(FAILS))
        for f in FAILS: print("  " + f)
        sys.exit(1)
    print("ALL OK")


if __name__ == "__main__":
    main()
