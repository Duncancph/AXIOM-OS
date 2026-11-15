"""Random but valid Parametric Stereo payloads for sbrgen.py (HE-AACv2 test streams)."""
import sbrgen

PS_NAMES = ["huff_iid_df1", "huff_iid_dt1", "huff_iid_df0", "huff_iid_dt0", "huff_icc_df", "huff_icc_dt",
            "huff_ipd_df", "huff_ipd_dt", "huff_opd_df", "huff_opd_dt"]
HUFF = [(sbrgen.A["ps_%s_code" % n], sbrgen.A["ps_%s_bits" % n]) for n in PS_NAMES]
OFFSET = sbrgen.A["aac_ps_huff_offset"]
IID_DF1, IID_DT1, IID_DF0, IID_DT0, ICC_DF, ICC_DT, IPD_DF, IPD_DT, OPD_DF, OPD_DT = range(10)
NR_IIDICC = [10, 20, 34, 10, 20, 34]
NR_IPDOPD = [5, 11, 17, 5, 11, 17]
NUM_ENV = [[0, 1, 2, 4], [1, 2, 3, 4]]


class PsWriter:
    def __init__(self, rng):
        self.r = rng
        self.par = {k: [[0] * 34 for _ in range(5)] for k in ("iid", "icc", "ipd", "opd")}
        self.num_env = 0
        self.h = None

    def values(self, bw, kind, e, num_env_old, table_df, table_dt, lo, hi, mod=None):
        r = self.r
        rows = self.par[kind]
        n = self.n[kind]
        dt = int(r.integers(0, 2))
        e_prev = max(e - 1 if e else num_env_old - 1, 0)
        prev_row = list(rows[e_prev])
        if not mod and any(not (lo <= v <= hi) for v in prev_row[:n]):
            dt = 0   # previous values outside this quantiser's range: the delta could not be coded
        bw.put(dt, 1)
        cur = [0] * 34
        acc = 0
        for b in range(n):
            target = int(r.integers(lo, hi + 1)) if r.random() < 0.5 or b == 0 else None
            base = prev_row[b] if dt else acc
            if target is None:
                target = base if lo <= base <= hi else lo
            if mod:
                d = (target - base) % mod
                val = (base + d) % mod
            else:
                d = target - base
                val = target
            bw.huff(HUFF[table_dt if dt else table_df], d + OFFSET[table_dt if dt else table_df])
            cur[b] = val
            acc = val
        rows[e] = cur + rows[e][34:]

    def write(self, bw, frame):
        r = self.r
        header = frame == 0 or r.random() < 0.15
        bw.put(1 if header else 0, 1)
        if header:
            h = {"iid": int(r.integers(0, 2)) if frame else 1, "iid_mode": int(r.integers(0, 6)),
                 "icc": int(r.integers(0, 2)), "icc_mode": int(r.integers(0, 6)), "ext": int(r.integers(0, 2))}
            bw.put(h["iid"], 1)
            if h["iid"]:
                bw.put(h["iid_mode"], 3)
            bw.put(h["icc"], 1)
            if h["icc"]:
                bw.put(h["icc_mode"], 3)
            bw.put(h["ext"], 1)
            old = self.h or {}
            # parameters not sent keep their previous mode (as in the decoder)
            if not h["iid"]:
                h["iid_mode"] = old.get("iid_mode", 0)
            if not h["icc"]:
                h["icc_mode"] = old.get("icc_mode", 0)
            self.h = h
        h = self.h
        self.n = {"iid": NR_IIDICC[h["iid_mode"]], "icc": NR_IIDICC[h["icc_mode"]],
                  "ipd": NR_IPDOPD[h["iid_mode"]], "opd": NR_IPDOPD[h["iid_mode"]]}
        quant = h["iid_mode"] > 2
        fc = int(r.integers(0, 2))
        bw.put(fc, 1)
        ne_idx = int(r.integers(0, 4))
        bw.put(ne_idx, 2)
        num_env_old = self.num_env
        ne = NUM_ENV[fc][ne_idx]
        borders = [-1]
        if fc:
            pos = sorted(int(x) for x in r.integers(0, 32, ne))
            if r.random() < 0.5 and ne:
                pos[-1] = 31
            for p in pos:
                bw.put(p, 5)
            borders += pos
        else:
            borders += [(e * 32 >> [0, 0, 1, 1, 2][ne]) - 1 for e in range(1, ne + 1)]
        if h["iid"]:
            lim = 15 if quant else 7
            for e in range(ne):
                self.values(bw, "iid", e, num_env_old, IID_DF1 if quant else IID_DF0, IID_DT1 if quant else IID_DT0, -lim, lim)
        else:
            self.par["iid"] = [[0] * 34 for _ in range(5)]
        if h["icc"]:
            for e in range(ne):
                self.values(bw, "icc", e, num_env_old, ICC_DF, ICC_DT, 0, 7)
        else:
            self.par["icc"] = [[0] * 34 for _ in range(5)]
        ipdopd = 0
        if h["ext"]:
            ext = sbrgen.BW()
            ext.put(0, 2)   # extension id 0: IPD/OPD
            ipdopd = int(r.integers(0, 2))
            ext.put(ipdopd, 1)
            if ipdopd:
                for e in range(ne):
                    self.values(ext, "ipd", e, num_env_old, IPD_DF, IPD_DT, 0, 7, mod=8)
                    self.values(ext, "opd", e, num_env_old, OPD_DF, OPD_DT, 0, 7, mod=8)
            ext.put(0, 1)
            nbytes = (len(ext) + 7) // 8
            ext.bits += [0] * (nbytes * 8 - len(ext))
            if nbytes >= 15:
                bw.put(15, 4)
                bw.put(nbytes - 15, 8)
            else:
                bw.put(nbytes, 4)
            bw.bits += ext.bits
        if not ipdopd:
            self.par["ipd"] = [[0] * 34 for _ in range(5)]
            self.par["opd"] = [[0] * 34 for _ in range(5)]
        # decoder fix-up: a missing final envelope repeats the last one
        if ne == 0 or borders[ne] < 31:
            src = ne - 1 if ne else num_env_old - 1
            if src >= 0 and src != ne:
                for k in ("iid", "icc", "ipd", "opd"):
                    self.par[k][ne] = list(self.par[k][src])
            ne += 1
        self.num_env = ne
