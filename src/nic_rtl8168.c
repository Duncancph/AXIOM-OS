/* Realtek RTL8111/8168/8169/810x Ethernet (very common on desktop motherboards).
 * C+ mode descriptors, polled. Note: written from the datasheet; not yet tested on hardware. */
#include "net.h"

#define NRX 64
#define NTX 64
struct desc { volatile uint32_t opts1, opts2; volatile uint64_t addr; };
#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS (1u << 29)
#define LS (1u << 28)

struct rtl {
    struct netif nif;
    volatile uint8_t *r;
    struct desc *rx, *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    uint32_t rx_cur, tx_cur;
};

static uint8_t r8(struct rtl *t, int o) { return *(volatile uint8_t *)(t->r + o); }
static void w8(struct rtl *t, int o, uint8_t v) { *(volatile uint8_t *)(t->r + o) = v; }
static void w16(struct rtl *t, int o, uint16_t v) { *(volatile uint16_t *)(t->r + o) = v; }
static uint16_t r16(struct rtl *t, int o) { return *(volatile uint16_t *)(t->r + o); }
static void w32(struct rtl *t, int o, uint32_t v) { *(volatile uint32_t *)(t->r + o) = v; }

static int r_send(struct netif *n, const void *frame, int len) {
    struct rtl *t = n->priv;
    if (len > 1514) return -1;
    struct desc *d = &t->tx[t->tx_cur];
    if (d->opts1 & OWN) return -1;
    memcpy(t->txbuf[t->tx_cur], frame, len);
    if (len < 60) { memset(t->txbuf[t->tx_cur] + len, 0, 60 - len); len = 60; }
    d->addr = (uint64_t)t->txbuf[t->tx_cur];
    d->opts2 = 0;
    mb();
    d->opts1 = OWN | FS | LS | (t->tx_cur == NTX - 1 ? EOR : 0) | len;
    t->tx_cur = (t->tx_cur + 1) % NTX;
    mb();
    w8(t, 0x38, 0x40);                   /* TxPoll: normal priority queue */
    n->tx_packets++; n->tx_bytes += len;
    return 0;
}

static void r_poll(struct netif *n) {
    struct rtl *t = n->priv;
    for (int budget = 0; budget < NRX; budget++) {
        struct desc *d = &t->rx[t->rx_cur];
        uint32_t o = d->opts1;
        if (o & OWN) break;
        mb();
        int len = (o & 0x3FFF) - 4;
        if ((o & FS) && (o & LS) && !(o & (1u << 21)) && len >= 14) {
            n->rx_packets++; n->rx_bytes += len;
            net_rx(n, t->rxbuf[t->rx_cur], len);
        }
        d->opts2 = 0;
        mb();
        d->opts1 = OWN | (t->rx_cur == NRX - 1 ? EOR : 0) | 2048;
        t->rx_cur = (t->rx_cur + 1) % NRX;
    }
    w16(t, 0x3E, 0xFFFF);                /* acknowledge status */
}

static bool r_link(struct netif *n) { return r8(n->priv, 0x6C) & 2; }

struct netif *rtl8168_probe(struct pci_dev *p) {
    if (p->vendor != 0x10EC) return 0;
    uint16_t id = p->device;
    if (id != 0x8168 && id != 0x8169 && id != 0x8161 && id != 0x8167 && id != 0x8136 && id != 0x8125) return 0;
    if (id == 0x8125) { LOG_WARN("Realtek RTL8125 (2.5 GbE) isn't supported yet\n"); return 0; }
    uint64_t size, base = 0;
    for (int b = 2; b >= 1 && !base; b--) {                 /* prefer the memory BAR */
        uint32_t raw = pci_read32(p->bus, p->dev, p->fn, 0x10 + b * 4);
        if (raw && !(raw & 1)) base = pci_bar(p, b, &size);
    }
    if (!base) return 0;
    struct rtl *t = kmalloc(sizeof *t);
    if (!t) return 0;
    t->r = map_mmio(base, 0x1000);
    uint16_t cmd = pci_read16(p->bus, p->dev, p->fn, 4);
    pci_write16(p->bus, p->dev, p->fn, 4, (cmd | 0x6) & ~0x400);

    w16(t, 0x3C, 0);                     /* no interrupts */
    w8(t, 0x37, 0x10);                   /* reset */
    for (int i = 0; i < 1000 && (r8(t, 0x37) & 0x10); i++) sleep_ms(1);
    for (int i = 0; i < 6; i++) t->nif.mac[i] = r8(t, i);

    t->rx = page_alloc(1); t->tx = page_alloc(1);
    for (int i = 0; i < NRX; i += 2) { uint8_t *pg = page_alloc(1); t->rxbuf[i] = pg; t->rxbuf[i + 1] = pg + 2048; }
    for (int i = 0; i < NTX; i += 2) { uint8_t *pg = page_alloc(1); t->txbuf[i] = pg; t->txbuf[i + 1] = pg + 2048; }
    for (int i = 0; i < NRX; i++) {
        t->rx[i].addr = (uint64_t)t->rxbuf[i]; t->rx[i].opts2 = 0;
        t->rx[i].opts1 = OWN | (i == NRX - 1 ? EOR : 0) | 2048;
    }
    for (int i = 0; i < NTX; i++) { t->tx[i].opts1 = i == NTX - 1 ? EOR : 0; t->tx[i].opts2 = 0; t->tx[i].addr = 0; }

    w8(t, 0x50, 0xC0);                   /* unlock config registers */
    w16(t, 0xE0, r16(t, 0xE0) | (1 << 3));   /* C+ command: PCI multiple read/write */
    w16(t, 0xDA, 2047);                  /* max RX packet size */
    w8(t, 0xEC, 0x3F);                   /* max TX packet size */
    w32(t, 0x20, (uint32_t)(uint64_t)t->tx); w32(t, 0x24, (uint64_t)t->tx >> 32);
    w32(t, 0xE4, (uint32_t)(uint64_t)t->rx); w32(t, 0xE8, (uint64_t)t->rx >> 32);
    w8(t, 0x37, 0x0C);                   /* enable RX + TX */
    w32(t, 0x40, 0x03000700);            /* TX config: standard IFG, unlimited DMA burst */
    w32(t, 0x44, 0x0000E70E);            /* RX config: broadcast + multicast + my MAC, unlimited burst */
    w32(t, 0x08, 0xFFFFFFFF); w32(t, 0x0C, 0xFFFFFFFF);   /* multicast filter: accept all */
    w8(t, 0x50, 0x00);                   /* lock */
    w16(t, 0x3E, 0xFFFF);

    ksnprintf(t->nif.adapter, sizeof t->nif.adapter, "Realtek RTL%04x", id);
    strncpy0(t->nif.driver, "rtl8168", sizeof t->nif.driver);
    t->nif.send = r_send; t->nif.poll = r_poll; t->nif.link_up = r_link; t->nif.priv = t;
    return &t->nif;
}
