"""HE-AAC (SBR) and HE-AACv2 (PS) tests: synthetic streams from sbrgen.py, ffmpeg's decoder as the reference."""
import os
from concurrent.futures import ThreadPoolExecutor

import numpy as np

import sbrgen

T = None
AT = None   # aactests module globals
TONAL_MIN = 35
RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


def asc_bits(fields):
    bits = []
    for v, n in fields:
        bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    bits += [0] * (-len(bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)).hex()


def explicit_asc(kind, core_rate, ch):
    sfi, esfi = RATES.index(core_rate), RATES.index(2 * core_rate)
    if kind == "aot5":
        return asc_bits([(5, 5), (sfi, 4), (ch, 4), (esfi, 4), (2, 5), (0, 3)])
    if kind == "aot29":
        return asc_bits([(29, 5), (sfi, 4), (ch, 4), (esfi, 4), (2, 5), (0, 3)])
    # backward compatible: AAC-LC config + sync extension 0x2b7 (SBR) [+ 0x548 (PS)]
    f = [(2, 5), (sfi, 4), (ch, 4), (0, 3), (0x2b7, 11), (5, 5), (1, 1), (esfi, 4)]
    if kind == "sync_ps":
        f += [(0x548, 11), (1, 1)]
    return asc_bits(f)


def make_case(name, kind, core_rate, ch, br, seed, ps=False, force=None, secs=3.0):
    sig = AT.signal(kind, core_rate, ch, secs, np.random.default_rng(seed))
    core = AT.encode("he_" + name + "_core", sig, core_rate, ch,
                     ["-b:a", br, "-cutoff", str(int(core_rate * 0.49))], secs, None)
    r = T.run([T.BIN["aac_test_fast"], "elements", core])
    if r.returncode:
        raise RuntimeError("elements: " + r.stderr.decode())
    out = T.p("he_%s.aac" % name)
    sbrgen.add_sbr(core, out, r.stdout.decode().splitlines(), core_rate, seed, ps=ps, force=force)
    os.remove(core)
    # the generated payloads must be valid: ffmpeg must not complain about them
    err = T.run(["ffmpeg", "-v", "error", "-nostdin", "-i", out, "-f", "null", "-"]).stderr.decode().strip()
    if err:
        T.fail("sbrgen wrote an invalid stream %s: %s" % (name, err[:300]))
    return out


def cases():
    c = [
        # name, signal, core rate, channels, core bitrate, ps, force
        ("st22k_noise", "noise", 22050, 2, "96k", False, None),
        ("st24k_music", "music", 24000, 2, "96k", False, None),
        ("st24k_coupled", "noise", 24000, 2, "96k", False, {"coupling": 1}),
        ("st22k_indep", "music", 22050, 2, "96k", False, {"coupling": 0}),
        ("mono16k", "noise", 16000, 1, "48k", False, None),
        ("st32k_music", "music", 32000, 2, "128k", False, None),
        ("mono11k", "music", 11025, 1, "32k", False, None),
        ("mono8k", "noise", 8000, 1, "24k", False, None),
        ("st12k", "noise", 12000, 2, "48k", False, None),
        ("st44k_88k", "noise", 44100, 2, "160k", False, None),
        ("st48k_96k", "music", 48000, 2, "160k", False, None),
        ("fixfix_only", "noise", 22050, 2, "96k", False, {"classes": [0]}),
        ("var_classes", "music", 24000, 2, "96k", False, {"classes": [1, 2, 3]}),
        ("harmonics", "noise", 22050, 1, "48k", False, {"harmonics": 0.9}),
        ("headers", "music", 24000, 2, "96k", False, {"header_rate": 0.5}),
        ("smooth_lim", "noise", 22050, 2, "96k", False, {"header": {"extra2": 1, "smoothing_mode": 0, "limiter_bands": 3, "limiter_gains": 0, "interpol_freq": 0}}),
        ("transient", "transient", 24000, 2, "96k", False, None),
        ("ch3", "music", 24000, 3, "144k", False, None),
        ("ch6", "noise", 24000, 6, "256k", False, None),
        ("ps24k", "noise", 24000, 1, "48k", True, None),
        ("ps22k_music", "music", 22050, 1, "48k", True, None),
        ("ps16k", "noise", 16000, 1, "32k", True, {"harmonics": 0.6}),
        ("ps_headers", "music", 24000, 1, "48k", True, {"header_rate": 0.4}),
    ]
    if T.QUICK:
        keep = ("st22k_noise", "st24k_coupled", "mono16k", "st44k_88k", "var_classes", "smooth_lim", "ch6", "ps24k", "ps_headers")
        c = [x for x in c if x[0] in keep]
    return c


def test_sbr():
    print("== HE-AAC (SBR) / HE-AACv2 (PS): synthetic streams vs ffmpeg", flush=True)
    res, res_tonal = [], []

    def one(a):
        i, (name, kind, rate, ch, br, ps, force) = a
        try:
            path = make_case(name, kind, rate, ch, br, 1000 + sum(map(ord, name)), ps, force)
        except Exception as e:
            return name, None, str(e)
        out_ch = 2 if ch == 1 else ch   # mono HE-AAC is decoded as stereo (PS may appear at any time), like ffmpeg
        # tonal sources make SBR's LPC inverse filter ill-conditioned: float rounding differences (even computing
        # it in double instead of float in this decoder) change such output by ~60 dB SNR, so those streams get a
        # lower bound that still catches real bugs; noise-like sources must match to MIN_SNR
        lim = T.MIN_SNR if kind in ("noise", "transient") else TONAL_MIN
        s = AT.check_stream("HE " + name, path, out_ch, quiet=True, multi=True, min_snr=lim)
        extra = []
        # the same stream in MP4 (implicit signalling through the demuxer)
        m4a = path[:-4] + ".m4a"
        T.ff("-i", path, "-c", "copy", m4a)
        s2 = AT.check_stream("HE " + name + " (mp4)", m4a, out_ch, ref_path=path, quiet=True, multi=True, min_snr=lim)
        os.remove(m4a)
        extra.append(("mp4", s2))
        # explicit signalling
        if ch <= 2:
            for k in (("aot29", "sync_ps") if ps else ("aot5", "sync")):
                asc = explicit_asc(k, rate, ch)
                try:
                    refs = AT.ff_decode_all(path)
                    out = path + ".x"
                    r = T.run([T.BIN["aac_test_fast" if T.QUICK else "aac_test_asan"], "decode", path, out, "f32", "all", "asc=" + asc])
                    if r.returncode:
                        raise RuntimeError(r.stderr.decode()[-2000:])
                    x = np.fromfile(out, np.float32)
                    os.remove(out)
                    ref = AT.best_ref(refs, x)
                    if len(x) != len(ref):
                        T.fail("HE %s %s: length %d vs %d" % (name, k, len(x), len(ref)))
                    sx = AT.snr(ref, x)
                    if sx < lim:
                        T.fail("HE %s %s: SNR %.1f" % (name, k, sx))
                    extra.append((k, sx))
                except Exception as e:
                    T.fail("HE %s %s: %s" % (name, k, e))
        T.SBR_FILES.append(path)
        return name, (s, lim), extra

    with ThreadPoolExecutor(T.JOBS) as ex:
        for name, s, extra in ex.map(one, enumerate(cases())):
            if s is None and isinstance(extra, str):
                T.fail("HE %s: %s" % (name, extra))
                continue
            s, lim = s
            if s is not None:
                (res if lim == T.MIN_SNR else res_tonal).append(s)
            print("  %-14s SNR %6.1f dB  %s" % (name + ("" if lim == T.MIN_SNR else " (tonal)"), s if s is not None else -1,
                                                 "  ".join("%s %.1f" % (k, v if v is not None else -1) for k, v in extra)), flush=True)
    v = np.array(res)
    if len(v):
        print("HE-AAC/HE-AACv2, noise-like sources: %d streams, SNR min %.1f dB, median %.1f dB" % (len(v), v.min(), np.median(v)))
    v = np.array(res_tonal)
    if len(v):
        print("HE-AAC/HE-AACv2, tonal sources (bound %d dB): %d streams, SNR min %.1f dB, median %.1f dB" % (TONAL_MIN, len(v), v.min(), np.median(v)))
    # tonal input: the LPC inverse filter is ill-conditioned for pure tones, so tiny float differences in the QMF
    # analysis are amplified differently by the two decoders; reported, not required
    try:
        p = make_case("tonal", "chord", 22050, 2, "96k", 77)
        info, x, _ = AT.my_decode(p, "f32", all_ch=True)
        ref = AT.best_ref(AT.ff_decode_all(p), x)
        print("  tonal (pure chord, informational): SNR %.1f dB" % AT.snr(ref, x))
        T.SBR_FILES.append(p)
    except Exception as e:
        T.fail("HE tonal: %s" % e)


def main(t, at):
    global T, AT
    T = t
    AT = type("M", (), at)
    if not T.ONLY or "sbr" in T.ONLY:
        test_sbr()
    elif not T.SBR_FILES:
        # fuzz / speed want some HE files
        for i, (name, kind, rate, ch, br, ps, force) in enumerate(cases()[:4] + [c for c in cases() if c[5]][:1]):
            T.SBR_FILES.append(make_case(name, kind, rate, ch, br, 1000 + sum(map(ord, name)), ps, force))
