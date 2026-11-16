# Checks AXIOM's WPA2 code (tests/test_wpa.c driving user/lib/crypto/wpa.c) against Python's hashlib and
# cryptography (OpenSSL), the IEEE 802.11 test vectors, and a simulated router doing the 4-way handshake and a
# group key update, including a wrong password and a replayed message.
import sys, subprocess, hashlib, hmac, random, struct
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap, aes_key_unwrap

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
        if fails < 12: print("FAIL", name, "\n got ", str(got)[:160], "\n want", str(want)[:160])
R = random.Random(7)
def rb(n): return bytes(R.getrandbits(8) for _ in range(n))

# hashes, HMAC
for n in [0, 3, 55, 56, 63, 64, 65, 119, 120, 1000] + [R.randrange(2000) for _ in range(15)]:
    m = rb(n)
    check("sha1", q("sha1", H(m)), hashlib.sha1(m).hexdigest())
    k = rb(R.choice([1, 16, 20, 64, 65, 100]))
    check("hmac-sha1", q("hmac", H(k), H(m)), hmac.new(k, m, hashlib.sha1).hexdigest())
# PSK -> PMK (PBKDF2-SHA1, 4096): IEEE 802.11 annex J vectors, then random ones
def pmk(pw, ssid): return hashlib.pbkdf2_hmac("sha1", pw, ssid, 4096, 32)
check("pmk-ieee", q("pmk", H(b"password"), H(b"IEEE")), "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e")
check("pmk-ieee2", q("pmk", H(b"ThisIsAPassword"), H(b"ThisIsASSID")), "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af")
for _ in range(4):
    pw = bytes(R.choice(b"abcdefghijklmnopqrstuvwxyz0123456789 !") for _ in range(R.randrange(8, 64))); ssid = rb(R.randrange(1, 33))
    check("pmk", q("pmk", H(pw), H(ssid)), pmk(pw, ssid).hex())
# the 802.11 PRF
def prf(key, label, data, n):
    out = b""; i = 0
    while len(out) < n: out += hmac.new(key, label + b"\0" + data + bytes([i]), hashlib.sha1).digest(); i += 1
    return out[:n]
check("prf-ieee", q("prf", H(b"\x0b" * 20), H(b"Hi There"), H(b"prefix"), "64"),
      "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a")
for n in [16, 32, 48, 64]:
    k, d = rb(32), rb(76)
    check("prf", q("prf", H(k), H(d), H(b"Pairwise key expansion"), str(n)), prf(k, b"Pairwise key expansion", d, n).hex())
# AES key wrap (RFC 3394 vector, then random)
check("wrap-rfc", q("wrap", "000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff"), "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5")
for n in [16, 24, 32, 40, 56]:
    k, d = rb(16), rb(n); w = aes_key_wrap(k, d)
    check("wrap", q("wrap", H(k), H(d)), w.hex())
    check("unwrap", q("unwrap", H(k), H(w)), d.hex())
    bad = bytearray(w); bad[3] ^= 1
    check("unwrap-bad", q("unwrap", H(k), H(bytes(bad))), "bad")

# CCMP against AES-CCM from OpenSSL with the nonce and header data built from the 802.11 header
def ccm_parts(hdr, pn):
    fc0, fc1 = hdr[0], hdr[1]
    data = (fc0 & 0x0C) == 0x08; qos = data and fc0 & 0x80; a4 = (fc1 & 3) == 3
    aad = bytes([fc0 & 0x8F if data else fc0, (fc1 & 0xC7) | 0x40]) + hdr[4:22] + bytes([hdr[22] & 0x0F, 0])
    off = 24; prio = 0
    if a4: aad += hdr[24:30]; off = 30
    if qos: aad += bytes([hdr[off] & 0x0F, 0]); prio = hdr[off] & 0x0F
    if (fc0 & 0x0C) == 0: prio = 0x10                                 # management frame: the nonce's management bit (802.11w)
    nonce = bytes([prio]) + hdr[10:16] + pn.to_bytes(6, "big")
    return aad, nonce
# IEEE 802.11 annex M CCMP vector
tk = bytes.fromhex("c97c1f67ce371185514a8a19f2bdd52f")
hdr = bytes.fromhex("0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033")
pt = bytes.fromhex("f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050")
aad, nonce = ccm_parts(hdr, 0xB5039776E70C)
want = AESCCM(tk, tag_length=8).encrypt(nonce, pt, aad)
got = q("ccmpenc", H(tk), "b5039776e70c", "0", H(hdr), H(pt))
check("ccmp-annexM", got, (bytes.fromhex("0ce7002076970 3b5".replace(" ", "")) + want).hex())   # CCMP header: PN B5039776E70C, key 0
check("ccmp-annexM-value", got[16:], "f3d0a2fe9a3dbf2342a643e43246e80c3c04d019" + "7845ce0b16f97623")
for _ in range(40):
    kind = R.choice(["data", "qos", "a4qos", "mgmt"])
    if kind == "data": hdr = bytes([0x08, R.choice([0x01, 0x02, 0x41, 0x42 | 0x08])]) + rb(22)
    elif kind == "qos": hdr = bytes([0x88, R.choice([0x01, 0x02])]) + rb(22) + bytes([R.randrange(16), 0])
    elif kind == "a4qos": hdr = bytes([0x88, 0x03]) + rb(28) + bytes([R.randrange(256), 0])
    else: hdr = bytes([0xD0, 0x00]) + rb(22)
    tk, pn, pt = rb(16), R.getrandbits(48), rb(R.randrange(0, 1600))
    aad, nonce = ccm_parts(hdr, pn)
    keyid = R.randrange(4)
    ccmph = bytes([pn & 255, (pn >> 8) & 255, 0, 0x20 | keyid << 6]) + (pn >> 16).to_bytes(4, "little")
    want = ccmph + AESCCM(tk, tag_length=8).encrypt(nonce, pt, aad)
    check("ccmp-enc", q("ccmpenc", H(tk), "%012x" % pn, "%x" % keyid, H(hdr), H(pt)), want.hex())
    check("ccmp-dec", q("ccmpdec", H(tk), H(hdr), H(want)), pt.hex())
    bad = bytearray(want); bad[R.randrange(8, len(bad))] ^= 0x10
    check("ccmp-dec-bad", q("ccmpdec", H(tk), H(hdr), H(bytes(bad))), "bad")

# ---- a simulated router: the 4-way handshake, then a group key update ----
VER, PAIR, INSTALL, ACK, MIC, SECURE, ENC = 2, 8, 0x40, 0x80, 0x100, 0x200, 0x1000
def eapol(info, replay, nonce=b"\0" * 32, data=b"", kck=None):
    body = bytes([2]) + struct.pack(">HH", info, 16 if info & PAIR else 0) + struct.pack(">Q", replay) + nonce + b"\0" * 16 + b"\0" * 8 + b"\0" * 8 + b"\0" * 16 + struct.pack(">H", len(data)) + data
    f = bytearray(bytes([2, 3]) + struct.pack(">H", len(body)) + body)
    if kck: f[81:97] = hmac.new(kck, bytes(f), hashlib.sha1).digest()[:16]
    return bytes(f)
def mic_ok(kck, f):
    c = bytearray(f); m = bytes(c[81:97]); c[81:97] = b"\0" * 16
    return hmac.compare_digest(hmac.new(kck, bytes(c), hashlib.sha1).digest()[:16], m)
def rx(f):
    r = q("rx", H(f)).split(" ", 2)
    code = int(r[0]); reply = b"" if r[1] == "-" else bytes.fromhex(r[1])
    why, ptk = r[2].rsplit(" ", 1)
    return code, reply, why, bytes.fromhex(ptk)
RSN_IE = bytes.fromhex("30140100000fac040100000fac040100000fac020c00")   # WPA2-PSK, CCMP
def handshake(password, router_password, ssid=b"HomeNet"):
    aa, spa, anonce, snonce = rb(6), rb(6), rb(32), rb(32)
    q("init", H(pmk(password, ssid)), H(aa), H(spa), H(RSN_IE), H(snonce))
    rpmk = pmk(router_password, ssid)
    # message 1
    code, m2, why, _ = rx(eapol(VER | PAIR | ACK, 1, anonce))
    check("hs-m1", code, 1)
    a, b = sorted([aa, spa]); n1, n2 = sorted([anonce, snonce])
    ptk = prf(rpmk, b"Pairwise key expansion", a + b + n1 + n2, 48)
    kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]
    if password != router_password:
        check("hs-m2-mic-fails-with-wrong-password", mic_ok(kck, m2), False)
        return
    check("hs-m2-mic", mic_ok(kck, m2), True)
    check("hs-m2-snonce", m2[17:49], snonce)
    check("hs-m2-ie", m2[99:], RSN_IE)
    check("hs-m2-info", struct.unpack(">H", m2[5:7])[0], VER | PAIR | MIC)
    # message 3 with the GTK (encrypted key data)
    gtk = rb(16)
    kd = RSN_IE + bytes([0xDD, 6 + 16, 0x00, 0x0F, 0xAC, 1, 1, 0]) + gtk
    kd += bytes([0xDD]) + b"\0" * ((8 - len(kd) % 8 - 1) % 8)        # padding: 0xDD then zeros, to a multiple of 8
    m3 = eapol(VER | PAIR | INSTALL | ACK | MIC | SECURE | ENC, 2, anonce, aes_key_wrap(kek, kd), kck)
    bad = bytearray(m3); bad[20] ^= 1                                     # a changed ANonce: must be refused
    check("hs-m3-tampered", rx(bytes(bad))[0], -1)
    code, m4, why, sptk = rx(m3)
    check("hs-m3", (code, why), (2, "-"))
    check("hs-ptk", sptk, ptk)
    check("hs-m4-mic", mic_ok(kck, m4), True)
    check("hs-m4-info", struct.unpack(">H", m4[5:7])[0], VER | PAIR | MIC | SECURE)
    check("hs-gtk", q("gtk"), "1 " + gtk.hex())
    check("hs-m3-replayed", rx(m3)[0], 0)                                # the same message again: ignored
    # group key update
    gtk2 = rb(16)
    kd = bytes([0xDD, 6 + 16, 0x00, 0x0F, 0xAC, 1, 2, 0]) + gtk2
    kd += bytes([0xDD]) + b"\0" * ((8 - len(kd) % 8 - 1) % 8)
    g1 = eapol(VER | ACK | MIC | SECURE | ENC, 3, b"\0" * 32, aes_key_wrap(kek, kd), kck)
    code, g2, why, _ = rx(g1)
    check("group-m1", (code, why), (3, "-"))
    check("group-m2-mic", mic_ok(kck, g2), True)
    check("group-gtk", q("gtk"), "2 " + gtk2.hex())
    forged = eapol(VER | ACK | MIC | SECURE | ENC, 4, b"\0" * 32, aes_key_wrap(kek, kd), b"\x11" * 16)
    check("group-forged", rx(forged)[0], -1)
for _ in range(5): handshake(b"correct horse battery", b"correct horse battery")
handshake(b"wrong password!", b"correct horse battery")

print(", ".join("%s %d" % kv for kv in count.items()))
print("WPA: %d failed" % fails)
sys.exit(1 if fails else 0)
