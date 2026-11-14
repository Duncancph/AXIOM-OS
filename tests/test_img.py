#!/usr/bin/env python3
"""Compares DuncOS user/lib/img against Pillow / Python zlib on a randomized corpus.

Usage: python3 tests/test_img.py [--quick] [--seed N]
Builds build/imgtest/test_img (plain) and build/imgtest/test_img_asan (ASan+UBSan),
then runs: PNG / JPEG / GIF / BMP decode comparisons, zlib/gzip round trips,
png_encode round trips and a mutation fuzz pass. Exits non-zero on any failure."""
import io, os, sys, struct, zlib, gzip, random, subprocess, select, time

try:
    from PIL import Image
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "pillow"])
    from PIL import Image
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "imgtest")
CORPUS = os.path.join(OUT, "corpus")
QUICK = "--quick" in sys.argv
SEED = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 1234
R = random.Random(SEED)
os.makedirs(CORPUS, exist_ok=True)

# ---------------------------------------------------------------- build
SRCS = sorted(os.path.join(ROOT, "user/lib/img", f) for f in os.listdir(os.path.join(ROOT, "user/lib/img")) if f.endswith(".c"))
SRCS += [os.path.join(ROOT, "common/str.c"), os.path.join(ROOT, "tests/test_img.c")]
BIN = os.path.join(OUT, "test_img")
ASAN = os.path.join(OUT, "test_img_asan")
subprocess.check_call(["gcc", "-std=gnu17", "-O2", "-fno-builtin", "-I" + os.path.join(ROOT, "user/lib/img"), "-o", BIN] + SRCS)
subprocess.check_call(["gcc", "-std=gnu17", "-O1", "-g", "-fno-builtin", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-I" + os.path.join(ROOT, "user/lib/img"), "-o", ASAN] + SRCS)


class Driver:
    def __init__(self, path):
        self.path = path
        self.start()

    def start(self):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
        self.p = subprocess.Popen([self.path, "batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)

    def _read(self, n, timeout):
        buf = b""
        end = time.time() + timeout
        fd = self.p.stdout.fileno()
        while len(buf) < n:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError
            r, _, _ = select.select([fd], [], [], left)
            if not r:
                raise TimeoutError
            chunk = os.read(fd, n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def req(self, op, data, arg=0, timeout=60):
        self.p.stdin.write(op.encode() + struct.pack("<II", arg, len(data)) + data)
        self.p.stdin.flush()
        st, ex, n = struct.unpack("<qqI", self._read(20, timeout))
        return st, ex, self._read(n, timeout) if n else b""


D = Driver(BIN)
fails = 0
counts = {}
notes = []


def check(name, ok, msg=""):
    global fails
    counts[name] = counts.get(name, 0) + 1
    if not ok:
        fails += 1
        if fails <= 25:
            print("FAIL", name, msg)


def save(name, data):
    p = os.path.join(CORPUS, name)
    with open(p, "wb") as f:
        f.write(data)
    return p


def decode(data):
    st, _, payload = D.req("D", data)
    if st != 0:
        return st, None
    w, h = struct.unpack("<ii", payload[:8])
    return 0, np.frombuffer(payload[8:], dtype=np.uint8).reshape(h, w, 4)


def pil_rgba(data):
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGBA"))


SIZES = [(1, 1), (7, 13), (333, 17), (1, 9), (9, 1), (8, 8), (16, 16), (17, 31), (64, 48)]


def rsize():
    if R.random() < 0.6:
        return R.choice(SIZES)
    return R.randint(1, 90), R.randint(1, 90)


def content(w, h, ch, maxv):
    """Random sample array (h, w, ch) with values 0..maxv: noise, gradients or flat blocks."""
    kind = R.choice(["noise", "grad", "flat"])
    if kind == "noise":
        return np.array([[[R.randint(0, maxv) for _ in range(ch)] for _ in range(w)] for _ in range(h)], dtype=np.int64)
    yy, xx = np.mgrid[0:h, 0:w]
    a = np.zeros((h, w, ch), dtype=np.int64)
    for c in range(ch):
        if kind == "grad":
            fx, fy, ph = R.random() * 3, R.random() * 3, R.randint(0, maxv)
            a[:, :, c] = (xx * fx * maxv / max(w, 1) + yy * fy * maxv / max(h, 1) + ph).astype(np.int64) % (maxv + 1)
        else:
            a[:, :, c] = R.randint(0, maxv)
            for _ in range(3):
                x0, y0 = R.randrange(w), R.randrange(h)
                a[y0:y0 + R.randint(1, h), x0:x0 + R.randint(1, w), c] = R.randint(0, maxv)
    return a


# ---------------------------------------------------------------- PNG writer (reference)
def png_chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def pack_row(samples, depth):
    """samples: flat list of ints for one row"""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in samples)
    out = bytearray()
    acc = nb = 0
    for v in samples:
        acc = (acc << depth) | v
        nb += depth
        if nb == 8:
            out.append(acc)
            acc = nb = 0
    if nb:
        out.append(acc << (8 - nb))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(f, row, prev, bpp):
    out = bytearray(len(row))
    for i in range(len(row)):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
        pr = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]
        out[i] = (row[i] - pr) & 255
    return bytes([f]) + bytes(out)


A7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def png_write(samples, ct, depth, interlace, plte=None, trns=None, fmode="random", level=6, nidat=1):
    h, w, ch = samples.shape
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace)
    bpp = max(1, ch * depth // 8)
    raw = bytearray()
    passes = A7 if interlace else [(0, 0, 1, 1)]
    for (xs, ys, dx, dy) in passes:
        sub = samples[ys::dy, xs::dx]
        if sub.size == 0:
            continue
        prev = None
        for y in range(sub.shape[0]):
            row = pack_row([int(v) for v in sub[y].reshape(-1)], depth)
            f = R.randrange(5) if fmode == "random" else fmode
            raw += filter_row(f, row, prev, bpp)
            prev = row
    z = zlib.compress(bytes(raw), level)
    out = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr)
    out += png_chunk(b"tEXt", b"Comment\x00test")
    if plte is not None:
        out += png_chunk(b"PLTE", bytes(v for rgb in plte for v in rgb))
    if trns is not None:
        out += png_chunk(b"tRNS", trns)
    cuts = sorted(R.sample(range(1, len(z)), min(nidat - 1, len(z) - 1))) if nidat > 1 and len(z) > 1 else []
    pos = 0
    for c in cuts + [len(z)]:
        out += png_chunk(b"IDAT", z[pos:c])
        pos = c
    out += png_chunk(b"IEND", b"")
    return out


def png_expected(samples, ct, depth, plte, trns):
    h, w, ch = samples.shape
    s = samples.astype(np.int64)
    to8 = (lambda v: v >> 8) if depth == 16 else (lambda v: v * (255 // ((1 << depth) - 1)))
    e = np.zeros((h, w, 4), dtype=np.int64)
    if ct == 3:
        pal = np.array(plte, dtype=np.int64)
        al = np.full(len(plte), 255, dtype=np.int64)
        if trns:
            al[:len(trns)] = list(trns)
        idx = s[:, :, 0]
        e[:, :, :3] = pal[idx]
        e[:, :, 3] = al[idx]
        return e.astype(np.uint8)
    if ct in (0, 4):
        for c in range(3):
            e[:, :, c] = to8(s[:, :, 0])
    else:
        for c in range(3):
            e[:, :, c] = to8(s[:, :, c])
    if ct in (4, 6):
        e[:, :, 3] = to8(s[:, :, ch - 1])
    else:
        e[:, :, 3] = 255
        if trns is not None:
            key = struct.unpack(">%dH" % (len(trns) // 2), trns)
            m = np.all(s[:, :, :len(key)] == np.array(key), axis=2) if ct == 2 else s[:, :, 0] == key[0]
            e[:, :, 3][m] = 0
    return e.astype(np.uint8)


def test_png():
    combos = [(0, d) for d in (1, 2, 4, 8, 16)] + [(3, d) for d in (1, 2, 4, 8)] + \
             [(2, 8), (2, 16), (4, 8), (4, 16), (6, 8), (6, 16)]
    reps = 3 if QUICK else 8
    n = 0
    for ct, depth in combos:
        for rep in range(reps):
            for interlace in (0, 1):
                w, h = rsize()
                ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
                plte = trns = None
                if ct == 3:
                    npal = R.randint(1, 1 << depth)
                    plte = [(R.randrange(256), R.randrange(256), R.randrange(256)) for _ in range(npal)]
                    samples = content(w, h, 1, npal - 1)
                    if R.random() < 0.6:
                        trns = bytes(R.randrange(256) for _ in range(R.randint(1, npal)))
                else:
                    samples = content(w, h, ch, (1 << depth) - 1)
                    if ct in (0, 2) and R.random() < 0.5:
                        # pick a key colour that occurs in the image (sometimes)
                        px = samples[R.randrange(h), R.randrange(w)]
                        key = [int(v) for v in px] if R.random() < 0.8 else [R.randrange(1 << depth) for _ in range(ch)]
                        trns = struct.pack(">%dH" % ch, *key)
                fmode = R.choice(["random", 0, 1, 2, 3, 4])
                data = png_write(samples, ct, depth, interlace, plte, trns, fmode, R.randint(0, 9), R.choice([1, 1, 2, 5]))
                name = "png_ct%d_d%d_i%d_%d.png" % (ct, depth, interlace, rep)
                save(name, data)
                exp = png_expected(samples, ct, depth, plte, trns)
                st, got = decode(data)
                ok = st == 0 and got.shape == exp.shape and np.array_equal(got, exp)
                check("png", ok, "%s st=%d" % (name, st))
                # cross-check the reference writer with Pillow where Pillow is unambiguous
                if depth == 8 and not (ct in (0, 2) and trns is not None):
                    check("png-vs-pillow", np.array_equal(pil_rgba(data), exp), name)
                st, wh, _ = D.req("P", data)
                check("png-probe", st == 0 and wh == (w << 32 | h), name)
                n += 1
    # Pillow-written PNGs
    for i in range(10 if QUICK else 30):
        w, h = rsize()
        mode = R.choice(["RGB", "RGBA", "L", "LA", "P", "1"])
        arr = content(w, h, 4, 255).astype(np.uint8)
        im = Image.fromarray(arr, "RGBA").convert(mode) if mode not in ("P",) else \
            Image.fromarray(arr[:, :, :3], "RGB").quantize(R.randint(2, 256))
        bio = io.BytesIO()
        kw = {"optimize": R.random() < 0.5}
        if mode == "P" and R.random() < 0.5:
            kw["transparency"] = R.randrange(len(im.getpalette()) // 3)
        im.save(bio, "PNG", **kw)
        data = bio.getvalue()
        st, got = decode(data)
        exp = pil_rgba(data)
        check("png-pillow", st == 0 and np.array_equal(got, exp), "pillow %s st=%d" % (mode, st))


# ---------------------------------------------------------------- JPEG
def smooth_image(w, h, gray=False):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    img = np.zeros((h, w, 3))
    for c in range(3):
        img[:, :, c] = 128 + 90 * np.sin(xx / R.uniform(3, 40) + R.uniform(0, 6)) * np.cos(yy / R.uniform(3, 40))
    for _ in range(R.randint(0, 4)):
        cx, cy, r = R.uniform(0, w), R.uniform(0, h), R.uniform(2, max(w, h) / 2 + 3)
        m = (xx - cx) ** 2 + (yy - cy) ** 2 < r * r
        img[m] = [R.randrange(256) for _ in range(3)]
    img += np.random.default_rng(R.randrange(1 << 30)).normal(0, R.choice([0, 2, 6]), img.shape)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    return im.convert("L") if gray else im


jpeg_stats = {"files": 0, "worst_max": 0, "worst_frac": 1.0}


def jpeg_compare(data, name):
    path = save(name, data)
    exp = np.asarray(Image.open(io.BytesIO(data)).convert("RGBA")).astype(np.int64)
    st, got = decode(data)
    if st != 0 or got.shape != exp.shape:
        check("jpeg", False, "%s st=%d" % (name, st))
        return
    err = np.abs(got.astype(np.int64) - exp).max(axis=2)
    mx, frac = int(err.max()), float((err <= 4).mean())
    jpeg_stats["files"] += 1
    jpeg_stats["worst_max"] = max(jpeg_stats["worst_max"], mx)
    jpeg_stats["worst_frac"] = min(jpeg_stats["worst_frac"], frac)
    check("jpeg", mx <= 24 and frac >= 0.99, "%s max=%d frac<=4=%.4f" % (name, mx, frac))
    st, wh, _ = D.req("P", data)
    check("jpeg-probe", st == 0 and wh == (exp.shape[1] << 32 | exp.shape[0]), name)


def test_jpeg():
    k = 0
    reps = 2 if QUICK else 5
    for gray in (False, True):
        for prog in (False, True):
            for sub in ([0] if gray else [0, 1, 2]):
                for q in (30, 50, 75, 90, 95, 100):
                    for rep in range(reps if q in (75, 90) else 1):
                        w, h = rsize() if R.random() < 0.7 else (R.randint(90, 300), R.randint(90, 200))
                        im = smooth_image(w, h, gray)
                        kw = dict(quality=q, progressive=prog)
                        if not gray:
                            kw["subsampling"] = sub
                        r = R.random()
                        if r < 0.3:
                            kw["restart_marker_blocks"] = R.randint(1, 9)
                        elif r < 0.45:
                            kw["restart_marker_rows"] = R.randint(1, 3)
                        bio = io.BytesIO()
                        im.save(bio, "JPEG", **kw)
                        jpeg_compare(bio.getvalue(), "jpg_%d_g%d_p%d_s%d_q%d.jpg" % (k, gray, prog, sub, q))
                        k += 1
    # unusual sampling factors via ImageMagick (libjpeg-turbo)
    factors = ["1x2", "2x2,2x1,1x2", "3x1", "4x1", "1x4", "2x2,1x2,2x1", "4x2", "2x4", "1x1,1x2,1x2", "2x1,1x1,2x1"]
    for f in factors:
        for prog in (False, True):
            for rep in range(1 if QUICK else 2):
                w, h = rsize() if rep == 0 else (R.randint(50, 200), R.randint(50, 200))
                src = os.path.join(OUT, "im_src.png")
                smooth_image(w, h).save(src)
                dst = os.path.join(OUT, "im_dst.jpg")
                cmd = ["convert", src, "-sampling-factor", f, "-quality", str(R.choice([60, 85, 95]))]
                if prog:
                    cmd += ["-interlace", "JPEG"]
                if R.random() < 0.4:
                    cmd += ["-define", "jpeg:restart-interval=%d" % R.randint(1, 5)]
                if subprocess.call(cmd + [dst], stderr=subprocess.DEVNULL) != 0:
                    notes.append("convert failed for sampling %s" % f)
                    continue
                jpeg_compare(open(dst, "rb").read(), "jpg_im_%s_p%d_%d.jpg" % (f.replace(",", "_"), prog, rep))
    # CMYK (Adobe, inverted) and YCCK
    for i in range(2 if QUICK else 4):
        w, h = rsize()
        bio = io.BytesIO()
        smooth_image(w, h).convert("CMYK").save(bio, "JPEG", quality=R.choice([70, 90]), progressive=bool(i & 1))
        jpeg_compare(bio.getvalue(), "jpg_cmyk_%d.jpg" % i)
    for i in range(2):
        src = os.path.join(OUT, "im_src.png")
        smooth_image(*rsize()).save(src)
        dst = os.path.join(OUT, "im_dst.jpg")
        if subprocess.call(["convert", src, "-colorspace", "CMYK", "-define", "jpeg:colorspace=11", dst],
                           stderr=subprocess.DEVNULL) == 0:
            jpeg_compare(open(dst, "rb").read(), "jpg_ycck_%d.jpg" % i)


# ---------------------------------------------------------------- GIF
def lzw_encode(idx, mcs):
    clear, eoi = 1 << mcs, (1 << mcs) + 1
    out, acc, nb = bytearray(), 0, 0
    csize = mcs + 1

    def emit(c):
        nonlocal acc, nb
        acc |= c << nb
        nb += csize
        while nb >= 8:
            out.append(acc & 255)
            acc >>= 8
            nb -= 8

    table, nxt = {}, eoi + 1
    emit(clear)
    wcode = idx[0]
    for k in idx[1:]:
        if (wcode, k) in table:
            wcode = table[(wcode, k)]
            continue
        emit(wcode)
        if nxt < 4096:
            table[(wcode, k)] = nxt
            nxt += 1
            if nxt > (1 << csize) and csize < 12:
                csize += 1
        else:
            emit(clear)
            table, nxt, csize = {}, eoi + 1, mcs + 1
        wcode = k
    emit(wcode)
    emit(eoi)
    if nb:
        out.append(acc & 255)
    return bytes(out)


def subblocks(b):
    out = bytearray()
    for i in range(0, len(b), 255):
        out += bytes([len(b[i:i + 255])]) + b[i:i + 255]
    return bytes(out) + b"\x00"


def gif_write(W, H, frame, fx, fy, gpal, lpal, tidx, interlace, ver=b"89a"):
    fh, fw = frame.shape
    def ptab(p):
        n = 2
        while n < len(p):
            n *= 2
        bits = n.bit_length() - 2
        return bits, bytes(v for rgb in p for v in rgb) + b"\x00" * 3 * (n - len(p))
    out = b"GIF" + ver + struct.pack("<HH", W, H)
    if gpal:
        bits, tab = ptab(gpal)
        out += bytes([0x80 | bits, 0, 0]) + tab
    else:
        out += b"\x00\x00\x00"
    out += b"\x21\xfe" + subblocks(b"comment")
    if tidx is not None:
        out += b"\x21\xf9\x04" + bytes([1]) + b"\x00\x00" + bytes([tidx]) + b"\x00"
    flags = 0x40 if interlace else 0
    lt = b""
    if lpal:
        bits, lt = ptab(lpal)
        flags |= 0x80 | bits
    out += b"\x2c" + struct.pack("<HHHH", fx, fy, fw, fh) + bytes([flags]) + lt
    rows = list(range(fh))
    if interlace:
        rows = list(range(0, fh, 8)) + list(range(4, fh, 8)) + list(range(2, fh, 4)) + list(range(1, fh, 2))
    idx = [int(v) for r in rows for v in frame[r]]
    pal = lpal or gpal
    mcs = max(2, (len(pal) - 1).bit_length())
    out += bytes([mcs]) + subblocks(lzw_encode(idx, mcs))
    # a second frame that must be ignored
    out += b"\x2c" + struct.pack("<HHHH", 0, 0, 1, 1) + b"\x00\x02" + subblocks(lzw_encode([1], 2)) + b"\x3b"
    return out


def test_gif():
    for i in range(20 if QUICK else 60):
        W, H = rsize()
        fw, fh = R.randint(1, W), R.randint(1, H)
        fx, fy = R.randint(0, W - fw), R.randint(0, H - fh)
        if R.random() < 0.4:
            fx = fy = 0
            fw, fh = W, H
        n = R.randint(1, 256)
        pal = [(R.randrange(256), R.randrange(256), R.randrange(256)) for _ in range(n)]
        uselocal = R.random() < 0.4
        gpal = [(R.randrange(256),) * 3 for _ in range(R.randint(2, 16))] if uselocal else pal
        lpal = pal if uselocal else None
        frame = content(fw, fh, 1, n - 1)[:, :, 0]
        tidx = R.randrange(n) if R.random() < 0.5 else None
        data = gif_write(W, H, frame, fx, fy, gpal, lpal, tidx, R.random() < 0.5, R.choice([b"87a", b"89a"]))
        name = "gif_%d.gif" % i
        save(name, data)
        exp = np.zeros((H, W, 4), dtype=np.uint8)
        for y in range(fh):
            for x in range(fw):
                c = int(frame[y, x])
                if c != tidx:
                    exp[fy + y, fx + x] = list(pal[c]) + [255]
        st, got = decode(data)
        check("gif", st == 0 and np.array_equal(got, exp), "%s st=%d" % (name, st))
        st, wh, _ = D.req("P", data)
        check("gif-probe", st == 0 and wh == (W << 32 | H), name)
    # Pillow-written GIFs
    for i in range(10 if QUICK else 30):
        w, h = rsize()
        im = Image.fromarray(content(w, h, 3, 255).astype(np.uint8), "RGB").quantize(R.randint(2, 256))
        kw = {"interlace": R.random() < 0.5}
        if R.random() < 0.5:
            kw["transparency"] = R.randrange(len(im.getpalette()) // 3)
        bio = io.BytesIO()
        im.save(bio, "GIF", **kw)
        data = bio.getvalue()
        exp = pil_rgba(data)
        st, got = decode(data)
        ok = st == 0 and got.shape == exp.shape
        if ok:
            vis = exp[:, :, 3] != 0
            ok = np.array_equal(got[:, :, 3], exp[:, :, 3]) and np.array_equal(got[vis], exp[vis])
        check("gif-pillow", ok, "pillow gif %d st=%d" % (i, st))


# ---------------------------------------------------------------- BMP
def bmp_write(px, bpp, topdown=False, pal=None, comp=0, masks=None, hsz=40):
    """px: (h,w) palette indices for bpp<=8, else (h,w,4) RGBA"""
    h, w = px.shape[:2]
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for y in range(h):
        r = bytearray()
        if bpp <= 8:
            r = bytearray(pack_row([int(v) for v in px[y]], bpp))
        else:
            for x in range(w):
                R_, G, B, A_ = (int(v) for v in px[y, x])
                if bpp == 24:
                    r += bytes([B, G, R_])
                elif masks is None:
                    r += bytes([B, G, R_, A_])
                else:
                    def put(v, m):
                        if not m:
                            return 0
                        sh = (m & -m).bit_length() - 1
                        bits = bin(m).count("1")
                        return ((v >> (8 - bits)) if bits <= 8 else v << (bits - 8)) << sh
                    word = put(R_, masks[0]) | put(G, masks[1]) | put(B, masks[2]) | (put(A_, masks[3]) if len(masks) > 3 else 0)
                    r += struct.pack("<H" if bpp == 16 else "<I", word)
        r += b"\x00" * (stride - len(r))
        rows.append(bytes(r))
    if not topdown:
        rows.reverse()
    paldata = b"".join(bytes([b, g, r, 0]) for (r, g, b) in pal) if pal else b""
    extra = b""
    if comp in (3, 6) and hsz == 40:
        extra = b"".join(struct.pack("<I", m) for m in masks[:3 if comp == 3 else 4])
    hdr = struct.pack("<IiiHHIIiiII", hsz, w, -h if topdown else h, 1, bpp, comp, stride * h, 2835, 2835,
                      len(pal) if pal else 0, 0)
    if hsz > 40:
        m = list(masks or [0, 0, 0, 0]) + [0]
        hdr += struct.pack("<IIII", m[0], m[1], m[2], m[3]) + b"\x00" * (hsz - 56)
    off = 14 + len(hdr) + len(extra) + len(paldata)
    body = hdr + extra + paldata + b"".join(rows)
    return b"BM" + struct.pack("<IHHI", 14 + len(body), 0, 0, off) + body


def test_bmp():
    for i in range(20 if QUICK else 50):
        w, h = rsize()
        kind = R.choice(["p1", "p4", "p8", "24", "32", "565", "555", "bf32a", "v5"])
        top = R.random() < 0.5
        if kind[0] == "p":
            bpp = int(kind[1:])
            n = R.randint(1, 1 << bpp)
            pal = [(R.randrange(256), R.randrange(256), R.randrange(256)) for _ in range(n)]
            idx = content(w, h, 1, n - 1)[:, :, 0]
            data = bmp_write(idx, bpp, top, pal)
            exp = np.zeros((h, w, 4), dtype=np.uint8)
            exp[:, :, :3] = np.array(pal, dtype=np.uint8)[idx]
            exp[:, :, 3] = 255
        else:
            rgba = content(w, h, 4, 255)
            exp = rgba.astype(np.uint8).copy()
            if kind == "24":
                data = bmp_write(rgba, 24, top)
                exp[:, :, 3] = 255
            elif kind == "32":
                data = bmp_write(rgba, 32, top)
                exp[:, :, 3] = 255
            elif kind in ("565", "555"):
                masks = [0xF800, 0x7E0, 0x1F] if kind == "565" else [0x7C00, 0x3E0, 0x1F]
                data = bmp_write(rgba, 16, top, comp=3, masks=masks)
                for c, m in enumerate(masks):
                    bits = bin(m).count("1")
                    v = rgba[:, :, c] >> (8 - bits)
                    exp[:, :, c] = v * 255 // ((1 << bits) - 1)
                exp[:, :, 3] = 255
            elif kind == "bf32a":
                data = bmp_write(rgba, 32, top, comp=6, masks=[0xFF0000, 0xFF00, 0xFF, 0xFF000000])
            else:
                data = bmp_write(rgba, 32, top, comp=3, masks=[0xFF0000, 0xFF00, 0xFF, 0xFF000000], hsz=124)
        name = "bmp_%d_%s_t%d.bmp" % (i, kind, top)
        save(name, data)
        st, got = decode(data)
        check("bmp", st == 0 and np.array_equal(got, exp), "%s st=%d" % (name, st))
        if kind in ("p1", "p4", "p8", "24", "32") or kind == "v5":
            check("bmp-vs-pillow", np.array_equal(pil_rgba(data), exp), name)
    for mode in ("RGB", "P", "L", "1"):
        w, h = rsize()
        im = Image.fromarray(content(w, h, 3, 255).astype(np.uint8), "RGB")
        im = im.quantize(200) if mode == "P" else im.convert(mode)
        bio = io.BytesIO()
        im.save(bio, "BMP")
        data = bio.getvalue()
        st, got = decode(data)
        check("bmp-pillow", st == 0 and np.array_equal(got, pil_rgba(data)), "pillow bmp " + mode)


# ---------------------------------------------------------------- zlib / gzip
def test_zlib():
    text = (open(os.path.join(ROOT, "tests/test_img.py"), "rb").read() * 3)
    datas = [b"", b"a", bytes(R.randrange(256) for _ in range(1000)), text[:100000], b"\x00" * 70000,
             bytes(R.randrange(4) for _ in range(50000)), text[:5]]
    if not QUICK:
        datas.append(os.urandom(300000))
    for d in datas:
        for lvl in range(10):
            z = zlib.compress(d, lvl)
            st, _, out = D.req("Z", z, len(d))
            check("zlib", st == len(d) and out == d, "zlib lvl %d len %d st %d" % (lvl, len(d), st))
            co = zlib.compressobj(lvl, zlib.DEFLATED, -15, R.randint(1, 9), R.choice([0, 1, 2, 3, 4]))
            raw = co.compress(d) + co.flush()
            st, used, out = D.req("R", raw + b"JUNK", len(d))
            check("inflate_raw", st == len(d) and out == d and used == len(raw), "raw lvl %d len %d" % (lvl, len(d)))
            g = gzip.compress(d, lvl, mtime=0)
            st, _, out = D.req("G", g, len(d))
            check("gunzip", st == len(d) and out == d, "gzip lvl %d len %d st %d" % (lvl, len(d), st))
            st, _, _ = D.req("S", g)
            check("gzip_size", st == len(d))
        # too-small output buffer must fail cleanly
        if d:
            st, _, _ = D.req("Z", zlib.compress(d), len(d) - 1)
            check("zlib-space", st < 0)
            bad = bytearray(zlib.compress(d))
            bad[-1] ^= 1
            st, _, _ = D.req("Z", bytes(bad), len(d))
            check("zlib-adler", st < 0)
    # gzip header with FNAME / FEXTRA / FCOMMENT / FHCRC, multi-member
    bio = io.BytesIO()
    with gzip.GzipFile(filename="hello.txt", mode="wb", fileobj=bio, mtime=1) as f:
        f.write(text[:3000])
    st, _, out = D.req("G", bio.getvalue(), 3000)
    check("gunzip-fname", st == 3000 and out == text[:3000])
    body = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = body.compress(b"xyz" * 100) + body.flush()
    hdr = b"\x1f\x8b\x08" + bytes([4 | 8 | 16 | 2]) + b"\x00" * 4 + b"\x00\x03" + b"\x04\x00abcd" + b"name\x00" + b"cmt\x00"
    hdr += struct.pack("<H", zlib.crc32(hdr) & 0xFFFF)
    g = hdr + payload + struct.pack("<II", zlib.crc32(b"xyz" * 100), 300)
    st, _, out = D.req("G", g + gzip.compress(b"tail"), 304)
    check("gunzip-flags-multi", st == 304 and out == b"xyz" * 100 + b"tail", "st=%d" % st)


# ---------------------------------------------------------------- png_encode
def ui_image(w, h):
    a = np.zeros((h, w, 4), dtype=np.uint8)
    a[:, :] = [235, 235, 240, 255]
    for _ in range(40):
        x0, y0 = R.randrange(w), R.randrange(h)
        a[y0:y0 + R.randint(2, 80), x0:x0 + R.randint(2, 200)] = [R.randrange(256), R.randrange(256), R.randrange(256), 255]
    for _ in range(300):  # "text"
        x0, y0 = R.randrange(w - 8), R.randrange(h - 12)
        glyph = np.array([[R.random() < 0.4 for _ in range(6)] for _ in range(9)])
        a[y0:y0 + 9, x0:x0 + 6][glyph] = [20, 20, 20, 255]
    return a


def test_encode():
    cases = [(1, 1), (7, 13), (333, 17), (3, 200), (64, 64)] + [rsize() for _ in range(6 if QUICK else 20)]
    for i, (w, h) in enumerate(cases + [(640, 400)]):
        arr = ui_image(w, h) if (w, h) == (640, 400) else content(w, h, 4, 255).astype(np.uint8)
        if R.random() < 0.3 and (w, h) != (640, 400):
            arr = ui_image(max(w, 16), max(h, 16))[:h, :w]
        for opaque in (0, 1):
            a = arr.astype(np.uint32)
            words = (a[:, :, 3] << 24) | (a[:, :, 0] << 16) | (a[:, :, 1] << 8) | a[:, :, 2]
            if opaque:
                words &= 0xFFFFFF
            st, bound, png = D.req("e" if opaque else "E", words.astype("<u4").tobytes(), w)
            if st <= 0:
                check("png_encode", False, "st=%d %dx%d" % (st, w, h))
                continue
            exp = arr.copy()
            if opaque:
                exp[:, :, 3] = 255
            im = Image.open(io.BytesIO(png))
            im.load()
            check("png_encode", np.array_equal(np.asarray(im.convert("RGBA")), exp), "%dx%d opaque=%d" % (w, h, opaque))
            st2, got = decode(png)
            check("png_encode-self", st2 == 0 and np.array_equal(got, exp))
            raw = (w * (3 if opaque else 4) + 1) * h
            check("png_encode-size", len(png) <= raw + 5 * (raw // 65535 + 1) + 100 and len(png) <= bound,
                  "%d vs raw %d" % (len(png), raw))
            if (w, h) == (640, 400):
                ref = io.BytesIO()
                Image.fromarray(exp[:, :, :3] if opaque else exp).save(ref, "PNG")
                notes.append("png_encode UI 640x400 opaque=%d: %d bytes (raw %d, Pillow %d)" % (opaque, len(png), raw, len(ref.getvalue())))


# ---------------------------------------------------------------- fuzz
def mutations(data, n):
    out = []
    L = len(data)
    for _ in range(n):
        r = R.random()
        if r < 0.3:
            out.append(data[:R.randrange(L)])
        elif r < 0.75:
            b = bytearray(data)
            for _ in range(R.randint(1, 8)):
                p = R.randrange(L)
                b[p] ^= 1 << R.randrange(8)
            out.append(bytes(b))
        elif r < 0.9:
            b = bytearray(data)
            for _ in range(R.randint(1, 4)):
                p = R.randrange(L)
                b[p] = R.choice([0, 0xFF, 0x7F, 0x80, R.randrange(256)])
            out.append(bytes(b))
        else:
            b = bytearray(data)
            p = R.randrange(L)
            q = min(L, p + R.randint(1, 64))
            del b[p:q]
            out.append(bytes(b))
    return out


def test_fuzz():
    F = Driver(ASAN)
    files = sorted(os.listdir(CORPUS))
    small = [f for f in files if os.path.getsize(os.path.join(CORPUS, f)) < 40000]
    seeds = R.sample(small, min(len(small), 30 if QUICK else 80))
    per = 10 if QUICK else 25
    cases = []
    for s in seeds:
        d = open(os.path.join(CORPUS, s), "rb").read()
        if len(d) > 1:
            cases += [(s, m) for m in mutations(d, per)]
    # hostile headers: huge dimensions
    big_png = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 100000, 100000, 8, 6, 0, 0, 0)) + \
              png_chunk(b"IDAT", zlib.compress(b"\x00" * 100)) + png_chunk(b"IEND", b"")
    big_png2 = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 16384, 16384, 8, 6, 0, 0, 0)) + \
               png_chunk(b"IDAT", zlib.compress(b"\x00" * 100)) + png_chunk(b"IEND", b"")
    big_gif = b"GIF89a" + struct.pack("<HH", 65535, 65535) + b"\x00\x00\x00" + b"\x2c" + struct.pack("<HHHH", 0, 0, 65535, 65535) + b"\x00\x02\x02\x4c\x01\x00\x3b"
    big_bmp = b"BM" + struct.pack("<IHHI", 0, 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, 30000, -30000, 1, 24, 0, 0, 0, 0, 0, 0)
    for nm, d in (("bigpng", big_png), ("bigpng2", big_png2), ("biggif", big_gif), ("bigbmp", big_bmp)):
        cases.append((nm, d))
    zseed = [zlib.compress(os.urandom(500) + b"hello" * 200, l) for l in (1, 6, 9)] + [gzip.compress(b"abc" * 1000)]
    zcases = [m for z in zseed for m in mutations(z, 60 if not QUICK else 20)]
    crashes = 0
    for src, d in cases:
        try:
            st, _, _ = F.req("D", d, timeout=30)
            check("fuzz-noleak", st != -1000 and st != -1001, src)
            F.req("P", d, timeout=30)
            counts["fuzz-cases"] = counts.get("fuzz-cases", 0) + 1
        except (EOFError, TimeoutError, BrokenPipeError) as e:
            crashes += 1
            p = save("CRASH_%d_%s" % (crashes, src), d)
            check("fuzz", False, "%s on mutation of %s -> %s" % (type(e).__name__, src, p))
            F.p.kill()
            F = Driver(ASAN)
    for nm, d in cases[-4:]:
        st, _, _ = F.req("D", d)
        check("too-big", st == -5, "%s st=%d" % (nm, st))
    for d in zcases:
        try:
            for op in "ZGR":
                F.req(op, d, 100000, timeout=30)
            counts["fuzz-cases"] = counts.get("fuzz-cases", 0) + 1
        except (EOFError, TimeoutError, BrokenPipeError) as e:
            crashes += 1
            p = save("CRASH_z%d" % crashes, d)
            check("fuzz", False, "%s on zlib mutation -> %s" % (type(e).__name__, p))
            F.p.kill()
            F = Driver(ASAN)
    F.p.stdin.close()
    F.p.wait()
    check("fuzz-exit", F.p.returncode == 0, "asan driver exit %d" % F.p.returncode)
    counts["fuzz"] = counts.get("fuzz", 0) + (0 if crashes else 1)


t0 = time.time()
test_zlib()
test_png()
test_jpeg()
test_gif()
test_bmp()
test_encode()
test_fuzz()
D.p.stdin.close()
D.p.wait()
for n in notes:
    print("note:", n)
total = sum(v for k, v in counts.items())
print("img: %s; JPEG %d files worst max err %d, worst frac(err<=4) %.4f; %d checks, %d failures (%.0fs)" % (
    " ".join("%s=%d" % kv for kv in sorted(counts.items())), jpeg_stats["files"], jpeg_stats["worst_max"],
    jpeg_stats["worst_frac"], total, fails, time.time() - t0))
sys.exit(1 if fails else 0)
