# Makes a signed DuncOS update from a build: release/latest.txt (the manifest) and release/b<build>/ (compressed files).
# Usage: python3 tools/make-release.py <private key .pem> "<one-line notes>"
# Publish the release/ folder where UPDATE_URL (src/kernel.h) points; Settings > Updates fetches latest.txt from there.
import gzip, hashlib, os, re, shutil, subprocess, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
key = serialization.load_pem_private_key(open(sys.argv[1], "rb").read(), None)
notes = (sys.argv[2] if len(sys.argv) > 2 else "").replace("\n", " ")[:200]
build = int(open("build/build_num").read())
version = re.search(r'#define OS_VERSION "([^"]+)"', open("src/kernel.h").read()).group(1)
pub = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
built_in = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", open("src/update_key.h").read().split("{", 1)[1]))
if pub != built_in: sys.exit("this key doesn't match the public key in src/update_key.h: DuncOS would reject the update")
files = [("kernel", "iso/boot/thane.elf"), ("initrd", "iso/boot/initrd.tar"), ("bootx64", "iso/boot/bootx64.efi")]
os.makedirs("release/b%d" % build, exist_ok=True)
lines = ["DuncOS update 1", "version " + version, "build %d" % build, "notes " + notes]
for role, path in files:
    data = open(path, "rb").read()
    name = "b%d/%s.gz" % (build, os.path.basename(path))
    with open("release/" + name, "wb") as f: f.write(gzip.compress(data, 9, mtime=0))
    lines.append("file %s %s %d %s" % (role, name, len(data), hashlib.sha256(data).hexdigest()))
body = ("\n".join(lines) + "\n").encode()
sig = key.sign(body, ec.ECDSA(hashes.SHA256()))
open("release/latest.txt", "wb").write(body + b"signature " + sig.hex().encode() + b"\n")
print("release/latest.txt: DuncOS %s build %d" % (version, build))
for role, path in files: print("  %-8s %s" % (role, path))
