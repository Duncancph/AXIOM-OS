"""AAC decoder tests (imported by run.py; see its docstring)."""
import os, subprocess, time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

T = None  # the run.py module (fail, run, ff, p, BIN, QUICK, ...)


# ---------------------------------------------------------------- signals
def signal(kind, rate, ch, secs, rng):
    n = int(rate * secs)
    t = np.arange(n) / rate
    out = np.zeros((n, ch), np.float32)
    for c in range(ch):
        if kind == "sine":
            f = [440, 1000, 3000, 250, 5000, 700, 1500, 90][c % 8] * min(1.0, rate / 44100 * 1.5)
            x = 0.5 * np.sin(2 * np.pi * f * t + c)
        elif kind == "sweep":
            f0, f1 = 50, rate * 0.45
            k = np.log(f1 / f0) / secs
            x = 0.4 * np.sin(2 * np.pi * f0 * (np.exp(k * t) - 1) / k + c)
        elif kind == "noise":
            x = rng.normal(0, 0.15, n)
        elif kind == "chord":
            x = sum(0.12 * np.sin(2 * np.pi * f * (1 + 0.01 * c) * t) for f in (220, 277.2, 329.6, 440, 554.4, 880, 1760, 3520))
            x = x * (0.6 + 0.4 * np.sin(2 * np.pi * 0.5 * t))
        elif kind == "music":   # notes with attacks + noise bursts (drums): mixes long and short windows
            x = np.zeros(n)
            pos = 0
            while pos < n:
                L = int(rate * rng.uniform(0.08, 0.4))
                f = 110 * 2 ** (rng.integers(0, 36) / 12)
                tt = np.arange(min(L, n - pos)) / rate
                env = np.exp(-tt * rng.uniform(2, 12))
                x[pos:pos + len(tt)] += 0.3 * env * (np.sin(2 * np.pi * f * tt) + 0.3 * np.sin(4 * np.pi * f * tt))
                if rng.random() < 0.4:
                    m = min(int(rate * 0.03), n - pos)
                    x[pos:pos + m] += rng.normal(0, 0.3, m) * np.exp(-np.arange(m) / (rate * 0.008))
                pos += L
            x = np.roll(x, c * 500)
        elif kind == "transient":   # clicks in silence: forces EIGHT_SHORT and start/stop windows
            x = np.zeros(n)
            for pos in range(int(rate * 0.05), n - 100, int(rate * 0.13)):
                m = min(200, n - pos)
                x[pos:pos + m] = rng.normal(0, 0.6, m) * np.exp(-np.arange(m) / 30)
        elif kind == "silence":
            x = np.zeros(n)
        elif kind == "loud":
            x = np.clip(rng.normal(0, 0.6, n), -0.99, 0.99) * (np.arange(n) % (rate // 3) < rate // 6)
        else:
            raise ValueError(kind)
        out[:, c] = x
    return out


def write_raw(path, x):
    x.astype("<f4").tofile(path)


LAYOUT = {1: "mono", 2: "stereo", 3: "3.0", 4: "4.0", 5: "5.0", 6: "5.1", 7: "6.1", 8: "7.1"}


def encode(name, sig, rate, ch, opts, kind_secs, rng):
    raw = T.p("aac_%s.f32" % name)
    write_raw(raw, sig)
    adts = T.p("aac_%s.aac" % name)
    T.ff("-f", "f32le", "-ar", rate, "-ac", ch, "-channel_layout", LAYOUT[ch], "-i", raw, "-c:a", "aac", *opts, adts)
    os.remove(raw)
    return adts


def ff_decode(path, extra=()):
    r = T.run(["ffmpeg", "-v", "error", "-nostdin", "-i", path, *extra, "-f", "f32le", "-"])
    if r.returncode:
        raise RuntimeError(r.stderr.decode())
    return np.frombuffer(r.stdout, np.float32)


def probe_ch(path):
    r = T.run(["ffprobe", "-v", "error", "-show_entries", "stream=channels,sample_rate", "-of", "csv=p=0", path])
    a, b = r.stdout.decode().strip().split("\n")[0].split(",")
    return int(a), int(b)   # sample_rate, channels


def my_decode(path, mode="f32", all_ch=False, binary="aac_test_asan"):
    out = path + ".mine"
    args = [T.BIN[binary], "decode", path, out, mode] + (["all"] if all_ch else [])
    r = T.run(args, timeout=600)
    if r.returncode:
        raise RuntimeError("decoder crashed (%d): %s" % (r.returncode, r.stderr.decode()[-3000:]))
    info = r.stdout.decode().split("\n")
    line = [l for l in info if l.startswith("rate")]
    if not line:
        raise RuntimeError("decoder: " + r.stdout.decode())
    f = line[0].split()
    d = {f[i]: int(f[i + 1]) for i in range(0, len(f) - 1, 2)}
    x = np.fromfile(out, np.float32 if mode == "f32" else np.int16)
    os.remove(out)
    return d, x, [l for l in info if not l.startswith("rate") and l]


def snr(ref, x):
    n = min(len(ref), len(x))
    ref, x = ref[:n].astype(np.float64), x[:n].astype(np.float64)
    e = np.sum((ref - x) ** 2)
    s = np.sum(ref ** 2)
    if s == 0:
        return 200.0 if e == 0 else -10 * np.log10(e + 1e-30)
    return 200.0 if e == 0 else 10 * np.log10(s / e)


def match_channels(ref, mine, n):
    """mine is in decode order, ffmpeg in its layout order: pair every channel with its best match."""
    R = ref.reshape(-1, n)
    M = mine.reshape(-1, n)
    L = min(len(R), len(M))
    order = []
    used = set()
    for c in range(n):
        best, bi = None, 0
        for j in range(n):
            if j in used:
                continue
            e = np.sum((R[:L, j] - M[:L, c]) ** 2)
            if best is None or e < best:
                best, bi = e, j
        used.add(bi)
        order.append(bi)
    return R[:L, order].reshape(-1), M[:L].reshape(-1)


RESULTS = {"lc": []}


def check_stream(name, path, nch, is_mp4=False, ref_path=None, min_snr=None, quiet=False):
    """Decodes `path` with the decoder (all channels) and compares with ffmpeg's decode of ref_path (or path)."""
    min_snr = T.MIN_SNR if min_snr is None else min_snr
    try:
        ref = ff_decode(ref_path or path)
        info, mine, msgs = my_decode(path, "f32", all_ch=True)
    except Exception as e:
        T.fail("%s: %s" % (name, e))
        return None
    if info["channels"] != nch:
        T.fail("%s: %d channels decoded, expected %d" % (name, info["channels"], nch))
        return None
    if len(ref) != len(mine):
        T.fail("%s: %d samples, ffmpeg %d" % (name, len(mine) // nch, len(ref) // nch))
    a, b = match_channels(ref, mine, nch) if nch > 2 else (ref, mine)
    s = snr(a, b)
    if s < min_snr:
        T.fail("%s: SNR %.1f dB < %.0f" % (name, s, min_snr))
    elif not quiet:
        print("  ok %-48s SNR %6.1f dB" % (name, s), flush=True)
    return s


def lc_cases():
    rng = np.random.default_rng(99)
    cases = []
    rates = [8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 7350]
    kinds = ["sine", "sweep", "noise", "chord", "music", "transient", "silence", "loud"]
    # sample rate sweep (stereo / mono)
    for i, r in enumerate(rates):
        ch = 2 if i % 3 else 1
        br = {8000: "16k", 7350: "16k", 11025: "24k", 12000: "24k", 16000: "32k"}.get(r, "96k" if r < 44100 else "160k")
        cases.append(("rate%d_%dch_%s" % (r, ch, kinds[i % len(kinds)]), kinds[i % len(kinds)], r, ch, ["-b:a", br]))
    # bitrates
    for br in (["16k", "32k", "64k", "128k", "192k", "256k", "320k"] if not T.QUICK else ["32k", "128k", "320k"]):
        cases.append(("br%s_music" % br, "music", 44100, 2, ["-b:a", br]))
    # coders / tools
    tools = [
        ("pns_on", ["-aac_pns", "1", "-b:a", "48k"], "noise"),
        ("pns_on_music", ["-aac_pns", "1", "-b:a", "64k"], "music"),
        ("pns_off", ["-aac_pns", "0", "-b:a", "48k"], "noise"),
        ("is_on", ["-aac_is", "1", "-b:a", "64k"], "chord"),
        ("is_off", ["-aac_is", "0", "-b:a", "64k"], "chord"),
        ("ms_force", ["-aac_ms", "1", "-b:a", "96k"], "music"),
        ("ms_off", ["-aac_ms", "0", "-b:a", "96k"], "music"),
        ("tns_on", ["-aac_tns", "1", "-b:a", "96k"], "transient"),
        ("tns_off", ["-aac_tns", "0", "-b:a", "96k"], "transient"),
        ("tns_music", ["-aac_tns", "1", "-b:a", "128k"], "music"),
        ("coder_fast", ["-aac_coder", "fast", "-b:a", "128k"], "music"),
        ("coder_fast_lo", ["-aac_coder", "fast", "-b:a", "24k"], "music"),
        ("coder_twoloop", ["-aac_coder", "twoloop", "-b:a", "96k"], "chord"),
        ("coder_anmr", ["-aac_coder", "anmr", "-b:a", "96k"], "music"),
        ("vbr_q1", ["-q:a", "0.5"], "music"),
        ("vbr_q2", ["-q:a", "2"], "sweep"),
        ("lowpass", ["-cutoff", "4000", "-b:a", "64k"], "noise"),
        ("all_tools_lo", ["-aac_pns", "1", "-aac_is", "1", "-aac_tns", "1", "-b:a", "32k"], "music"),
    ]
    for nm, opts, kind in tools:
        cases.append((nm, kind, 44100 if "lo" not in nm else 32000, 2, opts))
    # mono variants
    cases.append(("mono_pns", "noise", 22050, 1, ["-aac_pns", "1", "-b:a", "24k"]))
    cases.append(("mono_transient", "transient", 48000, 1, ["-b:a", "64k"]))
    # multichannel
    for ch in (3, 4, 5, 6, 8) if not T.QUICK else (3, 6, 8):
        cases.append(("ch%d_music" % ch, "music", 48000, ch, ["-b:a", "%dk" % (64 * ch)]))
        if not T.QUICK:
            cases.append(("ch%d_noise_pns" % ch, "noise", 44100, ch, ["-aac_pns", "1", "-b:a", "%dk" % (32 * ch)]))
    cases.append(("ch6_pce", "chord", 48000, 6, ["-aac_pce", "1", "-b:a", "256k"]))
    cases.append(("ch7_61", "music", 48000, 7, ["-b:a", "320k"]))
    # profiles
    cases.append(("ltp_music", "music", 44100, 2, ["-strict", "-2", "-profile:a", "aac_ltp", "-aac_ltp", "1", "-b:a", "96k"]))
    cases.append(("ltp_chord_mono", "chord", 32000, 1, ["-strict", "-2", "-profile:a", "aac_ltp", "-aac_ltp", "1", "-b:a", "48k"]))
    cases.append(("main_pred", "chord", 44100, 2, ["-profile:a", "aac_main", "-aac_pred", "1", "-b:a", "96k"]))
    cases.append(("main_pred_music", "music", 48000, 1, ["-profile:a", "aac_main", "-aac_pred", "1", "-b:a", "64k"]))
    cases.append(("mpeg2_lc", "music", 44100, 2, ["-profile:a", "mpeg2_aac_low", "-b:a", "96k"]))
    if T.QUICK:
        cases = cases[::2] + [c for c in cases[1::2] if c[0].startswith(("ltp", "main", "pns", "ch6", "mono"))]
    return cases, rng


def test_lc():
    print("== AAC-LC (ADTS + MP4) vs ffmpeg", flush=True)
    cases, rng = lc_cases()
    secs = 2.5 if T.QUICK else 4.0

    def one(c):
        name, kind, rate, ch, opts = c
        local = np.random.default_rng(abs(hash(name)) % (1 << 32))
        sig = signal(kind, rate, ch, secs, local)
        try:
            path = encode(name, sig, rate, ch, opts, secs, local)
        except Exception as e:
            return name, None, "encode: %s" % e
        res = check_stream(name, path, ch, quiet=True)
        # same stream in MP4 (through the demuxer and the AudioSpecificConfig path)
        m4a = path[:-4] + ".m4a"
        T.ff("-i", path, "-c", "copy", m4a)
        res2 = check_stream(name + " (mp4)", m4a, ch, ref_path=path, quiet=True)
        # int16 output and the stereo downmix
        try:
            info, s16, _ = my_decode(path, "s16")
            lay = T.run(["ffprobe", "-v", "error", "-show_entries", "stream=channel_layout", "-of", "csv=p=0", path]).stdout.decode().strip()
            if ch > 2 and lay in ("", "unknown"):
                # PCE layouts ffmpeg cannot name: its -ac 2 then guesses a default layout by channel count
                # (e.g. treats a side SCE as LFE); only the per-channel comparison above applies
                info, s16, _ = my_decode(path, "s16")
                os.remove(m4a)
                return name, (res, res2, 0), None
            if ch > 2:   # ffmpeg's downmix to float (its s16 path renormalises the matrix), then saturate
                f = ff_decode(path, ["-ac", "2"]).astype(np.float64) * 32768
                ref16 = np.clip(np.floor(f + 0.5), -32768, 32767).astype(np.int16)
            else:
                ref16 = np.frombuffer(T.run(["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-f", "s16le", "-"]).stdout, np.int16)
            n = min(len(ref16), len(s16))
            md = int(np.max(np.abs(ref16[:n].astype(int) - s16[:n].astype(int)))) if n else 0
            if len(ref16) != len(s16):
                T.fail("%s: s16 length %d vs %d" % (name, len(s16), len(ref16)))
            if md > 1:
                T.fail("%s: s16 output (downmixed for >2 ch) differs from ffmpeg by %d LSB" % (name, md))
            exp_ch = 1 if ch == 1 else 2
            if info["channels"] != exp_ch:
                T.fail("%s: aac_channels %d" % (name, info["channels"]))
        except Exception as e:
            T.fail("%s s16: %s" % (name, e))
            md = -1
        os.remove(m4a)
        return name, (res, res2, md), None

    with ThreadPoolExecutor(T.JOBS) as ex:
        for name, res, err in ex.map(one, cases):
            if err:
                T.fail("%s: %s" % (name, err))
                continue
            s1, s2, md = res
            if s1 is not None:
                RESULTS["lc"].append(s1)
            print("  %-22s SNR %6.1f dB  mp4 %6.1f dB  s16 maxdiff %d" % (name, s1 or -1, s2 or -1, md), flush=True)
    v = np.array(RESULTS["lc"])
    if len(v):
        print("AAC-LC: %d streams, SNR min %.1f dB, median %.1f dB" % (len(v), v.min(), np.median(v)))


def test_seek():
    print("== seek / reset", flush=True)
    sig = signal("music", 44100, 2, 6, np.random.default_rng(5))
    path = encode("seek", sig, 44100, 2, ["-b:a", "128k", "-aac_pns", "0"], 6, None)
    m4a = path[:-4] + ".m4a"
    T.ff("-i", path, "-c", "copy", m4a)
    info, full, _ = my_decode(m4a, "s16")
    for ms in (1000, 2345, 5000):
        out = m4a + ".seek"
        r = T.run([T.BIN["aac_test_asan"], "seek", m4a, out, str(ms)])
        x = np.fromfile(out, np.int16)
        os.remove(out)
        # the decode started one access unit before the sample at ms; its first frame lacks the overlap
        start = len(full) - len(x)
        if start % 2048 or start < 0:
            T.fail("seek %d: odd length" % ms)
            continue
        if not np.array_equal(full[start + 2048:], x[2048:]):
            T.fail("seek %d ms: output after the pre-roll frame differs from a linear decode" % ms)
        else:
            print("  ok seek %d ms (sample %d)" % (ms, start // 2048), flush=True)


def test_damaged():
    print("== damaged streams", flush=True)
    sig = signal("music", 44100, 2, 3, np.random.default_rng(6))
    path = encode("dmg", sig, 44100, 2, ["-b:a", "128k"], 3, None)
    data = open(path, "rb").read()
    rng = np.random.default_rng(7)
    variants = {
        "truncated": data[:len(data) * 2 // 3 + 37],
        "garbage_inserted": data[:5000] + bytes(rng.integers(0, 256, 3000, dtype=np.uint8)) + data[5000:],
        "bitflips": bytearray(data),
        "zeros": data[:8000] + bytes(4000) + data[12000:],
    }
    b = variants["bitflips"]
    for i in rng.integers(0, len(b), 60):
        b[int(i)] ^= 1 << int(rng.integers(0, 8))
    variants["bitflips"] = bytes(b)
    ref_frames = len(ff_decode(path)) // 2048
    for nm, blob in variants.items():
        f = T.p("aac_dmg_%s.aac" % nm)
        open(f, "wb").write(blob)
        try:
            info, x, msgs = my_decode(f, "s16")
            print("  ok %-20s decoded %d frames (intact file: %d)" % (nm, info["frames"], ref_frames), flush=True)
            if nm == "bitflips" and info["frames"] < ref_frames * 0.9:
                T.fail("damaged %s: only %d frames" % (nm, info["frames"]))
        except Exception as e:
            T.fail("damaged %s: %s" % (nm, e))
    # damaged MP4
    m4a = path[:-4] + ".m4a"
    T.ff("-i", path, "-c", "copy", m4a)
    d = bytearray(open(m4a, "rb").read())
    for i in rng.integers(100, len(d) - 2000, 80):
        d[int(i)] ^= 0xff
    f = T.p("aac_dmg_mp4.m4a")
    open(f, "wb").write(bytes(d))
    try:
        info, x, msgs = my_decode(f, "s16")
        print("  ok %-20s decoded %d frames" % ("mp4 bitflips", info["frames"]), flush=True)
    except Exception as e:
        T.fail("damaged mp4: %s" % e)


def test_fuzz():
    print("== AAC fuzz (ASan/UBSan)", flush=True)
    files = []
    for nm, kind, rate, ch, opts in (("fz1", "music", 44100, 2, ["-b:a", "96k", "-aac_pns", "1"]),
                                     ("fz2", "transient", 22050, 1, ["-b:a", "32k"]),
                                     ("fz3", "music", 48000, 6, ["-b:a", "256k"]),
                                     ("fz4", "chord", 44100, 2, ["-strict", "-2", "-profile:a", "aac_ltp", "-aac_ltp", "1"]),
                                     ("fz5", "chord", 44100, 2, ["-profile:a", "aac_main", "-aac_pred", "1"])):
        files.append(encode(nm, signal(kind, rate, ch, 2, np.random.default_rng(8)), rate, ch, opts, 2, None))
    m4a = files[0][:-4] + ".m4a"
    T.ff("-i", files[0], "-c", "copy", m4a)
    files.append(m4a)
    files += [f for f in T.SBR_FILES[:3]]
    iters = 3 if T.QUICK else 20

    def one(a):
        i, f = a
        return f, T.run([T.BIN["aac_test_asan"], "fuzz", f, str(i + 11), str(iters)], timeout=1200)
    with ThreadPoolExecutor(T.JOBS) as ex:
        for f, r in ex.map(one, enumerate(files)):
            if r.returncode:
                T.fail("aac fuzz %s: %s" % (f, r.stderr.decode()[-3000:]))
            else:
                print("  ", os.path.basename(f), r.stdout.decode().strip(), flush=True)


def test_speed():
    print("== speed", flush=True)
    res = {}
    for nm, kind, rate, ch, opts in (("sp_lc128", "music", 44100, 2, ["-b:a", "128k"]),
                                     ("sp_lc320", "music", 48000, 2, ["-b:a", "320k"]),
                                     ("sp_lc_51", "music", 48000, 6, ["-b:a", "384k"])):
        path = encode(nm, signal(kind, rate, ch, 20, np.random.default_rng(9)), rate, ch, opts, 20, None)
        r = T.run([T.BIN["aac_test_fast"], "bench", path, "3"]).stdout.decode().strip()
        print("  %-10s %s" % (nm, r), flush=True)
        res[nm] = r
    for f in T.SBR_FILES[-2:]:
        r = T.run([T.BIN["aac_test_fast"], "bench", f, "3"]).stdout.decode().strip()
        print("  %-10s %s" % (os.path.basename(f), r), flush=True)
    return res


def main(t):
    global T
    T = t
    if not hasattr(T, "SBR_FILES"):
        T.SBR_FILES = []
    only = T.ONLY
    if not only or "aac" in only:
        test_lc()
        test_seek()
        test_damaged()
    if not only or "sbr" in only or "fuzz" in only or "speed" in only:
        try:
            import sbrtests
            sbrtests.main(T, globals())
        except ImportError:
            print("(no SBR tests)")
    if not only or "fuzz" in only:
        test_fuzz()
    if not only or "speed" in only:
        test_speed()
