"""Synthetic H.264 streams for features libx264 never produces: I_PCM macroblocks (CAVLC and CABAC, with
CABAC re-initialisation after the PCM samples), mixed with Intra_16x16 macroblocks that predict from them.

make_pcm_stream(w_mbs, h_mbs, frames, cabac, seed) -> Annex B bytes. Every picture is an IDR picture whose
macroblocks are randomly I_PCM (random samples) or I_16x16 DC prediction without residual."""
import os, random, re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class BitWriter:
    def __init__(self): self.b = []
    def u(self, n, v): self.b += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0); self.u(n, v)
    def se(self, v): self.ue(2 * v - 1 if v > 0 else -2 * v)
    def align0(self):
        while len(self.b) % 8: self.b.append(0)
    def trailing(self):
        self.b.append(1); self.align0()
    def bytes(self):
        assert len(self.b) % 8 == 0
        return bytes(int("".join(map(str, self.b[i:i + 8])), 2) for i in range(0, len(self.b), 8))


def escape(b):
    out, z = bytearray(), 0
    for c in b:
        if z >= 2 and c <= 3: out.append(3); z = 0
        out.append(c)
        z = z + 1 if c == 0 else 0
    return bytes(out)


def nal(ref_idc, typ, payload):
    return b"\x00\x00\x00\x01" + bytes([(ref_idc << 5) | typ]) + escape(payload)


# ---- CABAC tables from the decoder sources (spec tables)
def load_tables():
    src = open(os.path.join(ROOT, "user", "lib", "video", "h264_tables.c")).read()
    def arr(name):
        m = re.search(r"%s[^=]*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
        return [int(x) for x in re.findall(r"-?\d+", m.group(1))]
    lps = arr("h264_cabac_lps_range")
    init = arr("h264_cabac_init")
    nxt = arr("h264_cabac_next")
    return [lps[i * 4:i * 4 + 4] for i in range(64)], [(init[2 * i], init[2 * i + 1]) for i in range(460)], nxt


LPS, INIT, NEXT = load_tables()


class CabacEnc:
    def __init__(self, w, qp):
        self.w = w
        self.st = []
        for m, n in INIT[:460]:
            pre = min(max(((m * qp) >> 4) + n, 1), 126)
            self.st.append(((63 - pre) << 1) if pre <= 63 else (((pre - 64) << 1) | 1))
        self.start()
    def start(self):
        self.low, self.range, self.first, self.out = 0, 510, True, 0
    def put(self, b):
        if self.first: self.first = False
        else: self.w.b.append(b)
        while self.out > 0:
            self.w.b.append(1 - b); self.out -= 1
    def renorm(self):
        while self.range < 256:
            if self.low < 256: self.put(0)
            elif self.low >= 512: self.low -= 512; self.put(1)
            else: self.low -= 256; self.out += 1
            self.range <<= 1; self.low <<= 1
    def bin(self, ctx, b):
        s = self.st[ctx]
        p, mps = s >> 1, s & 1
        rl = LPS[p][(self.range >> 6) & 3]
        self.range -= rl
        if b != mps:
            self.low += self.range; self.range = rl
            self.st[ctx] = NEXT[128 + s]
        else:
            self.st[ctx] = NEXT[s]
        self.renorm()
    def terminate(self, b):
        self.range -= 2
        if b:
            self.low += self.range
            self.range = 2
            self.renorm()
            self.put((self.low >> 9) & 1)
            self.w.u(2, ((self.low >> 7) & 3) | 1)
        else:
            self.renorm()


def make_pcm_stream(wm, hm, frames, cabac, seed):
    R = random.Random(seed)
    out = bytearray()
    sps = BitWriter()
    sps.u(8, 77 if cabac else 66); sps.u(8, 0); sps.u(8, 30); sps.ue(0)
    sps.ue(0); sps.ue(2); sps.ue(1); sps.u(1, 0); sps.ue(wm - 1); sps.ue(hm - 1); sps.u(1, 1); sps.u(1, 1); sps.u(1, 0); sps.u(1, 0)
    sps.trailing()
    out += nal(3, 7, sps.bytes())
    pps = BitWriter()
    pps.ue(0); pps.ue(0); pps.u(1, cabac); pps.u(1, 0); pps.ue(0); pps.ue(0); pps.ue(0); pps.u(1, 0); pps.u(2, 0)
    pps.se(0); pps.se(0); pps.se(0); pps.u(1, 1); pps.u(1, 0); pps.u(1, 0)
    pps.trailing()
    out += nal(3, 8, pps.bytes())
    qp = 30
    for f in range(frames):
        w = BitWriter()
        w.ue(0); w.ue(7); w.ue(0); w.u(4, 0); w.ue(f & 1)
        w.u(1, 0); w.u(1, 0)            # no_output_of_prior_pics, long_term_reference
        w.se(qp - 26)
        w.ue(0); w.se(0); w.se(0)       # deblocking on
        pcm = [[R.random() < 0.5 for x in range(wm)] for y in range(hm)]
        if cabac:
            while len(w.b) % 8: w.b.append(1)
            enc = CabacEnc(w, qp)
            for y in range(hm):
                for x in range(wm):
                    a = x > 0; b = y > 0
                    ctx = 3 + a + b          # neighbours are I_16x16 or I_PCM: condTerm 1
                    enc.bin(ctx, 1)
                    if pcm[y][x]:
                        enc.terminate(1)
                        w.align0()
                        for _ in range(384): w.u(8, R.randrange(256))
                        enc.start()
                    else:
                        enc.terminate(0)
                        enc.bin(3 + 3, 0)    # cbp luma 0
                        enc.bin(3 + 4, 0)    # cbp chroma 0
                        enc.bin(3 + 6, 1); enc.bin(3 + 7, 0)   # pred mode 2 (DC)
                        enc.bin(64 + 0, 0)   # intra_chroma_pred_mode DC (neighbours have 0 / are PCM)
                        enc.bin(60 + 0, 0)   # mb_qp_delta 0 (previous delta always 0)
                        # Intra16x16 DC coded_block_flag: neighbour PCM -> 1, I16 without DC -> 0, unavailable -> 1
                        ca = 1 if x == 0 or pcm[y][x - 1] else 0
                        cb = 1 if y == 0 or pcm[y - 1][x] else 0
                        enc.bin(85 + 0 + ca + 2 * cb, 0)
                    last = (y == hm - 1 and x == wm - 1)
                    enc.terminate(1 if last else 0)
            w.align0()
        else:
            for y in range(hm):
                for x in range(wm):
                    if pcm[y][x]:
                        w.ue(25)
                        w.align0()
                        for _ in range(384): w.u(8, R.randrange(256))
                    else:
                        w.ue(3)              # I_16x16_2_0_0 (DC, cbp 0)
                        w.ue(0)              # chroma DC
                        w.se(0)              # mb_qp_delta
                        na = (16 if pcm[y][x - 1] else 0) if x > 0 else None
                        nb = (16 if pcm[y - 1][x] else 0) if y > 0 else None
                        if na is not None and nb is not None: nc = (na + nb + 1) >> 1
                        elif na is not None: nc = na
                        elif nb is not None: nc = nb
                        else: nc = 0
                        # coeff_token TotalCoeff 0: '1', '11', '1111', '000011'
                        if nc < 2: w.u(1, 1)
                        elif nc < 4: w.u(2, 3)
                        elif nc < 8: w.u(4, 15)
                        else: w.u(6, 3)
            w.trailing()
        out += nal(3, 5, w.bytes())
    return bytes(out)


if __name__ == "__main__":
    import sys
    open(sys.argv[1], "wb").write(make_pcm_stream(int(sys.argv[2]), int(sys.argv[3]), 3, int(sys.argv[4]), 1))
