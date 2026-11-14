/* Intel PRO/1000 family: 8254x (e1000), 82574 (e1000e) and the I217/I218/I219 built into
 * Intel chipsets. Legacy descriptors, polled (no interrupts). */
#include "net.h"

#define CTRL 0x0000
#define STATUS 0x0008
#define EERD 0x0014
#define IMC 0x00D8
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define RDBAL 0x2800
#define RDBAH 0x2804
#define RDLEN 0x2808
#define RDH 0x2810
#define RDT 0x2818
#define TDBAL 0x3800
#define TDBAH 0x3804
#define TDLEN 0x3808
#define TDH 0x3810
#define TDT 0x3818
#define MTA 0x5200
#define RAL 0x5400
#define RAH 0x5404

#define NRX 64
#define NTX 64

struct rxd { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct txd { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

struct e1000 {
    struct netif nif;
    volatile uint8_t *regs;
    struct rxd *rx; struct txd *tx;
    uint8_t *rxbuf[NRX], *txbuf[NTX];
    uint32_t rx_cur, tx_cur;
    bool newer;          /* 82571+ register layout for EERD */
};

static uint32_t rr(struct e1000 *e, uint32_t r) { return mmio_r32(e->regs + r); }
static void wr(struct e1000 *e, uint32_t r, uint32_t v) { mmio_w32(e->regs + r, v); }

static const struct { uint16_t id; const char *name; bool newer; } ids[] = {
    {0x100E, "Intel 82540EM", 0}, {0x100F, "Intel 82545EM", 0}, {0x1004, "Intel 82543GC", 0}, {0x1015, "Intel 82540EM", 0},
    {0x1016, "Intel 82540EP", 0}, {0x1017, "Intel 82540EP", 0}, {0x101E, "Intel 82540EP", 0}, {0x1026, "Intel 82545GM", 0},
    {0x1075, "Intel 82547GI", 0}, {0x1076, "Intel 82541GI", 0}, {0x1078, "Intel 82541ER", 0}, {0x1079, "Intel 82546GB", 0},
    {0x107C, "Intel 82541PI", 0}, {0x10B5, "Intel 82546GB", 0},
    {0x105E, "Intel 82571EB", 1}, {0x105F, "Intel 82571EB", 1}, {0x107D, "Intel 82572EI", 1}, {0x108B, "Intel 82573V", 1},
    {0x108C, "Intel 82573E", 1}, {0x109A, "Intel 82573L", 1}, {0x10D3, "Intel 82574L", 1}, {0x10F6, "Intel 82574L", 1},
    {0x150C, "Intel 82583V", 1}, {0x10EA, "Intel 82577LM", 1}, {0x10EB, "Intel 82577LC", 1}, {0x10EF, "Intel 82578DM", 1},
    {0x10F0, "Intel 82578DC", 1}, {0x1502, "Intel 82579LM", 1}, {0x1503, "Intel 82579V", 1},
    {0x153A, "Intel I217-LM", 1}, {0x153B, "Intel I217-V", 1}, {0x155A, "Intel I218-LM", 1}, {0x1559, "Intel I218-V", 1},
    {0x15A0, "Intel I218-LM", 1}, {0x15A1, "Intel I218-V", 1}, {0x15A2, "Intel I218-LM", 1}, {0x15A3, "Intel I218-V", 1},
    {0x156F, "Intel I219-LM", 1}, {0x1570, "Intel I219-V", 1}, {0x15B7, "Intel I219-LM", 1}, {0x15B8, "Intel I219-V", 1},
    {0x15B9, "Intel I219-LM", 1}, {0x15BB, "Intel I219-LM", 1}, {0x15BC, "Intel I219-V", 1}, {0x15BD, "Intel I219-LM", 1},
    {0x15BE, "Intel I219-V", 1}, {0x15D6, "Intel I219-V", 1}, {0x15D7, "Intel I219-LM", 1}, {0x15D8, "Intel I219-V", 1},
    {0x15DF, "Intel I219-LM", 1}, {0x15E0, "Intel I219-V", 1}, {0x15E1, "Intel I219-LM", 1}, {0x15E2, "Intel I219-V", 1},
    {0x15E3, "Intel I219-LM", 1}, {0x0D4C, "Intel I219-LM", 1}, {0x0D4D, "Intel I219-V", 1}, {0x0D4E, "Intel I219-LM", 1},
    {0x0D4F, "Intel I219-V", 1}, {0x0D53, "Intel I219-LM", 1}, {0x0D55, "Intel I219-V", 1}, {0x15F4, "Intel I219-LM", 1},
    {0x15F5, "Intel I219-V", 1}, {0x15F9, "Intel I219-LM", 1}, {0x15FA, "Intel I219-V", 1}, {0x15FB, "Intel I219-LM", 1},
    {0x15FC, "Intel I219-V", 1}, {0x1A1C, "Intel I219-LM", 1}, {0x1A1D, "Intel I219-V", 1}, {0x1A1E, "Intel I219-LM", 1},
    {0x1A1F, "Intel I219-V", 1}, {0x0DC5, "Intel I219-LM", 1}, {0x0DC6, "Intel I219-V", 1}, {0x0DC7, "Intel I219-LM", 1},
    {0x0DC8, "Intel I219-V", 1}, {0x550A, "Intel I219-LM", 1}, {0x550B, "Intel I219-V", 1}, {0x550C, "Intel I219-LM", 1},
    {0x550D, "Intel I219-V", 1},
};

static int e_send(struct netif *n, const void *frame, int len) {
    struct e1000 *e = n->priv;
    if (len > 1514) return -1;
    struct txd *d = &e->tx[e->tx_cur];
    if (d->cmd && !(d->status & 1)) return -1;     /* ring full */
    memcpy(e->txbuf[e->tx_cur], frame, len);
    d->addr = (uint64_t)e->txbuf[e->tx_cur];
    d->len = len < 60 ? 60 : len;
    d->status = 0;
    d->cmd = 0x01 | 0x02 | 0x08;                   /* EOP | insert FCS | report status */
    e->tx_cur = (e->tx_cur + 1) % NTX;
    mb();
    wr(e, TDT, e->tx_cur);
    n->tx_packets++; n->tx_bytes += len;
    return 0;
}

static void e_poll(struct netif *n) {
    struct e1000 *e = n->priv;
    for (int budget = 0; budget < NRX; budget++) {
        struct rxd *d = &e->rx[e->rx_cur];
        if (!(d->status & 1)) break;               /* DD */
        mb();
        if ((d->status & 2) && !d->errors && d->len >= 14) {
            n->rx_packets++; n->rx_bytes += d->len;
            net_rx(n, e->rxbuf[e->rx_cur], d->len);
        }
        d->status = 0;
        uint32_t old = e->rx_cur;
        e->rx_cur = (e->rx_cur + 1) % NRX;
        wr(e, RDT, old);
    }
}

static bool e_link(struct netif *n) { return rr(n->priv, STATUS) & 2; }

static uint16_t eeprom_read(struct e1000 *e, int addr) {
    if (e->newer) wr(e, EERD, (addr << 2) | 1);
    else wr(e, EERD, (addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rr(e, EERD);
        if (v & (e->newer ? 2 : 0x10)) return v >> 16;
    }
    return 0;
}

struct netif *e1000_probe(struct pci_dev *p) {
    if (p->vendor != 0x8086 || p->class_code != 0x02) return 0;
    int k = -1;
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) if (ids[i].id == p->device) k = i;
    if (k < 0) return 0;
    uint64_t size, base = pci_bar(p, 0, &size);
    if (!base || size < 0x10000) size = 0x20000;
    struct e1000 *e = kmalloc(sizeof *e);
    if (!e) return 0;
    e->regs = map_mmio(base, size);
    e->newer = ids[k].newer;
    uint16_t cmd = pci_read16(p->bus, p->dev, p->fn, 4);
    pci_write16(p->bus, p->dev, p->fn, 4, (cmd | 0x6) & ~0x400);   /* memory + bus master, no INTx */

    wr(e, IMC, 0xFFFFFFFF);
    wr(e, RCTL, 0); wr(e, TCTL, 0);
    sleep_ms(10);
    wr(e, CTRL, rr(e, CTRL) | (1u << 26));                   /* device reset */
    sleep_ms(20);
    for (int i = 0; i < 1000 && (rr(e, CTRL) & (1u << 26)); i++) sleep_ms(1);
    wr(e, IMC, 0xFFFFFFFF);
    /* set link up, auto speed; clear link reset, PHY reset, invert loss-of-signal */
    uint32_t c = rr(e, CTRL);
    c |= (1u << 6) | (1u << 5);
    c &= ~((1u << 3) | (1u << 31) | (1u << 7) | (1u << 30));
    wr(e, CTRL, c);

    uint32_t ral = rr(e, RAL), rah = rr(e, RAH);
    uint8_t *m = e->nif.mac;
    if (rah & (1u << 31)) { m[0] = ral; m[1] = ral >> 8; m[2] = ral >> 16; m[3] = ral >> 24; m[4] = rah; m[5] = rah >> 8; }
    else {
        for (int i = 0; i < 3; i++) { uint16_t w = eeprom_read(e, i); m[i * 2] = w; m[i * 2 + 1] = w >> 8; }
        if (!(m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) {   /* last resort: a random local address */
            uint64_t t = rdtsc(); m[0] = 0x02; for (int i = 1; i < 6; i++) m[i] = t >> (i * 8);
        }
        wr(e, RAL, m[0] | m[1] << 8 | m[2] << 16 | (uint32_t)m[3] << 24);
        wr(e, RAH, m[4] | m[5] << 8 | (1u << 31));
    }
    for (int i = 0; i < 128; i++) wr(e, MTA + i * 4, 0);

    e->rx = page_alloc(1); e->tx = page_alloc(1);
    for (int i = 0; i < NRX; i += 2) { uint8_t *pg = page_alloc(1); e->rxbuf[i] = pg; e->rxbuf[i + 1] = pg + 2048; }
    for (int i = 0; i < NTX; i += 2) { uint8_t *pg = page_alloc(1); e->txbuf[i] = pg; e->txbuf[i + 1] = pg + 2048; }
    for (int i = 0; i < NRX; i++) { e->rx[i].addr = (uint64_t)e->rxbuf[i]; e->rx[i].status = 0; }
    wr(e, RDBAL, (uint32_t)(uint64_t)e->rx); wr(e, RDBAH, (uint64_t)e->rx >> 32);
    wr(e, RDLEN, NRX * 16); wr(e, RDH, 0); wr(e, RDT, NRX - 1);
    e->rx_cur = 0;
    wr(e, RCTL, (1u << 1) | (1u << 4) | (1u << 15) | (1u << 26));   /* enable, all multicast (IPv6), broadcast, strip CRC, 2 KiB buffers */
    wr(e, TDBAL, (uint32_t)(uint64_t)e->tx); wr(e, TDBAH, (uint64_t)e->tx >> 32);
    wr(e, TDLEN, NTX * 16); wr(e, TDH, 0); wr(e, TDT, 0);
    e->tx_cur = 0;
    wr(e, TCTL, (1u << 1) | (1u << 3) | (0x0F << 4) | (0x40 << 12));
    wr(e, TIPG, 0x0060200A);

    strncpy0(e->nif.adapter, ids[k].name, sizeof e->nif.adapter);
    strncpy0(e->nif.driver, e->newer ? "e1000e" : "e1000", sizeof e->nif.driver);
    e->nif.send = e_send; e->nif.poll = e_poll; e->nif.link_up = e_link; e->nif.priv = e;
    return &e->nif;
}
