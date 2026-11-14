/* USB network adapters and phone tethering:
 *   CDC-ECM  (Ethernet Control Model): many USB Ethernet adapters, e.g. Realtek RTL8152/8153 in their standard mode
 *   CDC-NCM  (Network Control Model): newer adapters and phones
 *   RNDIS    (Microsoft's protocol): Android "USB tethering"
 * Each becomes a network interface for the TCP/IP stack. Received frames are queued from the USB
 * completion callback and handed to the stack by the network thread (netif->poll). */
#include "usb.h"
#include "net.h"

enum { K_ECM, K_NCM, K_RNDIS };
#define RXQ 32
#define FRAME 1536

struct usbnet {
    struct netif nif;
    struct usb_dev *d;
    int kind, ctrl_if, data_if, in_dci, out_dci, int_dci;
    uint8_t out_ep, in_ep;
    uint8_t *txbuf;                                   /* DMA buffer for sending */
    uint32_t out_mps;
    uint16_t ntb_seq; uint32_t rndis_id;
    volatile bool link, gone;
    uint8_t rxq[RXQ][FRAME]; uint16_t rxlen[RXQ]; volatile uint32_t rxh, rxt;
    uint64_t rx_dropped;
};

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void rx_frame(struct usbnet *u, const uint8_t *f, uint32_t len) {
    if (len < 14 || len > FRAME) return;
    if (u->rxh - u->rxt >= RXQ) { u->rx_dropped++; return; }
    uint32_t i = u->rxh % RXQ;
    memcpy(u->rxq[i], f, len); u->rxlen[i] = (uint16_t)len;
    __asm__ volatile("" ::: "memory");
    u->rxh++;
}

/* ---- receive completions (called from the USB event handler) ---- */
static void on_bulk_in(struct usb_dev *d, int dci, int cc, uint32_t len) {
    struct usbnet *u = d->drv;
    if (!u || u->gone) return;
    if (cc != CC_SUCCESS && cc != CC_SHORT) return;          /* unplugged or error: stop */
    const uint8_t *b = d->ep_buf[dci];
    if (u->kind == K_ECM) rx_frame(u, b, len);
    else if (u->kind == K_RNDIS) {                           /* one or more REMOTE_NDIS_PACKET_MSG */
        for (uint32_t o = 0; o + 44 <= len;) {
            uint32_t type = le32(b + o), mlen = le32(b + o + 4), doff = le32(b + o + 8), dlen = le32(b + o + 12);
            if (type != 1 || mlen < 44 || o + mlen > len) break;
            if (8 + doff + dlen <= mlen) rx_frame(u, b + o + 8 + doff, dlen);
            o += mlen;
        }
    } else if (len >= 12 && !memcmp(b, "NCMH", 4)) {          /* NCM transfer block (16-bit) */
        uint32_t ndp = le16(b + 10);
        for (int guard = 0; ndp && ndp + 8 <= len && guard < 8; guard++) {
            if (memcmp(b + ndp, "NCM0", 4) && memcmp(b + ndp, "NCM1", 4)) break;
            uint32_t ndplen = le16(b + ndp + 4);
            for (uint32_t e = ndp + 8; e + 4 <= ndp + ndplen && e + 4 <= len; e += 4) {
                uint32_t idx = le16(b + e), dl = le16(b + e + 2);
                if (!idx || !dl) break;
                if (idx + dl <= len) rx_frame(u, b + idx, dl);
            }
            ndp = le16(b + ndp + 6);
        }
    }
    u->nif.rx_packets++; u->nif.rx_bytes += len;
    usb_queue_in(d, dci, on_bulk_in);
}

static void on_notify(struct usb_dev *d, int dci, int cc, uint32_t len) {
    struct usbnet *u = d->drv;
    if (!u || u->gone || (cc != CC_SUCCESS && cc != CC_SHORT)) return;
    const uint8_t *n = d->ep_buf[dci];
    if (len >= 8 && n[0] == 0xA1 && n[1] == 0x00) u->link = le16(n + 2) != 0;     /* NETWORK_CONNECTION */
    usb_queue_in(d, dci, on_notify);
}

/* ---- the netif ---- */
static int un_send(struct netif *nf, const void *frame, int len) {
    struct usbnet *u = (struct usbnet *)nf;
    if (u->gone || len > FRAME) return -1;
    uint8_t *b = u->txbuf; uint32_t total;
    if (u->kind == K_ECM) { memcpy(b, frame, len); total = len; }
    else if (u->kind == K_RNDIS) {
        memset(b, 0, 44);
        w32(b, 1); w32(b + 4, 44 + len); w32(b + 8, 36); w32(b + 12, len);
        memcpy(b + 44, frame, len); total = 44 + len;
    } else {                                                 /* NCM: NTH16, then NDP16, then the frame */
        memset(b, 0, 32);
        memcpy(b, "NCMH", 4); w16(b + 4, 12); w16(b + 6, u->ntb_seq++);
        w16(b + 10, 12);                                     /* NDP right after the header */
        memcpy(b + 12, "NCM0", 4); w16(b + 16, 16); w16(b + 18, 0);
        w16(b + 20, 32); w16(b + 22, len);                   /* datagram at 32 */
        memcpy(b + 32, frame, len); total = 32 + len;
        w16(b + 8, total);
    }
    usb_lock(u->d);
    long r = usb_bulk(u->d, u->out_dci, b, total, 1000);
    if (r == (long)total && u->out_mps && total % u->out_mps == 0) usb_bulk(u->d, u->out_dci, b, 0, 1000);   /* zero-length end */
    usb_unlock(u->d);
    if (r != (long)total) return -1;
    nf->tx_packets++; nf->tx_bytes += len;
    return 0;
}

static void un_poll(struct netif *nf) {
    struct usbnet *u = (struct usbnet *)nf;
    for (int n = 0; n < RXQ && u->rxt != u->rxh; n++) {
        uint32_t i = u->rxt % RXQ;
        net_rx(nf, u->rxq[i], u->rxlen[i]);
        u->rxt++;
    }
}

static bool un_link(struct netif *nf) { struct usbnet *u = (struct usbnet *)nf; return u->link && !u->gone; }

static void un_detach(struct usb_dev *d) {
    struct usbnet *u = d->drv;
    if (!u) return;
    u->gone = true;                     /* the netif stays allocated: the stack may still hold a pointer */
    net_remove_netif(&u->nif);
    d->drv = 0;
}

/* ---- RNDIS control messages ---- */
static int rndis_cmd(struct usbnet *u, uint8_t *msg, uint32_t len, uint8_t *resp, uint32_t rmax) {
    w32(msg + 8, ++u->rndis_id);
    if (usb_control(u->d, 0x21, 0x00, 0, u->ctrl_if, len, msg) < 0) return -1;    /* SEND_ENCAPSULATED_COMMAND */
    for (int tries = 0; tries < 50; tries++) {
        sleep_ms(10);
        memset(resp, 0, 16);
        if (usb_control(u->d, 0xA1, 0x01, 0, u->ctrl_if, rmax, resp) < 0) continue;  /* GET_ENCAPSULATED_RESPONSE */
        if ((le32(resp) & 0x80000000) && le32(resp + 8) == u->rndis_id) return le32(resp + 12) ? -1 : 0;   /* status */
    }
    return -1;
}

static bool rndis_init(struct usbnet *u) {
    uint8_t m[64], r[256];
    memset(m, 0, sizeof m);
    w32(m, 2); w32(m + 4, 24); w32(m + 12, 1); w32(m + 16, 0); w32(m + 20, 0x4000);        /* INITIALIZE */
    if (rndis_cmd(u, m, 24, r, sizeof r)) return false;
    memset(m, 0, sizeof m);
    w32(m, 4); w32(m + 4, 28); w32(m + 12, 0x01010101);                                   /* QUERY permanent address */
    if (rndis_cmd(u, m, 28, r, sizeof r)) return false;
    uint32_t ilen = le32(r + 16), ioff = le32(r + 20);
    if (ilen < 6 || 8 + ioff + 6 > sizeof r) return false;
    memcpy(u->nif.mac, r + 8 + ioff, 6);
    memset(m, 0, sizeof m);
    w32(m, 5); w32(m + 4, 32); w32(m + 12, 0x0001010E); w32(m + 16, 4); w32(m + 20, 20); w32(m + 28, 0x0F);   /* SET packet filter */
    return rndis_cmd(u, m, 32, r, sizeof r) == 0;
}

/* ---- CDC descriptors: union (data interface) and Ethernet functional descriptor (MAC string) ---- */
static void cdc_descs(struct usb_dev *d, int ctrl_if, int *data_if, int *mac_idx) {
    int cur = -1;
    for (int i = 0; i + 1 < d->config_len && d->config[i]; i += d->config[i]) {
        const uint8_t *p = &d->config[i];
        if (p[1] == 4) cur = p[2];
        if (cur != ctrl_if || p[1] != 0x24) continue;
        if (p[2] == 0x06 && p[0] >= 5) *data_if = p[4];           /* union: first subordinate */
        if (p[2] == 0x0F && p[0] >= 13) *mac_idx = p[3];          /* Ethernet networking */
    }
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

void usbnet_attach(struct usb_dev *d, int ctrl_if, uint8_t cls, uint8_t sub, uint8_t proto) {
    int kind = (cls == 0x02 && sub == 0x06) ? K_ECM : (cls == 0x02 && sub == 0x0D) ? K_NCM : K_RNDIS;
    struct usbnet *u = page_alloc((sizeof(struct usbnet) + 4095) / 4096);
    if (!u) return;
    u->d = d; u->kind = kind; u->ctrl_if = ctrl_if; u->data_if = ctrl_if + 1; u->link = true;
    int mac_idx = 0;
    cdc_descs(d, ctrl_if, &u->data_if, &mac_idx);
    const uint8_t *ss, *ssi, *sso;
    const uint8_t *nep = usb_find_endpoint(d, ctrl_if, 3, true, &ss);
    const uint8_t *iep = usb_find_endpoint(d, u->data_if, 2, true, &ssi), *oep = usb_find_endpoint(d, u->data_if, 2, false, &sso);
    if (!iep || !oep) { LOG_WARN("USB network: no data endpoints\n"); return; }
    if (kind != K_RNDIS) usb_control(d, 0x01, 11, 1, u->data_if, 0, 0);             /* SET_INTERFACE data alt 1 */
    u->in_dci = usb_configure_ep(d, iep, ssi); u->out_dci = usb_configure_ep(d, oep, sso);
    u->in_ep = iep[2]; u->out_ep = oep[2]; u->out_mps = (oep[4] | oep[5] << 8) & 0x7FF;
    if (nep) u->int_dci = usb_configure_ep(d, nep, ss);
    u->txbuf = page_alloc(1);
    if (u->in_dci < 0 || u->out_dci < 0 || !u->txbuf) { LOG_WARN("USB network: setup failed\n"); return; }
    d->drv = u; d->detach = un_detach;
    if (kind == K_RNDIS) {
        if (!rndis_init(u)) { LOG_WARN("USB network: RNDIS start-up failed\n"); d->drv = 0; return; }
        d->driver = "rndis";
    } else {
        char mac[16] = "";
        if (mac_idx) usb_get_string(d, mac_idx, mac, sizeof mac);
        for (int i = 0; i < 6; i++) {
            int h = hexv(mac[2 * i]), l = hexv(mac[2 * i + 1]);
            u->nif.mac[i] = h >= 0 && l >= 0 ? (uint8_t)(h << 4 | l) : 0;
        }
        if (!(u->nif.mac[0] | u->nif.mac[1] | u->nif.mac[2] | u->nif.mac[3] | u->nif.mac[4] | u->nif.mac[5])) {
            random_bytes(u->nif.mac, 6); u->nif.mac[0] = (u->nif.mac[0] & 0xFE) | 0x02;     /* locally administered */
        }
        if (kind == K_NCM) {
            uint8_t sz[4]; w32(sz, 4096);
            usb_control(d, 0x21, 0x86, 0, ctrl_if, 4, sz);                                  /* SET_NTB_INPUT_SIZE */
            d->driver = "cdc-ncm";
        } else d->driver = "cdc-ecm";
        usb_control(d, 0x21, 0x43, 0x0E, ctrl_if, 0, 0);                                    /* packet filter: all multicast, directed, broadcast */
    }
    ksnprintf(u->nif.adapter, sizeof u->nif.adapter, "%s", d->product[0] ? d->product : "USB network adapter");
    strncpy0(u->nif.driver, kind == K_ECM ? "USB CDC-ECM" : kind == K_NCM ? "USB CDC-NCM" : "USB RNDIS", sizeof u->nif.driver);
    u->nif.send = un_send; u->nif.poll = un_poll; u->nif.link_up = un_link; u->nif.priv = u;
    usb_queue_in(d, u->in_dci, on_bulk_in);
    if (u->int_dci > 0) usb_queue_in(d, u->int_dci, on_notify);
    LOG_OK("USB network: %s (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x\n", u->nif.adapter, u->nif.driver,
           u->nif.mac[0], u->nif.mac[1], u->nif.mac[2], u->nif.mac[3], u->nif.mac[4], u->nif.mac[5]);
    net_add_netif(&u->nif);
}
