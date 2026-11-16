#!/usr/bin/env python3
# Packs a folder into the archive of an AXIOM package (a plain tar, GNU format for long names), with the files
# at its top level (they unpack into /system/linux/opt/<id>/ on AXIOM). Prints the size and SHA-256 to give
# to tools/make-package.py. Symbolic links are stored as copies of what they point to (AXIOM's disks are FAT).
#   python3 tools/pack-app.py <folder> <out.tar>
import hashlib, os, sys, tarfile
src, out = sys.argv[1], sys.argv[2]
with tarfile.open(out, "w", format=tarfile.GNU_FORMAT, dereference=True) as t:
    for root, dirs, files in os.walk(src, followlinks=True):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            if os.path.isfile(full): t.add(full, arcname=os.path.relpath(full, src), recursive=False)
h = hashlib.sha256(); n = 0
with open(out, "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk); n += len(chunk)
inst = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(src, followlinks=True) for f in fs if os.path.isfile(os.path.join(r, f)))
print("size %d\nsha256 %s\ninstalled-size %d" % (n, h.hexdigest(), inst))
