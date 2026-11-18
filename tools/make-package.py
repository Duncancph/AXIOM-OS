#!/usr/bin/env python3
# Writes a signed AXIOM package description (packages/<id>.txt) for an archive made with tools/pack-app.py,
# signed with the update key (ECDSA P-256 over SHA-256, checked by the kernel: src/update.c), and adds the id
# to packages/index.txt. The archive itself can be anywhere on the web (a GitHub release asset, say): url.
#   python3 tools/make-package.py <key.pem> <out-dir> id=vertex name=Vertex version=1.0 icon=arc \
#       "description=The full web browser (Chromium)" min-build=150 url=https://... \
#       "run=vertex --no-sandbox" archive=vertex.tar      (or size=... sha256=... installed-size=...)
#   requires=<id>: another package Settings installs first (e.g. requires=gtk3 for a GTK app)
#   an archive ending .gz must be gzip members of at most 1 MiB each, expanded (ports/gtk/build-packages.py
#   writes them): compression=gzip is then set
#   root=yes: the archive unpacks into the Linux folder itself (/system/linux, the disk's /linux) instead of
#   opt/<id>, for a package that brings its libraries (lib/...) and data (usr/share/...); run is then relative
#   to that folder (e.g. run=opt/vertex/vertex)
import hashlib, os, sys, tarfile
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
key = serialization.load_pem_private_key(open(sys.argv[1], "rb").read(), None)
out = sys.argv[2]
kv = dict(a.split("=", 1) for a in sys.argv[3:])
if "archive" in kv:
    if kv["archive"].endswith(".gz"): kv.setdefault("compression", "gzip")
    h = hashlib.sha256(); n = 0
    with open(kv["archive"], "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk); n += len(chunk)
    kv["size"], kv["sha256"] = str(n), h.hexdigest()
    with tarfile.open(kv["archive"]) as t: kv.setdefault("installed-size", str(sum(m.size for m in t.getmembers() if m.isfile())))
for need in ("id", "name", "version", "url", "size", "sha256"):   # (no run: a library package, e.g. linux-base)
    if need not in kv: sys.exit("missing " + need)
lines = ["AXIOM package 1"] + ["%s %s" % (k, kv[k]) for k in ("id", "name", "version", "icon", "description", "min-build", "size", "sha256", "url", "installed-size", "root", "requires", "compression", "run") if k in kv]
body = ("\n".join(lines) + "\n").encode()
sig = key.sign(body, ec.ECDSA(hashes.SHA256()))
os.makedirs(os.path.join(out, "packages"), exist_ok=True)
open(os.path.join(out, "packages", kv["id"] + ".txt"), "wb").write(body + b"signature " + sig.hex().encode() + b"\n")
idx = os.path.join(out, "packages", "index.txt")
ids = [l.strip() for l in open(idx)] if os.path.exists(idx) else []
if kv["id"] not in ids: ids.append(kv["id"])
open(idx, "w").write("\n".join(i for i in ids if i) + "\n")
print(body.decode())
