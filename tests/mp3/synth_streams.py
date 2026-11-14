"""Synthetic MPEG audio bitstreams for features that no encoder on the test host produces.

Layer I: random (but valid) allocations, scalefactors and samples.
Layer III: random side info and spectra, Huffman-coded with the ISO tables taken from
user/lib/audio/mp3_tables.c: long/start/short/stop and mixed blocks, MS + intensity stereo (MPEG-1 and LSF),
scfsi, preflag, scalefac_scale, subblock gain, every Huffman table (incl. linbits), both count1 tables,
CRC protection, ancillary data and a well-used bit reservoir.
The content is noise-like; the test compares the decoder against ffmpeg on these streams.

Known interpretation differences between ISO 11172-3/13818-3 and ffmpeg are avoided here
(see README.md): MPEG-2 LSF intensity positions equal to the 'illegal' maximum or >= 16, illegal
window sequences (e.g. long -> short without a start block, mixed -> short), and short/mixed blocks
at 8 kHz."""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(HERE, "..", "..", "user", "lib", "audio", "mp3_tables.c")


def load_huff():
    src = open(TABLES).read()
    arr = {}
    for kind, num, body in re.findall(r"static const uint\d+_t h([cl])(\d+)\[\d+\] = \{([^}]*)\}", src):
        arr[(kind, int(num))] = [int(x) for x in body.replace("\n", " ").split(",") if x.strip()]
    return arr


H = load_huff()
DEBUG = []   # granules of every generated Layer III frame (for debugging)
LIN = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 8, 10, 13, 4, 5, 6, 7, 8, 9, 11, 13]


def tsrc(t):
    return t if t < 16 else (16 if t < 24 else 24)


def tside(t):
    return int(len(H[("c", tsrc(t))]) ** 0.5)


def tmax(t):   # largest magnitude a table can code
    if t in (0, 4, 14): return 0
    if t < 16: return tside(t) - 1
    return 15 + (1 << LIN[t]) - 1


class BW:
    def __init__(self):
        self.bits = []

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def extend(self, other):
        self.bits.extend(other.bits)

    def __len__(self):
        return len(self.bits)

    def tobytes(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


SR = {44100: (3, 0), 48000: (3, 1), 32000: (3, 2), 22050: (2, 0), 24000: (2, 1), 16000: (2, 2),
      11025: (0, 0), 12000: (0, 1), 8000: (0, 2)}
SRT = {44100: 0, 48000: 1, 32000: 2, 22050: 3, 24000: 4, 16000: 5, 11025: 6, 12000: 7, 8000: 8}
BR1 = {1: [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
       3: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]}
BR2_3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]

LONG_W = [
    [4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 20, 24, 28, 34, 42, 50, 54, 76, 158],
    [4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 10, 12, 16, 18, 22, 28, 34, 40, 46, 54, 54, 192],
    [4, 4, 4, 4, 4, 4, 6, 6, 8, 10, 12, 16, 20, 24, 30, 38, 46, 56, 68, 84, 102, 26],
    [6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54],
    [6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 18, 22, 26, 32, 38, 46, 54, 62, 70, 76, 36],
    [6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54],
    [6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54],
    [6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54],
    [12, 12, 12, 12, 12, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 76, 90, 2, 2, 2, 2, 2]]
SHORT_W = [
    [4, 4, 4, 4, 6, 8, 10, 12, 14, 18, 22, 30, 56], [4, 4, 4, 4, 6, 6, 10, 12, 14, 16, 20, 26, 66],
    [4, 4, 4, 4, 6, 8, 12, 16, 20, 26, 34, 42, 12], [4, 4, 4, 6, 6, 8, 10, 14, 18, 26, 32, 42, 18],
    [4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 32, 44, 12], [4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18],
    [4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18], [4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18],
    [8, 8, 8, 12, 16, 20, 24, 28, 36, 2, 2, 2, 26]]
SLEN = [[0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4], [0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3]]
LSF_NSFB = [[[6, 5, 5, 5], [9, 9, 9, 9], [6, 9, 9, 9]], [[6, 5, 7, 3], [9, 9, 12, 6], [6, 9, 12, 6]],
            [[11, 10, 0, 0], [18, 18, 0, 0], [15, 18, 0, 0]], [[7, 7, 7, 0], [12, 12, 12, 0], [6, 15, 12, 0]],
            [[6, 6, 6, 3], [12, 9, 9, 6], [6, 12, 9, 6]], [[8, 8, 5, 0], [15, 12, 9, 0], [6, 18, 9, 0]]]


def bands(srt, lsf, bt, mixed):
    """Band list in storage order: (start, width, window (-1 long), sfb)."""
    out, pos = [], 0
    if bt != 2:
        for s, w in enumerate(LONG_W[srt]):
            out.append((pos, w, -1, s)); pos += w
        return out
    s0 = 0
    if mixed:
        for s in range(6 if lsf else 8):
            w = LONG_W[srt][s]; out.append((pos, w, -1, s)); pos += w
        s0 = 3
    for s in range(s0, 13):
        for win in range(3):
            w = SHORT_W[srt][s]; out.append((pos, w, win, s)); pos += w
    return out


def lsf_slen(sfc, is_right):
    if not is_right:
        if sfc < 400: return [(sfc >> 4) // 5, (sfc >> 4) % 5, (sfc & 15) >> 2, sfc & 3], 0, 0
        if sfc < 500:
            s = sfc - 400; return [(s >> 2) // 5, (s >> 2) % 5, s & 3, 0], 1, 0
        s = sfc - 500; return [s // 3, s % 3, 0, 0], 2, 1
    isc = sfc >> 1
    if isc < 180: return [isc // 36, (isc % 36) // 6, (isc % 36) % 6, 0], 3, 0
    if isc < 244:
        s = isc - 180; return [(s & 63) >> 4, (s & 15) >> 2, s & 3, 0], 4, 0
    s = isc - 244; return [s // 3, s % 3, 0, 0], 5, 0


def huff_pair(bw, t, x, y):
    side = tside(t)
    lin = LIN[t]
    ax, ay = abs(x), abs(y)
    cx, cy = (min(ax, 15), min(ay, 15)) if lin else (ax, ay)
    i = cx * side + cy
    bw.put(H[("c", tsrc(t))][i], H[("l", tsrc(t))][i])
    if lin and cx == 15: bw.put(ax - 15, lin)
    if ax: bw.put(1 if x < 0 else 0, 1)
    if lin and cy == 15: bw.put(ay - 15, lin)
    if ay: bw.put(1 if y < 0 else 0, 1)


def huff_quad(bw, t, q):
    i = (abs(q[0]) << 3) | (abs(q[1]) << 2) | (abs(q[2]) << 1) | abs(q[3])
    bw.put(H[("c", t)][i], H[("l", t)][i])
    for v in q:
        if v: bw.put(1 if v < 0 else 0, 1)


NEXT_KIND = {"long": ["long", "start"], "stop": ["long", "start"], "start": ["short", "mixed"],
             "short": ["short", "mixed", "stop"], "mixed": ["mixed", "stop"]}
# (mixed -> short is legal, but ffmpeg's short-block IMDCT drops the overlap of the two long subbands
#  a mixed block leaves behind; this decoder adds it as ISO/dist10/libmad do. Not generated.)


class Gran:
    pass


def make_granule(rng, cfg, srt, lsf, ch, gr, prev_scf, is_right, budget_lines):
    """Random granule: returns Gran with side info fields and its part2_3 bit string."""
    g = Gran()
    choices = ["long", "long", "start", "short", "stop", "mixed"]
    if cfg.get("blocks"): choices = cfg["blocks"]
    kind = rng.choice(choices)
    g.wsf = kind != "long"
    g.bt = {"long": 0, "start": 1, "short": 2, "stop": 3, "mixed": 2}[kind]
    g.mixed = 1 if kind == "mixed" else 0
    g.gg = rng.randrange(125, 170)
    g.scale = rng.randrange(2)
    g.sbg = [rng.randrange(8) for _ in range(3)] if g.bt == 2 else [0, 0, 0]
    g.c1 = rng.randrange(2)
    bl = bands(srt, lsf, g.bt, g.mixed)
    nb = len(bl)
    # ---- scalefactors
    scf = [0] * 40
    bw = BW()
    g.preflag = 0
    if not lsf:
        g.sfc = rng.randrange(16)
        s1, s2 = SLEN[0][g.sfc], SLEN[1][g.sfc]
        g.scfsi = 0
        if g.bt == 2:
            n1 = 17 if g.mixed else 18
            for k in range(n1): scf[k] = rng.randrange(1 << s1); bw.put(scf[k], s1)
            for k in range(n1, n1 + 18): scf[k] = rng.randrange(1 << s2); bw.put(scf[k], s2)
        else:
            g.preflag = rng.randrange(2)
            grp = [0, 6, 11, 16, 21]
            scfsi = rng.randrange(16) if (gr == 1 and cfg.get("scfsi") and prev_scf is not None) else 0
            # scfsi only makes sense if the slen of reused groups is irrelevant: values are copied as-is
            for k in range(4):
                sl = s1 if k < 2 else s2
                for i in range(grp[k], grp[k + 1]):
                    if scfsi >> (3 - k) & 1:
                        scf[i] = prev_scf[i]
                    else:
                        scf[i] = rng.randrange(1 << sl); bw.put(scf[i], sl)
            g.scfsi = scfsi
        ismax = [7] * 40
    else:
        bti = (2 if g.mixed else 1) if g.bt == 2 else 0
        while True:
            g.sfc = rng.randrange(512)
            slen, tab, pf = lsf_slen(g.sfc, is_right)
            nsfb = LSF_NSFB[tab][bti]
            if is_right and any(slen[p] == 0 and nsfb[p] > 0 for p in range(4)): continue   # avoid 'illegal' 0-bit IS positions
            break
        g.preflag = pf
        ismax = [0] * 40
        n = 0
        for p in range(4):
            for _ in range(nsfb[p]):
                mx = (1 << slen[p]) - 1
                # right channel with intensity stereo: never the 'illegal' maximum, and below 16 (ffmpeg
                # treats LSF positions >= 16 as 'not intensity'; ISO allows up to 30 with 5-bit scalefactors)
                v = rng.randrange(min(mx, 16)) if (is_right and mx > 0) else rng.randrange(mx + 1)
                scf[n] = v; ismax[n] = mx; bw.put(v, slen[p]); n += 1
    g.scf = scf
    part2 = len(bw)
    # ---- spectrum (storage order)
    iv = [0] * 576
    top = rng.randrange(max(4, budget_lines // 4), max(8, budget_lines))
    zero_from = [576, 576, 576, 576]   # per window (+1); long = index 0
    if is_right and cfg.get("is"):
        # right channel of intensity stereo: zero above a band per window
        if g.bt == 2:
            for w in range(3):
                cut = rng.randrange(0 if not g.mixed else 3, 14)
                zero_from[w + 1] = cut
            if g.mixed and rng.randrange(3) == 0:
                zero_from[1] = zero_from[2] = zero_from[3] = 3  # no short data at all: long part may become IS
                zero_from[0] = rng.randrange(0, 9)
            else:
                zero_from[0] = 99
        else:
            zero_from[0] = rng.randrange(0, 23)
    big_lim = rng.choice([1, 2, 3, 5, 7, 15, 15, 40, 300, 2000, 8000])
    count1_from = rng.randrange(0, top + 1)
    for (st, w, win, sfb) in bl:
        for i in range(st, st + w):
            if i >= top: break
            if is_right and cfg.get("is"):
                if win < 0 and sfb >= zero_from[0]: continue
                if win >= 0 and sfb >= zero_from[win + 1]: continue
            r = rng.random()
            if r < 0.35: v = 0
            elif i >= count1_from or r < 0.75: v = rng.choice([-1, 1])
            else: v = rng.randrange(-big_lim, big_lim + 1)
            if i >= count1_from: v = max(-1, min(1, v))
            iv[i] = v
    # keep the requantized amplitude in a realistic range (|x| <= ~0.7 of full scale); ffmpeg zeroes values
    # that overflow its fixed-point dequantizer, which real streams never reach
    mx = max(abs(v) for v in iv)
    if mx:
        import math
        lim = int(math.floor(210 + 4 * math.log2(0.7 / mx ** (4.0 / 3))))
        g.gg = max(0, min(g.gg, lim))
    # regions
    last_big = max([i for i in range(576) if abs(iv[i]) > 1] + [-1])
    big_end = ((last_big + 2) // 2) * 2
    if big_end < 576 and rng.randrange(4) == 0: big_end = min(576, big_end + 2 * rng.randrange(1, 20))
    last_nz = max([i for i in range(576) if iv[i]] + [-1])
    c1_end = big_end
    while c1_end <= last_nz: c1_end += 4
    if c1_end > 576:   # quads cannot cross 576: move the tail into big values
        big_end = min(576, ((last_nz + 2) // 2) * 2); c1_end = big_end
    g.big_values = big_end // 2
    lw = LONG_W[srt]
    if g.wsf:
        if g.bt == 2: r1 = 3 * sum(SHORT_W[srt][:3])
        else: r1 = sum(lw[:8])
        r2 = 576
        g.r0 = g.r1 = 0
    else:
        g.r0 = rng.randrange(16); g.r1 = rng.randrange(8)
        r1 = sum(lw[:min(g.r0 + 1, 22)]); r2 = sum(lw[:min(g.r0 + g.r1 + 2, 22)])
    rends = [min(r1, big_end), min(r2, big_end), big_end]
    g.tables = []
    i = 0
    for r in range(3):
        e = rends[r]
        mx = max([abs(iv[k]) for k in range(i, max(i, e))] + [0])
        if g.wsf and r == 2:
            g.tables.append(0); continue
        ok = [t for t in range(32) if t not in (4, 14) and tmax(t) >= mx and (t != 0 or mx == 0 or e <= i)]
        if mx == 0 and rng.randrange(2) == 0: t = 0
        else: t = rng.choice([t for t in ok if t != 0] or [0])
        g.tables.append(t)
        i = max(i, e)
    i = 0
    for r in range(3):
        e = rends[r]
        t = g.tables[r]
        while i < e:
            if t == 0: i += 2; continue
            huff_pair(bw, t, iv[i], iv[i + 1]); i += 2
        i = max(i, e)
    i = big_end
    while i < c1_end:
        huff_quad(bw, 32 + g.c1, iv[i:i + 4]); i += 4
    g.part23 = len(bw)
    g.bits = bw
    g.iv = iv
    return g


def side_info(gl, lsf, nch, mdb, scfsi):
    bw = BW()
    if not lsf:
        bw.put(mdb, 9); bw.put(0, 5 if nch == 1 else 3)
        for ch in range(nch): bw.put(scfsi[ch], 4)
    else:
        bw.put(mdb, 8); bw.put(0, 1 if nch == 1 else 2)
    for grs in gl:
        for g in grs:
            bw.put(g.part23, 12); bw.put(g.big_values, 9); bw.put(g.gg, 8); bw.put(g.sfc, 9 if lsf else 4)
            bw.put(1 if g.wsf else 0, 1)
            if g.wsf:
                bw.put(g.bt, 2); bw.put(g.mixed, 1)
                for t in g.tables[:2]: bw.put(t, 5)
                for s in g.sbg: bw.put(s, 3)
            else:
                for t in g.tables: bw.put(t, 5)
                bw.put(g.r0, 4); bw.put(g.r1, 3)
            if not lsf: bw.put(g.preflag, 1)
            bw.put(g.scale, 1); bw.put(g.c1, 1)
    return bw


def layer3_stream(rng, sr, kbps, mode, nframes, cfg):
    ver, sri = SR[sr]
    lsf = ver != 3
    srt = SRT[sr]
    nch = 1 if mode == 3 else 2
    bri = (BR1[3] if not lsf else BR2_3).index(kbps)
    ngr = 1 if lsf else 2
    side_len = (9 if nch == 1 else 17) if lsf else (17 if nch == 1 else 32)
    crc = cfg.get("crc", 0)
    num = (72 if lsf else 144) * kbps * 1000
    frac = 0
    frames, stream = [], bytearray()   # stream: global main data byte stream
    cap_before = 0
    prev_kind = "long"
    out = bytearray()
    for k in range(nframes):
        frac += num % sr
        pad = 0
        if frac >= sr: frac -= sr; pad = 1
        size = num // sr + pad
        cap = size - 4 - (2 if crc else 0) - side_len
        mode_ext = cfg.get("mode_ext", rng.randrange(4)) if mode == 1 else 0
        start = max(len(stream), cap_before - (255 if lsf else 511), 0)
        avail_bits = (cap_before + cap - start) * 8
        budget = cfg.get("lines", 400)
        for attempt in range(40):
            gl = []
            pk = prev_kind
            for gr in range(ngr):
                # block types follow a legal window sequence (ffmpeg relies on it), same for both channels
                nxt = NEXT_KIND[pk]
                pool = [b for b in cfg.get("blocks", ["long", "long", "start", "short", "stop", "mixed"]) if b in nxt] or nxt
                pk = rng.choice(pool)
                c = dict(cfg); c["blocks"] = [pk]
                row = []
                for ch in range(nch):
                    g = make_granule(rng, c, srt, lsf, ch, gr, gl[0][ch].scf if gr == 1 else None,
                                     ch == 1 and (mode_ext & 1) and mode == 1, budget)
                    row.append(g)
                gl.append(row)
            total = sum(g.part23 for row in gl for g in row)
            if total <= avail_bits:
                prev_kind = pk
                break
            budget = max(8, budget * 2 // 3)
        else:
            raise RuntimeError("cannot fit granules")
        # scfsi: granule 1 may copy granule 0's long scalefactors (only valid when both are long-ish)
        scfsi = [0, 0]
        if not lsf:
            for ch in range(nch):
                g0, g1 = gl[0][ch], gl[1][ch]
                scfsi[ch] = getattr(g1, "scfsi", 0) if (g1.bt != 2) else 0
        DEBUG.append(gl)
        md = BW()
        for row in gl:
            for g in row: md.extend(g.bits)
        mdb = cap_before - start
        data = md.tobytes()
        # ancillary bytes after the granule data, if room
        room = cap_before + cap - start - len(data)
        anc = bytes(rng.randrange(256) for _ in range(rng.randrange(0, min(room, 40) + 1))) if room > 0 else b""
        if len(stream) < start: stream += bytes(start - len(stream))
        stream += data + anc
        hdr = 0xFFE00000 | ver << 19 | 1 << 17 | (0 if crc else 1) << 16 | bri << 12 | sri << 10 | pad << 9 | mode << 6 | mode_ext << 4
        hb = hdr.to_bytes(4, "big")
        sb = side_info(gl, lsf, nch, mdb, scfsi).tobytes()
        frame = bytearray(hb)
        if crc:
            c = crc16(hb[2:4] + sb)
            frame += c.to_bytes(2, "big")
        frame += sb
        # main data of this frame: bytes cap_before .. cap_before+cap of the global stream
        if len(stream) < cap_before + cap: stream += bytes(cap_before + cap - len(stream))
        frame += stream[cap_before: cap_before + cap]
        assert len(frame) == size, (len(frame), size)
        out += frame
        cap_before += cap
    return bytes(out)


def layer1_stream(rng, sr, kbps, mode, nframes):
    ver, sri = SR[sr]
    assert ver == 3
    bri = BR1[1].index(kbps)
    nch = 1 if mode == 3 else 2
    out = bytearray()
    num = 12 * kbps * 1000
    frac = 0
    for k in range(nframes):
        frac += num % sr
        pad = 0
        if frac >= sr: frac -= sr; pad = 1
        size = (num // sr + pad) * 4
        mode_ext = rng.randrange(4) if mode == 1 else 0
        bound = (mode_ext + 1) * 4 if mode == 1 else 32
        budget = size * 8 - 32
        while True:
            alloc = [[0] * 32 for _ in range(2)]
            for sb in range(32):
                for ch in range(nch):
                    if sb >= bound and ch == 1: alloc[1][sb] = alloc[0][sb]; continue
                    alloc[ch][sb] = rng.choice([0, 0, 1, 2, 3, 4, 5, 6, 8, 10, 14]) if sb < 28 else rng.choice([0, 1, 2])
            nbits = 0
            for sb in range(32):
                nbits += 4 * (nch if sb < bound else 1)
                for ch in range(nch):
                    if alloc[ch][sb]: nbits += 6
                shared = sb >= bound and nch == 2
                nbits += 12 * sum((alloc[ch][sb] + 1) if alloc[ch][sb] else 0 for ch in range(1 if shared else nch))
            if nbits <= budget: break
        bw = BW()
        hdr = 0xFFE00000 | ver << 19 | 3 << 17 | 1 << 16 | bri << 12 | sri << 10 | pad << 9 | mode << 6 | mode_ext << 4
        bw.put(hdr, 32)
        for sb in range(32):
            for ch in range(nch):
                if sb >= bound and ch == 1: continue
                bw.put(alloc[ch][sb], 4)
        for sb in range(32):
            for ch in range(nch):
                if alloc[ch][sb]: bw.put(rng.randrange(6, 63), 6)
        for s in range(12):
            for sb in range(32):
                for ch in range(nch):
                    if sb >= bound and ch == 1: continue
                    a = alloc[ch][sb]
                    if a: bw.put(rng.randrange((1 << (a + 1)) - 1), a + 1)
        b = bw.tobytes()
        b += bytes(size - len(b))
        out += b
    return bytes(out)


# ---------------------------------------------------------------- Layer II
_QA0 = [3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535]
_QA1 = [3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535]
_QA2 = [3, 5, 7, 9, 15, 31, 65535]
_QA3 = [3, 5, 65535]
_QC0 = [3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767]
_QC1 = [3, 5, 9, 15, 31, 63, 127]
_QL0 = [3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383]
_QL2 = [3, 5, 9]
L2TAB = [
    [(4, _QA0)] * 3 + [(4, _QA1)] * 8 + [(3, _QA2)] * 12 + [(2, _QA3)] * 4,
    [(4, _QA0)] * 3 + [(4, _QA1)] * 8 + [(3, _QA2)] * 12 + [(2, _QA3)] * 7,
    [(4, _QC0)] * 2 + [(3, _QC1)] * 6,
    [(4, _QC0)] * 2 + [(3, _QC1)] * 10,
    [(4, _QL0)] * 4 + [(3, _QC1)] * 7 + [(2, _QL2)] * 19,
]
BR2 = {False: [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384], True: BR2_3}


def layer2_stream(rng, sr, kbps, mode, nframes):
    """Random Layer II frames (MPEG-1 or LSF), incl. joint stereo with intensity coding above the bound."""
    ver, sri = SR[sr]
    lsf = ver != 3
    nch = 1 if mode == 3 else 2
    bri = BR2[lsf].index(kbps)
    if lsf: t = 4
    else:
        chbr = kbps // nch
        if (sr == 48000 and chbr >= 56) or (56 <= chbr <= 80): t = 0
        elif sr != 48000 and chbr >= 96: t = 1
        elif sr != 32000 and chbr <= 48: t = 2
        else: t = 3
    tab = L2TAB[t]
    sbl = len(tab)
    num = 144 * kbps * 1000
    frac = 0
    out = bytearray()
    for k in range(nframes):
        frac += num % sr
        pad = 0
        if frac >= sr: frac -= sr; pad = 1
        size = num // sr + pad
        mode_ext = rng.randrange(4) if mode == 1 else 0
        bound = min((mode_ext + 1) * 4, sbl) if mode == 1 else sbl
        budget = size * 8 - 32
        for attempt in range(200):
            alloc = [[0] * 32 for _ in range(2)]
            for sb in range(sbl):
                nbal = tab[sb][0]
                for ch in range(nch):
                    if sb >= bound and ch == 1: alloc[1][sb] = alloc[0][sb]; continue
                    alloc[ch][sb] = 0 if rng.random() < 0.3 + attempt * 0.01 else rng.randrange(1, 1 << nbal)
            scfsi = [[rng.randrange(4) for _ in range(32)] for _ in range(2)]
            nbits = 0
            for sb in range(sbl):
                nbits += tab[sb][0] * (nch if sb < bound else 1)
                for ch in range(nch):
                    if alloc[ch][sb]:
                        nbits += 2 + 6 * [3, 2, 1, 2][scfsi[ch][sb]]
                for ch in range(nch if sb < bound else 1):
                    a = alloc[ch][sb]
                    if not a: continue
                    lv = tab[sb][1][a - 1]
                    nbits += 12 * ({3: 5, 5: 7, 9: 10}[lv] if lv in (3, 5, 9) else 3 * (lv + 1).bit_length() - 3)
            if nbits <= budget: break
        bw = BW()
        hdr = 0xFFE00000 | ver << 19 | 2 << 17 | 1 << 16 | bri << 12 | sri << 10 | pad << 9 | mode << 6 | mode_ext << 4
        bw.put(hdr, 32)
        for sb in range(sbl):
            for ch in range(nch):
                if sb >= bound and ch == 1: continue
                bw.put(alloc[ch][sb], tab[sb][0])
        for sb in range(sbl):
            for ch in range(nch):
                if alloc[ch][sb]: bw.put(scfsi[ch][sb], 2)
        for sb in range(sbl):
            for ch in range(nch):
                if alloc[ch][sb]:
                    for _ in range([3, 2, 1, 2][scfsi[ch][sb]]): bw.put(rng.randrange(4, 63), 6)
        for gr in range(12):
            for sb in range(sbl):
                for ch in range(nch):
                    if sb >= bound and ch == 1: continue
                    a = alloc[ch][sb]
                    if not a: continue
                    lv = tab[sb][1][a - 1]
                    v = [rng.randrange(lv) for _ in range(3)]
                    if lv in (3, 5, 9):
                        bw.put(v[0] + lv * v[1] + lv * lv * v[2], {3: 5, 5: 7, 9: 10}[lv])
                    else:
                        nb = (lv + 1).bit_length() - 1
                        for x in v: bw.put(x, nb)
        b = bw.tobytes()
        assert len(b) <= size, (len(b), size)
        out += b + bytes(size - len(b))
    return bytes(out)


def make_all(rng, quick=False):
    n = 12 if quick else 40
    res = []
    res.append(("synth_l1_48000_stereo.mp1", layer1_stream(rng, 48000, 384, 0, n)))
    res.append(("synth_l1_48000_joint.mp1", layer1_stream(rng, 48000, 384, 1, n)))
    res.append(("synth_l1_44100_mono.mp1", layer1_stream(rng, 44100, 192, 3, n)))
    res.append(("synth_l1_32000_joint.mp1", layer1_stream(rng, 32000, 448, 1, n)))
    res.append(("synth_l2_48000_joint.mp2", layer2_stream(rng, 48000, 256, 1, n)))
    res.append(("synth_l2_44100_joint_lowrate.mp2", layer2_stream(rng, 44100, 64, 1, n)))
    res.append(("synth_l2_32000_joint.mp2", layer2_stream(rng, 32000, 384, 1, n)))
    res.append(("synth_l2_24000_joint_lsf.mp2", layer2_stream(rng, 24000, 160, 1, n)))
    res.append(("synth_l2_16000_mono_lsf.mp2", layer2_stream(rng, 16000, 64, 3, n)))
    res.append(("synth_l3_mpeg1_mixed_is.mp3", layer3_stream(rng, 48000, 320, 1, n, {"is": 1, "scfsi": 1})))
    res.append(("synth_l3_mpeg1_is_only.mp3", layer3_stream(rng, 44100, 320, 1, n, {"is": 1, "mode_ext": 1})))
    res.append(("synth_l3_mpeg1_ms_crc.mp3", layer3_stream(rng, 32000, 320, 1, n, {"mode_ext": 2, "crc": 1, "scfsi": 1})))
    res.append(("synth_l3_mpeg1_stereo_scfsi.mp3", layer3_stream(rng, 44100, 256, 0, n, {"scfsi": 1, "blocks": ["long", "start", "stop"]})))
    res.append(("synth_l3_mpeg1_mono_mixed.mp3", layer3_stream(rng, 44100, 160, 3, n, {"blocks": ["mixed", "short", "long"]})))
    res.append(("synth_l3_mpeg2_is.mp3", layer3_stream(rng, 22050, 160, 1, n, {"is": 1})))
    res.append(("synth_l3_mpeg2_24k_mixed_ms.mp3", layer3_stream(rng, 24000, 160, 1, n, {"mode_ext": 2, "crc": 1})))
    res.append(("synth_l3_mpeg25_is_mixed.mp3", layer3_stream(rng, 11025, 160, 1, n, {"is": 1, "blocks": ["mixed", "long", "short"]})))
    res.append(("synth_l3_mpeg25_12k_mono.mp3", layer3_stream(rng, 12000, 128, 3, n, {})))
    return res
