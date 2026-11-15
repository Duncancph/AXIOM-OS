"""Adds random Long Term Prediction data to an AAC-LTP ADTS stream (ffmpeg's encoder writes the LTP profile but
never actually uses LTP): every long-window ics_info gets predictor_data_present = 1 and random ltp_data.
The decoder's `aac_test elements` listing gives the bit position after each predictor_data_present bit (negative
for a CPE's common ics_info, where the second channel's ltp_data_present follows the first channel's data)."""
import numpy as np

from sbrgen import BW, bits_of, to_bytes


def add_ltp(adts_in, adts_out, elements_listing, seed, lags=(0, 2048)):
    rng = np.random.default_rng(seed)
    data = open(adts_in, "rb").read()
    out = bytearray()

    def ltp_data(bw, max_sfb):
        bw.put(int(rng.integers(lags[0], lags[1])), 11)       # lag
        bw.put(int(rng.integers(0, 5)), 3)           # coefficient index (keep the loop stable: <= 0.98)
        for _ in range(min(max_sfb, 40)):
            bw.put(1 if rng.random() < 0.4 else 0, 1)

    for line in elements_listing:
        f = line.split()
        off, ln = int(f[0]), int(f[1])
        pdp = [(int(x.split(":")[1]), int(x.split(":")[2])) for x in f[2:] if x.startswith("P")]
        els = [tuple(map(int, x.split(":"))) for x in f[2:] if not x.startswith("P")]
        bits = bits_of(data[off:off + ln])
        hdr = 56 if data[off + 1] & 1 else 72
        new, pos = [], 0
        ch0_ltp = {}
        for p, max_sfb in pdp:
            common = p < 0
            p = abs(p)
            if p - 1 < pos or p > len(bits):
                continue
            # which element is this in? (ffmpeg only applies a CPE's second-channel LTP when the first channel
            # of the same frame has predictor data too; keep the streams inside what both decoders agree on)
            start, typ = hdr, None
            for t, end in els:
                if p <= end:
                    typ = t
                    break
                start = end
            allow = True
            if typ == 1 and not common:
                if p == start + 27:
                    ch0_ltp[start] = None
                else:
                    allow = ch0_ltp.get(start) is True
            new += bits[pos:p - 1]
            ins = BW()
            second_of_cpe = typ == 1 and not common and p != start + 27
            # ffmpeg also keeps a stale ltp_data_present for a second channel whose ics_info has no predictor
            # data: when the first channel has it, give the second channel fresh predictor data as well
            use = allow and (second_of_cpe or rng.random() < 0.8)
            if typ == 1 and not common and p == start + 27:
                ch0_ltp[start] = use
            if use:
                ins.put(1, 1)                  # predictor_data_present
                present = not second_of_cpe or rng.random() < 0.7
                ins.put(1 if present else 0, 1)   # ltp_data_present (first / only channel)
                if present:
                    ltp_data(ins, max_sfb)
                if common:
                    second = rng.random() < 0.7
                    ins.put(1 if second else 0, 1)
                    if second:
                        ltp_data(ins, max_sfb)
            else:
                ins.put(0, 1)
            new += ins.bits
            pos = p
        new += bits[pos:]
        b = bytearray(to_bytes(new))
        flen = len(b)
        b[3] = (b[3] & 0xfc) | (flen >> 11)
        b[4] = (flen >> 3) & 0xff
        b[5] = ((flen & 7) << 5) | (b[5] & 0x1f)
        out += b
    open(adts_out, "wb").write(bytes(out))
