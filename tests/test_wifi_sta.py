# Plays a Wi-Fi access point against AXIOM's station code (common/wifi_sta.c via tests/test_wifi_sta.c):
# authentication, association, the WPA2-PSK 4-way handshake, encrypted data both ways (unicast and group,
# with OpenSSL's AES-CCM on this side), a group key update, replays, an open network, and the failure cases
# (wrong password, no answer, refusal, being disconnected, frames from another access point).
import sys, subprocess, struct, hmac, hashlib, random
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def cmd(*a):
    p.stdin.write(" ".join(str(x) for x in a) + "\n"); p.stdin.flush()
    st, why, rest = p.stdout.readline().rstrip("\n").split("|", 2)
    S = [bytes.fromhex(t[2:]) for t in rest.split() if t.startswith("S:")]
    D = [bytes.fromhex(t[2:]) for t in rest.split() if t.startswith("D:")]
    return int(st), why, S, D, "refused" in rest
fails = 0; count = 0
def check(name, got, want):
    global fails, count
    count += 1
    if got != want:
        fails += 1; print("FAIL", name, "\n got ", str(got)[:200], "\n want", str(want)[:200])
R = random.Random(11)
rb = lambda n: bytes(R.getrandbits(8) for _ in range(n))
def mac(): m = bytearray(rb(6)); m[0] = (m[0] & 0xFC) | 0x02; return bytes(m)   # a unicast, locally administered address
IDLE, AUTH, ASSOC, HANDSHAKE, CONNECTED, FAILED = range(6)

def pmk(pw, ssid): return hashlib.pbkdf2_hmac("sha1", pw, ssid, 4096, 32)
def prf(key, label, data, n):
    out = b""; i = 0
    while len(out) < n: out += hmac.new(key, label + b"\0" + data + bytes([i]), hashlib.sha1).digest(); i += 1
    return out[:n]
def ccm_parts(hdr, pn):
    fc0, fc1 = hdr[0], hdr[1]
    data = (fc0 & 0x0C) == 0x08; qos = data and fc0 & 0x80
    aad = bytes([fc0 & 0x8F if data else fc0, (fc1 & 0xC7) | 0x40]) + hdr[4:22] + bytes([hdr[22] & 0x0F, 0])
    prio = 0
    if qos: aad += bytes([hdr[24] & 0x0F, 0]); prio = hdr[24] & 0x0F
    return aad, bytes([prio]) + hdr[10:16] + pn.to_bytes(6, "big")
def encrypt(key, hdr, body, pn, keyid=0):
    hdr = bytearray(hdr); hdr[1] |= 0x40; hdr = bytes(hdr)
    aad, nonce = ccm_parts(hdr, pn)
    ch = bytes([pn & 255, (pn >> 8) & 255, 0, 0x20 | keyid << 6]) + (pn >> 16).to_bytes(4, "little")
    return hdr + ch + AESCCM(key, tag_length=8).encrypt(nonce, body, aad)
def decrypt(key, f, hl=24):
    hdr, ch = f[:hl], f[hl:hl + 8]
    pn = ch[0] | ch[1] << 8 | int.from_bytes(ch[4:8], "little") << 16
    aad, nonce = ccm_parts(hdr, pn)
    return AESCCM(key, tag_length=8).decrypt(nonce, f[hl + 8:], aad), pn
SNAP = lambda t: bytes([0xAA, 0xAA, 3, 0, 0, 0]) + struct.pack(">H", t)

class AP:
    def __init__(s, ssid, pw=None, seq=0):
        s.bssid = mac(); s.ssid = ssid; s.pmk = pmk(pw, ssid) if pw else None; s.seq = 0; s.now = 1000
    def mgmt(s, sub, to, body):
        s.seq += 1
        return bytes([sub, 0, 0, 0]) + to + s.bssid + s.bssid + struct.pack("<H", s.seq << 4) + body
    def data(s, to, src, body, qos=False):
        s.seq += 1
        h = bytes([0x88 if qos else 0x08, 0x02, 0, 0]) + to + s.bssid + src + struct.pack("<H", s.seq << 4) + (b"\x05\x00" if qos else b"")
        return h, body
    def rx(s, f): s.now += 5; return cmd("rx", f.hex(), s.now)

def eapol(info, replay, nonce=b"\0" * 32, data=b"", kck=None):
    body = bytes([2]) + struct.pack(">HH", info, 16 if info & 8 else 0) + struct.pack(">Q", replay) + nonce + b"\0" * 32 + b"\0" * 16 + struct.pack(">H", len(data)) + data
    f = bytearray(bytes([2, 3]) + struct.pack(">H", len(body)) + body)
    if kck: f[81:97] = hmac.new(kck, bytes(f), hashlib.sha1).digest()[:16]
    return bytes(f)
def mic_ok(kck, f):
    c = bytearray(f); m = bytes(c[81:97]); c[81:97] = b"\0" * 16
    return hmac.new(kck, bytes(c), hashlib.sha1).digest()[:16] == m
def parse_data_out(f, ap, sta_mac, key=None):
    """a station data frame: check the addressing, decrypt, return (dest, ethertype, payload)"""
    assert f[0] == 0x08 and f[1] & 3 == 1, f[:2].hex()
    check("data-a1-is-ap", f[4:10], ap.bssid); check("data-a2-is-me", f[10:16], sta_mac)
    if key: body, pn = decrypt(key, f)
    else: body = f[24:]; check("data-not-protected", f[1] & 0x40, 0)
    check("data-snap", body[:6], bytes([0xAA, 0xAA, 3, 0, 0, 0]))
    return f[16:22], struct.unpack(">H", body[6:8])[0], body[8:]

def join_wpa2(sta_pw, ap_pw, refuse_assoc=0):
    ap = AP(b"HomeNet", ap_pw); me = mac(); snonce = rb(32)
    st, why, S, D, _ = cmd("join", me.hex(), ap.bssid.hex(), b"HomeNet".hex(), pmk(sta_pw, b"HomeNet").hex(), snonce.hex(), ap.now)
    check("auth-sent", (st, len(S)), (AUTH, 1)); a = S[0]
    check("auth-frame", (a[0], a[4:10], a[10:16], a[16:22], a[24:30]), (0xB0, ap.bssid, me, ap.bssid, bytes([0, 0, 1, 0, 0, 0])))
    st, why, S, D, _ = ap.rx(ap.mgmt(0xB0, me, bytes([0, 0, 2, 0, 0, 0])))
    check("assoc-sent", (st, len(S)), (ASSOC, 1)); q = S[0]
    check("assoc-ssid", q[28:28 + 2 + 7], bytes([0, 7]) + b"HomeNet")
    check("assoc-rsn", bytes.fromhex("30140100000fac040100000fac040100000fac020000") in q, True)
    if refuse_assoc:
        st, why, S, D, _ = ap.rx(ap.mgmt(0x10, me, struct.pack("<HHH", 0x11, refuse_assoc, 0)))
        check("assoc-refused", (st, "refused" in why), (FAILED, True)); return
    st, why, S, D, _ = ap.rx(ap.mgmt(0x10, me, struct.pack("<HHH", 0x11, 0, 0xC001)))
    check("handshake-state", st, HANDSHAKE)
    # message 1 (unencrypted data frame with EAPOL)
    anonce = rb(32)
    h, b = ap.data(me, ap.bssid, SNAP(0x888E) + eapol(0x008A, 1, anonce))
    st, why, S, D, _ = ap.rx(h + b)
    check("m2-sent", len(S), 1)
    dst, typ, m2 = parse_data_out(S[0], ap, me)
    check("m2-type", (dst, typ), (ap.bssid, 0x888E))
    a1, a2 = sorted([ap.bssid, me]); n1, n2 = sorted([anonce, snonce])
    ptk = prf(ap.pmk, b"Pairwise key expansion", a1 + a2 + n1 + n2, 48); kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]
    if not mic_ok(kck, m2):
        check("wrong-password-m2-mic-bad", sta_pw != ap_pw, True)
        st, why, S, D, _ = ap.rx(ap.mgmt(0xC0, me, struct.pack("<H", 15)))    # the AP gives up: 4-way handshake timeout
        check("wrong-password-reason", (st, "wrong password" in why), (FAILED, True))
        return
    gtk = rb(16)
    kd = bytes.fromhex("30140100000fac040100000fac040100000fac020000") + bytes([0xDD, 22, 0, 0x0F, 0xAC, 1, 1, 0]) + gtk
    kd += bytes([0xDD]) + b"\0" * ((8 - len(kd) % 8 - 1) % 8)
    h, b = ap.data(me, ap.bssid, SNAP(0x888E) + eapol(0x13CA, 2, anonce, aes_key_wrap(kek, kd), kck))
    st, why, S, D, _ = ap.rx(h + b)
    check("connected", (st, why), (CONNECTED, "-"))
    dst, typ, m4 = parse_data_out(S[0], ap, me)                                  # message 4 goes out unencrypted
    check("m4-mic", mic_ok(kck, m4), True)
    # unicast data from the AP, encrypted (an IPv4 packet), and a QoS one
    for qos in (False, True):
        src = mac(); payload = rb(R.randrange(28, 1400))
        h, b = ap.data(me, src, SNAP(0x0800) + payload, qos)
        f = encrypt(tk, h, b, 10 + qos)
        st, why, S, D, _ = ap.rx(f)
        check("rx-unicast-qos%d" % qos, D, [me + src + b"\x08\x00" + payload])
    st, why, S, D, _ = ap.rx(f)
    check("rx-replay-dropped", D, [])
    bad = bytearray(encrypt(tk, *ap.data(me, src, SNAP(0x0800) + b"x" * 60), 50)); bad[40] ^= 1
    check("rx-tampered-dropped", ap.rx(bytes(bad))[3], [])
    h, b = ap.data(me, src, SNAP(0x0800) + b"plain text")
    check("rx-unencrypted-dropped", ap.rx(h + b)[3], [])
    # group (broadcast) data, with the group key (key id 1)
    bc = b"\xff" * 6; src = mac(); payload = rb(100)
    h, b = ap.data(bc, src, SNAP(0x0806) + payload)
    st, why, S, D, _ = ap.rx(encrypt(gtk, h, b, 1, keyid=1))
    check("rx-group", D, [bc + src + b"\x08\x06" + payload])
    # sending: an Ethernet frame becomes an encrypted To-DS data frame
    dst = mac(); payload = rb(R.randrange(40, 1500))
    st, why, S, D, refused = cmd("eth", (dst + me + b"\x08\x00" + payload).hex())
    d2, typ, body = parse_data_out(S[0], ap, me, tk)
    check("tx-unicast", (d2, typ, body), (dst, 0x0800, payload))
    st, why, S, D, refused = cmd("eth", (dst + me + b"\x86\xdd" + payload).hex())
    d2, typ, body = parse_data_out(S[0], ap, me, tk)
    check("tx-second-pn", (typ, body), (0x86DD, payload))
    # a group key update, then group data with the new key (key id 2)
    gtk2 = rb(16)
    kd = bytes([0xDD, 22, 0, 0x0F, 0xAC, 1, 2, 0]) + gtk2; kd += bytes([0xDD]) + b"\0" * ((8 - len(kd) % 8 - 1) % 8)
    h, b = ap.data(me, ap.bssid, SNAP(0x888E) + eapol(0x1382, 3, b"\0" * 32, aes_key_wrap(kek, kd), kck))
    st, why, S, D, _ = ap.rx(encrypt(tk, h, b, 20))
    _, typ, g2 = parse_data_out(S[0], ap, me, tk)                                # the reply is encrypted now
    check("group-update-reply", (typ, mic_ok(kck, g2)), (0x888E, True))
    h, b = ap.data(bc, src, SNAP(0x0800) + payload)
    check("rx-group-new-key", ap.rx(encrypt(gtk2, h, b, 1, keyid=2))[3], [bc + src + b"\x08\x00" + payload])
    # frames from another access point are ignored
    other = AP(b"HomeNet"); h, b = other.data(me, src, SNAP(0x0800) + payload)
    check("other-ap-ignored", other.rx(encrypt(tk, h, b, 99))[3], [])
    # disconnected by the AP
    st, why, S, D, _ = ap.rx(ap.mgmt(0xC0, me, struct.pack("<H", 1)))
    check("deauth", (st, "disconnected" in why), (FAILED, True))
    check("no-send-after", cmd("eth", (dst + me + b"\x08\x00" + payload).hex())[4], True)

for _ in range(3): join_wpa2(b"correct horse battery", b"correct horse battery")
join_wpa2(b"wrong password!!", b"correct horse battery")
join_wpa2(b"correct horse battery", b"correct horse battery", refuse_assoc=17)

# an open network: no handshake, unencrypted data
ap = AP(b"Cafe Free WiFi"); me = mac()
cmd("join", me.hex(), ap.bssid.hex(), b"Cafe Free WiFi".hex(), "-", rb(32).hex(), ap.now)
st, why, S, D, _ = ap.rx(ap.mgmt(0xB0, me, bytes([0, 0, 2, 0, 0, 0])))
check("open-no-rsn", bytes([0x30]) in S[0][28 + 16:28 + 16 + 1], False)
st, why, S, D, _ = ap.rx(ap.mgmt(0x10, me, struct.pack("<HHH", 0x01, 0, 0xC002)))
check("open-connected", st, CONNECTED)
src = mac(); payload = rb(300); h, b = ap.data(me, src, SNAP(0x0800) + payload)
check("open-rx", ap.rx(h + b)[3], [me + src + b"\x08\x00" + payload])
st, why, S, D, _ = cmd("eth", (src + me + b"\x08\x00" + payload).hex())
check("open-tx", parse_data_out(S[0], ap, me)[2], payload)
st, why, S, D, _ = cmd("leave")
check("leave-deauth", (st, S[0][0], S[0][24:26]), (IDLE, 0xC0, b"\x03\x00"))

# no answer at all: retries, then gives up
ap = AP(b"Gone", b"whatever123"); me = mac()
cmd("join", me.hex(), ap.bssid.hex(), b"Gone".hex(), pmk(b"whatever123", b"Gone").hex(), rb(32).hex(), 0)
sent = 1
for t in range(1, 30):
    st, why, S, D, _ = cmd("tick", t * 100); sent += len(S)
check("no-answer-retries", (sent, st, "didn't answer" in why), (5, FAILED, True))

print("Wi-Fi station: %d checks, %d failed" % (count, fails))
sys.exit(1 if fails else 0)
