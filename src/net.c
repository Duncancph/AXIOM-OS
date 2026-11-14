/* TCP/IP stack, IPv4 and IPv6.
 *   Ethernet, ARP, IPv4, ICMP, DHCP
 *   IPv6, ICMPv6, Neighbour Discovery, stateless address autoconfiguration (with duplicate address detection),
 *   router advertisements (prefixes, MTU, DNS servers), stateless DHCPv6 for DNS, MLDv2 reports
 *   UDP, TCP and DNS over both.
 * A kernel thread polls the network card; programs use it through system calls.
 * IPv4 addresses held as uint32_t are in network byte order. */
#include "net.h"

static struct netif *nif;
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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

/* TCP/UDP/ICMPv6 pseudo-header sum */
static uint32_t pseudo(const struct ipaddr *s, const struct ipaddr *d, uint8_t proto, uint32_t len) {
    uint32_t c = 0;
    if (s->v6) { c = csum_add(c, s->b, 16); c = csum_add(c, d->b, 16); c += len >> 16; c += len & 0xFFFF; c += proto; }
    else { c = csum_add(c, s->b, 4); c = csum_add(c, d->b, 4); c += proto; c += len; }
    return c;
}

static const char *ipstr(const struct ipaddr *a, char *buf) { ip_format(a, buf); return buf; }
static struct ipaddr ip6(const uint8_t *b) { struct ipaddr a; memset(&a, 0, sizeof a); a.v6 = 1; memcpy(a.b, b, 16); return a; }
static uint32_t rnd32(void) { uint32_t r; random_bytes(&r, 4); return r; }

/* ---------------- IPv4 state ---------------- */
static uint32_t my_ip, my_mask, my_gw, my_dns, lease_s;
static bool configured;
static uint16_t ip_id = 1;

/* ---------------- IPv6 state ---------------- */
#define NA6 4
struct a6 { bool used; uint8_t a[16], plen, state, dad_left; uint64_t valid_until, pref_until, dad_at; };
static struct a6 a6[NA6];                       /* a6[0]: link-local */
static uint8_t iid_g[8];                        /* random interface ID for global addresses (new each start-up) */
static int iid_regens;
struct pfx { bool used; uint8_t p[16], plen; uint64_t until; };
static struct pfx onlink[4];
static uint8_t rtr6[16]; static uint64_t rtr6_until;
static uint8_t dns6[16]; static uint64_t dns6_until;
static uint32_t mtu6 = 1500;
static int rs_left; static uint64_t rs_next;
static int d6_left; static uint8_t d6_xid[3]; static uint64_t d6_next;
static uint64_t mld_at;
static bool v6_announced;
#define FOREVER (~0ULL)

static const uint8_t ALL_NODES[16]   = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const uint8_t ALL_ROUTERS[16] = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
static const uint8_t MLDV2_ROUTERS[16] = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16 };
static const uint8_t DHCP6_SERVERS[16] = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2 };

/* the user's DNS choice */
static struct ipaddr dns_user; static bool dns_manual;

/* ---------------- Ethernet, neighbours (ARP and NDP) ---------------- */
struct nb { struct ipaddr ip; uint8_t mac[6]; bool valid; uint64_t t; };
static struct nb nb[32];
struct pend { bool used; struct ipaddr hop; uint16_t len; uint64_t t; uint8_t frame[1514]; };
static struct pend pend[8];

static void eth_send(const uint8_t *dst, uint16_t type, const void *payload, int len) {
    if (!nif || len > 1500) return;
    uint8_t f[1514];
    memcpy(f, dst, 6); memcpy(f + 6, nif->mac, 6); p16(f + 12, type);
    memcpy(f + 14, payload, len);
    nif->send(nif, f, 14 + len);
}

static struct nb *nb_find(const struct ipaddr *ip) {
    for (int i = 0; i < 32; i++) if (nb[i].valid && ip_eq(&nb[i].ip, ip)) return &nb[i];
    return 0;
}

static void nb_learn(const struct ipaddr *ip, const uint8_t *mac) {
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & 1)) return;
    struct nb *e = nb_find(ip);
    if (!e) { int old = 0; for (int i = 0; i < 32; i++) { if (!nb[i].valid) { old = i; break; } if (nb[i].t < nb[old].t) old = i; } e = &nb[old]; }
    e->ip = *ip; memcpy(e->mac, mac, 6); e->valid = true; e->t = uptime_ms();
    for (int i = 0; i < 8; i++)          /* send packets that were waiting for this neighbour */
        if (pend[i].used && ip_eq(&pend[i].hop, ip)) { memcpy(pend[i].frame, mac, 6); nif->send(nif, pend[i].frame, pend[i].len); pend[i].used = false; }
}

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip) {
    uint8_t a[28];
    p16(a, 1); p16(a + 2, 0x0800); a[4] = 6; a[5] = 4; p16(a + 6, op);
    memcpy(a + 8, nif->mac, 6); memcpy(a + 14, &my_ip, 4);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6); memcpy(a + 24, &tip, 4);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, a, 28);
}

static void arp_input(const uint8_t *a, int len) {
    if (len < 28 || g16(a) != 1 || g16(a + 2) != 0x0800) return;
    uint32_t sip, tip; memcpy(&sip, a + 14, 4); memcpy(&tip, a + 24, 4);
    if (configured && sip && (sip & my_mask) == (my_ip & my_mask)) { struct ipaddr s = ip_v4(sip); nb_learn(&s, a + 8); }
    if (g16(a + 6) == 1 && configured && tip == my_ip) arp_send(2, a + 8, sip);
}

static void ns_send(const uint8_t *target, bool dad);

/* send frame f (destination MAC still to fill in) to on-link neighbour hop, resolving its address first if needed */
static void send_via(const struct ipaddr *hop, uint8_t *f, int flen) {
    struct nb *e = nb_find(hop);
    if (e && uptime_ms() - e->t < 300000) { memcpy(f, e->mac, 6); nif->send(nif, f, flen); return; }
    int slot = -1;                                  /* not resolved yet: park the frame and ask */
    for (int i = 0; i < 8; i++) if (!pend[i].used) { slot = i; break; }
    if (slot < 0) { slot = 0; for (int i = 1; i < 8; i++) if (pend[i].t < pend[slot].t) slot = i; }
    pend[slot].used = true; pend[slot].hop = *hop; pend[slot].len = flen; pend[slot].t = uptime_ms(); memcpy(pend[slot].frame, f, flen);
    bool asked = false;                             /* one question per neighbour is enough */
    for (int i = 0; i < 8; i++) if (i != slot && pend[i].used && ip_eq(&pend[i].hop, hop) && uptime_ms() - pend[i].t < 1000) asked = true;
    if (asked) return;
    if (hop->v6) ns_send(hop->b, false); else arp_send(1, 0, ip_to_v4(hop));
}

/* ---------------- IPv4 ---------------- */
static int ip_send(uint32_t dst, uint8_t proto, const void *payload, int len) {
    if (!nif || len > 1480) return NET_EINVAL;
    uint8_t f[1514];
    uint8_t *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0; p16(ip + 2, 20 + len); p16(ip + 4, ip_id++); p16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto; p16(ip + 10, 0);
    memcpy(ip + 12, &my_ip, 4); memcpy(ip + 16, &dst, 4);
    p16(ip + 10, csum_fin(csum_add(0, ip, 20)));
    memcpy(ip + 20, payload, len);
    memcpy(f + 6, nif->mac, 6); p16(f + 12, 0x0800);
    int flen = 14 + 20 + len;
    if (dst == 0xFFFFFFFF || (configured && dst == (my_ip | ~my_mask))) { memcpy(f, BCAST, 6); nif->send(nif, f, flen); return 0; }
    uint32_t hop = (dst & my_mask) == (my_ip & my_mask) ? dst : my_gw;
    if (!hop) return NET_EUNREACH;
    struct ipaddr h = ip_v4(hop);
    send_via(&h, f, flen);
    return 0;
}

/* ---------------- IPv6 addresses and routing ---------------- */
static void solicited(const uint8_t *a, uint8_t *out) {
    static const uint8_t base[13] = { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF };
    memcpy(out, base, 13); memcpy(out + 13, a + 13, 3);
}

/* our address a: with tentative=false only addresses that are ready to use */
static struct a6 *a6_find(const uint8_t *a, bool tentative) {
    for (int i = 0; i < NA6; i++) {
        struct a6 *x = &a6[i];
        if (!x->used || memcmp(x->a, a, 16)) continue;
        if (x->state == V6_PREFERRED || x->state == V6_DEPRECATED || (tentative && x->state == V6_TENTATIVE)) return x;
    }
    return 0;
}

static bool for_us6(const uint8_t *dst) {
    if (!memcmp(dst, ALL_NODES, 16)) return true;
    if (a6_find(dst, false)) return true;
    uint8_t sn[16];
    for (int i = 0; i < NA6; i++) if (a6[i].used && a6[i].state != V6_DUPLICATE) { solicited(a6[i].a, sn); if (!memcmp(sn, dst, 16)) return true; }
    return false;
}

static bool v6_global(void) {
    for (int i = 1; i < NA6; i++) if (a6[i].used && a6[i].state == V6_PREFERRED) return true;
    return false;
}
static bool v6_ready(void) { return v6_global() && rtr6_until > uptime_ms(); }

static bool onlink6(const uint8_t *dst) {
    if (ip6_is_linklocal(dst) || ip6_is_multicast(dst)) return true;
    uint64_t now = uptime_ms();
    for (int i = 0; i < 4; i++) if (onlink[i].used && onlink[i].until > now && ip_same_prefix(onlink[i].p, dst, onlink[i].plen)) return true;
    return false;
}

/* pick our source address for dst (a simplified RFC 6724: same scope, then longest matching prefix, preferred first) */
static bool src6_for(const uint8_t *dst, uint8_t *out) {
    if (ip6_is_linklocal(dst) || (ip6_is_multicast(dst) && (dst[1] & 15) <= 2)) {
        if (a6[0].used && a6[0].state == V6_PREFERRED) { memcpy(out, a6[0].a, 16); return true; }
        return false;
    }
    int best = -1, bestscore = -1;
    for (int i = 1; i < NA6; i++) {
        struct a6 *x = &a6[i];
        if (!x->used || (x->state != V6_PREFERRED && x->state != V6_DEPRECATED)) continue;
        int common = 0; while (common < 128 && ip_same_prefix(x->a, dst, common + 1)) common++;
        int score = (x->state == V6_PREFERRED ? 1000 : 0) + common;
        if (score > bestscore) { bestscore = score; best = i; }
    }
    if (best < 0) return false;
    memcpy(out, a6[best].a, 16);
    return true;
}

static bool route6_ok(const uint8_t *dst) { return onlink6(dst) || rtr6_until > uptime_ms(); }

/* send an IPv6 packet. hbh_ra: add a hop-by-hop Router Alert option (for MLD). */
static int ip6_out(const uint8_t *src, const uint8_t *dst, uint8_t nh, const void *payload, int len, uint8_t hlim, bool hbh_ra) {
    if (!nif) return NET_ENODEV;
    int ext = hbh_ra ? 8 : 0;
    if (40 + ext + len > (int)mtu6 || 40 + ext + len > 1500) return NET_EINVAL;
    uint8_t f[1514], *ip = f + 14;
    p32(ip, 6u << 28); p16(ip + 4, (uint16_t)(ext + len)); ip[6] = hbh_ra ? 0 : nh; ip[7] = hlim;
    memcpy(ip + 8, src, 16); memcpy(ip + 24, dst, 16);
    if (hbh_ra) { uint8_t h[8] = { nh, 0, 5, 2, 0, 0, 1, 0 }; memcpy(ip + 40, h, 8); }
    memcpy(ip + 40 + ext, payload, len);
    memcpy(f + 6, nif->mac, 6); p16(f + 12, 0x86DD);
    int flen = 14 + 40 + ext + len;
    if (ip6_is_multicast(dst)) { f[0] = 0x33; f[1] = 0x33; memcpy(f + 2, dst + 12, 4); nif->send(nif, f, flen); return 0; }
    struct ipaddr hop;
    if (onlink6(dst)) hop = ip6(dst);
    else if (rtr6_until > uptime_ms()) hop = ip6(rtr6);
    else return NET_EUNREACH;
    send_via(&hop, f, flen);
    return 0;
}

static void icmp6_send(const uint8_t *src, const uint8_t *dst, uint8_t *m, int len, uint8_t hlim) {
    struct ipaddr s = ip6(src), d = ip6(dst);
    p16(m + 2, 0); p16(m + 2, csum_fin(csum_add(pseudo(&s, &d, 58, len), m, len)));
    ip6_out(src, dst, 58, m, len, hlim, false);
}

/* ---------------- Neighbour Discovery (RFC 4861), DAD (RFC 4862), MLDv2 (RFC 3810) ---------------- */
static void ns_send(const uint8_t *target, bool dad) {
    uint8_t m[32], dst[16], src[16]; int len = 24;
    memset(m, 0, sizeof m); m[0] = 135; memcpy(m + 8, target, 16);
    solicited(target, dst);
    if (dad) memset(src, 0, 16);
    else {
        if (!src6_for(target, src)) {
            if (!a6[0].used || a6[0].state != V6_PREFERRED) return;
            memcpy(src, a6[0].a, 16);
        }
        m[24] = 1; m[25] = 1; memcpy(m + 26, nif->mac, 6); len = 32;      /* our link-layer address */
    }
    icmp6_send(src, dst, m, len, 255);
}

static void na_send(const uint8_t *target, const uint8_t *dst, bool solicited_flag) {
    uint8_t m[32]; memset(m, 0, sizeof m);
    m[0] = 136; m[4] = (solicited_flag ? 0x40 : 0) | 0x20;                /* S, O */
    memcpy(m + 8, target, 16); m[24] = 2; m[25] = 1; memcpy(m + 26, nif->mac, 6);
    icmp6_send(target, dst, m, 32, 255);
}

static void rs_send(void) {
    uint8_t m[16]; memset(m, 0, sizeof m); m[0] = 133;
    m[8] = 1; m[9] = 1; memcpy(m + 10, nif->mac, 6);
    icmp6_send(a6[0].a, ALL_ROUTERS, m, 16, 255);
}

static void mld_report(void) {
    uint8_t m[8 + 20 * NA6]; memset(m, 0, sizeof m);
    m[0] = 143; int n = 0;
    for (int i = 0; i < NA6; i++) {
        if (!a6[i].used || a6[i].state == V6_DUPLICATE) continue;
        uint8_t *r = m + 8 + 20 * n++;
        r[0] = 4; solicited(a6[i].a, r + 4);                             /* CHANGE_TO_EXCLUDE, no sources: "listening" */
    }
    if (!n) return;
    p16(m + 6, (uint16_t)n);
    int len = 8 + 20 * n;
    uint8_t src[16]; memset(src, 0, 16);
    if (a6[0].used && a6[0].state == V6_PREFERRED) memcpy(src, a6[0].a, 16);
    struct ipaddr s = ip6(src), d = ip6(MLDV2_ROUTERS);
    p16(m + 2, csum_fin(csum_add(pseudo(&s, &d, 58, len), m, len)));
    ip6_out(src, MLDV2_ROUTERS, 58, m, len, 1, true);
}

static void a6_add(int slot, const uint8_t *a, int plen, uint64_t valid_until, uint64_t pref_until) {
    struct a6 *x = &a6[slot];
    memset(x, 0, sizeof *x);
    x->used = true; memcpy(x->a, a, 16); x->plen = (uint8_t)plen;
    x->state = V6_TENTATIVE; x->valid_until = valid_until; x->pref_until = pref_until;
    x->dad_left = 1; x->dad_at = uptime_ms() + 50 + rnd32() % 400;      /* join the group first, then probe */
    mld_report();
}

static void dad_failed(struct a6 *x) {
    char b[IP_STRLEN]; struct ipaddr a = ip6(x->a);
    kprintf("[net6] address %s is already used by another device\n", ipstr(&a, b));
    x->state = V6_DUPLICATE;
    if (x == &a6[0]) { desktop_notify("IPv6 turned off", "Another device on the network uses the same IPv6 address."); return; }
    x->used = false;                                                    /* try again with a new random interface ID */
    if (iid_regens++ < 3) { random_bytes(iid_g, 8); rs_left = 1; rs_next = uptime_ms(); }
}

static void v6_start(void) {
    memset(a6, 0, sizeof a6); memset(onlink, 0, sizeof onlink);
    rtr6_until = 0; dns6_until = 0; mtu6 = 1500; d6_left = 0; rs_left = 0; v6_announced = false; iid_regens = 0;
    random_bytes(iid_g, 8);
    uint8_t ll[16] = { 0xFE, 0x80 };                                   /* link-local: fe80:: + modified EUI-64 from the MAC */
    ll[8] = nif->mac[0] ^ 2; ll[9] = nif->mac[1]; ll[10] = nif->mac[2]; ll[11] = 0xFF; ll[12] = 0xFE;
    ll[13] = nif->mac[3]; ll[14] = nif->mac[4]; ll[15] = nif->mac[5];
    a6_add(0, ll, 64, FOREVER, FOREVER);
}

static void v6_stop(void) { memset(a6, 0, sizeof a6); rtr6_until = 0; dns6_until = 0; rs_left = 0; d6_left = 0; }

static void d6_start(void);

static void ra_input(const uint8_t *src, const uint8_t *p, int len) {
    if (len < 16 || !ip6_is_linklocal(src)) return;
    uint64_t now = uptime_ms();
    uint8_t flags = p[5]; uint16_t life = g16(p + 6);
    bool got_dns = false;
    for (int o = 16; o + 2 <= len;) {
        const uint8_t *q = p + o; int ol = q[1] * 8;
        if (!ol || o + ol > len) break;
        if (q[0] == 1 && ol >= 8) { struct ipaddr s = ip6(src); nb_learn(&s, q + 2); }
        if (q[0] == 5 && ol >= 8) { uint32_t m = g32(q + 4); if (m >= 1280 && m <= 1500) mtu6 = m; }
        if (q[0] == 3 && ol >= 32) {                                    /* prefix information */
            uint8_t plen = q[2], pf = q[3]; uint32_t valid = g32(q + 4), pref = g32(q + 8); const uint8_t *pfx = q + 16;
            if (ip6_is_linklocal(pfx) || ip6_is_multicast(pfx) || plen > 128 || pref > valid) { o += ol; continue; }
            uint64_t vu = valid == 0xFFFFFFFF ? FOREVER : now + valid * 1000ULL, pu = pref == 0xFFFFFFFF ? FOREVER : now + pref * 1000ULL;
            if (pf & 0x80) {                                            /* on-link */
                int s = -1;
                for (int i = 0; i < 4; i++) if (onlink[i].used && onlink[i].plen == plen && ip_same_prefix(onlink[i].p, pfx, plen)) s = i;
                if (s < 0) for (int i = 0; i < 4; i++) if (!onlink[i].used || onlink[i].until <= now) { s = i; break; }
                if (s >= 0) { onlink[s].used = true; memcpy(onlink[s].p, pfx, 16); onlink[s].plen = plen; onlink[s].until = vu; }
            }
            if ((pf & 0x40) && plen == 64 && valid) {                   /* autonomous: make an address from it */
                uint8_t a[16]; memcpy(a, pfx, 8); memcpy(a + 8, iid_g, 8);
                struct a6 *x = 0;
                for (int i = 1; i < NA6; i++) if (a6[i].used && ip_same_prefix(a6[i].a, a, 64)) x = &a6[i];
                if (x) {                                                /* refresh lifetimes (RFC 4862 5.5.3 e) */
                    uint64_t left = x->valid_until > now ? x->valid_until - now : 0;
                    if (valid > 7200 || valid * 1000ULL > left) x->valid_until = vu;
                    else if (left > 7200000) x->valid_until = now + 7200000;
                    x->pref_until = pu;
                    if (x->state == V6_DEPRECATED && pref) x->state = V6_PREFERRED;
                } else {
                    for (int i = 1; i < NA6; i++) if (!a6[i].used) { a6_add(i, a, 64, vu, pu); break; }
                }
            }
        }
        if (q[0] == 25 && ol >= 24) {                                   /* RDNSS (RFC 8106) */
            uint32_t l = g32(q + 4);
            if (l) { memcpy(dns6, q + 8, 16); dns6_until = l == 0xFFFFFFFF ? FOREVER : now + l * 1000ULL; got_dns = true; }
            else if (!memcmp(dns6, q + 8, 16)) dns6_until = 0;
        }
        o += ol;
    }
    if (life) { if (rtr6_until <= now || !memcmp(rtr6, src, 16)) { memcpy(rtr6, src, 16); rtr6_until = now + life * 1000ULL; } }
    else if (!memcmp(rtr6, src, 16)) rtr6_until = 0;
    rs_left = 0;
    if ((flags & 0xC0) && !got_dns && dns6_until <= now && !d6_left) d6_start();   /* "ask DHCPv6 for other configuration" */
}

static void ns_input(const uint8_t *src, const uint8_t *p, int len) {
    if (len < 24) return;
    const uint8_t *target = p + 8;
    if (ip6_is_multicast(target)) return;
    bool from_unspec = ip6_is_unspec(src);
    for (int i = 0; i < NA6; i++) {
        struct a6 *x = &a6[i];
        if (!x->used || memcmp(x->a, target, 16)) continue;
        if (x->state == V6_TENTATIVE) { if (from_unspec) dad_failed(x); return; }   /* someone else wants it too */
        if (x->state == V6_DUPLICATE) return;
        if (from_unspec) { na_send(target, ALL_NODES, false); return; }
        for (int o = 24; o + 8 <= len; o += p[o + 1] * 8) {
            if (!p[o + 1]) break;
            if (p[o] == 1) { struct ipaddr s = ip6(src); nb_learn(&s, p + o + 2); }
        }
        na_send(target, src, true);
        return;
    }
}

static void na_input(const uint8_t *p, int len) {
    if (len < 24) return;
    const uint8_t *target = p + 8;
    for (int i = 0; i < NA6; i++)
        if (a6[i].used && !memcmp(a6[i].a, target, 16) && a6[i].state == V6_TENTATIVE) { dad_failed(&a6[i]); return; }
    if (a6_find(target, false)) return;
    for (int o = 24; o + 8 <= len; o += p[o + 1] * 8) {
        if (!p[o + 1]) break;
        if (p[o] == 2) { struct ipaddr t = ip6(target); nb_learn(&t, p + o + 2); }
    }
}

static void v6_tick(void) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < NA6; i++) {
        struct a6 *x = &a6[i];
        if (!x->used) continue;
        if (x->state == V6_TENTATIVE && now >= x->dad_at) {
            if (x->dad_left) { x->dad_left--; ns_send(x->a, true); x->dad_at = now + 1000; }
            else {
                x->state = V6_PREFERRED;
                char b[IP_STRLEN]; struct ipaddr a = ip6(x->a);
                kprintf("[net6] address %s/%u ready\n", ipstr(&a, b), x->plen);
                if (i == 0) { rs_left = 3; rs_next = now + rnd32() % 1000; mld_report(); }
            }
        }
        if (i && x->valid_until <= now) { x->used = false; continue; }
        if (i && x->state == V6_PREFERRED && x->pref_until <= now) x->state = V6_DEPRECATED;
    }
    if (rs_left && now >= rs_next && a6[0].state == V6_PREFERRED) { rs_send(); rs_left--; rs_next = now + 4000; }
    if (d6_left && now >= d6_next) d6_start();
    if (mld_at && now >= mld_at) { mld_at = 0; mld_report(); }
    if (!v6_announced && v6_ready()) {
        v6_announced = true;
        uint8_t s[16]; char b[IP_STRLEN];
        for (int i = 1; i < NA6; i++) if (a6[i].used && a6[i].state == V6_PREFERRED) { memcpy(s, a6[i].a, 16); break; }
        struct ipaddr a = ip6(s); ipstr(&a, b);
        kprintf("[net6] IPv6 ready: %s\n", b);
        if (!configured) { char m[120]; ksnprintf(m, sizeof m, "Your IPv6 address is %s.", b); desktop_notify("Connected to the network (IPv6)", m); }
    }
    if (v6_announced && !v6_ready()) v6_announced = false;
}

/* ---------------- ICMP (v4) ---------------- */
static volatile uint16_t ping_id, ping_seq; static volatile bool ping_got; static uint64_t ping_t0, ping_rtt;
static void tcp_unreach(const struct ipaddr *dst, uint16_t lport, uint16_t rport);
static void tcp_pmtu(const struct ipaddr *dst, uint32_t mtu);

static void icmp_input(uint32_t src, const uint8_t *p, int len) {
    if (len < 8 || csum_fin(csum_add(0, p, len))) return;
    if (p[0] == 8) {                                  /* echo request: answer it */
        uint8_t r[1480]; if (len > 1480) return;
        memcpy(r, p, len); r[0] = 0; p16(r + 2, 0); p16(r + 2, csum_fin(csum_add(0, r, len)));
        ip_send(src, 1, r, len);
    } else if (p[0] == 0 && g16(p + 4) == ping_id && g16(p + 6) == ping_seq) {
        ping_rtt = (rdtsc() - ping_t0) / (tsc_hz() / 1000000); ping_got = true;
    } else if (p[0] == 3 && len >= 8 + 20 + 4) {      /* destination unreachable: about one of our connections? */
        const uint8_t *in = p + 8; int ihl = (in[0] & 15) * 4;
        if (in[9] == 6 && len >= 8 + ihl + 4) {
            uint32_t d; memcpy(&d, in + 16, 4); struct ipaddr da = ip_v4(d);
            if (p[1] == 4) tcp_pmtu(&da, g16(p + 6));
            else tcp_unreach(&da, g16(in + ihl), g16(in + ihl + 2));
        }
    }
}

/* ---------------- UDP ---------------- */
struct udp_wait { bool active; uint16_t port; volatile int len; uint8_t buf[1232]; };
static struct udp_wait uwait[4];

static int udp_send(const struct ipaddr *src, const struct ipaddr *dst, uint16_t sport, uint16_t dport, const void *data, int len) {
    uint8_t u[1480];
    if (len > 1440) return NET_EINVAL;
    p16(u, sport); p16(u + 2, dport); p16(u + 4, 8 + len); p16(u + 6, 0);
    memcpy(u + 8, data, len);
    uint16_t c = csum_fin(csum_add(pseudo(src, dst, 17, 8 + len), u, 8 + len));
    p16(u + 6, c ? c : 0xFFFF);
    if (dst->v6) return ip6_out(src->b, dst->b, 17, u, 8 + len, ip6_is_multicast(dst->b) ? 1 : 64, false);
    return ip_send(ip_to_v4(dst), 17, u, 8 + len);
}

static void dhcp_input(const uint8_t *p, int len);
static void d6_input(const uint8_t *p, int len);
static void udp_input(const struct ipaddr *src, const struct ipaddr *dst, const uint8_t *p, int len) {
    if (len < 8) return;
    uint16_t dport = g16(p + 2), ulen = g16(p + 4);
    if (ulen < 8 || ulen > len) return;
    if (src->v6 && csum_fin(csum_add(pseudo(src, dst, 17, ulen), p, ulen))) return;   /* mandatory in IPv6 */
    if (!src->v6 && dport == 68) { dhcp_input(p + 8, ulen - 8); return; }
    if (src->v6 && dport == 546) { d6_input(p + 8, ulen - 8); return; }
    for (int i = 0; i < 4; i++)
        if (uwait[i].active && uwait[i].port == dport && uwait[i].len < 0) {
            int n = ulen - 8 < (int)sizeof uwait[i].buf ? ulen - 8 : (int)sizeof uwait[i].buf;
            memcpy(uwait[i].buf, p + 8, n); uwait[i].len = n;
        }
}

/* ---------------- DHCP (v4) ---------------- */
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
    struct ipaddr s = ip_v4(0), b = ip_v4(0xFFFFFFFF);
    udp_send(&s, &b, 68, 67, d, o < 300 ? 300 : o);
    my_ip = save;
}

static void dhcp_start(void) {
    dh_xid = rnd32();
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
        my_dns = dns ? dns : gw;
        lease_s = lease; configured = true; dh_state = DH_BOUND;
        dh_renew_at = uptime_ms() + (uint64_t)(lease > 120 ? lease / 2 : 60) * 1000;
        char a[IP_STRLEN], g[IP_STRLEN]; struct ipaddr ia = ip_v4(my_ip), ig = ip_v4(my_gw);
        kprintf("[net] DHCP: address %s, gateway %s, lease %u s\n", ipstr(&ia, a), ipstr(&ig, g), lease);
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

/* ---------------- stateless DHCPv6 (RFC 8415 Information-request), for DNS servers ---------------- */
static void d6_start(void) {
    if (!a6[0].used || a6[0].state != V6_PREFERRED) { d6_left = 0; return; }
    if (!d6_left) { d6_left = 4; random_bytes(d6_xid, 3); }
    uint8_t m[40]; int o = 0;
    m[o++] = 11; memcpy(m + o, d6_xid, 3); o += 3;
    p16(m + o, 1); p16(m + o + 2, 10); p16(m + o + 4, 3); p16(m + o + 6, 1); memcpy(m + o + 8, nif->mac, 6); o += 14;  /* client ID: DUID-LL */
    p16(m + o, 8); p16(m + o + 2, 2); p16(m + o + 4, 0); o += 6;                                                   /* elapsed time */
    p16(m + o, 6); p16(m + o + 2, 2); p16(m + o + 4, 23); o += 6;                                                  /* please send: DNS servers */
    struct ipaddr s = ip6(a6[0].a), d = ip6(DHCP6_SERVERS);
    udp_send(&s, &d, 546, 547, m, o);
    d6_left--;
    d6_next = uptime_ms() + (uint64_t)(1000 << (3 - (d6_left < 3 ? d6_left : 3)));
}

static void d6_input(const uint8_t *m, int len) {
    if (len < 4 || m[0] != 7 || memcmp(m + 1, d6_xid, 3)) return;
    for (int o = 4; o + 4 <= len;) {
        uint16_t code = g16(m + o), l = g16(m + o + 2);
        if (o + 4 + l > len) break;
        if (code == 23 && l >= 16) {
            memcpy(dns6, m + o + 4, 16); dns6_until = uptime_ms() + 86400000ULL;
            char b[IP_STRLEN]; struct ipaddr a = ip6(dns6);
            kprintf("[net6] DHCPv6: DNS server %s\n", ipstr(&a, b));
        }
        o += 4 + l;
    }
    d6_left = 0;
}

/* ---------------- DNS ---------------- */
struct dns_cache { char name[64]; int n; struct ipaddr a[4]; uint64_t until; };
static struct dns_cache dcache[16];

static int skip_name(const uint8_t *m, int len, int o) {
    while (o < len) {
        if (m[o] == 0) return o + 1;
        if ((m[o] & 0xC0) == 0xC0) return o + 2;
        o += m[o] + 1;
    }
    return -1;
}

/* the DNS server lookups use now */
static bool dns_server(struct ipaddr *out) {
    if (dns_manual) { *out = dns_user; return true; }
    if (configured && my_dns) { *out = ip_v4(my_dns); return true; }
    if (dns6_until > uptime_ms()) { *out = ip6(dns6); return true; }
    return false;
}

/* one question (qtype 1 = A, 28 = AAAA): number of addresses, or an error */
static int dns_query(const struct ipaddr *srv, int w, uint16_t port, const char *name, uint16_t qtype,
                     struct ipaddr *out, int max, uint32_t *ttl_out) {
    struct ipaddr src;
    if (srv->v6) { if (!src6_for(srv->b, src.b)) return NET_ENOTCONN; src.v6 = 1; src.prefix = 0; }
    else { if (!configured) return NET_ENOTCONN; src = ip_v4(my_ip); }
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t q[300]; uint16_t id = (uint16_t)rnd32();
        p16(q, id); p16(q + 2, 0x0100); p16(q + 4, 1); p16(q + 6, 0); p16(q + 8, 0); p16(q + 10, 0);
        int o = 12;
        for (const char *s = name; *s;) {
            const char *dot = s; while (*dot && *dot != '.') dot++;
            int l = dot - s; if (l == 0 || l > 63) return NET_EINVAL;
            q[o++] = l; memcpy(q + o, s, l); o += l;
            s = *dot ? dot + 1 : dot;
        }
        q[o++] = 0; p16(q + o, qtype); p16(q + o + 2, 1); o += 4;
        uwait[w].len = -1;
        if (udp_send(&src, srv, port, 53, q, o) == NET_EUNREACH) return NET_EUNREACH;
        uint64_t end = uptime_ms() + 1500;
        while (uwait[w].len < 0 && uptime_ms() < end) task_sleep(1);
        int len = uwait[w].len;
        if (len < 12) continue;
        const uint8_t *m = uwait[w].buf;
        if (g16(m) != id) continue;
        if ((m[3] & 15) == 3) return NET_ENOTFOUND;                     /* no such name */
        int an = g16(m + 6), off = skip_name(m, len, 12);
        if (off < 0) continue;
        off += 4;
        int n = 0; *ttl_out = 3600;
        for (int i = 0; i < an && off > 0 && off + 10 <= len; i++) {
            off = skip_name(m, len, off);
            if (off < 0 || off + 10 > len) break;
            uint16_t type = g16(m + off), rdl = g16(m + off + 8); uint32_t ttl = g32(m + off + 4);
            off += 10;
            if (off + rdl > len) break;
            if (type == qtype && n < max && ((qtype == 1 && rdl == 4) || (qtype == 28 && rdl == 16))) {
                memset(&out[n], 0, sizeof out[n]);
                out[n].v6 = qtype == 28; memcpy(out[n].b, m + off, rdl); n++;
                if (ttl < *ttl_out) *ttl_out = ttl;
            }
            off += rdl;
        }
        return n;
    }
    return NET_ETIMEDOUT;
}

/* resolve a name or address literal to up to max addresses, best first; returns the count or an error */
int net_resolve(const char *name, struct ipaddr *out, int max) {
    if (max < 1) return NET_EINVAL;
    if (ip_parse(name, &out[0])) return 1;
    if (!nif) return NET_ENODEV;
    int nl = strlen(name);
    if (!nl || nl > 63) return NET_EINVAL;
    for (int i = 0; i < 16; i++)
        if (dcache[i].until > uptime_ms() && !strcmp(dcache[i].name, name)) {
            int n = dcache[i].n < max ? dcache[i].n : max;
            memcpy(out, dcache[i].a, n * sizeof *out);
            return n;
        }
    struct ipaddr srv;
    bool want6 = v6_ready(), want4 = configured;
    if (!(want4 || want6) || !dns_server(&srv)) return NET_ENOTCONN;
    int w = -1;
    for (int i = 0; i < 4; i++) if (!uwait[i].active) { w = i; break; }
    if (w < 0) return NET_EAGAIN;
    static uint16_t next_port = 53000;
    uint16_t port = next_port++; if (next_port > 53999) next_port = 53000;
    uwait[w].active = true; uwait[w].port = port;
    struct ipaddr got[8]; int n = 0, err = 0; uint32_t ttl = 3600, t;
    /* IPv6 first when we have it (RFC 6724); the caller falls back to the IPv4 addresses */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 ? !want6 : !want4) continue;
        int r = dns_query(&srv, w, port, name, pass == 0 ? 28 : 1, got + n, 4, &t);
        if (r > 0) { n += r; if (t < ttl) ttl = t; }
        else if (r == NET_ENOTFOUND) { err = r; break; }                /* the name doesn't exist at all */
        else if (r < 0 && !err) err = r;
    }
    uwait[w].active = false;
    if (!n) return err ? err : NET_ENOTFOUND;                         /* no error, no addresses: no record of this type */
    int slot = 0; for (int k = 0; k < 16; k++) if (dcache[k].until < dcache[slot].until) slot = k;
    strncpy0(dcache[slot].name, name, 64);
    dcache[slot].n = n < 4 ? n : 4; memcpy(dcache[slot].a, got, dcache[slot].n * sizeof got[0]);
    dcache[slot].until = uptime_ms() + (ttl > 3600 ? 3600 : ttl < 30 ? 30 : ttl) * 1000ULL;
    if (n > max) n = max;
    memcpy(out, got, n * sizeof *out);
    return n;
}

/* ---------------- ICMPv6 ---------------- */
static volatile bool ping6; static uint8_t ping6_dst[16];

static void icmp6_input(const uint8_t *src, const uint8_t *dst, const uint8_t *p, int len, uint8_t hlim) {
    if (len < 4) return;
    struct ipaddr s = ip6(src), d = ip6(dst);
    if (csum_fin(csum_add(pseudo(&s, &d, 58, len), p, len))) return;
    switch (p[0]) {
    case 128: {                                       /* echo request */
        if (len > 1400 || ip6_is_unspec(src)) return;
        uint8_t r[1400], from[16];
        if (a6_find(dst, false)) memcpy(from, dst, 16); else if (!src6_for(src, from)) return;
        memcpy(r, p, len); r[0] = 129;
        icmp6_send(from, src, r, len, 64);
        break;
    }
    case 129:
        if (len >= 8 && ping6 && g16(p + 4) == ping_id && g16(p + 6) == ping_seq && !memcmp(src, ping6_dst, 16)) {
            ping_rtt = (rdtsc() - ping_t0) / (tsc_hz() / 1000000); ping_got = true;
        }
        break;
    case 1:                                           /* destination unreachable */
        if (len >= 8 + 40 + 4 && p[8 + 6] == 6) { struct ipaddr da = ip6(p + 8 + 24); tcp_unreach(&da, g16(p + 48), g16(p + 50)); }
        break;
    case 2:                                           /* packet too big */
        if (len >= 8 + 40) { struct ipaddr da = ip6(p + 8 + 24); tcp_pmtu(&da, g32(p + 4)); }
        break;
    case 130: if (!mld_at) mld_at = uptime_ms() + rnd32() % 1000; break;   /* MLD query: report soon */
    case 134: if (hlim == 255) ra_input(src, p, len); break;
    case 135: if (hlim == 255) ns_input(src, p, len); break;
    case 136: if (hlim == 255) na_input(p, len); break;
    }
}

/* ---------------- ping ---------------- */
long net_ping(const struct ipaddr *ip, uint32_t timeout_ms) {
    if (!nif) return NET_ENODEV;
    uint8_t p[40]; memset(p, 0, sizeof p);
    ping_id = 0xD0C5; ping_seq++; ping_got = false;
    p[0] = ip->v6 ? 128 : 8; p16(p + 4, ping_id); p16(p + 6, ping_seq);
    memcpy(p + 8, "DuncOS ping, Thane kernel.....", 32);
    if (ip->v6) {
        uint8_t src[16];
        if (!src6_for(ip->b, src)) return a6[0].state == V6_PREFERRED ? NET_EUNREACH : NET_ENOTCONN;
        if (!route6_ok(ip->b)) return NET_EUNREACH;
        memcpy(ping6_dst, ip->b, 16); ping6 = true;
        ping_t0 = rdtsc();
        icmp6_send(src, ip->b, p, 40, 64);
    } else {
        if (!configured) return NET_ENOTCONN;
        ping6 = false;
        p16(p + 2, csum_fin(csum_add(0, p, 40)));
        ping_t0 = rdtsc();
        if (ip_send(ip_to_v4(ip), 1, p, 40) < 0) return NET_EUNREACH;
    }
    uint64_t end = uptime_ms() + timeout_ms;
    while (!ping_got && uptime_ms() < end) task_sleep(1);
    return ping_got ? (long)ping_rtt : NET_ETIMEDOUT;
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
    struct ipaddr rip, lip; uint16_t rport, lport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint16_t mss;
    uint8_t *rbuf; uint32_t rhead, rlen;     /* received, not yet read */
    uint8_t *sbuf; uint32_t slen;            /* sbuf[0] is the byte at snd_una */
    bool fin_sent, fin_rcvd, closing;
    uint32_t rto; int retries; uint64_t rto_at, time_wait_until;
    uint32_t last_adv;
};
static struct tcb tcbs[TCP_MAX];
static uint16_t next_lport;

static uint32_t rwin(struct tcb *t) { uint32_t f = RBUF - t->rlen; return f > 65535 ? 65535 : f; }

static void tcp_raw(const struct ipaddr *src, const struct ipaddr *dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    int flags, uint16_t win, const uint8_t *data, int len, uint16_t mss_opt) {
    uint8_t s[1480];
    int hl = mss_opt ? 24 : 20;
    if (hl + len > 1480) return;
    p16(s, sport); p16(s + 2, dport); p32(s + 4, seq); p32(s + 8, ack);
    s[12] = (hl / 4) << 4; s[13] = flags; p16(s + 14, win); p16(s + 16, 0); p16(s + 18, 0);
    if (mss_opt) { s[20] = 2; s[21] = 4; p16(s + 22, mss_opt); }
    if (len) memcpy(s + hl, data, len);
    p16(s + 16, csum_fin(csum_add(pseudo(src, dst, 6, hl + len), s, hl + len)));
    if (dst->v6) ip6_out(src->b, dst->b, 6, s, hl + len, 64, false);
    else ip_send(ip_to_v4(dst), 6, s, hl + len);
}

static void tcp_seg(struct tcb *t, int flags, uint32_t seq, const uint8_t *data, int len) {
    t->last_adv = rwin(t);
    uint16_t mss = (flags & F_SYN) ? (uint16_t)(t->rip.v6 ? mtu6 - 60 : 1460) : 0;
    tcp_raw(&t->lip, &t->rip, t->lport, t->rport, seq, t->rcv_nxt, flags, (uint16_t)t->last_adv, data, len, mss);
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

static struct tcb *tcb_find(const struct ipaddr *rip, uint16_t rport, uint16_t lport) {
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state != T_FREE && t->rport == rport && t->lport == lport && ip_eq(&t->rip, rip)) return t;
    }
    return 0;
}

static void tcp_free(struct tcb *t) {
    kfree(t->rbuf); kfree(t->sbuf);
    t->rbuf = t->sbuf = 0;
    t->state = T_FREE; t->gen++;
}

/* an ICMP error said the far end can't be reached: give up on a connection that is still being set up */
static void tcp_unreach(const struct ipaddr *dst, uint16_t lport, uint16_t rport) {
    struct tcb *t = tcb_find(dst, rport, lport);
    if (t && t->state == T_SYN_SENT) { t->err = NET_EUNREACH; t->state = T_CLOSED; }
}

/* path MTU discovery: a router said our packets are too big */
static void tcp_pmtu(const struct ipaddr *dst, uint32_t mtu) {
    uint32_t hdr = dst->v6 ? 60 : 40, minmtu = dst->v6 ? 1280 : 576;
    if (mtu < minmtu) mtu = minmtu;
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state == T_FREE || !ip_eq(&t->rip, dst) || t->mss <= mtu - hdr) continue;
        t->mss = (uint16_t)(mtu - hdr);
        if (t->snd_nxt != t->snd_una) t->rto_at = uptime_ms();       /* resend in smaller pieces now */
    }
}

static void tcp_input(const struct ipaddr *src, const struct ipaddr *dst, const uint8_t *s, int len) {
    if (len < 20) return;
    if (csum_fin(csum_add(pseudo(src, dst, 6, len), s, len))) return;
    uint16_t sport = g16(s), dport = g16(s + 2);
    uint32_t seq = g32(s + 4), ack = g32(s + 8);
    int hl = (s[12] >> 4) * 4, flags = s[13];
    uint16_t win = g16(s + 14);
    if (hl < 20 || hl > len) return;
    const uint8_t *data = s + hl; int dlen = len - hl;
    struct tcb *t = tcb_find(src, sport, dport);
    if (!t) {                                       /* nobody listening: reset */
        if (!(flags & F_RST)) {
            if (flags & F_ACK) tcp_raw(dst, src, dport, sport, ack, 0, F_RST, 0, 0, 0, 0);
            else tcp_raw(dst, src, dport, sport, 0, seq + dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), F_RST | F_ACK, 0, 0, 0, 0);
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
            if (k == 2 && s[o + 1] == 4) { uint16_t m = g16(s + o + 2); if (m < t->mss && m >= 64) t->mss = m; }
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

int tcp_connect(const struct ipaddr *ip, uint16_t port, uint32_t timeout_ms) {
    if (!nif) return NET_ENODEV;
    struct ipaddr lip; memset(&lip, 0, sizeof lip);
    if (ip->v6) {
        if (!a6[0].used || a6[0].state != V6_PREFERRED) return NET_ENOTCONN;
        if (!src6_for(ip->b, lip.b) || !route6_ok(ip->b)) return NET_EUNREACH;
        lip.v6 = 1;
    } else {
        if (!configured) return NET_ENOTCONN;
        lip = ip_v4(my_ip);
    }
    if (!timeout_ms || timeout_ms > 60000) timeout_ms = 15000;
    struct tcb *t = 0; int idx = -1;
    for (int i = 0; i < TCP_MAX; i++) if (tcbs[i].state == T_FREE) { t = &tcbs[i]; idx = i; break; }
    if (!t) return NET_ENOMEM;
    int gen = t->gen;
    memset(t, 0, sizeof *t);
    t->gen = gen;
    t->rbuf = kmalloc(RBUF); t->sbuf = kmalloc(SBUF);
    if (!t->rbuf || !t->sbuf) { kfree(t->rbuf); kfree(t->sbuf); t->rbuf = t->sbuf = 0; return NET_ENOMEM; }
    if (!next_lport) next_lport = 49152 + rnd32() % 10000;
    t->owner = current; t->rip = *ip; t->rip.prefix = 0; t->lip = lip; t->rport = port;
    t->lport = next_lport++; if (next_lport > 65000) next_lport = 49152;
    t->iss = rnd32(); t->snd_una = t->iss; t->snd_nxt = t->iss + 1;
    t->mss = ip->v6 ? (uint16_t)(mtu6 - 60) : 1460; t->rto = 1000;
    t->state = T_SYN_SENT;
    tcp_seg(t, F_SYN, t->iss, 0, 0);
    arm_rto(t);
    uint64_t end = uptime_ms() + timeout_ms;
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
            tcp_raw(&t->lip, &t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, F_RST | F_ACK, 0, 0, 0, 0);
        t->state = T_CLOSED;
    }
}

/* ---------------- receive path ---------------- */
static void ip4_input(const uint8_t *f, const uint8_t *p, int len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    int ihl = (p[0] & 15) * 4, tot = g16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || csum_fin(csum_add(0, p, ihl))) return;
    if (g16(p + 6) & 0x3FFF) return;                 /* fragments aren't supported */
    uint32_t src, dst; memcpy(&src, p + 12, 4); memcpy(&dst, p + 16, 4);
    bool for_us = dst == 0xFFFFFFFF || (configured && (dst == my_ip || dst == (my_ip | ~my_mask))) || (!configured && p[9] == 17);
    if (!for_us) return;
    struct ipaddr s = ip_v4(src), d = ip_v4(dst);
    if (configured && src && (src & my_mask) == (my_ip & my_mask) && !nb_find(&s)) nb_learn(&s, f + 6);
    switch (p[9]) {
    case 1: icmp_input(src, p + ihl, tot - ihl); break;
    case 6: if (configured && dst == my_ip) tcp_input(&s, &d, p + ihl, tot - ihl); break;
    case 17: udp_input(&s, &d, p + ihl, tot - ihl); break;
    }
}

static void ip6_input(const uint8_t *p, int len) {
    if (len < 40 || (p[0] >> 4) != 6) return;
    int plen = g16(p + 4);
    if (40 + plen > len) return;
    uint8_t nh = p[6], hlim = p[7];
    const uint8_t *src = p + 8, *dst = p + 24;
    if (ip6_is_multicast(src) || !for_us6(dst)) return;
    const uint8_t *q = p + 40; int ql = plen;
    for (int guard = 0; guard < 8 && (nh == 0 || nh == 43 || nh == 60); guard++) {   /* skip extension headers */
        if (ql < 8) return;
        int l = (q[1] + 1) * 8;
        if (l > ql) return;
        nh = q[0]; q += l; ql -= l;
    }
    struct ipaddr s = ip6(src), d = ip6(dst);
    switch (nh) {
    case 58: icmp6_input(src, dst, q, ql, hlim); break;
    case 6: if (a6_find(dst, false)) tcp_input(&s, &d, q, ql); break;
    case 17: udp_input(&s, &d, q, ql); break;
    }                                                /* fragments (44) and anything else: dropped */
}

void net_rx(struct netif *n, const uint8_t *f, int len) {
    if (len < 14) return;
    random_event(len ^ ((uint64_t)f[len - 1] << 16));     /* packet timing as an entropy source */
    uint16_t type = g16(f + 12);
    if (type == 0x0806) arp_input(f + 14, len - 14);
    else if (type == 0x0800) ip4_input(f, f + 14, len - 14);
    else if (type == 0x86DD && a6[0].used) ip6_input(f + 14, len - 14);
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
bool net_ready(void) { return nif && nif->link && (configured || v6_ready()); }

void net_thread(void *arg) {
    if (!nif) for (;;) task_sleep(60000);
    uint64_t last_link = 0;
    for (;;) {
        nif->poll(nif);
        uint64_t now = uptime_ms();
        if (now - last_link > 500) {
            last_link = now;
            bool up = nif->link_up(nif);
            if (up && !nif->link) { nif->link = true; kprintf("[net] link up\n"); dh_state = DH_INIT; dh_next = 0; v6_start(); }
            if (!up && nif->link) {
                nif->link = false; configured = false; dh_state = DH_INIT; v6_stop();
                memset(nb, 0, sizeof nb); memset(dcache, 0, sizeof dcache);
                kprintf("[net] link down\n");
                desktop_notify("Network disconnected", "The network cable was unplugged.");
            }
            for (int i = 0; i < 8; i++) if (pend[i].used && now - pend[i].t > 3000) pend[i].used = false;
        }
        if (nif->link) { dhcp_tick(); v6_tick(); }
        tcp_timers();
        task_sleep(1);
    }
}

void net_get_info(struct dunc_netinfo *ni) {
    memset(ni, 0, sizeof *ni);
    if (!nif) return;
    uint64_t now = uptime_ms();
    ni->present = 1; ni->link = nif->link; ni->configured = configured; ni->dns_manual = dns_manual;
    strncpy0(ni->adapter, nif->adapter, sizeof ni->adapter);
    strncpy0(ni->driver, nif->driver, sizeof ni->driver);
    memcpy(ni->mac, nif->mac, 6);
    ni->ip = my_ip; ni->mask = my_mask; ni->gw = my_gw; ni->dns = configured ? my_dns : 0; ni->lease_s = lease_s;
    ni->rx_packets = nif->rx_packets; ni->tx_packets = nif->tx_packets; ni->rx_bytes = nif->rx_bytes; ni->tx_bytes = nif->tx_bytes;
    ni->v6_configured = v6_ready();
    for (int i = 0; i < NA6; i++) {
        if (!a6[i].used) continue;
        int k = ni->v6_count++;
        ni->v6[k] = ip6(a6[i].a); ni->v6[k].prefix = a6[i].plen; ni->v6_state[k] = a6[i].state;
        ni->v6_valid_s[k] = a6[i].valid_until == FOREVER ? 0xFFFFFFFF : a6[i].valid_until > now ? (uint32_t)((a6[i].valid_until - now) / 1000) : 0;
    }
    if (rtr6_until > now) ni->router6 = ip6(rtr6);
    if (dns6_until > now) ni->dns6 = ip6(dns6);
    if (dns_manual) ni->dns_user = dns_user;
    dns_server(&ni->dns_now);
    ni->mtu6 = mtu6;
}

void net_set_dns(const struct ipaddr *a) {
    memset(dcache, 0, sizeof dcache);
    if (a) { dns_user = *a; dns_user.prefix = 0; dns_manual = true; }
    else dns_manual = false;                                  /* back to the DNS server from the network */
}
void net_renew(void) {
    if (!nif) return;
    dh_state = DH_INIT; dh_next = 0;
    if (nif->link) { rs_left = 3; rs_next = uptime_ms(); }    /* and ask IPv6 routers to announce themselves */
}
