#!/usr/bin/env python3
"""An IPv6-only test network for DuncOS running in QEMU (needs scapy).

Start QEMU with
  -netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=5556,remote.type=inet,remote.host=127.0.0.1,remote.port=5555
  -device e1000,netdev=n0
and this script plays everything on the other end of the cable:
  router        fe80::1 / 2001:db8:1::1, advertises prefix 2001:db8:1::/64 (router advertisements)
  DNS server    2001:db8:1::53 (announced by RDNSS, or by DHCPv6 with --dhcpv6)
  web server    2001:db8:2::80, one hop away through the router. Its TCP connections are relayed to
                127.0.0.1:<same port> on this machine, so ordinary IPv4 test servers can serve it.
  dead end      2001:db8:3::1: connections get "destination unreachable" back
It also pings and neighbour-solicits DuncOS's addresses, and with --defend claims the first global
address DuncOS tries, to test duplicate address detection. Everything it sees is logged to stdout."""
import argparse, random, select, socket, sys, time
from scapy.all import (Ether, IPv6, ICMPv6ND_RS, ICMPv6ND_RA, ICMPv6NDOptPrefixInfo, ICMPv6NDOptMTU, ICMPv6NDOptRDNSS,
                       ICMPv6NDOptSrcLLAddr, ICMPv6NDOptDstLLAddr, ICMPv6ND_NS, ICMPv6ND_NA, ICMPv6EchoRequest,
                       ICMPv6EchoReply, ICMPv6DestUnreach, UDP, TCP, DNS, DNSRR, Raw, in6_getnsmac, inet_pton)
from scapy.layers.dhcp6 import DHCP6_InfoRequest, DHCP6_Reply, DHCP6OptServerId, DHCP6OptClientId, DHCP6OptDNSServers, DUID_LL

ap = argparse.ArgumentParser()
ap.add_argument("--dhcpv6", action="store_true", help="no RDNSS; set the O flag and hand out DNS by DHCPv6")
ap.add_argument("--mtu", type=int, default=0, help="advertise this link MTU")
ap.add_argument("--defend", action="store_true", help="claim the first global address DuncOS tries (DAD conflict)")
ap.add_argument("--port", type=int, default=5555)
args = ap.parse_args()

RMAC = "52:54:00:aa:bb:01"
RLL, RG, DNS6, WEB, DEAD = "fe80::1", "2001:db8:1::1", "2001:db8:1::53", "2001:db8:2::80", "2001:db8:3::1"
OWNED = {RLL, RG, DNS6, WEB}
NAMES = {"duncos.test": WEB, "www.duncos.test": WEB, "dead.test": DEAD}

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", args.port))
peer = ("127.0.0.1", args.port + 1)
guest_mac = None
seen = set()
defended = None

def log(*a): print("%7.2f" % (time.time() - T0), *a, flush=True)
def send(pkt): sock.sendto(bytes(pkt), peer)
def to_guest(ip6): return Ether(src=RMAC, dst=guest_mac) / ip6
def norm(a): return socket.inet_ntop(socket.AF_INET6, inet_pton(socket.AF_INET6, a))

def send_ra(dst):
    ra = ICMPv6ND_RA(chlim=64, O=1 if args.dhcpv6 else 0, routerlifetime=1800) / ICMPv6NDOptSrcLLAddr(lladdr=RMAC)
    if args.mtu: ra = ra / ICMPv6NDOptMTU(mtu=args.mtu)
    ra = ra / ICMPv6NDOptPrefixInfo(prefixlen=64, L=1, A=1, validlifetime=86400, preferredlifetime=14400, prefix="2001:db8:1::")
    if not args.dhcpv6: ra = ra / ICMPv6NDOptRDNSS(lifetime=600, dns=[DNS6])
    dmac = "33:33:00:00:00:01" if dst == "ff02::1" else guest_mac
    send(Ether(src=RMAC, dst=dmac) / IPv6(src=RLL, dst=dst, hlim=255) / ra)

# ---------------- TCP relay ----------------
class Conn:
    def __init__(self, gip, gport, sip, sport, seq, win, mss):
        self.gip, self.gport, self.sip, self.sport = gip, gport, sip, sport
        self.rcv_nxt = seq + 1; self.iss = random.getrandbits(32)
        self.snd_una = self.snd_nxt = (self.iss + 1) & 0xFFFFFFFF
        self.win, self.mss = win, min(mss, 1440)
        self.out = bytearray(); self.eof = False; self.fin_sent = False; self.guest_fin = False; self.done = False
        self.progress = time.time(); self.sent = 0
        self.b = socket.create_connection(("127.0.0.1", sport), timeout=2)
        self.b.setblocking(False)
        self.seg("SA", self.iss, b"", opts=[("MSS", 1440)])

    def seg(self, flags, seq, data, opts=None):
        t = TCP(sport=self.sport, dport=self.gport, seq=seq & 0xFFFFFFFF, ack=self.rcv_nxt & 0xFFFFFFFF, flags=flags, window=65535, options=opts or [])
        send(to_guest(IPv6(src=self.sip, dst=self.gip) / t / Raw(data)) if data else to_guest(IPv6(src=self.sip, dst=self.gip) / t))

    def guest(self, t):
        if "R" in t.flags: self.close(); return
        if "A" in t.flags:
            acked = (t.ack - self.snd_una) & 0xFFFFFFFF
            if 0 < acked <= (self.snd_nxt - self.snd_una) & 0xFFFFFFFF:
                data_acked = min(acked, len(self.out))
                del self.out[:data_acked]
                self.snd_una = t.ack; self.progress = time.time()
                if self.fin_sent and self.snd_una == self.snd_nxt and self.guest_fin: self.close()
            self.win = t.window
        data = bytes(t.payload) if t.payload else b""
        if data:
            if t.seq == self.rcv_nxt:
                self.b.sendall(data); self.rcv_nxt = (self.rcv_nxt + len(data)) & 0xFFFFFFFF
            self.seg("A", self.snd_nxt, b"")
        if "F" in t.flags and ((t.seq + len(data)) & 0xFFFFFFFF) == self.rcv_nxt and not self.guest_fin:
            self.guest_fin = True; self.rcv_nxt = (self.rcv_nxt + 1) & 0xFFFFFFFF
            try: self.b.shutdown(socket.SHUT_WR)
            except OSError: pass
            self.seg("A", self.snd_nxt, b"")
            if self.fin_sent and self.snd_una == self.snd_nxt: self.close()

    def pump(self):
        while not self.eof and len(self.out) < 262144:
            try:
                d = self.b.recv(65536)
            except BlockingIOError: break
            except OSError: d = b""
            if not d: self.eof = True; break
            self.out += d
        if self.snd_nxt != self.snd_una and time.time() - self.progress > 0.4:     # go back and resend
            self.snd_nxt = self.snd_una; self.fin_sent = False; self.progress = time.time()
        while True:
            inflight = (self.snd_nxt - self.snd_una) & 0xFFFFFFFF
            off = inflight
            if off >= len(self.out) or inflight >= self.win: break
            n = min(self.mss, len(self.out) - off, self.win - inflight)
            self.seg("PA", self.snd_nxt, bytes(self.out[off:off + n]))
            self.snd_nxt = (self.snd_nxt + n) & 0xFFFFFFFF; self.sent += n
        if self.eof and not self.fin_sent and (self.snd_nxt - self.snd_una) & 0xFFFFFFFF == len(self.out):
            self.seg("FA", self.snd_nxt, b""); self.snd_nxt = (self.snd_nxt + 1) & 0xFFFFFFFF; self.fin_sent = True

    def close(self):
        if not self.done: log("TCP closed [%s]:%d <-> [%s]:%d, %d bytes to DuncOS" % (self.gip, self.gport, self.sip, self.sport, self.sent))
        self.done = True
        try: self.b.close()
        except OSError: pass

conns = {}

# ---------------- packet handling ----------------
def handle(f):
    global guest_mac, defended
    e = Ether(f)
    if IPv6 not in e: return
    ip = e[IPv6]
    if e.src != RMAC: guest_mac = e.src
    src, dst = ip.src, ip.dst
    if src != "::" and not src.startswith("fe80") and src not in seen and src.startswith("2001:db8:1:"):
        seen.add(src); log("new DuncOS address", src)
        send(Ether(src=RMAC, dst=in6_getnsmac(inet_pton(socket.AF_INET6, solicited(src)))) /
             IPv6(src=RG, dst=solicited(src), hlim=255) / ICMPv6ND_NS(tgt=src) / ICMPv6NDOptSrcLLAddr(lladdr=RMAC))
        send(to_guest(IPv6(src=RG, dst=src) / ICMPv6EchoRequest(id=0x4242, seq=1, data=b"router ping")))
    if ICMPv6ND_RS in ip:
        log("RS from", src); send_ra(src if src != "::" else "ff02::1")
    elif ICMPv6ND_NS in ip:
        tgt = ip[ICMPv6ND_NS].tgt
        if src == "::":
            log("DAD probe for", tgt)
            if args.defend and defended is None and tgt.startswith("2001:db8:1:"):
                defended = tgt; log("DEFENDING", tgt)
                send(Ether(src=RMAC, dst="33:33:00:00:00:01") / IPv6(src=tgt, dst="ff02::1", hlim=255) /
                     ICMPv6ND_NA(tgt=tgt, R=0, S=0, O=1) / ICMPv6NDOptDstLLAddr(lladdr=RMAC))
        elif tgt in OWNED:
            log("NS for", tgt, "from", src)
            send(to_guest(IPv6(src=tgt, dst=src, hlim=255) / ICMPv6ND_NA(tgt=tgt, R=1, S=1, O=1) / ICMPv6NDOptDstLLAddr(lladdr=RMAC)))
    elif ICMPv6ND_NA in ip:
        log("NA from DuncOS for", ip[ICMPv6ND_NA].tgt, "S=%d" % ip[ICMPv6ND_NA].S)
    elif ICMPv6EchoRequest in ip and dst in OWNED:
        r = ip[ICMPv6EchoRequest]
        log("echo request to", dst, "from", src)
        send(to_guest(IPv6(src=dst, dst=src) / ICMPv6EchoReply(id=r.id, seq=r.seq, data=r.data)))
    elif ICMPv6EchoReply in ip:
        log("echo reply from DuncOS", src)
    elif ip.nh == 0 and bytes(ip.payload)[8:9] == b"\x8f":           # hop-by-hop header, then ICMPv6 type 143
        log("MLD report from", src or "::")
    elif UDP in ip and ip[UDP].dport == 547 and DHCP6_InfoRequest in ip:
        log("DHCPv6 information-request from", src)
        cid = ip[DHCP6OptClientId]
        send(to_guest(IPv6(src=RLL, dst=src) / UDP(sport=547, dport=546) / DHCP6_Reply(trid=ip[DHCP6_InfoRequest].trid) /
                      DHCP6OptServerId(duid=DUID_LL(lladdr=RMAC)) / cid / DHCP6OptDNSServers(dnsservers=[DNS6])))
    elif UDP in ip and ip[UDP].dport == 53 and dst == DNS6 and DNS in ip:
        q = ip[DNS]; qn = q.qd[0].qname.decode().rstrip("."); qt = q.qd[0].qtype
        log("DNS %s %s from %s" % ({1: "A", 28: "AAAA"}.get(qt, qt), qn, src))
        if qn in NAMES:
            an = DNSRR(rrname=q.qd[0].qname, type="AAAA", ttl=60, rdata=NAMES[qn]) if qt == 28 else None
            r = DNS(id=q.id, qr=1, aa=1, rd=q.rd, ra=1, qd=q.qd, an=an)
        else:
            r = DNS(id=q.id, qr=1, aa=1, rd=q.rd, ra=1, rcode=3, qd=q.qd)
        send(to_guest(IPv6(src=DNS6, dst=src) / UDP(sport=53, dport=ip[UDP].sport) / r))
    elif TCP in ip:
        t = ip[TCP]; key = (src, t.sport, dst, t.dport)
        if dst == DEAD:
            if "S" in t.flags:
                log("SYN to dead end", dst, "-> unreachable")
                send(to_guest(IPv6(src=RG, dst=src) / ICMPv6DestUnreach(code=3) / bytes(ip)[:min(len(bytes(ip)), 1232)]))
            return
        if dst != WEB: return
        if "S" in t.flags and "A" not in t.flags:
            mss = dict((o[0], o[1]) for o in t.options if isinstance(o, tuple)).get("MSS", 536)
            log("SYN [%s]:%d -> [%s]:%d MSS %d" % (src, t.sport, dst, t.dport, mss))
            old = conns.get(key)
            if old and not old.done: old.seg("SA", old.iss, b"", opts=[("MSS", 1440)]); return
            try: conns[key] = Conn(src, t.sport, dst, t.dport, t.seq, t.window, mss)
            except OSError as ex:
                log("backend refused:", ex)
                send(to_guest(IPv6(src=dst, dst=src) / TCP(sport=t.dport, dport=t.sport, seq=0, ack=t.seq + 1, flags="RA")))
        elif key in conns: conns[key].guest(t)

def solicited(a):
    b = inet_pton(socket.AF_INET6, a)
    return socket.inet_ntop(socket.AF_INET6, bytes.fromhex("ff0200000000000000000001ff") + b[13:])

T0 = time.time()
log("IPv6 test network on udp %d (DNS by %s, MTU %s%s)" % (args.port, "DHCPv6" if args.dhcpv6 else "RDNSS", args.mtu or "default", ", defending" if args.defend else ""))
last_ra = 0
while True:
    r, _, _ = select.select([sock] + [c.b for c in conns.values() if not c.done], [], [], 0.05)
    if sock in r:
        data, _ = sock.recvfrom(65535)
        try: handle(data)
        except Exception as ex: log("error handling frame:", repr(ex))
    for k in list(conns):
        c = conns[k]
        if c.done: del conns[k]; continue
        try: c.pump()
        except OSError as ex: log("relay error:", ex); c.close()
    if guest_mac and time.time() - last_ra > 20:
        last_ra = time.time(); send_ra("ff02::1")
