/* TCP/IP stack: Ethernet, ARP, IPv4, ICMP (ping), UDP, DHCP client, DNS resolver, TCP.
 * A kernel thread polls the network card; programs use it through system calls.
 * IPv4 addresses are stored in network byte order. */
#include "net.h"

static struct netif *nif;
static uint32_t my_ip, my_mask, my_gw, my_dns, lease_s;
static bool configured, dns_manual;
static uint16_t ip_id = 1;
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static inline uint16_t hs(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t hl(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static inline uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void p16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint32_t csum_add(uint32_t s, const void *data, int len) {
    const uint8_t *p = data;
    for (; len > 1; len -= 2, p += 2) s += p[0] << 8 | p[1];
    if (len) s += p[0] << 8;
    return s;
}
static uint16_t csum_fin(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }

static void fmt_ip(char *o, uint32_t ip) { ksnprintf(o, 16, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24); }

/* ---------------- Ethernet + ARP ---------------- */
struct arp_ent { uint32_t ip; uint8_t mac[6]; bool valid; uint64_t t; };
static struct arp_ent arp[32];
struct pend { bool used; uint32_t hop; uint16_t len; uint64_t t; uint8_t frame[1514]; };
static struct pend pend[8];

static void eth_send(const uint8_t *dst, uint16_t type, const void *payload, int len) {
    if (!nif || len > 1500) return;
    uint8_t f[1514];
    memcpy(f, dst, 6); memcpy(f + 6, nif->mac, 6); p16(f + 12, type);
    memcpy(f + 14, payload, len);
    nif->send(nif, f, 14 + len);
}

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip) {
    uint8_t a[28];
    p16(a, 1); p16(a + 2, 0x0800); a[4] = 6; a[5] = 4; p16(a + 6, op);
    memcpy(a + 8, nif->mac, 6); memcpy(a + 14, &my_ip, 4);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6); memcpy(a + 24, &tip, 4);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, a, 28);
}

static struct arp_ent *arp_find(uint32_t ip) {
    for (int i = 0; i < 32; i++) if (arp[i].valid && arp[i].ip == ip) return &arp[i];
    return 0;
}

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    struct arp_ent *e = arp_find(ip);
    if (!e) { int old = 0; for (int i = 0; i < 32; i++) { if (!arp[i].valid) { old = i; break; } if (arp[i].t < arp[old].t) old = i; } e = &arp[old]; }
    e->ip = ip; memcpy(e->mac, mac, 6); e->valid = true; e->t = uptime_ms();
    for (int i = 0; i < 8; i++)          /* send packets that were waiting for this address */
        if (pend[i].used && pend[i].hop == ip) { memcpy(pend[i].frame, mac, 6); nif->send(nif, pend[i].frame, pend[i].len); pend[i].used = false; }
}

static void arp_input(const uint8_t *a, int len) {
    if (len < 28 || g16(a) != 1 || g16(a + 2) != 0x0800) return;
    uint32_t sip, tip; memcpy(&sip, a + 14, 4); memcpy(&tip, a + 24, 4);
    if (configured && (sip & my_mask) == (my_ip & my_mask)) arp_learn(sip, a + 8);
    if (g16(a + 6) == 1 && configured && tip == my_ip) arp_send(2, a + 8, sip);
}

/* ---------------- IPv4 ---------------- */
static void ip_send(uint32_t dst, uint8_t proto, const void *payload, int len) {
    if (!nif || len > 1480) return;
    uint8_t f[1514];
    uint8_t *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 20 + len); p16(ip + 4, ip_id++); p16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto; p16(ip + 10, 0);
    memcpy(ip + 12, &my_ip, 4); memcpy(ip + 16, &dst, 4);
    p16(ip + 10, csum_fin(csum_add(0, ip, 20)));
    memcpy(ip + 20, payload, len);
    memcpy(f + 6, nif->mac, 6); p16(f + 12, 0x0800);
    int flen = 14 + 20 + len;
    if (dst == 0xFFFFFFFF || (configured && dst == (my_ip | ~my_mask))) { memcpy(f, BCAST, 6); nif->send(nif, f, flen); return; }
    uint32_t hop = (dst & my_mask) == (my_ip & my_mask) ? dst : my_gw;
    struct arp_ent *e = arp_find(hop);
    if (e && uptime_ms() - e->t < 300000) { memcpy(f, e->mac, 6); nif->send(nif, f, flen); return; }
    /* not resolved yet: park the frame and ask */
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!pend[i].used) { slot = i; break; }
    if (slot >= 0) { pend[slot].used = true; pend[slot].hop = hop; pend[slot].len = flen; pend[slot].t = uptime_ms(); memcpy(pend[slot].frame, f, flen); }
    arp_send(1, 0, hop);
}

/* ---------------- ICMP ---------------- */
static volatile uint16_t ping_id, ping_seq; static volatile bool ping_got; static uint64_t ping_t0, ping_rtt;

static void icmp_input(uint32_t src, const uint8_t *p, int len) {
    if (len < 8 || csum_fin(csum_add(0, p, len))) return;
    if (p[0] == 8) {                                  /* echo request: answer it */
        uint8_t r[1480]; if (len > 1480) return;
        memcpy(r, p, len); r[0] = 0; p16(r + 2, 0); p16(r + 2, csum_fin(csum_add(0, r, len)));
        ip_send(src, 1, r, len);
    } else if (p[0] == 0 && g16(p + 4) == ping_id && g16(p + 6) == ping_seq) {
        ping_rtt = (rdtsc() - ping_t0) / (tsc_hz() / 1000000); ping_got = true;
    }
}

long net_ping(uint32_t ip, uint32_t timeout_ms) {
    if (!nif) return NET_ENODEV;
    if (!configured) return NET_ENOTCONN;
    uint8_t p[40]; memset(p, 0, sizeof p);
    ping_id = 0xD0C5; ping_seq++; ping_got = false;
    p[0] = 8; p16(p + 4, ping_id); p16(p + 6, ping_seq);
    memcpy(p + 8, "DuncOS ping, Thane kernel.....", 32);
    p16(p + 2, csum_fin(csum_add(0, p, 40)));
    ping_t0 = rdtsc();
    ip_send(ip, 1, p, 40);
    uint64_t end = uptime_ms() + timeout_ms;
    while (!ping_got && uptime_ms() < end) task_sleep(1);
    return ping_got ? (long)ping_rtt : NET_ETIMEDOUT;
}

/* ---------------- UDP ---------------- */
struct udp_wait { bool active; uint16_t port; volatile int len; uint8_t buf[600]; };
static struct udp_wait uwait[4];

static void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len) {
    uint8_t u[1480];
    if (len > 1472) return;
    p16(u, sport); p16(u + 2, dport); p16(u + 4, 8 + len); p16(u + 6, 0);
    memcpy(u + 8, data, len);
    uint8_t ph[12]; memcpy(ph, &my_ip, 4); memcpy(ph + 4, &dst, 4); ph[8] = 0; ph[9] = 17; p16(ph + 10, 8 + len);
    uint16_t c = csum_fin(csum_add(csum_add(0, ph, 12), u, 8 + len));
    p16(u + 6, c ? c : 0xFFFF);
    ip_send(dst, 17, u, 8 + len);
}

static void dhcp_input(const uint8_t *p, int len);
static void udp_input(uint32_t src, const uint8_t *p, int len) {
    if (len < 8) return;
    uint16_t dport = g16(p + 2), ulen = g16(p + 4);
    if (ulen < 8 || ulen > len) return;
    if (dport == 68) { dhcp_input(p + 8, ulen - 8); return; }
    for (int i = 0; i < 4; i++)
        if (uwait[i].active && uwait[i].port == dport && uwait[i].len < 0) {
            int n = ulen - 8 < (int)sizeof uwait[i].buf ? ulen - 8 : (int)sizeof uwait[i].buf;
            memcpy(uwait[i].buf, p + 8, n); uwait[i].len = n;
        }
}

/* ---------------- DHCP ---------------- */
enum { DH_INIT, DH_SELECTING, DH_REQUESTING, DH_BOUND };
static int dh_state; static uint32_t dh_xid, dh_offer, dh_server; static uint64_t dh_next, dh_renew_at;

static void dhcp_send(int type) {
    uint8_t d[300]; memset(d, 0, sizeof d);
    d[0] = 1; d[1] = 1; d[2] = 6; memcpy(d + 4, &dh_xid, 4); p16(d + 10, 0x8000);
    memcpy(d + 28, nif->mac, 6);
    p32(d + 236, 0x63825363);
    int o = 240;
    d[o++] = 53; d[o++] = 1; d[o++] = type;
    d[o++] = 61; d[o++] = 7; d[o++] = 1; memcpy(d + o, nif->mac, 6); o += 6;
    d[o++] = 12; d[o++] = 6; memcpy(d + o, "duncos", 6); o += 6;
    if (type == 3) { d[o++] = 50; d[o++] = 4; memcpy(d + o, &dh_offer, 4); o += 4; d[o++] = 54; d[o++] = 4; memcpy(d + o, &dh_server, 4); o += 4; }
    d[o++] = 55; d[o++] = 4; d[o++] = 1; d[o++] = 3; d[o++] = 6; d[o++] = 51;
    d[o++] = 255;
    uint32_t save = my_ip; my_ip = 0;
    udp_send(0xFFFFFFFF, 68, 67, d, o < 300 ? 300 : o);
    my_ip = save;
}

static void dhcp_start(void) {
    dh_xid = (uint32_t)rdtsc() ^ 0x5EED;
    dh_state = DH_SELECTING; dh_next = uptime_ms() + 4000;
    dhcp_send(1);
}

static void dhcp_input(const uint8_t *d, int len) {
    if (len < 240 || d[0] != 2 || memcmp(d + 4, &dh_xid, 4) || g32(d + 236) != 0x63825363) return;
    int type = 0; uint32_t mask = 0, gw = 0, dns = 0, server = 0, lease = 3600;
    for (int o = 240; o < len && d[o] != 255;) {
        if (d[o] == 0) { o++; continue; }
        if (o + 1 >= len) break;
        int c = d[o], l = d[o + 1]; const uint8_t *v = d + o + 2;
        if (o + 2 + l > len) break;
        if (c == 53 && l >= 1) type = v[0];
        if (c == 1 && l >= 4) memcpy(&mask, v, 4);
        if (c == 3 && l >= 4) memcpy(&gw, v, 4);
        if (c == 6 && l >= 4) memcpy(&dns, v, 4);
        if (c == 54 && l >= 4) memcpy(&server, v, 4);
        if (c == 51 && l >= 4) lease = g32(v);
        o += 2 + l;
    }
    uint32_t yi; memcpy(&yi, d + 16, 4);
    if (type == 2 && dh_state == DH_SELECTING) {
        dh_offer = yi; dh_server = server; dh_state = DH_REQUESTING; dh_next = uptime_ms() + 4000;
        dhcp_send(3);
    } else if (type == 5 && dh_state == DH_REQUESTING) {
        bool was = configured;
        my_ip = yi; my_mask = mask ? mask : IP4(255, 255, 255, 0); my_gw = gw;
        if (!dns_manual) my_dns = dns ? dns : gw;
        lease_s = lease; configured = true; dh_state = DH_BOUND;
        dh_renew_at = uptime_ms() + (uint64_t)(lease > 120 ? lease / 2 : 60) * 1000;
        char a[16], g[16]; fmt_ip(a, my_ip); fmt_ip(g, my_gw);
        kprintf("[net] DHCP: address %s, gateway %s, lease %u s\n", a, g, lease);
        if (!was) { char m[96]; ksnprintf(m, sizeof m, "Your address is %s (%s).", a, nif->adapter); desktop_notify("Connected to the network", m); }
    } else if (type == 6 && dh_state == DH_REQUESTING) {
        dh_state = DH_INIT; dh_next = uptime_ms() + 2000;     /* NAK: start over */
    }
}

static void dhcp_tick(void) {
    uint64_t now = uptime_ms();
    if (dh_state == DH_BOUND) { if (now >= dh_renew_at) dhcp_start(); return; }   /* simple renewal: run DHCP again */
    if (now < dh_next) return;
    if (dh_state == DH_REQUESTING) { dh_state = DH_SELECTING; }
    dhcp_start();
}

/* ---------------- DNS ---------------- */
struct dns_cache { char name[64]; uint32_t ip; uint64_t until; };
static struct dns_cache dcache[16];

static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t v[4]; int k = 0;
    for (; k < 4; k++) {
        if (*s < '0' || *s > '9') return false;
        v[k] = 0;
        while (*s >= '0' && *s <= '9') { v[k] = v[k] * 10 + (*s++ - '0'); if (v[k] > 255) return false; }
        if (k < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = IP4(v[0], v[1], v[2], v[3]);
    return true;
}

static int skip_name(const uint8_t *m, int len, int o) {
    while (o < len) {
        if (m[o] == 0) return o + 1;
        if ((m[o] & 0xC0) == 0xC0) return o + 2;
        o += m[o] + 1;
    }
    return -1;
}

int net_resolve(const char *name, uint32_t *ip) {
    if (parse_ip(name, ip)) return NET_OK;
    if (!nif) return NET_ENODEV;
    if (!configured || !my_dns) return NET_ENOTCONN;
    int nl = strlen(name);
    if (!nl || nl > 63) return NET_EINVAL;
    for (int i = 0; i < 16; i++)
        if (dcache[i].until > uptime_ms() && !strcmp(dcache[i].name, name)) { *ip = dcache[i].ip; return NET_OK; }
    int w = -1;
    for (int i = 0; i < 4; i++) if (!uwait[i].active) { w = i; break; }
    if (w < 0) return NET_EAGAIN;
    static uint16_t next_port = 53000;
    uint16_t port = next_port++; if (next_port > 53999) next_port = 53000;
    uwait[w].active = true; uwait[w].port = port;
    int result = NET_ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && result == NET_ETIMEDOUT; attempt++) {
        uint8_t q[300]; uint16_t id = (uint16_t)rdtsc();
        p16(q, id); p16(q + 2, 0x0100); p16(q + 4, 1); p16(q + 6, 0); p16(q + 8, 0); p16(q + 10, 0);
        int o = 12;
        for (const char *s = name; *s;) {
            const char *dot = s; while (*dot && *dot != '.') dot++;
            int l = dot - s; if (l == 0 || l > 63) { uwait[w].active = false; return NET_EINVAL; }
            q[o++] = l; memcpy(q + o, s, l); o += l;
            s = *dot ? dot + 1 : dot;
        }
        q[o++] = 0; p16(q + o, 1); p16(q + o + 2, 1); o += 4;
        uwait[w].len = -1;
        udp_send(my_dns, port, 53, q, o);
        uint64_t end = uptime_ms() + 1500;
        while (uwait[w].len < 0 && uptime_ms() < end) task_sleep(1);
        int len = uwait[w].len;
        if (len < 12) continue;
        const uint8_t *m = uwait[w].buf;
        if (g16(m) != id) continue;
        if ((m[3] & 15) == 3) { result = NET_ENOTFOUND; break; }
        int an = g16(m + 6), off = skip_name(m, len, 12);
        if (off < 0) continue;
        off += 4;
        result = NET_ENOTFOUND;
        for (int i = 0; i < an && off > 0 && off + 10 <= len; i++) {
            off = skip_name(m, len, off);
            if (off < 0 || off + 10 > len) break;
            uint16_t type = g16(m + off), rdl = g16(m + off + 8); uint32_t ttl = g32(m + off + 4);
            off += 10;
            if (type == 1 && rdl == 4 && off + 4 <= len) {
                memcpy(ip, m + off, 4);
                int slot = 0; for (int k = 0; k < 16; k++) if (dcache[k].until < dcache[slot].until) slot = k;
                strncpy0(dcache[slot].name, name, 64); dcache[slot].ip = *ip;
                dcache[slot].until = uptime_ms() + (ttl > 3600 ? 3600 : ttl < 30 ? 30 : ttl) * 1000ULL;
                result = NET_OK;
                break;
            }
            off += rdl;
        }
    }
    uwait[w].active = false;
    return result;
}

/* ---------------- TCP ---------------- */
enum { T_FREE, T_SYN_SENT, T_ESTAB, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSING, T_TIME_WAIT, T_CLOSE_WAIT, T_LAST_ACK, T_CLOSED };
#define F_FIN 1
#define F_SYN 2
#define F_RST 4
#define F_PSH 8
#define F_ACK 16
#define TCP_MAX 16
#define RBUF 65536
#define SBUF 65536

struct tcb {
    int state, gen, err;
    struct task *owner;
    uint32_t rip; uint16_t rport, lport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint16_t mss;
    uint8_t *rbuf; uint32_t rhead, rlen;     /* received, not yet read */
    uint8_t *sbuf; uint32_t slen;            /* sbuf[0] is the byte at snd_una */
    bool fin_sent, fin_rcvd, closing;
    uint32_t rto; int retries; uint64_t rto_at, time_wait_until;
    uint32_t last_adv;
};
static struct tcb tcbs[TCP_MAX];
static uint16_t next_lport = 49152;

static uint32_t rwin(struct tcb *t) { uint32_t f = RBUF - t->rlen; return f > 65535 ? 65535 : f; }

static void tcp_raw(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, int flags, uint16_t win,
                    const uint8_t *data, int len, bool mss_opt) {
    uint8_t s[1480];
    int hl = mss_opt ? 24 : 20;
    if (hl + len > 1480) return;
    p16(s, sport); p16(s + 2, dport); p32(s + 4, seq); p32(s + 8, ack);
    s[12] = (hl / 4) << 4; s[13] = flags; p16(s + 14, win); p16(s + 16, 0); p16(s + 18, 0);
    if (mss_opt) { s[20] = 2; s[21] = 4; p16(s + 22, 1460); }
    if (len) memcpy(s + hl, data, len);
    uint8_t ph[12]; memcpy(ph, &my_ip, 4); memcpy(ph + 4, &dst, 4); ph[8] = 0; ph[9] = 6; p16(ph + 10, hl + len);
    p16(s + 16, csum_fin(csum_add(csum_add(0, ph, 12), s, hl + len)));
    ip_send(dst, 6, s, hl + len);
}

static void tcp_seg(struct tcb *t, int flags, uint32_t seq, const uint8_t *data, int len) {
    t->last_adv = rwin(t);
    tcp_raw(t->rip, t->lport, t->rport, seq, t->rcv_nxt, flags, (uint16_t)t->last_adv, data, len, flags & F_SYN);
}

static void arm_rto(struct tcb *t) { t->rto_at = uptime_ms() + t->rto; }

/* send whatever the window allows */
static void tcp_output(struct tcb *t) {
    if (t->state != T_ESTAB && t->state != T_CLOSE_WAIT && t->state != T_FIN_WAIT1 && t->state != T_LAST_ACK && t->state != T_CLOSING) return;
    uint32_t wnd = t->snd_wnd ? t->snd_wnd : 1;
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una;
        if (t->fin_sent && off) off--;          /* FIN occupies one sequence number after the data */
        if (off >= t->slen) break;
        uint32_t inflight = t->snd_nxt - t->snd_una;
        if (inflight >= wnd) break;
        uint32_t n = t->slen - off;
        if (n > t->mss) n = t->mss;
        if (n > wnd - inflight) n = wnd - inflight;
        tcp_seg(t, F_ACK | F_PSH, t->snd_nxt, t->sbuf + off, n);
        if (t->snd_nxt == t->snd_una) arm_rto(t);
        t->snd_nxt += n;
    }
    if (t->closing && !t->fin_sent && t->snd_nxt - t->snd_una == t->slen) {
        tcp_seg(t, F_ACK | F_FIN, t->snd_nxt, 0, 0);
        if (t->snd_nxt == t->snd_una) arm_rto(t);
        t->snd_nxt++; t->fin_sent = true;
        if (t->state == T_ESTAB) t->state = T_FIN_WAIT1;
        else if (t->state == T_CLOSE_WAIT) t->state = T_LAST_ACK;
    }
}

static struct tcb *tcb_find(uint32_t rip, uint16_t rport, uint16_t lport) {
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state != T_FREE && t->rip == rip && t->rport == rport && t->lport == lport) return t;
    }
    return 0;
}

static void tcp_free(struct tcb *t) {
    kfree(t->rbuf); kfree(t->sbuf);
    t->rbuf = t->sbuf = 0;
    t->state = T_FREE; t->gen++;
}

static void tcp_input(uint32_t src, const uint8_t *s, int len) {
    if (len < 20) return;
    uint8_t ph[12]; memcpy(ph, &src, 4); memcpy(ph + 4, &my_ip, 4); ph[8] = 0; ph[9] = 6; p16(ph + 10, len);
    if (csum_fin(csum_add(csum_add(0, ph, 12), s, len))) return;
    uint16_t sport = g16(s), dport = g16(s + 2);
    uint32_t seq = g32(s + 4), ack = g32(s + 8);
    int hl = (s[12] >> 4) * 4, flags = s[13];
    uint16_t win = g16(s + 14);
    if (hl < 20 || hl > len) return;
    const uint8_t *data = s + hl; int dlen = len - hl;
    struct tcb *t = tcb_find(src, sport, dport);
    if (!t) {                                       /* nobody listening: reset */
        if (!(flags & F_RST)) {
            if (flags & F_ACK) tcp_raw(src, dport, sport, ack, 0, F_RST, 0, 0, 0, false);
            else tcp_raw(src, dport, sport, 0, seq + dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), F_RST | F_ACK, 0, 0, 0, false);
        }
        return;
    }
    if (flags & F_RST) {
        if (t->state == T_SYN_SENT) { if ((flags & F_ACK) && ack == t->iss + 1) { t->err = NET_EREFUSED; t->state = T_CLOSED; } }
        else if (seq == t->rcv_nxt || (seq - t->rcv_nxt) < rwin(t) + 1) { t->err = NET_ERESET; t->state = T_CLOSED; }
        return;
    }
    if (t->state == T_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != t->iss + 1) return;
        t->rcv_nxt = seq + 1; t->snd_una = ack; t->snd_wnd = win;
        for (int o = 20; o < hl;) {                 /* options: MSS */
            uint8_t k = s[o];
            if (k == 0) break;
            if (k == 1) { o++; continue; }
            if (o + 1 >= hl || s[o + 1] < 2) break;
            if (k == 2 && s[o + 1] == 4) { uint16_t m = g16(s + o + 2); if (m < t->mss) t->mss = m; }
            o += s[o + 1];
        }
        t->state = T_ESTAB; t->retries = 0; t->rto_at = 0;
        tcp_seg(t, F_ACK, t->snd_nxt, 0, 0);
        return;
    }
    if (t->state == T_CLOSED || t->state == T_FREE) return;
    /* ACK */
    if (flags & F_ACK) {
        uint32_t acked = ack - t->snd_una, inflight = t->snd_nxt - t->snd_una;
        if (acked && acked <= inflight) {
            uint32_t data_acked = acked;
            bool fin_acked = t->fin_sent && ack == t->snd_nxt;
            if (fin_acked) data_acked--;
            if (data_acked > t->slen) data_acked = t->slen;
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
            t->snd_una = ack;
            t->retries = 0; t->rto = 1000;
            t->rto_at = t->snd_una == t->snd_nxt ? 0 : uptime_ms() + t->rto;
            if (fin_acked) {
                if (t->state == T_FIN_WAIT1) t->state = T_FIN_WAIT2;
                else if (t->state == T_CLOSING) { t->state = T_TIME_WAIT; t->time_wait_until = uptime_ms() + 2000; }
                else if (t->state == T_LAST_ACK) { t->state = T_CLOSED; }
            }
        }
        if (ack == t->snd_una) t->snd_wnd = win;
    }
    /* data */
    bool need_ack = false;
    if (dlen > 0) {
        if (seq == t->rcv_nxt && !t->fin_rcvd) {
            uint32_t space = RBUF - t->rlen, n = (uint32_t)dlen < space ? (uint32_t)dlen : space;
            for (uint32_t i = 0; i < n; i++) t->rbuf[(t->rhead + t->rlen + i) % RBUF] = data[i];
            t->rlen += n; t->rcv_nxt += n;
            if (n < (uint32_t)dlen) flags &= ~F_FIN;  /* didn't take it all: the FIN comes again later */
        }
        need_ack = true;                            /* also re-acks duplicates / out-of-order */
    }
    if ((flags & F_FIN) && seq + dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++; t->fin_rcvd = true; need_ack = true;
        if (t->state == T_ESTAB) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1) t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT2) { t->state = T_TIME_WAIT; t->time_wait_until = uptime_ms() + 2000; }
    }
    if (need_ack) tcp_seg(t, F_ACK, t->snd_nxt, 0, 0);
    tcp_output(t);
}

static void tcp_timers(void) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state == T_FREE) continue;
        if (t->state == T_TIME_WAIT && now >= t->time_wait_until) { t->state = T_CLOSED; }
        if (t->state == T_CLOSED) { if (!t->owner) tcp_free(t); continue; }
        if (!t->rto_at || now < t->rto_at) continue;
        if (++t->retries > 7) { t->err = NET_ETIMEDOUT; t->state = T_CLOSED; continue; }
        t->rto = t->rto * 2 > 16000 ? 16000 : t->rto * 2;
        if (t->state == T_SYN_SENT) { tcp_seg(t, F_SYN, t->iss, 0, 0); arm_rto(t); continue; }
        /* go back to the first unacknowledged byte and send again */
        if (t->fin_sent) t->fin_sent = false;
        if (t->state == T_FIN_WAIT1) t->state = T_ESTAB;
        if (t->state == T_LAST_ACK) t->state = T_CLOSE_WAIT;
        if (t->state == T_CLOSING) t->state = T_ESTAB;   /* FIN will be re-sent; peer's FIN already counted */
        t->snd_nxt = t->snd_una;
        t->rto_at = 0;
        tcp_output(t);
        if (!t->rto_at) arm_rto(t);
    }
}

static struct tcb *handle_tcb(int h) {
    int i = h & 0xFF, gen = h >> 8;
    if (h < 0 || i >= TCP_MAX) return 0;
    struct tcb *t = &tcbs[i];
    if (t->state == T_FREE || t->gen != gen || t->owner != current) return 0;
    return t;
}

int tcp_connect(uint32_t ip, uint16_t port) {
    if (!nif) return NET_ENODEV;
    if (!configured) return NET_ENOTCONN;
    struct tcb *t = 0; int idx = -1;
    for (int i = 0; i < TCP_MAX; i++) if (tcbs[i].state == T_FREE) { t = &tcbs[i]; idx = i; break; }
    if (!t) return NET_ENOMEM;
    int gen = t->gen;
    memset(t, 0, sizeof *t);
    t->gen = gen;
    t->rbuf = kmalloc(RBUF); t->sbuf = kmalloc(SBUF);
    if (!t->rbuf || !t->sbuf) { kfree(t->rbuf); kfree(t->sbuf); return NET_ENOMEM; }
    t->owner = current; t->rip = ip; t->rport = port;
    t->lport = next_lport++; if (next_lport > 65000) next_lport = 49152;
    t->iss = (uint32_t)rdtsc(); t->snd_una = t->iss; t->snd_nxt = t->iss + 1;
    t->mss = 1460; t->rto = 1000;
    t->state = T_SYN_SENT;
    tcp_seg(t, F_SYN, t->iss, 0, 0);
    arm_rto(t);
    uint64_t end = uptime_ms() + 15000;
    while (t->state == T_SYN_SENT && uptime_ms() < end) task_sleep(1);
    if (t->state != T_ESTAB) {
        int e = t->err ? t->err : NET_ETIMEDOUT;
        t->owner = 0; t->state = T_CLOSED; tcp_free(t);
        return e;
    }
    return (t->gen << 8) | idx;
}

long tcp_send(int h, const void *buf, uint32_t len) {
    struct tcb *t = handle_tcb(h);
    if (!t) return NET_EINVAL;
    uint32_t done = 0;
    while (done < len) {
        if (t->state != T_ESTAB && t->state != T_CLOSE_WAIT) return t->err ? t->err : NET_ERESET;
        uint32_t space = SBUF - t->slen;
        if (!space) { task_sleep(1); continue; }
        uint32_t n = len - done < space ? len - done : space;
        memcpy(t->sbuf + t->slen, (const uint8_t *)buf + done, n);
        t->slen += n; done += n;
        tcp_output(t);
    }
    return done;
}

long tcp_recv(int h, void *buf, uint32_t len, int64_t timeout_ms) {
    struct tcb *t = handle_tcb(h);
    if (!t) return NET_EINVAL;
    uint64_t end = timeout_ms < 0 ? ~0ULL : uptime_ms() + timeout_ms;
    while (!t->rlen) {
        if (t->fin_rcvd) return 0;                                   /* end of stream */
        if (t->state == T_CLOSED) return t->err ? t->err : 0;
        if (uptime_ms() >= end) return NET_EAGAIN;
        task_sleep(1);
    }
    uint32_t n = len < t->rlen ? len : t->rlen;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = t->rbuf[(t->rhead + i) % RBUF];
    t->rhead = (t->rhead + n) % RBUF; t->rlen -= n;
    if (t->last_adv < 2u * t->mss && rwin(t) >= 2u * t->mss && !t->fin_rcvd) tcp_seg(t, F_ACK, t->snd_nxt, 0, 0);  /* window update */
    return n;
}

int tcp_close(int h) {
    struct tcb *t = handle_tcb(h);
    if (!t) return NET_EINVAL;
    t->owner = 0;                       /* the net thread frees it once the connection has closed */
    if (t->state == T_ESTAB || t->state == T_CLOSE_WAIT) { t->closing = true; tcp_output(t); }
    else if (t->state != T_FIN_WAIT1 && t->state != T_FIN_WAIT2 && t->state != T_CLOSING && t->state != T_LAST_ACK && t->state != T_TIME_WAIT) t->state = T_CLOSED;
    return NET_OK;
}

void net_task_exit(struct task *task) {
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state == T_FREE || t->owner != task) continue;
        t->owner = 0;
        if (t->state == T_ESTAB || t->state == T_CLOSE_WAIT || t->state == T_SYN_SENT)
            tcp_raw(t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, F_RST | F_ACK, 0, 0, 0, false);
        t->state = T_CLOSED;
    }
}

/* ---------------- receive path ---------------- */
void net_rx(struct netif *n, const uint8_t *f, int len) {
    if (len < 14) return;
    uint16_t type = g16(f + 12);
    const uint8_t *p = f + 14; len -= 14;
    if (type == 0x0806) { arp_input(p, len); return; }
    if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4) return;
    int ihl = (p[0] & 15) * 4, tot = g16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || csum_fin(csum_add(0, p, ihl))) return;
    if (g16(p + 6) & 0x3FFF) return;                 /* fragments aren't supported */
    uint32_t src, dst; memcpy(&src, p + 12, 4); memcpy(&dst, p + 16, 4);
    bool for_us = dst == 0xFFFFFFFF || (configured && (dst == my_ip || dst == (my_ip | ~my_mask))) || (!configured && p[9] == 17);
    if (!for_us) return;
    if (configured && (src & my_mask) == (my_ip & my_mask) && !arp_find(src)) arp_learn(src, f + 6);
    switch (p[9]) {
    case 1: icmp_input(src, p + ihl, tot - ihl); break;
    case 6: if (configured) tcp_input(src, p + ihl, tot - ihl); break;
    case 17: udp_input(src, p + ihl, tot - ihl); break;
    }
}

/* ---------------- setup, thread, info ---------------- */
void net_init(void) {
    for (int i = 0; i < pci_count && !nif; i++) {
        struct pci_dev *p = &pci_devs[i];
        if (p->class_code != 0x02) continue;
        nif = e1000_probe(p);
        if (!nif) nif = rtl8168_probe(p);
    }
    if (!nif) { LOG_INFO("No supported wired network adapter found\n"); return; }
    LOG_OK("Network: %s (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x\n", nif->adapter, nif->driver,
           nif->mac[0], nif->mac[1], nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5]);
}

bool net_present(void) { return nif != 0; }
bool net_ready(void) { return nif && configured && nif->link; }

void net_thread(void *arg) {
    if (!nif) for (;;) task_sleep(60000);
    uint64_t last_link = 0;
    for (;;) {
        nif->poll(nif);
        uint64_t now = uptime_ms();
        if (now - last_link > 500) {
            last_link = now;
            bool up = nif->link_up(nif);
            if (up && !nif->link) { nif->link = true; kprintf("[net] link up\n"); dh_state = DH_INIT; dh_next = 0; }
            if (!up && nif->link) {
                nif->link = false; configured = false; dh_state = DH_INIT;
                kprintf("[net] link down\n");
                desktop_notify("Network disconnected", "The network cable was unplugged.");
            }
            for (int i = 0; i < 8; i++) if (pend[i].used && now - pend[i].t > 3000) pend[i].used = false;
        }
        if (nif->link) dhcp_tick();
        tcp_timers();
        task_sleep(1);
    }
}

void net_get_info(struct dunc_netinfo *ni) {
    memset(ni, 0, sizeof *ni);
    if (!nif) return;
    ni->present = 1; ni->link = nif->link; ni->configured = configured; ni->dns_manual = dns_manual;
    strncpy0(ni->adapter, nif->adapter, sizeof ni->adapter);
    strncpy0(ni->driver, nif->driver, sizeof ni->driver);
    memcpy(ni->mac, nif->mac, 6);
    ni->ip = my_ip; ni->mask = my_mask; ni->gw = my_gw; ni->dns = my_dns; ni->lease_s = lease_s;
    ni->rx_packets = nif->rx_packets; ni->tx_packets = nif->tx_packets; ni->rx_bytes = nif->rx_bytes; ni->tx_bytes = nif->tx_bytes;
}

void net_set_dns(uint32_t ip) {
    memset(dcache, 0, sizeof dcache);
    if (ip) { my_dns = ip; dns_manual = true; }
    else { dns_manual = false; net_renew(); }      /* back to the DNS server from DHCP */
}
void net_renew(void) { if (nif) { dh_state = DH_INIT; dh_next = 0; } }
