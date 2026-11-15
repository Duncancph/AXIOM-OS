"""Small ISO BMFF box rewriter used by the demuxer tests to produce variants ffmpeg's muxer never writes:
co64 instead of stco, stz2 instead of stsz, 64-bit box sizes, uuid / unknown boxes, ctts v0 vs v1."""
import struct

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts", b"dinf", b"mvex", b"moof", b"traf", b"udta"}


class Box:
    def __init__(self, typ, payload=b"", children=None, large=False, uuid=None):
        self.typ, self.payload, self.children, self.large, self.uuid = typ, payload, children, large, uuid

    def serialize(self):
        body = b"".join(c.serialize() for c in self.children) if self.children is not None else self.payload
        ext = self.uuid if self.typ == b"uuid" else b""
        if self.large:
            return struct.pack(">I4sQ", 1, self.typ, 16 + len(ext) + len(body)) + ext + body
        return struct.pack(">I4s", 8 + len(ext) + len(body), self.typ) + ext + body


def parse(data, start=0, end=None):
    end = len(data) if end is None else end
    out, p = [], start
    while p + 8 <= end:
        size, typ = struct.unpack(">I4s", data[p:p + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[p + 8:p + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - p
        uuid = None
        if typ == b"uuid":
            uuid = data[p + hdr:p + hdr + 16]
            hdr += 16
        if typ in CONTAINERS:
            b = Box(typ, children=parse(data, p + hdr, p + size), large=hdr >= 16 and typ != b"uuid")
        else:
            b = Box(typ, data[p + hdr:p + size], large=(hdr == 16 and typ != b"uuid"), uuid=uuid)
        b.offset = p
        out.append(b)
        p += size
    return out


def walk(boxes):
    for b in boxes:
        yield b
        if b.children is not None:
            yield from walk(b.children)


def serialize(boxes):
    return b"".join(b.serialize() for b in boxes)


def total_size(boxes):
    return len(serialize(boxes))


def stco_to_co64(boxes, delta=0):
    for b in walk(boxes):
        if b.children is not None:
            for i, c in enumerate(b.children):
                if c.typ == b"stco":
                    n = struct.unpack(">I", c.payload[4:8])[0]
                    offs = struct.unpack(">%dI" % n, c.payload[8:8 + 4 * n])
                    b.children[i] = Box(b"co64", c.payload[:8] + struct.pack(">%dQ" % n, *[o + delta for o in offs]))


def shift_offsets(boxes, delta):
    for b in walk(boxes):
        if b.typ == b"stco":
            n = struct.unpack(">I", b.payload[4:8])[0]
            offs = struct.unpack(">%dI" % n, b.payload[8:8 + 4 * n])
            b.payload = b.payload[:8] + struct.pack(">%dI" % n, *[o + delta for o in offs])
        elif b.typ == b"co64":
            n = struct.unpack(">I", b.payload[4:8])[0]
            offs = struct.unpack(">%dQ" % n, b.payload[8:8 + 8 * n])
            b.payload = b.payload[:8] + struct.pack(">%dQ" % n, *[o + delta for o in offs])


def stsz_to_stz2(boxes):
    for b in walk(boxes):
        if b.children is not None:
            for i, c in enumerate(b.children):
                if c.typ == b"stsz":
                    const, n = struct.unpack(">II", c.payload[4:12])
                    sizes = [const] * n if const else list(struct.unpack(">%dI" % n, c.payload[12:12 + 4 * n]))
                    if max(sizes) < 256:
                        field, body = 8, bytes(sizes)
                    elif max(sizes) < 65536:
                        field, body = 16, struct.pack(">%dH" % n, *sizes)
                    else:
                        continue
                    b.children[i] = Box(b"stz2", b"\0\0\0\0" + b"\0\0\0" + bytes([field]) + struct.pack(">I", n) + body)


def make_variant(data, co64=False, stz2=False, large_mdat=False, junk=False):
    """Rewrites a moov-at-end file (ftyp, [free], mdat, moov). Offsets are fixed up for added bytes before mdat data."""
    boxes = parse(data)
    moov = [b for b in boxes if b.typ == b"moov"][0]
    mdat_i = [i for i, b in enumerate(boxes) if b.typ == b"mdat"][0]
    if [i for i, b in enumerate(boxes) if b.typ == b"moov"][0] < mdat_i:
        raise ValueError("needs moov at end")
    old_data_start = boxes[mdat_i].offset + (16 if boxes[mdat_i].large else 8)
    if junk:
        boxes.insert(1, Box(b"uuid", b"hello world" * 3, uuid=b"0123456789abcdef"))
        boxes.insert(2, Box(b"abcd", b"\x01\x02\x03"))
        moov.children.insert(1, Box(b"zzzz", b"unknown box inside moov"))
        moov.children.append(Box(b"uuid", b"x" * 40, uuid=b"fedcba9876543210"))
    mdat_i = [i for i, b in enumerate(boxes) if b.typ == b"mdat"][0]
    if large_mdat:
        boxes[mdat_i].large = True
    if co64:
        stco_to_co64([moov])
    if stz2:
        stsz_to_stz2([moov])
    # recompute where mdat data now starts
    pos = 0
    for b in boxes[:mdat_i]:
        pos += len(b.serialize())
    new_data_start = pos + (16 if boxes[mdat_i].large else 8)
    shift_offsets([moov], new_data_start - old_data_start)
    return serialize(boxes)
