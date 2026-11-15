#!/usr/bin/env python3
"""Tests the AXIOM media library (user/lib/media): MP4 demuxer and AAC decoder.

Usage: python3 tests/media/run.py [--quick] [--out DIR] [--only demux,aac,sbr,fuzz,speed]

1. Builds the host drivers (tests/media/*_test.c + user/lib/media/*.c, -DMEDIA_HOST_TEST) with ASan+UBSan and
   with -O2, and compiles every library source with the freestanding target flags (-Werror; `nm -u` must list
   only the allowed functions; reports the largest stack frame).
2. Demuxer: muxes many files with ffmpeg (libx264 + aac: moov at end, faststart, fragmented with and without
   default-base-moof / separate moofs / every frame, B-frames (ctts), negative cts offsets (ctts v1), audio-only m4a,
   video-only, several audio tracks, odd timescales, MOV, edit lists from -itsoffset / -ss stream copy) plus box
   rewrites ffmpeg never writes (co64, stz2, 64-bit mdat size, uuid/unknown boxes). Every track and every sample
   (offset, size, dts, pts, keyframe) is compared with `ffprobe -show_packets`; seeking is checked.
3. AAC: encodes many streams with ffmpeg's aac encoder (mono/stereo/3..8 channels, 7.35-96 kHz, 8-320 kbps,
   sines, noise, chords, transients, silence; coders twoloop/fast/anmr, PNS, intensity, M/S, TNS on/off,
   PCE, LTP, AAC Main prediction), as ADTS and in MP4. Decodes them with ffmpeg (float) and with the decoder
   (float and int16), requires SNR >= 70 dB (all channels, before downmix) and checks the stereo downmix.
4. SBR / PS: no HE-AAC encoder is available, so tests/media/sbrgen.py writes HE-AAC / HE-AACv2 streams itself:
   an LC core from ffmpeg's encoder with synthetic (random but valid) SBR and PS payloads added. ffmpeg decodes
   them as the reference.
5. Damaged / truncated files and a mutation fuzz pass under ASan+UBSan, then decode speed.
Exits non-zero on any failure."""
import json, os, random, struct, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "user", "lib", "media")
sys.path.insert(0, HERE)
import boxes  # noqa: E402

QUICK = "--quick" in sys.argv
OUT = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(tempfile.gettempdir(), "axiom-mediatest")
ONLY = set(sys.argv[sys.argv.index("--only") + 1].split(",")) if "--only" in sys.argv else None
os.makedirs(OUT, exist_ok=True)
MIN_SNR = 70.0
FAILS = []
R = random.Random(4321)
NP = np.random.default_rng(4321)
JOBS = os.cpu_count() or 4


def fail(msg):
    FAILS.append(msg)
    print("FAIL:", msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def ff(*args):
    r = run(["ffmpeg", "-v", "error", "-y", "-nostdin"] + [str(a) for a in args])
    if r.returncode:
        raise RuntimeError("ffmpeg failed: %s\n%s" % (" ".join(map(str, args)), r.stderr.decode()))


def p(name):
    return os.path.join(OUT, name)


# ---------------------------------------------------------------- build
TARGET_FLAGS = ("-std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -m64 "
                "-mcmodel=large -msse2 -mfpmath=sse -fno-tree-loop-distribute-patterns -Wall -Wextra "
                "-Wno-unused-parameter").split()
ALLOWED = {"memset", "memcpy", "memmove", "memcmp", "malloc", "calloc", "realloc", "free"}
CSRC = sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".c"))
BIN = {}


def build():
    print("== build", flush=True)
    for name in ("mp4_test", "aac_test"):
        drv = os.path.join(HERE, name + ".c")
        if not os.path.exists(drv):
            continue
        for kind, flags in (("asan", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]),
                            ("fast", ["-O2"])):
            out = p("%s_%s" % (name, kind))
            r = run(["gcc", "-std=gnu11", "-DMEDIA_HOST_TEST", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror"]
                    + flags + ["-o", out, drv] + CSRC)
            if r.returncode:
                print(r.stderr.decode())
                sys.exit("host build failed: " + name)
            BIN[name + "_" + kind] = out
    objdir = p("target")
    os.makedirs(objdir, exist_ok=True)
    maxstack = (0, "")
    for c in CSRC:
        o = os.path.join(objdir, os.path.basename(c)[:-2] + ".o")
        r = run(["gcc"] + TARGET_FLAGS + ["-Werror", "-fstack-usage", "-c", c, "-o", o])
        if r.returncode:
            fail("target compile %s:\n%s" % (c, r.stderr.decode()))
            continue
        und = set(run(["nm", "-u", o]).stdout.decode().split()) - {"U"}
        if und - ALLOWED:
            fail("%s uses %s" % (c, sorted(und - ALLOWED)))
        su = o[:-2] + ".su"
        if os.path.exists(su):
            for line in open(su):
                parts = line.split("\t")
                if len(parts) >= 2 and int(parts[1]) > maxstack[0]:
                    maxstack = (int(parts[1]), parts[0])
    print("target build ok, largest stack frame %d bytes (%s)" % maxstack)
    if maxstack[0] > 16000:
        fail("stack frame too large: %s" % (maxstack,))


# ---------------------------------------------------------------- demuxer
def probe(path):
    r = run(["ffprobe", "-v", "error", "-show_entries",
             "stream=index,codec_type,codec_tag_string,width,height,sample_rate,channels,time_base:"
             "packet=stream_index,pts,dts,size,pos,flags", "-of", "json", path])
    if r.returncode:
        raise RuntimeError("ffprobe failed on " + path)
    return json.loads(r.stdout)


def dump(path, binary="mp4_test_asan"):
    r = run([BIN[binary], "dump", path])
    tracks, samples = [], {}
    for line in r.stdout.decode().splitlines():
        f = line.split()
        if f[0] == "track":
            d = {f[i]: f[i + 1] for i in range(2, len(f) - 1, 2)}
            tracks.append(d)
        elif f[0] == "s":
            samples.setdefault(int(f[1]), []).append(tuple(int(x) for x in f[2:]))
        elif f[0] == "ntracks":
            head = f
    if r.returncode or not tracks:
        return None, None, r.stdout.decode() + r.stderr.decode()
    return tracks, samples, head


PTS_QUIRK = {"cmaf", "frag_negcts"}


def check_demux(name, path, expect_shift=None):
    tracks, samples, head = dump(path)
    if tracks is None:
        fail("%s: open failed %s" % (name, head))
        return 0
    pr = probe(path)
    streams = pr["streams"]
    if len(streams) != len(tracks):
        fail("%s: %d tracks, ffprobe %d" % (name, len(tracks), len(streams)))
        return 0
    bad = 0
    nsamp = 0
    for st in streams:
        i = st["index"]
        t = tracks[i]
        typ = {"video": "1", "audio": "2"}.get(st["codec_type"], "0")
        if t["type"] != typ:
            fail("%s track %d type %s vs %s" % (name, i, t["type"], st["codec_type"]))
        if st.get("codec_tag_string") and st["codec_tag_string"] != t["codec"] and not st["codec_tag_string"].startswith("["):
            fail("%s track %d codec %s vs %s" % (name, i, t["codec"], st["codec_tag_string"]))
        if typ == "1" and (int(t["w"]), int(t["h"])) != (st["width"], st["height"]):
            fail("%s track %d size %sx%s vs %sx%s" % (name, i, t["w"], t["h"], st["width"], st["height"]))
        if typ == "2" and (int(t["rate"]), int(t["ch"])) != (int(st["sample_rate"]), st["channels"]):
            fail("%s track %d audio %s/%s vs %s/%s" % (name, i, t["rate"], t["ch"], st["sample_rate"], st["channels"]))
        num, den = map(int, st["time_base"].split("/"))
        ts = int(t["ts"])
        if num != 1 or den != ts:
            fail("%s track %d time_base %s vs timescale %d" % (name, i, st["time_base"], ts))
        ref = [pk for pk in pr["packets"] if pk["stream_index"] == i]
        mine = samples.get(i, [])
        nsamp += len(mine)
        if len(ref) != len(mine):
            fail("%s track %d: %d samples, ffprobe %d" % (name, i, len(mine), len(ref)))
        # ffmpeg quirk: in fragments with negative composition offsets (trun v1) it adds the largest negative
        # offset to every pts; we keep pts = dts + offset as the spec (and browsers) do. Expect a constant shift.
        quirk = int(ref[0]["pts"]) - mine[0][3] if name in PTS_QUIRK and ref and mine else 0
        for k, (pk, s) in enumerate(zip(ref, mine)):
            exp = (int(pk["pos"]), int(pk["size"]), int(pk.get("dts", -1 << 60)), int(pk.get("pts", -1 << 60)) - quirk,
                   1 if "K" in pk["flags"] else 0)
            if exp != s:
                bad += 1
                if bad <= 3:
                    fail("%s track %d sample %d: mine (off,size,dts,pts,key)=%s ffprobe %s" % (name, i, k, s, exp))
        if expect_shift is not None and typ == "2":
            pass
        # seeking: compare with the same rule evaluated on ffprobe's packet list
        if ref:
            pts = [int(pk["pts"]) - quirk for pk in ref]
            keys = ["K" in pk["flags"] for pk in ref]
            end_ms = max(pts) * 1000 // ts + 100
            targets = sorted(set([0, 1, end_ms // 3, end_ms // 2, end_ms - 150, end_ms + 5000] + [R.randrange(-50, end_ms) for _ in range(6)]))
            r = run([BIN["mp4_test_asan"], "seek", path, str(i)] + [str(x) for x in targets]).stdout.decode().split("\n")
            for line, tms in zip(r, targets):
                f = line.split()
                tgt = tms * ts // 1000 if tms >= 0 else -((-tms * ts + 999) // 1000)

                def expect(konly):
                    best, bp, first = -1, None, -1
                    for j, v in enumerate(pts):
                        if konly and not keys[j]:
                            continue
                        if first < 0:
                            first = j
                        if v <= tgt and (best < 0 or v > bp):
                            best, bp = j, v
                    return best if best >= 0 else max(first, 0)
                e = (expect(False), expect(True))
                if (int(f[2]), int(f[3])) != e:
                    fail("%s track %d seek %d ms: got %s %s expected %s" % (name, i, tms, f[2], f[3], e))
                elif keys[e[1]] is False and any(keys):
                    fail("%s seek: not a keyframe" % name)
    if not bad:
        print("  ok %-34s tracks %d samples %d" % (name, len(tracks), nsamp), flush=True)
    return nsamp


def gen_demux_files():
    """Returns [(name, path)]."""
    files = []
    v = ["-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25"]
    v30 = ["-f", "lavfi", "-i", "testsrc2=size=176x144:rate=30000/1001"]
    a = ["-f", "lavfi", "-i", "sine=f=440:r=44100"]
    a2 = ["-f", "lavfi", "-i", "sine=f=660:r=48000"]
    a3 = ["-f", "lavfi", "-i", "anoisesrc=r=22050:a=0.1"]
    x264 = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "veryfast"]
    dur = ["-t", "4" if QUICK else "6"]
    jobs = []

    def add(name, args, ext="mp4"):
        path = p("dm_%s.%s" % (name, ext))
        jobs.append((name, path, args))

    add("basic", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-b:a", "96k"])
    add("faststart", v + a + dur + x264 + ["-bf", "3", "-g", "50", "-c:a", "aac", "-movflags", "+faststart"])
    add("frag_moof", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-movflags", "frag_keyframe+empty_moov+default_base_moof"])
    add("frag_implicit", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-movflags", "frag_keyframe+empty_moov"])
    add("frag_separate", v + a + dur + x264 + ["-bf", "1", "-g", "12", "-c:a", "aac", "-movflags", "frag_keyframe+empty_moov+separate_moof"])
    add("frag_dur", v + a + dur + x264 + ["-g", "100", "-c:a", "aac", "-frag_duration", "700000", "-movflags", "empty_moov+default_base_moof+omit_tfhd_offset"])
    add("frag_every", v + dur + x264 + ["-bf", "2", "-g", "10", "-movflags", "frag_every_frame+empty_moov"])
    add("frag_nonempty", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-movflags", "frag_keyframe"])
    add("dash", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-movflags", "dash"])
    add("cmaf", v + a + dur + x264 + ["-bf", "2", "-g", "25", "-c:a", "aac", "-movflags", "cmaf"])
    add("frag_negcts", v + a + dur + x264 + ["-bf", "3", "-g", "30", "-c:a", "aac", "-movflags", "negative_cts_offsets+frag_keyframe+empty_moov"])
    add("negcts", v + a + dur + x264 + ["-bf", "3", "-g", "30", "-c:a", "aac", "-movflags", "negative_cts_offsets"])
    add("audio_only", a3 + dur + ["-c:a", "aac", "-b:a", "48k"], "m4a")
    add("audio_mono8k", ["-f", "lavfi", "-i", "sine=f=300:r=8000"] + dur + ["-c:a", "aac", "-ac", "1"], "m4a")
    add("video_only", v + dur + x264 + ["-bf", "0", "-g", "7"])
    add("multi_audio", v + a + a2 + a3 + dur + ["-map", "0", "-map", "1", "-map", "2", "-map", "3"] + x264 +
        ["-bf", "2", "-c:a", "aac", "-ac:a:1", "2"])
    add("ntsc", v30 + a2 + dur + x264 + ["-bf", "2", "-c:a", "aac", "-video_track_timescale", "30000"])
    add("ts_odd", v30 + a + dur + x264 + ["-bf", "2", "-c:a", "aac", "-video_track_timescale", "9973"])
    add("itsoffset", ["-itsoffset", "0.5"] + v + a + dur + x264 + ["-bf", "2", "-c:a", "aac", "-map", "0", "-map", "1"])
    add("mov", v + a + dur + x264 + ["-bf", "2", "-c:a", "aac"], "mov")
    add("mov_frag", v + a + dur + x264 + ["-bf", "2", "-c:a", "aac", "-movflags", "frag_keyframe+empty_moov"], "mov")
    add("hevc", v + a + dur + ["-c:v", "libx265", "-x265-params", "log-level=none", "-tag:v", "hvc1", "-c:a", "aac"])
    add("mpeg4_mp3", v + a + dur + ["-c:v", "mpeg4", "-c:a", "libmp3lame"])
    add("opus_vp9", v + a2 + ["-t", "2", "-c:v", "libvpx-vp9", "-deadline", "realtime", "-c:a", "libopus"])
    add("aac_51", ["-f", "lavfi", "-i", "sine=f=500:r=48000"] + dur + ["-af", "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0", "-c:a", "aac"], "m4a")
    if not QUICK:
        add("long_gop", v + a + ["-t", "20"] + x264 + ["-bf", "3", "-b_strategy", "2", "-g", "250", "-c:a", "aac"])
        add("frag_long", v + a + ["-t", "20"] + x264 + ["-bf", "3", "-g", "48", "-c:a", "aac", "-movflags", "frag_keyframe+empty_moov+default_base_moof"])

    def make(job):
        name, path, args = job
        ff(*(args + [path]))
        return name, path
    with ThreadPoolExecutor(JOBS) as ex:
        files += list(ex.map(make, jobs))
    # stream copies with edit lists
    base = p("dm_basic.mp4")
    for ss in (["1.3"], ["0.75"]):
        path = p("dm_sscopy_%s.mp4" % ss[0])
        ff("-ss", ss[0], "-i", base, "-c", "copy", path)
        files.append(("sscopy_" + ss[0], path))
    path = p("dm_frag_from_basic.mp4")
    ff("-i", base, "-c", "copy", "-movflags", "frag_keyframe+empty_moov+default_base_moof", path)
    files.append(("frag_copy", path))
    # box rewrites
    data = open(base, "rb").read()
    for nm, kw in (("co64", dict(co64=True)), ("stz2", dict(stz2=True)), ("large_mdat", dict(large_mdat=True)),
                   ("junk_boxes", dict(junk=True)), ("all_rewrites", dict(co64=True, stz2=True, large_mdat=True, junk=True))):
        path = p("dm_%s.mp4" % nm)
        open(path, "wb").write(boxes.make_variant(data, **kw))
        files.append((nm, path))
    aud = open(p("dm_audio_only.m4a"), "rb").read()
    path = p("dm_stz2_audio.m4a")
    open(path, "wb").write(boxes.make_variant(aud, stz2=True, co64=True))
    files.append(("stz2_audio", path))
    return files


def test_demux():
    print("== demuxer", flush=True)
    files = gen_demux_files()
    total = 0
    for name, path in files:
        total += check_demux(name, path)
    # duration
    tracks, samples, head = dump(p("dm_basic.mp4"))
    fd = float(json.loads(run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "json",
                               p("dm_basic.mp4")]).stdout)["format"]["duration"])
    if abs(int(head[3]) - fd * 1000) > 60:
        fail("duration %s ms vs ffprobe %.0f" % (head[3], fd * 1000))
    # not mp4
    for nm, blob in (("garbage", bytes(R.randrange(256) for _ in range(5000))), ("empty_moov", b"\0\0\0\x08moov" + b"\0" * 100),
                     ("adts", open(p("dm_audio_only.m4a"), "rb").read()[:16])):
        path = p("dm_bad_%s.bin" % nm)
        open(path, "wb").write(blob)
        if dump(path)[0] is not None:
            fail("bad file %s opened" % nm)
    print("demuxer: %d files, %d samples compared" % (len(files), total))
    return files


# ---------------------------------------------------------------- fuzz
def test_demux_fuzz(files):
    print("== demuxer fuzz", flush=True)
    iters = 150 if QUICK else 1000
    sel = [path for name, path in files if name in ("basic", "frag_moof", "frag_implicit", "all_rewrites", "audio_only",
                                                     "multi_audio", "mov", "negcts", "sscopy_1.3", "frag_separate")]

    def one(a):
        i, path = a
        r = run([BIN["mp4_test_asan"], "fuzz", path, str(i + 1), str(iters)], timeout=900)
        return path, r
    with ThreadPoolExecutor(JOBS) as ex:
        for path, r in ex.map(one, enumerate(sel)):
            if r.returncode:
                fail("demux fuzz %s: %s" % (path, r.stderr.decode()[-3000:]))
            else:
                print("  ", os.path.basename(path), r.stdout.decode().strip())


def main():
    t0 = time.time()
    build()
    files = []
    if not ONLY or "demux" in ONLY:
        files = test_demux()
    if not ONLY or "fuzz" in ONLY:
        if not files:
            files = gen_demux_files()
        test_demux_fuzz(files)
    if not ONLY or "aac" in ONLY or "sbr" in ONLY or "speed" in ONLY:
        import aactests
        aactests.main(sys.modules[__name__])
    print("\n%d failures, %.0f s" % (len(FAILS), time.time() - t0))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
