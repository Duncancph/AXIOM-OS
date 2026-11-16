/* Joining a Wi-Fi network as a station: see wifi_sta.h. */
#include <stdbool.h>
#include "wifi_sta.h"
#include "str.h"


#define FC_AUTH   0xB0
#define FC_ASSOCREQ 0x00
#define FC_ASSOCRESP 0x10
#define FC_DEAUTH 0xC0
#define FC_DISASSOC 0xA0
#define FC_DATA   0x08
#define FC_QOSDATA 0x88
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
/* our RSN element: version 1, group CCMP, pairwise CCMP, AKM PSK, no capabilities */
static const uint8_t rsn_ie[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };

static void fail(struct wifi_sta *s, const char *why) { s->state = WS_FAILED; strncpy0(s->why, why, sizeof s->why); }
static int hdr(struct wifi_sta *s, uint8_t *f, int fc0, int fc1, const uint8_t *a1, const uint8_t *a3) {
    f[0] = (uint8_t)fc0; f[1] = (uint8_t)fc1; f[2] = f[3] = 0;          /* duration: the card fills it */
    memcpy(f + 4, a1, 6); memcpy(f + 10, s->me, 6); memcpy(f + 16, a3, 6);
    uint16_t sc = (uint16_t)(s->seq++ << 4); f[22] = (uint8_t)sc; f[23] = (uint8_t)(sc >> 8);
    return 24;
}
static void send_auth(struct wifi_sta *s) {
    uint8_t f[64]; int n = hdr(s, f, FC_AUTH, 0, s->bssid, s->bssid);
    f[n++] = 0; f[n++] = 0;               /* open system */
    f[n++] = 1; f[n++] = 0;               /* sequence 1 */
    f[n++] = 0; f[n++] = 0;               /* status */
    s->send(s->ctx, f, n);
}
static void send_assoc(struct wifi_sta *s) {
    uint8_t f[160]; int n = hdr(s, f, FC_ASSOCREQ, 0, s->bssid, s->bssid);
    uint16_t cap = 0x0001 | 0x0400 | (s->secure ? 0x0010 : 0);          /* ESS, short slot time, privacy */
    f[n++] = (uint8_t)cap; f[n++] = (uint8_t)(cap >> 8);
    f[n++] = 10; f[n++] = 0;              /* listen interval */
    int sl = (int)strlen(s->ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, s->ssid, sl); n += sl;
    static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };   /* 1 2 5.5 11 basic; 6 9 12 18 */
    memcpy(f + n, rates, sizeof rates); n += sizeof rates;
    static const uint8_t xrates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6C };                        /* 24 36 48 54 */
    memcpy(f + n, xrates, sizeof xrates); n += sizeof xrates;
    if (s->secure) { memcpy(f + n, rsn_ie, sizeof rsn_ie); n += sizeof rsn_ie; }
    s->send(s->ctx, f, n);
}
static void arm(struct wifi_sta *s, uint64_t now) { s->tries = 0; s->deadline = now + 300; }

void wsta_join(struct wifi_sta *s, const uint8_t me[6], const uint8_t bssid[6], const char *ssid, const uint8_t *pmk, const uint8_t snonce[32], uint64_t now) {
    void (*send)(void *, const uint8_t *, int) = s->send; void (*deliver)(void *, const uint8_t *, int) = s->deliver; void *ctx = s->ctx;
    memset(s, 0, sizeof *s);
    s->send = send; s->deliver = deliver; s->ctx = ctx;
    memcpy(s->me, me, 6); memcpy(s->bssid, bssid, 6); strncpy0(s->ssid, ssid, sizeof s->ssid);
    s->secure = pmk != 0;
    if (pmk) wpa_sup_init(&s->sup, pmk, bssid, me, rsn_ie, sizeof rsn_ie, snonce);
    s->state = WS_AUTH; arm(s, now);
    send_auth(s);
}
void wsta_leave(struct wifi_sta *s) {
    if (s->state == WS_IDLE || s->state == WS_FAILED) return;
    uint8_t f[32]; int n = hdr(s, f, FC_DEAUTH, 0, s->bssid, s->bssid);
    f[n++] = 3; f[n++] = 0;               /* reason: leaving */
    s->send(s->ctx, f, n);
    s->state = WS_IDLE; s->keys = false;
    memset(s->tk, 0, sizeof s->tk); memset(s->gtk, 0, sizeof s->gtk);
}
void wsta_tick(struct wifi_sta *s, uint64_t now) {
    if (s->state != WS_AUTH && s->state != WS_ASSOC && s->state != WS_HANDSHAKE) return;
    if (now < s->deadline) return;
    if (s->state == WS_HANDSHAKE) {       /* the access point leads the handshake: just wait for it */
        fail(s, s->sup.why ? s->sup.why : "the access point didn't finish the security handshake (wrong password?)");
        return;
    }
    if (++s->tries > 4) { fail(s, s->state == WS_AUTH ? "the access point didn't answer" : "the access point didn't accept us (association)"); return; }
    s->deadline = now + 300;
    if (s->state == WS_AUTH) send_auth(s); else send_assoc(s);
}

/* 802.11 data -> Ethernet: the LLC/SNAP header carries the EtherType */
static void data_in(struct wifi_sta *s, const uint8_t *f, int len, int hl, const uint8_t *body, int blen) {
    (void)len;
    if (blen < 8 || body[0] != 0xAA || body[1] != 0xAA || body[2] != 0x03) return;
    uint16_t type = (uint16_t)(body[6] << 8 | body[7]);
    const uint8_t *da = f + 4, *sa = f + 16;                              /* from the AP: A1 us/group, A3 the source */
    (void)hl;
    if (type == 0x888E) {                                                /* EAPOL: the handshake */
        if (!s->secure) return;
        uint8_t reply[256]; int rl = 0;
        int r = wpa_sup_rx(&s->sup, body + 8, blen - 8, reply, &rl);
        if (rl) {                                                        /* back to the AP, unencrypted until the keys are in */
            uint8_t eth[300]; memcpy(eth, s->bssid, 6); memcpy(eth + 6, s->me, 6); eth[12] = 0x88; eth[13] = 0x8E;
            memcpy(eth + 14, reply, rl);
            bool was = s->keys;
            if (r == WPA_KEYS && s->state != WS_CONNECTED) s->keys = false;   /* message 4 goes out before the new keys are used */
            wsta_send_eth(s, eth, 14 + rl);
            s->keys = was;
        }
        if (r == WPA_KEYS) {
            memcpy(s->tk, s->sup.ptk + 32, 16); memcpy(s->gtk, s->sup.gtk, 32); s->gtk_idx = s->sup.gtk_idx;
            s->keys = true; s->tx_pn = 0; s->rx_pn = 0; s->rx_gpn = 0;
            s->state = WS_CONNECTED; s->why[0] = 0;
        } else if (r == WPA_GROUP) { memcpy(s->gtk, s->sup.gtk, 32); s->gtk_idx = s->sup.gtk_idx; s->rx_gpn = 0; }
        else if (r == WPA_BAD && s->state == WS_HANDSHAKE) strncpy0(s->why, s->sup.why ? s->sup.why : "bad handshake message", sizeof s->why);
        return;
    }
    if (s->state != WS_CONNECTED) return;
    uint8_t eth[2400]; int n = blen - 8;
    if (n + 14 > (int)sizeof eth) return;
    memcpy(eth, da, 6); memcpy(eth + 6, sa, 6); eth[12] = (uint8_t)(type >> 8); eth[13] = (uint8_t)type;
    memcpy(eth + 14, body + 8, n);
    s->deliver(s->ctx, eth, n + 14);
}

void wsta_rx(struct wifi_sta *s, const uint8_t *f, int len, uint64_t now) {
    if (len < 24 || s->state == WS_IDLE) return;
    int fc0 = f[0], fc1 = f[1];
    const uint8_t *a2 = f + 10;
    if (memcmp(a2, s->bssid, 6)) return;                                  /* not from our access point */
    if ((fc0 & 0x0C) == 0x00) {                                           /* management */
        if (memcmp(f + 4, s->me, 6) && memcmp(f + 4, bcast, 6)) return;
        const uint8_t *b = f + 24; int bl = len - 24;
        if (fc0 == FC_AUTH && s->state == WS_AUTH && bl >= 6) {
            int alg = b[0] | b[1] << 8, seq = b[2] | b[3] << 8, st = b[4] | b[5] << 8;
            if (alg != 0 || seq != 2) return;
            if (st) { char m[96]; ksnprintf(m, sizeof m, "the access point refused us (authentication status %d)", st); fail(s, m); return; }
            s->state = WS_ASSOC; arm(s, now); send_assoc(s);
        } else if ((fc0 == FC_ASSOCRESP || fc0 == 0x30) && s->state == WS_ASSOC && bl >= 6) {
            int st = b[2] | b[3] << 8;
            if (st) { char m[96]; ksnprintf(m, sizeof m, "the access point refused us (association status %d)", st); fail(s, m); return; }
            s->aid = (uint16_t)((b[4] | b[5] << 8) & 0x3FFF);
            if (s->secure) { s->state = WS_HANDSHAKE; s->deadline = now + 3000; }
            else { s->state = WS_CONNECTED; s->why[0] = 0; }
        } else if (fc0 == FC_DEAUTH || fc0 == FC_DISASSOC) {
            int reason = bl >= 2 ? b[0] | b[1] << 8 : 0; char m[96];
            if (s->state == WS_HANDSHAKE && (reason == 15 || reason == 2)) strncpy0(m, "wrong password (the access point gave up on the handshake)", sizeof m);
            else ksnprintf(m, sizeof m, "the access point disconnected us (reason %d)", reason);
            if (s->state == WS_HANDSHAKE || s->state == WS_CONNECTED || s->state == WS_ASSOC) fail(s, m);
            s->keys = false;
        }
        return;
    }
    if ((fc0 & 0x0C) != 0x08) return;                                     /* control frames: the card handles them */
    if ((fc1 & 3) != 2) return;                                           /* data: only From-DS (from the AP) */
    if (memcmp(f + 4, s->me, 6) && !(f[4] & 1)) return;                   /* to us, or group */
    int hl = 24 + ((fc0 & 0x80) ? 2 : 0);
    if ((fc0 & 0x40) || len < hl) return;                                 /* null / no-data subtypes */
    if (fc1 & 0x40) {                                                     /* protected */
        if (!s->keys) return;
        bool group = f[4] & 1;
        static uint8_t plain[2400]; uint64_t pn;
        int keyid = (len > hl + 3) ? f[hl + 3] >> 6 : 0;
        if (group && keyid != s->gtk_idx) return;
        int n = ccmp_decrypt(group ? s->gtk : s->tk, f, hl, f + hl, len - hl, plain, &pn);
        if (n < 0) return;
        uint64_t *last = group ? &s->rx_gpn : &s->rx_pn;
        if (pn <= *last && *last) return;                                 /* replayed */
        *last = pn;
        data_in(s, f, len, hl, plain, n);
    } else {
        /* unencrypted: only EAPOL is taken before the keys are set (and nothing after, on a secure network) */
        const uint8_t *b = f + hl; int bl = len - hl;
        if (s->secure && !(bl >= 8 && b[6] == 0x88 && b[7] == 0x8E)) return;
        data_in(s, f, len, hl, b, bl);
    }
}

int wsta_send_eth(struct wifi_sta *s, const uint8_t *eth, int len) {
    bool eapol = len >= 14 && eth[12] == 0x88 && eth[13] == 0x8E;
    if (len < 14 || (s->state != WS_CONNECTED && !(eapol && s->state == WS_HANDSHAKE))) return -1;
    static uint8_t f[2400]; uint8_t body[2400];
    int pl = len - 14;
    if (pl + 8 > (int)sizeof body - 16) return -1;
    /* To-DS data: A1 the AP, A2 us, A3 the destination */
    f[0] = FC_DATA; f[1] = 0x01; f[2] = f[3] = 0;
    memcpy(f + 4, s->bssid, 6); memcpy(f + 10, s->me, 6); memcpy(f + 16, eth, 6);
    uint16_t sc = (uint16_t)(s->seq++ << 4); f[22] = (uint8_t)sc; f[23] = (uint8_t)(sc >> 8);
    body[0] = 0xAA; body[1] = 0xAA; body[2] = 0x03; body[3] = body[4] = body[5] = 0; body[6] = eth[12]; body[7] = eth[13];
    memcpy(body + 8, eth + 14, pl);
    int n;
    if (s->keys) { f[1] |= 0x40; n = 24 + ccmp_encrypt(s->tk, ++s->tx_pn, 0, f, 24, body, pl + 8, f + 24); }
    else { memcpy(f + 24, body, pl + 8); n = 24 + pl + 8; }
    s->send(s->ctx, f, n);
    return 0;
}
