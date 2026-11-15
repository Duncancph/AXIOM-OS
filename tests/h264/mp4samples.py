"""Minimal MP4 reader for the H.264 tests: extracts the avcC box and the raw (length-prefixed) samples of the
first video track and writes the sample file format read by h264_test.c:
  "H264SMP1", u32 avcC length, avcC, then per sample: u32 length, i64 pts, u8 keyframe, data   (little endian)
"""
import struct


def boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size, typ = struct.unpack(">I4s", data[pos:pos + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - pos
        if size < hdr or pos + size > end:
            break
        yield typ.decode("latin-1"), pos + hdr, pos + size
        pos += size


def find(data, start, end, path):
    for typ, s, e in boxes(data, start, end):
        if typ == path[0]:
            if len(path) == 1:
                return s, e
            r = find(data, s, e, path[1:])
            if r:
                return r
    return None


def read_track(data):
    moov = find(data, 0, len(data), ["moov"])
    for typ, s, e in boxes(data, moov[0], moov[1]):
        if typ != "trak":
            continue
        hdlr = find(data, s, e, ["mdia", "hdlr"])
        if not hdlr or data[hdlr[0] + 8:hdlr[0] + 12] != b"vide":
            continue
        stbl = find(data, s, e, ["mdia", "minf", "stbl"])
        stsd = find(data, stbl[0], stbl[1], ["stsd"])
        # stsd: version/flags(4) count(4) then sample entry; avc1 entry has 78 bytes before child boxes
        ent = stsd[0] + 8
        esize, etype = struct.unpack(">I4s", data[ent:ent + 8])
        avcc = None
        for t, bs, be in boxes(data, ent + 8 + 78, ent + esize):
            if t == "avcC":
                avcc = data[bs:be]
        def table(name):
            return find(data, stbl[0], stbl[1], [name])
        stsz = table("stsz")
        ssize, scount = struct.unpack(">II", data[stsz[0] + 4:stsz[0] + 12])
        sizes = [ssize] * scount if ssize else list(struct.unpack(">%dI" % scount, data[stsz[0] + 12:stsz[0] + 12 + 4 * scount]))
        stco = table("stco")
        if stco:
            n = struct.unpack(">I", data[stco[0] + 4:stco[0] + 8])[0]
            chunks = list(struct.unpack(">%dI" % n, data[stco[0] + 8:stco[0] + 8 + 4 * n]))
        else:
            co = table("co64")
            n = struct.unpack(">I", data[co[0] + 4:co[0] + 8])[0]
            chunks = list(struct.unpack(">%dQ" % n, data[co[0] + 8:co[0] + 8 + 8 * n]))
        stsc = table("stsc")
        n = struct.unpack(">I", data[stsc[0] + 4:stsc[0] + 8])[0]
        sc = [struct.unpack(">III", data[stsc[0] + 8 + 12 * i:stsc[0] + 20 + 12 * i]) for i in range(n)]
        offsets = []
        si = 0
        for ci in range(len(chunks)):
            per = 1
            for first, spc, _ in sc:
                if first - 1 <= ci:
                    per = spc
            off = chunks[ci]
            for _ in range(per):
                if si >= len(sizes):
                    break
                offsets.append(off)
                off += sizes[si]
                si += 1
        stts = table("stts")
        n = struct.unpack(">I", data[stts[0] + 4:stts[0] + 8])[0]
        dts = []
        t = 0
        for i in range(n):
            c, dlt = struct.unpack(">II", data[stts[0] + 8 + 8 * i:stts[0] + 16 + 8 * i])
            for _ in range(c):
                dts.append(t)
                t += dlt
        ctts = table("ctts")
        cts = [0] * len(sizes)
        if ctts:
            ver = data[ctts[0]]
            n = struct.unpack(">I", data[ctts[0] + 4:ctts[0] + 8])[0]
            k = 0
            for i in range(n):
                c, o = struct.unpack(">I" + ("i" if ver else "I"), data[ctts[0] + 8 + 8 * i:ctts[0] + 16 + 8 * i])
                for _ in range(c):
                    if k < len(cts):
                        cts[k] = o
                    k += 1
        stss = table("stss")
        keys = None
        if stss:
            n = struct.unpack(">I", data[stss[0] + 4:stss[0] + 8])[0]
            keys = set(x - 1 for x in struct.unpack(">%dI" % n, data[stss[0] + 8:stss[0] + 8 + 4 * n]))
        samples = []
        for i, (o, s_) in enumerate(zip(offsets, sizes)):
            pts = (dts[i] if i < len(dts) else 0) + cts[i]
            samples.append((data[o:o + s_], pts, 1 if keys is None or i in keys else 0))
        return avcc, samples
    raise ValueError("no video track")


def write_samples(mp4_path, out_path):
    data = open(mp4_path, "rb").read()
    avcc, samples = read_track(data)
    with open(out_path, "wb") as f:
        f.write(b"H264SMP1" + struct.pack("<I", len(avcc)) + avcc)
        for d, pts, key in samples:
            f.write(struct.pack("<IqB", len(d), pts, key) + d)
    return samples


if __name__ == "__main__":
    import sys
    s = write_samples(sys.argv[1], sys.argv[2])
    print(len(s), "samples")
