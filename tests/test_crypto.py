# Compares DuncOS crypto against Python's cryptography (OpenSSL) on random inputs.
import os, sys, subprocess, hashlib, hmac, random
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.asymmetric import x25519, ec, rsa, padding, utils
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def q(*args):
    p.stdin.write(" ".join(a if a else "-" for a in args) + "\n"); p.stdin.flush()
    return p.stdout.readline().strip()
H = lambda b: b.hex()
fails = 0; count = {}
def check(name, got, want):
    global fails
    count[name] = count.get(name, 0) + 1
    if got != want:
        fails += 1
        if fails < 10: print("FAIL", name, "\n got ", got[:120], "\n want", want[:120])
R = random.Random(1)
def rb(n): return bytes(R.getrandbits(8) for _ in range(n))

for n in [0, 1, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000] + [R.randrange(3000) for _ in range(20)]:
    m = rb(n)
    check("sha256", q("sha256", H(m)), hashlib.sha256(m).hexdigest())
    check("sha384", q("sha384", H(m)), hashlib.sha384(m).hexdigest())
    check("sha512", q("sha512", H(m)), hashlib.sha512(m).hexdigest())
    k = rb(R.choice([0, 16, 32, 64, 100, 200]))
    check("hmac256", q("hmac256", H(k), H(m)), hmac.new(k, m, hashlib.sha256).hexdigest())
    check("hmac384", q("hmac384", H(k), H(m)), hmac.new(k, m, hashlib.sha384).hexdigest())
for _ in range(15):
    salt, ikm, info, L = rb(R.choice([0, 13, 32])), rb(R.randrange(1, 80)), rb(R.randrange(0, 60)), R.choice([16, 32, 42, 64, 100])
    want = HKDF(hashes.SHA256(), L, salt or None, info).derive(ikm).hex()
    check("hkdf", q("hkdf256", H(salt), H(ikm), H(info), str(L)), want)
for _ in range(40):
    key, iv, aad, pt = rb(16), rb(12), rb(R.choice([0, 5, 13, 16, 40])), rb(R.choice([0, 1, 15, 16, 17, 100, 1000, R.randrange(2000)]))
    check("aes-gcm", q("gcm", H(key), H(iv), H(aad), H(pt)), AESGCM(key).encrypt(iv, pt, aad).hex())
    ct = bytearray(AESGCM(key).encrypt(iv, pt, aad))
    check("aes-gcm open", q("gcmopen", H(key), H(iv), H(aad), H(bytes(ct))), "ok")
    ct[R.randrange(len(ct))] ^= 1 << R.randrange(8)
    check("aes-gcm reject tampered", q("gcmopen", H(key), H(iv), H(aad), H(bytes(ct))), "bad")
    key2 = rb(32)
    check("chacha20-poly1305", q("chacha", H(key2), H(iv), H(aad), H(pt)), ChaCha20Poly1305(key2).encrypt(iv, pt, aad).hex())
for _ in range(20):
    a, b = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
    araw = a.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    bpub = b.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    check("x25519 public", q("x25519base", H(araw)), a.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex())
    check("x25519 shared", q("x25519", H(araw), H(bpub)), a.exchange(b.public_key()).hex())
for _ in range(10):
    a, b = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
    araw = a.private_numbers().private_value.to_bytes(32, "big")
    bpub = b.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    check("p256 public", q("p256pub", H(araw)), a.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint).hex())
    check("p256 ecdh", q("p256ecdh", H(araw), H(bpub)), a.exchange(ec.ECDH(), b.public_key()).hex())
HA = {0: hashes.SHA256(), 1: hashes.SHA384(), 2: hashes.SHA512()}
HL = {0: hashlib.sha256, 1: hashlib.sha384, 2: hashlib.sha512}
for bits in [2048, 3072, 4096]:
    key = rsa.generate_private_key(65537, bits)
    n = key.public_key().public_numbers().n.to_bytes(bits // 8, "big"); e = (65537).to_bytes(3, "big")
    for halg in [0, 1, 2]:
        m = rb(100); h = HL[halg](m).digest()
        s1 = key.sign(m, padding.PKCS1v15(), HA[halg])
        check("rsa-pkcs1-%d" % bits, q("rsapkcs1", H(n), H(e), str(halg), H(h), H(s1)), "ok")
        s2 = key.sign(m, padding.PSS(padding.MGF1(HA[halg]), padding.PSS.DIGEST_LENGTH), HA[halg])
        check("rsa-pss-%d" % bits, q("rsapss", H(n), H(e), str(halg), H(h), H(s2)), "ok")
        bad = bytearray(s1); bad[5] ^= 1
        check("rsa reject bad sig", q("rsapkcs1", H(n), H(e), str(halg), H(h), H(bytes(bad))), "bad")
        check("rsa reject wrong hash", q("rsapss", H(n), H(e), str(halg), H(HL[halg](m + b"x").digest()), H(s2)), "bad")
for cid, curve, hh in [(0, ec.SECP256R1(), hashes.SHA256()), (1, ec.SECP384R1(), hashes.SHA384()), (1, ec.SECP384R1(), hashes.SHA256()), (0, ec.SECP256R1(), hashes.SHA384())]:
    for _ in range(6):
        key = ec.generate_private_key(curve)
        pub = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        m = rb(50); dg = hashes.Hash(hh); dg.update(m); h = dg.finalize()
        sig = key.sign(m, ec.ECDSA(hh))
        check("ecdsa-%s" % curve.name, q("ecdsa", str(cid), H(pub), H(h), H(sig)), "ok")
        check("ecdsa reject", q("ecdsa", str(cid), H(pub), H(bytes([h[0] ^ 1]) + h[1:]), H(sig)), "bad")
p.stdin.close()
print(", ".join("%s x%d" % kv for kv in count.items()))
print("ALL CRYPTO TESTS PASSED" if not fails else "%d FAILURES" % fails)
sys.exit(1 if fails else 0)
