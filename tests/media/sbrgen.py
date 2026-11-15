"""Synthetic HE-AAC (SBR) and HE-AACv2 (SBR + PS) stream writer for the decoder tests.

No HE-AAC encoder is available here, so this takes an AAC-LC ADTS stream from ffmpeg's encoder (the core) and adds
random but valid SBR (and optionally PS) extension payloads to every frame: an EXT_SBR_DATA fill element right
after each SCE/CPE. The SBR parameters exercise everything the decoder implements (all frame classes, time and
frequency delta coding, both amplitude resolutions, coupling, noise floors, inverse filtering modes, sinusoids,
header changes, limiter / smoothing / interpolation settings); the PS payloads cover IID/ICC/IPD/OPD with all
band configurations, frame classes and envelope counts. ffmpeg's decoder output is the reference.

Huffman tables are read from user/lib/media/aac_tables.c (so a table error there would also break the
reference decode and show up as a mismatch)."""
import math, os, re, struct

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(os.path.dirname(os.path.dirname(HERE)), "user", "lib", "media", "aac_tables.c")


def _arrays():
    src = open(TABLES).read()
    out = {}
    for m in re.finditer(r"(\w+)\s*((?:\[[^\]]*\])+)\s*=\s*\{(.*?)\};", src, re.S):
        body = m.group(3).replace("{", " ").replace("}", " ")
        vals = []
        for t in body.split(","):
            t = t.strip()
            if not t:
                continue
            if t.lower().startswith("0x"):
                vals.append(int(t, 16))
            elif "." in t:
                vals.append(float(t.rstrip("fF")))
            else:
                try:
                    vals.append(int(t))
                except ValueError:
                    pass
        out[m.group(1)] = vals
    return out


A = _arrays()
SBR_NAMES = ["t_huffman_env_1_5dB", "f_huffman_env_1_5dB", "t_huffman_env_bal_1_5dB", "f_huffman_env_bal_1_5dB",
             "t_huffman_env_3_0dB", "f_huffman_env_3_0dB", "t_huffman_env_bal_3_0dB", "f_huffman_env_bal_3_0dB",
             "t_huffman_noise_3_0dB", "t_huffman_noise_bal_3_0dB"]
SBR_LAV = [60, 60, 24, 24, 31, 31, 12, 12, 31, 12]
HUFF = [(A["sbr_%s_code" % n], A["sbr_%s_bits" % n]) for n in SBR_NAMES]
SBR_OFFSET = [A["aac_sbr_offset"][16 * i:16 * i + 16] for i in range(6)]
(T_ENV_15, F_ENV_15, T_BAL_15, F_BAL_15, T_ENV_30, F_ENV_30, T_BAL_30, F_BAL_30, T_NOISE_30, T_NOISE_BAL_30) = range(10)


class BW:
    def __init__(self):
        self.bits = []

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def huff(self, table, sym):
        codes, lens = table
        self.put(codes[sym], lens[sym])

    def __len__(self):
        return len(self.bits)


# ---------------------------------------------------------------- frequency tables (mirror of the decoder)
f32 = np.float32


def lrintf(x):
    return int(np.rint(np.float32(x)))


def log2f(x):
    return f32(math.log2(float(x)))


def make_bands(start, stop, num):
    base = f32(np.power(f32(stop) / f32(start), f32(1.0) / f32(num)))
    prod = f32(start)
    prev = start
    out = []
    for _ in range(num - 1):
        prod = f32(prod * base)
        pres = lrintf(prod)
        out.append(pres - prev)
        prev = pres
    out.append(stop - prev)
    return out


def derive(rate, h):
    """rate: SBR (output) rate; h: header dict. Returns dict with n, n_q, n_master, ... or None if invalid."""
    offs = {16000: 0, 22050: 1, 24000: 2, 32000: 3, 44100: 4, 48000: 4, 64000: 4, 88200: 5, 96000: 5}
    if rate not in offs:
        return None
    off = SBR_OFFSET[offs[rate]]
    temp = 3000 if rate < 32000 else 4000 if rate < 64000 else 5000
    start_min = ((temp << 7) + (rate >> 1)) // rate
    stop_min = ((temp << 8) + (rate >> 1)) // rate
    k0 = start_min + off[h["start"]]
    if h["stop"] < 14:
        dk = sorted(make_bands(stop_min, 64, 13))
        k2 = stop_min + sum(dk[:h["stop"]])
    elif h["stop"] == 14:
        k2 = 2 * k0
    else:
        k2 = 3 * k0
    k2 = min(64, k2)
    maxq = 48 if rate <= 32000 else 35 if rate == 44100 else 32
    if k2 - k0 > maxq or k0 <= 0:
        return None
    if h["freq_scale"] == 0:
        dk = h["alter_scale"] + 1
        n_master = ((k2 - k0 + (dk & 2)) >> dk) << 1
        if n_master <= 0 or h["xover"] >= n_master or n_master > 48:
            return None
        fm = [0] + [dk] * n_master
        k2diff = k2 - k0 - n_master * dk
        if k2diff < 0:
            fm[1] -= 1
            fm[2] -= (k2diff < -1)
        elif k2diff:
            fm[n_master] += 1
        fm[0] = k0
        for k in range(1, n_master + 1):
            fm[k] += fm[k - 1]
    else:
        half = 7 - h["freq_scale"]
        two = 49 * k2 > 110 * k0
        k1 = 2 * k0 if two else k2
        nb0 = lrintf(f32(half) * log2f(f32(k1) / f32(k0))) * 2
        if nb0 <= 0 or nb0 > 48:
            return None
        vk0 = sorted(make_bands(k0, k1, nb0))
        vdk0_max = vk0[-1]
        if min(vk0) <= 0:
            return None
        fm = [k0]
        for v in vk0:
            fm.append(fm[-1] + v)
        if two:
            invwarp = f32(0.76923076923076923077) if h["alter_scale"] else f32(1.0)
            nb1 = lrintf(f32(f32(half) * invwarp) * log2f(f32(k2) / f32(k1))) * 2
            if nb1 <= 0 or nb0 + nb1 > 48:
                return None
            vk1 = make_bands(k1, k2, nb1)
            if min(vk1) < vdk0_max:
                vk1 = sorted(vk1)
                change = min(vdk0_max - vk1[0], (vk1[-1] - vk1[0]) >> 1)
                vk1[0] += change
                vk1[-1] -= change
            vk1 = sorted(vk1)
            if min(vk1) <= 0:
                return None
            for v in vk1:
                fm.append(fm[-1] + v)
            n_master = nb0 + nb1
        else:
            n_master = nb0
        if h["xover"] >= n_master:
            return None
    nh = n_master - h["xover"]
    nl = (nh + 1) >> 1
    fh = fm[h["xover"]:h["xover"] + nh + 1]
    m = fh[nh] - fh[0]
    kx = fh[0]
    if kx + m > 64 or kx > 32:
        return None
    nq = max(1, lrintf(f32(h["noise_bands"]) * log2f(f32(k2) / f32(kx))))
    if nq > 5:
        return None
    # patches (only validity matters here)
    goal_sb = ((1000 << 11) + (rate >> 1)) // rate
    msb, usb, sb = k0, kx, 0
    if goal_sb < kx + m:
        k = 0
        while k < n_master and fm[k] < goal_sb:
            k += 1
    else:
        k = n_master
    npatch, last_k, last_msb, patches = 0, -1, -1, []
    while True:
        odd = 0
        if k == last_k and msb == last_msb:
            return None
        last_k, last_msb = k, msb
        i = k
        while True:
            if i < 0:
                return None
            sb = fm[i]
            odd = (sb + k0) & 1
            if not (sb > (k0 - 1 + msb - odd)):
                break
            i -= 1
        if npatch > 5:
            return None
        pn = max(sb - usb, 0)
        ps = k0 - odd - pn
        if pn > 0:
            patches.append((ps, pn))
            usb = msb = sb
            npatch += 1
        else:
            msb = kx
        if fm[k] - sb < 3:
            k = n_master
        if sb == kx + m:
            break
    if npatch > 1 and patches[-1][1] < 3:
        patches.pop()
    for ps, pn in patches:
        if ps < 0 or ps + pn > 32:
            return None
    return {"n": [nl, nh], "n_q": nq, "kx": kx, "m": m}


# ---------------------------------------------------------------- SBR payload writer
class SbrChannelState:
    def __init__(self):
        self.env = [0] * 48       # last envelope (env_q[0])
        self.fr_last = 0          # freq_res of the last envelope
        self.noise = [0] * 5
        self.trail = 16           # last time border of the previous frame
        self.num_env = 0


class SbrWriter:
    def __init__(self, rng, rate, stereo, ps=False, force=None):
        self.rng, self.rate, self.stereo, self.ps = rng, rate, stereo, ps
        self.force = force or {}
        self.ch = [SbrChannelState(), SbrChannelState()]
        self.h = None
        self.d = None
        self.frame = 0
        self.ps_writer = None
        if ps:
            from psgen import PsWriter
            self.ps_writer = PsWriter(rng)

    def new_header(self):
        r = self.rng
        for _ in range(500):
            h = {"amp_res": int(r.integers(0, 2)), "start": int(r.integers(0, 16)), "stop": int(r.integers(0, 16)),
                 "xover": int(r.integers(0, 4)), "extra1": int(r.integers(0, 2)), "extra2": int(r.integers(0, 2)),
                 "freq_scale": int(r.integers(0, 4)), "alter_scale": int(r.integers(0, 2)), "noise_bands": int(r.integers(0, 4)),
                 "limiter_bands": int(r.integers(0, 4)), "limiter_gains": int(r.integers(0, 4)),
                 "interpol_freq": int(r.integers(0, 2)), "smoothing_mode": int(r.integers(0, 2))}
            if not h["extra1"]:
                h.update(freq_scale=2, alter_scale=1, noise_bands=2)
            if not h["extra2"]:
                h.update(limiter_bands=2, limiter_gains=2, interpol_freq=1, smoothing_mode=1)
            h.update(self.force.get("header", {}))
            d = derive(self.rate, h)
            if d:
                return h, d
        raise RuntimeError("no valid SBR header for rate %d" % self.rate)

    def write_header(self, bw, h):
        bw.put(h["amp_res"], 1)
        bw.put(h["start"], 4)
        bw.put(h["stop"], 4)
        bw.put(h["xover"], 3)
        bw.put(0, 2)
        bw.put(h["extra1"], 1)
        bw.put(h["extra2"], 1)
        if h["extra1"]:
            bw.put(h["freq_scale"], 2)
            bw.put(h["alter_scale"], 1)
            bw.put(h["noise_bands"], 2)
        if h["extra2"]:
            bw.put(h["limiter_bands"], 2)
            bw.put(h["limiter_gains"], 2)
            bw.put(h["interpol_freq"], 1)
            bw.put(h["smoothing_mode"], 1)

    def grid(self, bw, st):
        """writes a random grid consistent with the previous frame; returns (num_env, freq_res list, amp_res)"""
        r = self.rng
        lead_needed = st.trail - 16
        classes = [0, 1, 2, 3] if lead_needed == 0 else [2, 3]
        cls = int(r.choice(classes))
        if "classes" in self.force:
            cls = int(r.choice([c for c in classes if c in self.force["classes"]] or classes))
        bw.put(cls, 2)
        if cls == 0:   # FIXFIX
            e = int(r.integers(0, 3))
            ne = 1 << e
            bw.put(e, 2)
            fr = int(r.integers(0, 2))
            bw.put(fr, 1)
            frs = [fr] * ne
            trail = 16
        elif cls == 1:   # FIXVAR
            x = int(r.integers(0, 4))
            nrt = int(r.integers(0, 4))
            bw.put(x, 2)
            bw.put(nrt, 2)
            ne = nrt + 1
            border = 16 + x
            for i in range(nrt):
                remaining = nrt - i - 1
                maxrel = min(3, (border - 2 * remaining - 3) // 2)
                rel = int(r.integers(0, max(0, maxrel) + 1))
                bw.put(rel, 2)
                border -= 2 * rel + 2
            ne = nrt + 1
            self.pointer(bw, ne)
            frs = [int(r.integers(0, 2)) for _ in range(ne)]
            for i in range(ne):
                bw.put(frs[ne - 1 - i], 1)
            trail = 16 + x
        elif cls == 2:   # VARFIX
            lead = lead_needed
            nrl = int(r.integers(0, 4))
            bw.put(lead, 2)
            bw.put(nrl, 2)
            ne = nrl + 1
            border = lead
            for i in range(nrl):
                remaining = nrl - i - 1
                maxrel = min(3, (16 - border - 2 * remaining - 3) // 2)
                rel = int(r.integers(0, max(0, maxrel) + 1))
                bw.put(rel, 2)
                border += 2 * rel + 2
            self.pointer(bw, ne)
            frs = [int(r.integers(0, 2)) for _ in range(ne)]
            for f in frs:
                bw.put(f, 1)
            trail = 16
        else:   # VARVAR
            lead = lead_needed
            x = int(r.integers(0, 4))
            nrl = int(r.integers(0, 3))
            nrt = int(r.integers(0, 3))
            ne = nrl + nrt + 1
            bw.put(lead, 2)
            bw.put(x, 2)
            bw.put(nrl, 2)
            bw.put(nrt, 2)
            border = lead
            for _ in range(nrl):
                bw.put(0, 2)
                border += 2
            tb = 16 + x
            for _ in range(nrt):
                bw.put(0, 2)
                tb -= 2
            assert tb > border
            self.pointer(bw, ne)
            frs = [int(r.integers(0, 2)) for _ in range(ne)]
            for f in frs:
                bw.put(f, 1)
            trail = 16 + x
        amp = self.h["amp_res"] if not (cls == 0 and ne == 1) else 0
        return ne, frs, amp, trail

    def pointer(self, bw, ne):
        bits = [0, 1, 2, 2, 3, 3][ne]
        p = int(self.rng.integers(0, min(ne + 1, (1 << bits) - 1) + 1)) if bits else 0
        bw.put(p, bits)

    def envelope(self, bw, st, ne, frs, df, amp, bal, delta):
        """writes envelope data for one channel, updating st"""
        r = self.rng
        n = self.d["n"]
        odd = n[1] & 1
        if bal:
            t_i, f_i, bits = (T_BAL_30, F_BAL_30, 5) if amp else (T_BAL_15, F_BAL_15, 6)
            lo, hi = 0, (24 if amp else 48)
        else:
            t_i, f_i, bits = (T_ENV_30, F_ENV_30, 6) if amp else (T_ENV_15, F_ENV_15, 7)
            lo, hi = (8, 22) if amp else (16, 44)
            if "env" in self.force:
                lo = hi = self.force["env"] // (2 if amp else 1)
        tl, fl = SBR_LAV[t_i], SBR_LAV[f_i]
        prev, prev_fr = st.env, st.fr_last
        for e in range(ne):
            fr = frs[e]
            nb = n[fr]
            cur = [0] * 48
            if df[e]:   # time delta
                for j in range(nb):
                    if fr == prev_fr:
                        k = j
                    elif fr:
                        k = (j + odd) >> 1
                    else:
                        k = 2 * j - odd if j else 0
                    target = int(r.integers(lo, hi + 1)) // delta * delta if r.random() < 0.3 else prev[k]
                    dd = (target - prev[k]) // delta
                    dd = max(-tl, min(tl, dd))
                    v = prev[k] + delta * dd
                    if v < 0 or v > 127:
                        dd = 0
                        v = prev[k]
                    bw.huff(HUFF[t_i], dd + tl)
                    cur[j] = v
            else:
                start = int(r.integers(lo, hi + 1)) // delta
                start = min(start, (1 << bits) - 1)
                bw.put(start, bits)
                cur[0] = start * delta
                for j in range(1, nb):
                    target = cur[j - 1] + delta * int(r.integers(-3, 4))
                    target = max(lo, min(hi, target))
                    dd = (target - cur[j - 1]) // delta
                    dd = max(-fl, min(fl, dd))
                    v = cur[j - 1] + delta * dd
                    if v < 0 or v > 127:
                        dd = 0
                        v = cur[j - 1]
                    bw.huff(HUFF[f_i], dd + fl)
                    cur[j] = v
            prev, prev_fr = cur, fr
        st.env, st.fr_last = prev, prev_fr

    def noise(self, bw, st, nn, dfn, bal, delta):
        r = self.rng
        nq = self.d["n_q"]
        if bal:
            t_i, f_i, lo, hi = T_NOISE_BAL_30, F_BAL_30, 0, 24
        else:
            t_i, f_i, lo, hi = T_NOISE_30, F_ENV_30, 0, 30
            if "noise" in self.force:
                lo = hi = self.force["noise"]
        tl, fl = SBR_LAV[t_i], SBR_LAV[f_i]
        prev = st.noise
        for i in range(nn):
            cur = [0] * 5
            if dfn[i]:
                for j in range(nq):
                    target = int(r.integers(lo, hi + 1)) // delta * delta
                    dd = max(-tl, min(tl, (target - prev[j]) // delta))
                    v = prev[j] + delta * dd
                    if v < 0 or v > 30:
                        dd, v = 0, prev[j]
                    bw.huff(HUFF[t_i], dd + tl)
                    cur[j] = v
            else:
                start = int(r.integers(lo, hi + 1)) // delta
                bw.put(start, 5)
                cur[0] = start * delta
                for j in range(1, nq):
                    target = max(lo, min(hi, cur[j - 1] + delta * int(r.integers(-4, 5))))
                    dd = max(-fl, min(fl, (target - cur[j - 1]) // delta))
                    v = cur[j - 1] + delta * dd
                    if v < 0 or v > 30:
                        dd, v = 0, cur[j - 1]
                    bw.huff(HUFF[f_i], dd + fl)
                    cur[j] = v
            prev = cur
        st.noise = prev

    def payload(self, is_cpe):
        """one SBR extension payload (after the 4-bit extension type)"""
        r = self.rng
        bw = BW()
        send_header = self.frame == 0 or r.random() < self.force.get("header_rate", 0.06)
        if send_header:
            if self.h is None or r.random() < 0.5:
                self.h, self.d = self.new_header()
        bw.put(1 if send_header else 0, 1)
        if send_header:
            self.write_header(bw, self.h)
        nq = self.d["n_q"]
        if not is_cpe:
            bw.put(0, 1)   # bs_data_extra
            st = self.ch[0]
            ne, frs, amp, trail = self.grid(bw, st)
            nn = 2 if ne > 1 else 1
            df = [int(r.integers(0, 2)) for _ in range(ne)]
            dfn = [int(r.integers(0, 2)) for _ in range(nn)]
            for v in df + dfn:
                bw.put(v, 1)
            for _ in range(nq):
                bw.put(int(r.integers(0, 4)) if "invf" not in self.force else self.force["invf"], 2)
            self.envelope(bw, st, ne, frs, df, amp, False, 1)
            self.noise(bw, st, nn, dfn, False, 1)
            st.trail = trail
            self.harmonics(bw)
        else:
            bw.put(0, 1)   # bs_data_extra
            coupling = int(r.integers(0, 2)) if "coupling" not in self.force else self.force["coupling"]
            bw.put(coupling, 1)
            if coupling:
                st0, st1 = self.ch
                ne, frs, amp, trail = self.grid(bw, st0)
                nn = 2 if ne > 1 else 1
                df0 = [int(r.integers(0, 2)) for _ in range(ne)]
                dfn0 = [int(r.integers(0, 2)) for _ in range(nn)]
                df1 = [int(r.integers(0, 2)) for _ in range(ne)]
                dfn1 = [int(r.integers(0, 2)) for _ in range(nn)]
                for v in df0 + dfn0 + df1 + dfn1:
                    bw.put(v, 1)
                for _ in range(nq):
                    bw.put(int(r.integers(0, 4)) if "invf" not in self.force else self.force["invf"], 2)
                self.envelope(bw, st0, ne, frs, df0, amp, False, 1)
                self.noise(bw, st0, nn, dfn0, False, 1)
                self.envelope(bw, st1, ne, frs, df1, amp, True, 2)
                self.noise(bw, st1, nn, dfn1, True, 2)
                st0.trail = st1.trail = trail
            else:
                st0, st1 = self.ch
                g0 = self.grid(bw, st0)
                g1 = self.grid(bw, st1)
                dfs = []
                for g in (g0, g1):
                    ne = g[0]
                    nn = 2 if ne > 1 else 1
                    df = [int(r.integers(0, 2)) for _ in range(ne)]
                    dfn = [int(r.integers(0, 2)) for _ in range(nn)]
                    for v in df + dfn:
                        bw.put(v, 1)
                    dfs.append((df, dfn, nn))
                for _ in range(2 * nq):
                    bw.put(int(r.integers(0, 4)) if "invf" not in self.force else self.force["invf"], 2)
                self.envelope(bw, st0, g0[0], g0[1], dfs[0][0], g0[2], False, 1)
                self.envelope(bw, st1, g1[0], g1[1], dfs[1][0], g1[2], False, 1)
                self.noise(bw, st0, dfs[0][2], dfs[0][1], False, 1)
                self.noise(bw, st1, dfs[1][2], dfs[1][1], False, 1)
                st0.trail, st1.trail = g0[3], g1[3]
            self.harmonics(bw)
            self.harmonics(bw)
        # extended data: PS
        if self.ps_writer and not is_cpe:
            ext = BW()
            ext.put(2, 2)   # EXTENSION_ID_PS
            self.ps_writer.write(ext, self.frame)
            nbytes = (len(ext) + 7) // 8
            while len(ext) < nbytes * 8:
                ext.put(0, 1)
            bw.put(1, 1)
            if nbytes >= 15:
                bw.put(15, 4)
                bw.put(nbytes - 15, 8)
            else:
                bw.put(nbytes, 4)
            bw.bits += ext.bits
        else:
            bw.put(0, 1)   # bs_extended_data
        self.frame += 1
        return bw.bits

    def harmonics(self, bw):
        r = self.rng
        f = 1 if r.random() < self.force.get("harmonics", 0.4) else 0
        bw.put(f, 1)
        if f:
            for _ in range(self.d["n"][1]):
                bw.put(1 if r.random() < 0.15 else 0, 1)


def fil_element(payload_bits):
    """ID_FIL with EXT_SBR_DATA"""
    bw = BW()
    nbits = 4 + len(payload_bits)
    cnt = (nbits + 7) // 8
    bw.put(6, 3)
    if cnt >= 15:
        bw.put(15, 4)
        bw.put(cnt - 14, 8)
    else:
        bw.put(cnt, 4)
    bw.put(13, 4)
    bw.bits += payload_bits
    bw.bits += [0] * (cnt * 8 - nbits)
    return bw.bits


def bits_of(data):
    return [(byte >> (7 - i)) & 1 for byte in data for i in range(8)]


def to_bytes(bits):
    bits = bits + [0] * (-len(bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def add_sbr(adts_in, adts_out, elements_listing, core_rate, seed, ps=False, force=None):
    """elements_listing: lines from `aac_test elements` (offset len type:end ...)."""
    rng = np.random.default_rng(seed)
    data = open(adts_in, "rb").read()
    writers = {}
    out = bytearray()
    for line in elements_listing:
        f = line.split()
        off, ln = int(f[0]), int(f[1])
        els = [tuple(map(int, x.split(":"))) for x in f[2:]]
        frame = data[off:off + ln]
        bits = bits_of(frame)
        hdr_bits = 56 if frame[1] & 1 else 72
        new = bits[:hdr_bits]
        pos = hdr_bits
        count = {}
        for typ, end in els:
            if typ == 7:   # END
                new += bits[pos:end]
                pos = end
                break
            new += bits[pos:end]
            pos = end
            if typ in (0, 1):
                k = (typ, count.get(typ, 0))
                count[typ] = k[1] + 1
                if k not in writers:
                    writers[k] = SbrWriter(np.random.default_rng(seed * 100 + len(writers)), 2 * core_rate, typ == 1,
                                           ps=ps and typ == 0, force=force)
                new += fil_element(writers[k].payload(typ == 1))
        new += [1, 1, 1]
        b = bytearray(to_bytes(new))
        flen = len(b)
        if flen >= 8192:
            raise RuntimeError("frame too long")
        b[3] = (b[3] & 0xfc) | (flen >> 11)
        b[4] = (flen >> 3) & 0xff
        b[5] = ((flen & 7) << 5) | (b[5] & 0x1f)
        out += b
    open(adts_out, "wb").write(bytes(out))
