// SPDX-License-Identifier: GPL-2.0
/* Realtek RTL8821AE Wi-Fi (PCIe, 802.11ac, with Bluetooth on the same card).
 *
 * This file follows Linux's rtlwifi/rtl8821ae driver (Copyright Realtek Corporation, GPL-2.0): the power-on
 * steps, register numbers and the order of the start-up are taken from it, so this file is under the GPL v2.
 *
 * Stage 1: find the card, read its built-in settings (eFuse: the MAC address among them), power it up, set up
 * its packet memory, and load and start Realtek's firmware (/system/firmware/rtlwifi/rtl8821aefw_29.bin).
 * Stage 2: the receive ring, the MAC/baseband/radio tables (wifi_rtl8821ae_tables.h), 2.4 GHz at 20 MHz, and a
 * passive scan: each channel in turn, listening for the beacons access points send ~10 times a second.
 * Nothing here runs at start-up: 'wifi start' / 'wifi scan' do it, and 'wifi' shows what happened, so a surprise
 * on new hardware can't stop AXIOM from starting. */
#include "kernel.h"
#include "vfs.h"
#include "wifi_rtl8821ae_tables.h"
#include "net.h"
#include "../common/wifi_sta.h"

void wpa_pmk(const char *pass, const uint8_t *ssid, int ssid_len, uint8_t pmk[32]);   /* user/lib/crypto/wpa.c */

static struct pci_dev *dev;
static volatile uint8_t *mm;
static char report[3000]; static int rlen;
static bool started, fw_ok, radio_ok;
static uint8_t efuse[512], mac[6];

static void add(const char *f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    char line[200]; kvsnprintf(line, sizeof line, f, ap); __builtin_va_end(ap);
    int n = ksnprintf(report + rlen, sizeof report - rlen, "%s\n", line);
    if (n > 0 && rlen + n < (int)sizeof report) rlen += n;
}
/* the card's registers: a 16 KiB window. An offset outside it (a bad table entry) is skipped and reported, not
 * written somewhere else in memory */
#define MM_SIZE 0x4000
static int bad_reg;
static bool reg_ok(int o, int n) {
    if (o >= 0 && o + n <= MM_SIZE) return true;
    if (bad_reg++ < 5) add("skipped a register outside the card's window: %x", (unsigned)o);
    return false;
}
static uint8_t r8(int o) { return reg_ok(o, 1) ? mm[o] : 0xFF; }
static uint16_t r16(int o) { return reg_ok(o, 2) ? *(volatile uint16_t *)(mm + o) : 0xFFFF; }
static uint32_t r32(int o) { return reg_ok(o, 4) ? *(volatile uint32_t *)(mm + o) : 0xFFFFFFFF; }
static void w8(int o, uint8_t v) { if (reg_ok(o, 1)) mm[o] = v; }
static void w16(int o, uint16_t v) { if (reg_ok(o, 2)) *(volatile uint16_t *)(mm + o) = v; }
static void w32(int o, uint32_t v) { if (reg_ok(o, 4)) *(volatile uint32_t *)(mm + o) = v; }

/* registers */
#define REG_SYS_FUNC_EN 0x0002
#define REG_APS_FSMCO   0x0004
#define REG_9346CR      0x000A
#define REG_RSV_CTRL    0x001C
#define REG_EFUSE_CTRL  0x0030
#define REG_GPIO_MUXCFG 0x0040
#define REG_MCUFWDL     0x0080
#define REG_HISR        0x00B4
#define REG_HISRE       0x00BC
#define REG_SYS_CFG     0x00F0
#define REG_CR          0x0100
#define REG_PBP         0x0104
#define REG_TRXFF_BNDY  0x0114
#define REG_LLT_INIT    0x01E0
#define REG_RQPN        0x0200
#define REG_TDECTRL     0x0208
#define REG_RQPN_NPQ    0x0214
#define REG_HWSEQ_CTRL  0x0423
#define REG_TXPKTBUF_BCNQ_BDNY 0x0424
#define REG_TXPKTBUF_MGQ_BDNY  0x0425
#define REG_RX_DRVINFO_SZ 0x060F
#define MAX_RX_DMA_BUFFER_SIZE 0x3E80
#define FW_START        0x1000              /* the firmware is written through this window, 4 KiB at a time */
#define MCUFWDL_RDY     (1u << 1)
#define FWDL_CHKSUM_RPT (1u << 2)
#define WINTINI_RDY     (1u << 6)

/* ---- receiving: a ring of descriptors (32 bytes each) pointing at buffers the card fills ---- */
#define REG_RCR 0x0608
#define RCR_VALUE 0xF4007B0Eu      /* append FCS/MIC/ICV and PHY status; accept management, control, data, broadcast,
                                    * multicast and our own frames (Linux's setting for this chip) */
#define NRXD 64
#define RXBUF 8192
struct rxdesc { volatile uint32_t d[8]; };
static struct rxdesc *rxd; static uint8_t *rxbuf[NRXD]; static int rx_next; static void *txring;
static bool rings_setup(void) {
    if (rxd) return true;
    rxd = page_alloc(1); txring = page_alloc(1);
    if (!rxd || !txring) { add("receive ring: out of memory"); return false; }
    for (int i = 0; i < NRXD; i++) {
        rxbuf[i] = page_alloc(RXBUF / 4096);
        if (!rxbuf[i]) { add("receive buffers: out of memory"); return false; }
        rxd[i].d[6] = (uint32_t)(uintptr_t)rxbuf[i];
        rxd[i].d[0] = RXBUF | (i == NRXD - 1 ? 1u << 30 : 0) | 1u << 31;   /* length, end of ring, owned by the card */
    }
    return true;
}
/* ---- sending ----
 * Two send rings (as Linux's rtl8821ae): management frames on the management queue at 1 Mb/s, data on the
 * best-effort queue at 6 Mb/s, both at a fixed rate (no rate control yet). A descriptor is 64 bytes: the
 * 40-byte TX descriptor, the buffer's address in dword 10 and the next descriptor's in dword 12. The card owns
 * an entry while bit 31 of dword 0 is set; writing the queue's bit to PCIE_CTRL_REG (0x300) tells it to look. */
#define NTXD 16
#define TXBUF 2048
enum { TQ_MGMT, TQ_DATA };
struct txq { uint32_t *d; uint8_t *buf[NTXD]; int next; int reg, poll, qsel; };
static struct txq txq[2] = { { 0, {0}, 0, 0x0318, 1 << 6, 0x12 }, { 0, {0}, 0, 0x0330, 1 << 1, 0x00 } };
static int tx_full, tx_sent;
static bool txq_setup(void) {
    for (int q = 0; q < 2; q++) {
        struct txq *t = &txq[q];
        if (t->d) continue;
        t->d = page_alloc(1); if (!t->d) return false;
        uint8_t *bufs = page_alloc(NTXD * TXBUF / 4096); if (!bufs) return false;
        for (int i = 0; i < NTXD; i++) {
            t->buf[i] = bufs + i * TXBUF;
            t->d[i * 16 + 12] = (uint32_t)(uintptr_t)(t->d + ((i + 1) % NTXD) * 16);   /* the next descriptor */
        }
    }
    return true;
}
/* an 802.11 frame (no FCS: the card adds it) out on a queue; false if the ring is full or it's too long */
static bool tx_frame(int q, const uint8_t *f, int len) {
    struct txq *t = &txq[q];
    if (!t->d || len < 24 || len > TXBUF) return false;
    uint32_t *d = t->d + t->next * 16;
    for (int w = 0; w < 50 && (d[0] & (1u << 31)); w++) udelay(100);   /* (the card still has it: wait up to 5 ms) */
    if (d[0] & (1u << 31)) { tx_full++; return false; }
    memcpy(t->buf[t->next], f, len);
    uint32_t next = d[12];
    for (int i = 0; i < 16; i++) d[i] = 0;
    d[12] = next;
    d[1] = (uint32_t)t->qsel << 8 | (q == TQ_MGMT ? 7u : 4u) << 16;   /* queue; rate set: management / G+B */
    d[3] = 1u << 8 | 1u << 10;                                  /* use this rate, no fallback */
    d[4] = (q == TQ_MGMT ? 0u : 4u) | 0x1Fu << 8 | 0xFu << 13;  /* 1 Mb/s (CCK) / 6 Mb/s (OFDM) */
    d[7] = (uint32_t)len;
    d[9] = (uint32_t)((f[22] | f[23] << 8) >> 4) << 12;          /* the frame's sequence number */
    d[10] = (uint32_t)(uintptr_t)t->buf[t->next];
    __asm__ volatile("" ::: "memory");
    d[0] = (uint32_t)len | 40u << 16 | (f[4] & 1 ? 1u << 24 : 0) | 1u << 26 | 1u << 27 | 1u << 31;   /* size, offset 40, group, last, first, the card's */
    __asm__ volatile("mfence" ::: "memory");
    w16(0x0300, (uint16_t)t->poll);
    t->next = (t->next + 1) % NTXD; tx_sent++;
    return true;
}

/* the next received frame, or 0: *len, and the PHY status (signal) */
static int rx_resyncs;
static uint8_t *rx_take(int *len, int *dbm, int *rate) {
    struct rxdesc *d = &rxd[rx_next];
    if (d->d[0] & (1u << 31)) {                              /* nothing here: if the card filled another slot, we and it
                                                                are out of step (it then waits on that one): go there */
        int k = 1; while (k < NRXD && (rxd[(rx_next + k) % NRXD].d[0] & (1u << 31))) k++;
        if (k == NRXD) return 0;
        rx_next = (rx_next + k) % NRXD; rx_resyncs++; d = &rxd[rx_next];
    }
    uint32_t w0 = d->d[0];
    int plen = w0 & 0x3FFF, drv = ((w0 >> 16) & 0xF) * 8, shift = (w0 >> 24) & 3;
    bool crc_bad = w0 & (1u << 14), physt = w0 & (1u << 26);
    *rate = d->d[3] & 0x7F;
    uint8_t *b = rxbuf[rx_next];
    *dbm = 0;
    if (physt && drv >= 8) {
        if (*rate < 4) {                                     /* CCK (1-11 Mb/s): from the AGC report */
            int lna = (b[4] >> 5) & 7, vga = b[4] & 0x1F, p = -6;
            *dbm = lna == 5 ? p - 32 - 2 * vga : lna == 4 ? p - 24 - 2 * vga : lna == 2 ? p - 11 - 2 * vga :
                   lna == 1 ? p + 5 - 2 * vga : lna == 0 ? p + 21 - 2 * vga : -100;
        } else *dbm = (b[0] & 0x7F) - 110;                   /* OFDM */
    }
    static uint8_t frame[RXBUF];
    int n = plen; if (n > RXBUF - drv - shift) n = RXBUF - drv - shift;
    if (n < 0) n = 0;
    memcpy(frame, b + drv + shift, n);
    d->d[0] = RXBUF | (rx_next == NRXD - 1 ? 1u << 30 : 0) | 1u << 31;  /* back to the card */
    rx_next = (rx_next + 1) % NRXD;
    *len = crc_bad ? -1 : n;
    return frame;
}

/* ---- baseband (BB) and radio (RF) registers ---- */
static int ctz32(uint32_t m) { int n = 0; while (m && !(m & 1)) { m >>= 1; n++; } return n; }
static void bb_set(uint32_t reg, uint32_t mask, uint32_t v) {
    if (mask != 0xFFFFFFFF) v = (r32(reg) & ~mask) | ((v << ctz32(mask)) & mask);
    w32(reg, v);
}
static uint32_t bb_get(uint32_t reg, uint32_t mask) { return (r32(reg) & mask) >> ctz32(mask); }
static uint32_t rf_read(int reg) {                           /* path A, through the LSSI read-back */
    bool pi = bb_get(0xC00, 0x4);
    bb_set(0x8B0, 0xFF, reg & 0xFF);
    udelay(20);
    return bb_get(pi ? 0xD04 : 0xD08, 0xFFFFF);
}
static void rf_write(int reg, uint32_t mask, uint32_t v) {
    if (mask != 0xFFFFF) v = (rf_read(reg) & ~mask) | ((v << ctz32(mask)) & mask);
    w32(0xC90, (((uint32_t)reg & 0xFF) << 20 | (v & 0xFFFFF)) & 0x0FFFFFFF);   /* RA_LSSIWRITE */
}

/* the tables' conditions: chip cut, interface (PCIe) and board options from the eFuse */
static uint32_t drv1;
static bool cond_ok(uint32_t c1, uint32_t c2) {
    if ((c1 & 0x0000F000) && (c1 & 0x0000F000) != (drv1 & 0x0000F000)) return false;
    if ((c1 & 0x0F000000) && (c1 & 0x0F000000) != (drv1 & 0x0F000000)) return false;
    c1 &= 0x00FF0FFF;
    uint32_t d = drv1 & 0x00FF0FFF;
    if ((c1 & d) != c1) return false;
    if (!(c1 & 0x0F)) return true;
    /* the external amplifier types per path (Linux's type_glna/gpa/alna/apa) are 0 on the 8821AE */
    uint32_t mask = (c1 & 1 ? 0xFFu : 0) | (c1 & 2 ? 0xFF00u : 0) | (c1 & 4 ? 0xFF0000u : 0) | (c1 & 8 ? 0xFF000000u : 0);
    return (c2 & mask) == 0;
}
enum { T_MAC, T_BB, T_AGC, T_RF };
static int run_table(const uint32_t *t, int n, int kind) {
    bool matched = true, skipped = false; int writes = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        uint32_t a = t[i], v = t[i + 1];
        if (a & 0xC0000000) {
            if (!(a & 0x80000000)) continue;
            int c = (a >> 28) & 3;
            if (c == 3) { matched = true; skipped = false; }             /* end */
            else if (c == 2) matched = !skipped;                          /* else */
            else if (skipped) matched = false;                            /* if / else if, after a match */
            else if (cond_ok(a, v)) { matched = true; skipped = true; }
            else { matched = false; skipped = false; }
            continue;
        }
        if (!matched) continue;
        writes++;
        if (kind == T_MAC) w8(a, (uint8_t)v);
        else if (kind == T_RF) { if (a == 0xFE || a == 0xFFE) sleep_ms(50); else { rf_write(a, 0xFFFFF, v); udelay(1); } }
        else if (kind == T_BB && a >= 0xF9 && a <= 0xFE) {
            if (a == 0xFE) sleep_ms(50); else if (a == 0xFD) sleep_ms(5); else if (a == 0xFC) sleep_ms(1);
            else udelay(a == 0xFB ? 50 : a == 0xFA ? 5 : 1);
        } else { w32(a, v); udelay(1); }
    }
    return writes;
}

static int channel;
static void set_channel(int ch) {
    bb_set(0x860, 0x1FFE0000, 0x96A);                        /* RFC_AREA, 2.4 GHz */
    rf_write(0x18, (1u << 18) | (1u << 17) | (1u << 16) | (1u << 9) | (1u << 8), 0);
    rf_write(0x18, 0xFF, (uint32_t)ch);
    channel = ch;
}
static uint8_t efuse_board(void);
static bool radio_init(void) {
    uint32_t cfg = r32(REG_SYS_CFG);
    uint8_t b = efuse_board();
    drv1 = (cfg & 0xF000) << 12 | 0x04u << 16 | 1u << 8 | b;
    add("radio: chip cut %u, board options %02x", (unsigned)((cfg >> 12) & 0xF), b);
    int n = run_table(t_mac, sizeof t_mac / 4, T_MAC);
    w8(REG_SYS_FUNC_EN, r8(REG_SYS_FUNC_EN) | 0x40);         /* FEN_PCIEA */
    w8(REG_SYS_FUNC_EN, r8(REG_SYS_FUNC_EN) | 0x03);         /* baseband out of reset */
    w8(0x001F, 7); w8(0x0074 + 2, 7);                        /* RF_CTRL, OPT_CTRL */
    int nb = run_table(t_phy, sizeof t_phy / 4, T_BB);
    int na = run_table(t_agc, sizeof t_agc / 4, T_AGC);
    uint8_t xtal = efuse[0xB9] == 0xFF ? 0x20 : efuse[0xB9] & 0x3F;
    bb_set(0x002C, 0xFFF000, xtal | xtal << 6);              /* crystal trim */
    int nr = run_table(t_radioa, sizeof t_radioa / 4, T_RF);
    add("radio: %d MAC, %d baseband, %d gain-table and %d radio settings written; RF 0x00 = %05x, 0x18 = %05x",
        n, nb, na, nr, rf_read(0), rf_read(0x18));
    /* 2.4 GHz */
    bb_set(0x808, 0x30000000, 3);                            /* OFDM and CCK on */
    bb_set(0xCB0, 0xF000, 7); bb_set(0xCB0, 0xF0, 7);        /* RFE pins: LNA on, PA for 2.4 GHz */
    bb_set(0xC1C, 0xF00, 0);                                 /* the 2.4 GHz gain table */
    bb_set(0x80C, 0xF0, 1); bb_set(0xA04, 0x0F000000, 1);
    w8(0x0454, 0);                                           /* CCK_CHECK */
    /* 20 MHz */
    w16(0x0668, r16(0x0668) & 0xFE7F);
    w8(0x0483, 0);
    bb_set(0x8AC, 0x003003C3, 0x00300200);
    bb_set(0x8C4, 1u << 30, 0);
    bb_set(0x848, 0x03C00000, 8);
    rf_write(0x18, (1u << 11) | (1u << 10), 3);
    w8(0x0102, r8(0x0102) & ~3);                             /* MSR: no link */
    w16(0x06A0, 0xFFFF);                                     /* RXFLTMAP0: every management frame */
    w32(REG_RCR, RCR_VALUE);
    w8(0x0286, r8(0x0286) & ~4);                             /* RXDMA_CONTROL: release receive DMA */
    w8(0x0300 + 1, 0);                                       /* PCIE_CTRL_REG: release send/receive DMA */
    set_channel(1);
    add("radio: 2.4 GHz, 20 MHz, channel 1; RCR %08x, CR %04x", r32(REG_RCR), r16(REG_CR));
    return true;
}

/* power steps: write (bits under the mask), poll until (reg & mask) == value, or wait */
enum { PWR_WRITE = 1, PWR_POLL = 2, PWR_DELAY = 3, PWR_END = 4 };
struct pwr_step { uint16_t reg; uint8_t cmd, mask, value; };
/* Linux's RTL8821A_TRANS_CARDDIS_TO_CARDEMU + RTL8821A_TRANS_CARDEMU_TO_ACT, the PCIe steps for every chip cut */
static const struct pwr_step pwr_on[] = {
    { 0x0005, 1, 0x88, 0x00 }, { 0x0005, 1, 0x18, 0x00 }, { 0x0301, 1, 0xff, 0x00 }, { 0x0005, 1, 0x1c, 0x00 },
    { 0x0075, 1, 0x01, 0x01 }, { 0x0006, 2, 0x02, 0x02 }, { 0x0075, 1, 0x01, 0x00 }, { 0x0006, 1, 0x01, 0x01 },
    { 0x0005, 1, 0x80, 0x00 }, { 0x0005, 1, 0x18, 0x00 }, { 0x0005, 1, 0x01, 0x01 }, { 0x0005, 2, 0x01, 0x00 },
    { 0x004f, 1, 0x01, 0x01 }, { 0x0067, 1, 0x30, 0x30 }, { 0x0025, 1, 0x40, 0x00 }, { 0x0049, 1, 0x02, 0x02 },
    { 0x0063, 1, 0x02, 0x02 }, { 0x0062, 1, 0x02, 0x00 }, { 0x0058, 1, 0x01, 0x01 }, { 0x005a, 1, 0x02, 0x02 },
    { 0x002e, 1, 0xff, 0x82 }, { 0x0010, 1, 0x40, 0x40 }, { 0, PWR_END, 0, 0 }
};
/* RTL8821A_TRANS_ACT_TO_CARDEMU + RTL8821A_TRANS_CARDEMU_TO_CARDDIS */
static const struct pwr_step pwr_off[] = {
    { 0x001f, 1, 0xff, 0x00 }, { 0x004f, 1, 0x01, 0x00 }, { 0x0049, 1, 0x02, 0x00 }, { 0x0005, 1, 0x02, 0x02 },
    { 0x0005, 2, 0x02, 0x00 }, { 0x0005, 1, 0x04, 0x04 }, { 0, PWR_END, 0, 0 }
};
static bool run_steps(const struct pwr_step *s, const char *name) {
    for (int i = 0; s[i].cmd != PWR_END; i++) {
        if (s[i].cmd == PWR_WRITE) w8(s[i].reg, (uint8_t)((r8(s[i].reg) & ~s[i].mask) | (s[i].value & s[i].mask)));
        else if (s[i].cmd == PWR_POLL) {
            int n = 0;
            while ((r8(s[i].reg) & s[i].mask) != (s[i].value & s[i].mask)) {
                if (++n > 5000) { add("%s: step %d: register %03x stayed %02x (waiting for %02x under mask %02x)", name, i, s[i].reg, r8(s[i].reg), s[i].value, s[i].mask); return false; }
                udelay(10);
            }
        } else if (s[i].cmd == PWR_DELAY) udelay(s[i].value ? s[i].reg * 1000u : s[i].reg);
    }
    return true;
}

/* eFuse: the card's one-time-programmed settings, stored compressed (a header byte per 8-byte section says
 * which of its four words follow) */
static uint8_t efuse_byte(int a) {
    w8(REG_EFUSE_CTRL + 1, a & 0xFF);
    w8(REG_EFUSE_CTRL + 2, (uint8_t)(((a >> 8) & 3) | (r8(REG_EFUSE_CTRL + 2) & 0xFC)));
    w8(REG_EFUSE_CTRL + 3, r8(REG_EFUSE_CTRL + 3) & 0x7F);
    for (int i = 0; i < 10000 && !(r32(REG_EFUSE_CTRL) & 0x80000000u); i++) udelay(1);
    udelay(50);
    return (uint8_t)r32(REG_EFUSE_CTRL);
}
static int read_efuse(void) {                                   /* returns the bytes used */
    memset(efuse, 0xFF, sizeof efuse);
    int a = 0, used = 0;
    while (a < 256) {
        uint8_t h = efuse_byte(a);
        if (h == 0xFF) break;
        a++;
        int off, wren;
        if ((h & 0x1F) == 0x0F) {                               /* extended header */
            uint8_t h2 = efuse_byte(a);
            if ((h2 & 0x0F) == 0x0F) { a++; continue; }
            off = ((h2 & 0xF0) >> 1) | ((h & 0xE0) >> 5); wren = h2 & 0x0F; a++;
        } else { off = (h >> 4) & 0x0F; wren = h & 0x0F; }
        for (int w = 0; w < 4; w++, wren >>= 1) {
            if (wren & 1) continue;                             /* 0 = this word is present */
            uint8_t lo = efuse_byte(a++), hi = a < 256 ? efuse_byte(a++) : 0xFF;
            if (off < 64) { efuse[off * 8 + w * 2] = lo; efuse[off * 8 + w * 2 + 1] = hi; }
        }
        used = a;
    }
    return used;
}

static bool llt_write(int addr, int data) {
    w32(REG_LLT_INIT, (uint32_t)(data & 0xFF) | (uint32_t)(addr & 0xFF) << 8 | 1u << 30);
    for (int i = 0; i < 100; i++) { if (!((r32(REG_LLT_INIT) >> 30) & 3)) return true; udelay(10); }
    add("packet memory: no answer writing link %d", addr);
    return false;
}
static bool llt_init(void) {                                   /* the packet buffer's page list */
    const int bndy = 0xF7, maxpage = 255;
    w8(REG_TRXFF_BNDY, bndy); w16(REG_TRXFF_BNDY + 2, MAX_RX_DMA_BUFFER_SIZE - 1);
    w8(REG_TDECTRL + 1, bndy); w8(REG_TXPKTBUF_BCNQ_BDNY, bndy); w8(REG_TXPKTBUF_MGQ_BDNY, bndy);
    w8(REG_PBP, 0x31); w8(REG_RX_DRVINFO_SZ, 4);
    for (int i = 0; i < bndy - 1; i++) if (!llt_write(i, i + 1)) return false;
    if (!llt_write(bndy - 1, 0xFF)) return false;
    for (int i = bndy; i < maxpage; i++) if (!llt_write(i, i + 1)) return false;
    if (!llt_write(maxpage, bndy)) return false;
    w32(REG_RQPN, 0x80e60808); w8(REG_RQPN_NPQ, 0);
    return true;
}

static void fw_selfreset(void) {                              /* reset the card's 8051 processor */
    w8(REG_RSV_CTRL + 1, r8(REG_RSV_CTRL + 1) & ~1);
    w8(REG_SYS_FUNC_EN + 1, r8(REG_SYS_FUNC_EN + 1) & ~4);
    udelay(50);
    w8(REG_RSV_CTRL + 1, r8(REG_RSV_CTRL + 1) | 1);
    w8(REG_SYS_FUNC_EN + 1, r8(REG_SYS_FUNC_EN + 1) | 4);
}
static bool load_firmware(void) {
    const char *path = "/system/firmware/rtlwifi/rtl8821aefw_29.bin";
    uint64_t len; uint8_t *fw = vfs_read_all(path, &len);
    if (!fw || len < 64) { add("firmware: couldn't read %s", path); return false; }
    uint16_t sig = fw[0] | fw[1] << 8;
    add("firmware: %u bytes, signature %04x, version %u.%u (%02u-%02u %02u:%02u)", (unsigned)len, sig, fw[4] | fw[5] << 8, fw[6], fw[8], fw[9], fw[10], fw[11]);
    uint8_t *p = fw; uint32_t n = (uint32_t)len;
    if ((sig & 0xFFF0) == 0x2100 || (sig & 0xFFF0) == 0x9500) { p += 32; n -= 32; }   /* skip the header */
    if (r8(REG_MCUFWDL) & 0x80) { w8(REG_MCUFWDL, 0); fw_selfreset(); }                /* firmware already running */
    /* download on */
    w8(REG_MCUFWDL, 0x05);
    w8(REG_MCUFWDL + 2, r8(REG_MCUFWDL + 2) & 0xF7);
    uint32_t padded = (n + 3) & ~3u;
    for (uint32_t page = 0; page * 4096 < padded; page++) {
        w8(REG_MCUFWDL + 2, (uint8_t)((r8(REG_MCUFWDL + 2) & 0xF8) | (page & 7)));
        uint32_t chunk = padded - page * 4096 < 4096 ? padded - page * 4096 : 4096;
        for (uint32_t i = 0; i < chunk; i++) { uint32_t k = page * 4096 + i; w8(FW_START + i, k < n ? p[k] : 0); }
    }
    w8(REG_MCUFWDL, r8(REG_MCUFWDL) & 0xFE);                  /* download off */
    kfree(fw);
    uint32_t v = 0; int i;
    for (i = 0; i < 6000; i++) { v = r32(REG_MCUFWDL); if (v & FWDL_CHKSUM_RPT) break; udelay(5); }
    if (!(v & FWDL_CHKSUM_RPT)) { add("firmware: the card didn't confirm the checksum (MCUFWDL %08x)", v); return false; }
    w32(REG_MCUFWDL, (r32(REG_MCUFWDL) | MCUFWDL_RDY) & ~WINTINI_RDY);
    fw_selfreset();
    for (i = 0; i < 6000; i++) { v = r32(REG_MCUFWDL); if (v & WINTINI_RDY) break; udelay(5); }
    if (!(v & WINTINI_RDY)) { add("firmware: loaded, but it didn't report ready (MCUFWDL %08x)", v); return false; }
    add("firmware: running (MCUFWDL %08x, ready after %d checks)", v, i);
    return true;
}

static bool init_mac(void) {
    w8(REG_RSV_CTRL, 0);
    w8(REG_APS_FSMCO + 1, r8(REG_APS_FSMCO + 1) & ~0x80);
    if (!run_steps(pwr_on, "power on")) return false;
    w8(REG_APS_FSMCO, r8(REG_APS_FSMCO) | 0x10);
    w8(REG_CR, 0xFF); sleep_ms(2);
    w8(REG_HWSEQ_CTRL, 0xFF); sleep_ms(2);
    if (r8(REG_SYS_CFG + 3) & 1) w8(0x7C, r8(0x7C) | 0x40);
    w8(REG_GPIO_MUXCFG + 1, r8(REG_GPIO_MUXCFG + 1) & ~0x10);
    w16(REG_CR, 0x2FF);
    add("power: on (APS_FSMCO %08x, CR %04x)", r32(REG_APS_FSMCO), r16(REG_CR));
    if (!llt_init()) return false;
    add("packet memory: set up");
    w32(REG_HISR, 0xFFFFFFFF); w32(REG_HISRE, 0xFFFFFFFF);
    w8(0x0130 + 3, r8(0x0130 + 3) | 0x40);                   /* FWIMR: firmware beamformer interrupt */
    w16(0x010C, (uint16_t)((r16(0x010C) & 0xF) | 0xF5B1));   /* TRXDMA_CTRL: queue to DMA mapping */
    w8(0x0420 + 1, 0x1F);                                    /* FWHW_TXQ_CTRL */
    w32(REG_RCR, RCR_VALUE);
    w16(0x06A4, 0xFFFF);                                     /* RXFLTMAP2: every data frame type */
    w32(0x0604, (1u << 9) | (1u << 15));                     /* TCR: CFENDFORM | bit 15, as Linux's driver sets it */
    if (!rings_setup()) return false;
    for (int q = 0; q < 7; q++) w32(0x0308 + q * 8, (uint32_t)(uintptr_t)txring);   /* the send queues (0x308-0x338): an empty ring each */
    if (txq_setup()) { w32(txq[TQ_MGMT].reg, (uint32_t)(uintptr_t)txq[TQ_MGMT].d); w32(txq[TQ_DATA].reg, (uint32_t)(uintptr_t)txq[TQ_DATA].d); }
    else add("send rings: out of memory");
    w32(0x0340, (uint32_t)(uintptr_t)rxd);                   /* RX_DESA */
    w8(0x0300 + 3, 0x77);                                    /* PCIE_CTRL_REG */
    w32(0x0304, 0);                                          /* INT_MIG */
    w32(0x01C0, 0);                                          /* MCUTST_1 */
    w8(0x0577, 3);                                           /* SECONDARY_CCA_CTRL */
    return true;
}

static uint8_t efuse_board(void) {               /* the tables' board bits: ext LNA/PA 2.4 and 5 GHz, Bluetooth */
    uint8_t pa = efuse[0xBC], lna2 = efuse[0xBD], lna5 = efuse[0xBF];
    if (pa == 0xFF && lna2 == 0xFF) pa = lna2 = 0;
    if (pa == 0xFF && lna5 == 0xFF) lna5 = 0;
    bool glna = lna2 & 0x80, gpa = pa & 0x20, alna = lna5 & 0x80, apa = pa & 0x02, bt = r32(0x0068) & (1u << 18);
    return (uint8_t)(glna | gpa << 1 | alna << 2 | apa << 3 | bt << 4);
}
static void show_mac(const char *when) {
    if (efuse[0] == 0x29 && efuse[1] == 0x81) {                 /* the eFuse's ID: 0x8129 */
        memcpy(mac, efuse + 0xD0, 6);
        add("settings (%s): MAC %02x:%02x:%02x:%02x:%02x:%02x, IDs %02x%02x:%02x%02x, channel plan %02x, version %02x",
            when, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], efuse[0xD7], efuse[0xD6], efuse[0xD9], efuse[0xD8], efuse[0xB8], efuse[0xC4]);
    } else add("settings (%s): not readable (starts %02x %02x)", when, efuse[0], efuse[1]);
}

/* 'wifi start' */
void wifi_start(void) {
    if (!dev) { kprintf("No supported Wi-Fi card found (AXIOM knows the Realtek RTL8821AE so far).\n"); return; }
    if (started) { kprintf("Already started once; restart AXIOM to try again.\n%s", report); return; }
    started = true;
    rlen = 0; report[0] = 0;
    uint64_t sz, base = pci_bar(dev, 2, &sz);
    if (!base) { add("no memory window (BAR2)"); kprintf("%s", report); return; }
    mm = map_mmio(base, MM_SIZE);
    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->fn, 4);
    pci_write16(dev->bus, dev->dev, dev->fn, 4, (cmd | 0x6) & ~0x400);
    uint32_t cfg = r32(REG_SYS_CFG);
    add("card: registers at %llx, SYS_CFG %08x (cut %c%s), 9346CR %02x, CR %02x, MCUFWDL %08x",
        (unsigned long long)base, cfg, 'A' + ((cfg >> 12) & 0xF), cfg & (1u << 23) ? ", BT" : "", r8(REG_9346CR), r8(REG_CR), r32(REG_MCUFWDL));
    int used = read_efuse();
    add("settings: %d bytes used", used);
    show_mac("before power-on");
    uint8_t crv = r8(REG_CR);
    if (crv != 0 && crv != 0xEA) { add("power: was on already (CR %02x); turning it off first", crv); run_steps(pwr_off, "power off"); }
    if (!init_mac()) { add("stopped at power-on"); kprintf("%s", report); vfs_write("/home/Documents/wifi-report.txt", report, (uint64_t)rlen); return; }
    if (efuse[0] != 0x29) { read_efuse(); show_mac("after power-on"); }
    w8(REG_SYS_CFG, r8(REG_SYS_CFG) & 0x7F);
    fw_ok = load_firmware();
    /* show and save what we have before the radio step, in case it hangs on real hardware */
    kprintf("%s", report);
    vfs_write("/home/Documents/wifi-report.txt", report, (uint64_t)rlen);
    int shown = rlen;
    if (fw_ok) { kprintf("Setting up the radio...\n"); radio_ok = radio_init(); }
    add(!fw_ok ? "Stopped: the firmware didn't start (see above)." : radio_ok ? "The card is on, its firmware is running and the radio is set up. Type 'wifi scan'." : "Stopped while setting up the radio (see above).");
    kprintf("%s", report + shown);
    vfs_write("/home/Documents/wifi-report.txt", report, (uint64_t)rlen);
    kprintf("(Saved to Documents/wifi-report.txt)\n");
    LOG_INFO("wifi: %s\n", fw_ok ? "firmware running" : "start failed (wifi shows why)");
}
void wifi_info(void) {
    if (!dev) { kprintf("No supported Wi-Fi card found.\n"); return; }
    kprintf("Wi-Fi: Realtek RTL8821AE at PCI %02x:%02x.%x\n", dev->bus, dev->dev, dev->fn);
    if (!started) kprintf("Not started. Type 'wifi start' to power it up and load its firmware (test stage).\n");
    else kprintf("%s", report);
}
/* at start-up: only note the card (nothing is switched on) */
bool wifi_probe(struct pci_dev *p) {
    if (p->vendor != 0x10EC || p->device != 0x8821) return false;
    dev = p;
    dev_claim(p, "rtl8821ae", DEV_OFF, "Realtek RTL8821AE Wi-Fi", "Being added: type 'wifi start' in CMD to test");
    LOG_INFO("Wi-Fi: Realtek RTL8821AE found (support is being added: 'wifi start')\n");
    return true;
}

/* ---- scanning ---- */
struct bss { uint8_t bssid[6]; char ssid[33]; int ch, dbm, seen; bool rsn, wpa, privacy, sae; };
static struct bss found[48]; static int nfound;
static void note_beacon(const uint8_t *f, int len, int dbm) {
    if (len < 36) return;
    int fc = f[0];
    if (fc != 0x80 && fc != 0x50) return;                   /* beacon, probe response */
    const uint8_t *bssid = f + 16, *ie = f + 36; int n = len - 36 - 4;   /* minus the FCS */
    struct bss b; memset(&b, 0, sizeof b); memcpy(b.bssid, bssid, 6); b.ch = channel; b.dbm = dbm;
    b.privacy = f[34] & 0x10;
    for (int i = 0; i + 2 <= n;) {
        int t = ie[i], l = ie[i + 1];
        if (i + 2 + l > n) break;
        const uint8_t *v = ie + i + 2;
        if (t == 0 && l <= 32) { memcpy(b.ssid, v, l); b.ssid[l] = 0; }
        else if (t == 3 && l >= 1) b.ch = v[0];
        else if (t == 48 && l >= 2) {
            b.rsn = true;
            /* AKM suites: 00-0F-AC-8 is SAE (WPA3) */
            if (l >= 8) { int pc = v[6] | v[7] << 8, off = 8 + pc * 4;
                if (off + 2 <= l) { int ac = v[off] | v[off + 1] << 8; for (int k = 0; k < ac && off + 2 + k * 4 + 4 <= l; k++) if (v[off + 2 + k * 4 + 3] == 8) b.sae = true; } }
        }
        else if (t == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) b.wpa = true;
        i += 2 + l;
    }
    for (int i = 0; i < nfound; i++) if (!memcmp(found[i].bssid, b.bssid, 6)) {
        found[i].seen++; if (dbm > found[i].dbm) found[i].dbm = dbm;
        if (!found[i].ssid[0] && b.ssid[0]) strncpy0(found[i].ssid, b.ssid, 33);
        return;
    }
    if (nfound < 48) { b.seen = 1; found[nfound++] = b; }
}
static bool sim;                                  /* 'wifi simulate': a pretend card, for trying the Network app without one */
static int scan_frames, scan_bad, scan_other;
static int ch_frames[15], ch_beacons[15], ch_rf[15];        /* per channel: what was heard, and where the radio said it was */
static void do_scan(void) {
    nfound = 0; scan_frames = scan_bad = scan_other = 0;
    if (sim) {
        static const struct { const char *n; int ch, dbm, rsn, sae, priv; } fake[] = {
            { "HomeNet", 6, -41, 1, 0, 1 }, { "HomeNet-Guest", 6, -47, 0, 0, 0 }, { "BT-7XKQ2A", 1, -63, 1, 0, 1 },
            { "Neighbour 5G-ready", 11, -71, 1, 1, 1 }, { "", 3, -77, 1, 0, 1 }, { "Cafe Free WiFi", 9, -84, 0, 0, 0 },
            { "Old Router", 13, -88, 0, 0, 1 } };
        for (unsigned i = 0; i < sizeof fake / sizeof *fake; i++) {
            struct bss *b = &found[nfound++]; memset(b, 0, sizeof *b);
            strncpy0(b->ssid, fake[i].n, 33); b->ch = fake[i].ch; b->dbm = fake[i].dbm; b->rsn = fake[i].rsn; b->sae = fake[i].sae; b->privacy = fake[i].priv;
            for (int k = 0; k < 6; k++) b->bssid[k] = (uint8_t)(0x10 * i + k);
        }
        sleep_ms(1500);
        return;
    }
    for (int ch = 1; ch <= 13; ch++) {
        set_channel(ch);
        ch_rf[ch] = (int)(rf_read(0x18) & 0xFF); ch_frames[ch] = ch_beacons[ch] = 0;
        uint64_t until = uptime_ms() + 220;
        while (uptime_ms() < until) {
            int len, dbm, rate; uint8_t *f;
            while ((f = rx_take(&len, &dbm, &rate))) {
                scan_frames++; ch_frames[ch]++;
                if (len < 0) { scan_bad++; continue; }
                if (len >= 1 && (f[0] == 0x80 || f[0] == 0x50)) { ch_beacons[ch]++; note_beacon(f, len, dbm); } else scan_other++;
            }
            sleep_ms(2);
        }
    }
    for (int i = 0; i < nfound; i++) for (int j = i + 1; j < nfound; j++)       /* strongest first */
        if (found[j].dbm > found[i].dbm) { struct bss t = found[i]; found[i] = found[j]; found[j] = t; }
}
static void scan_report(bool show);
static int sec_of(const struct bss *b) { return b->sae && b->rsn ? WSEC_WPA3 : b->rsn ? WSEC_WPA2 : b->wpa ? WSEC_WPA : b->privacy ? WSEC_WEP : WSEC_OPEN; }
void wifi_scan(void) {
    if (!started && !sim) wifi_start();
    if (!radio_ok && !sim) { kprintf("The radio isn't ready (see 'wifi').\n"); return; }
    kprintf("Listening on channels 1-13 (about 3 seconds)...\n");
    do_scan();
    scan_report(true);
}
/* what the last scan heard, into the report and Documents/wifi-scan.txt (and on the screen if asked) */
static void scan_report(bool show) {
    static const char *secn[] = { "open", "WEP", "WPA", "WPA2", "WPA3" };
    char line[160];
    rlen = 0; report[0] = 0;
    add("Scan%s: %d frames received (%d damaged, %d not beacons), %d networks:", sim ? " (simulated)" : "", scan_frames, scan_bad, scan_other, nfound);
    for (int i = 0; i < nfound; i++) {
        struct bss *b = &found[i];
        ksnprintf(line, sizeof line, "  %-32s ch %2d  %4d dBm  %-4s  %02x:%02x:%02x:%02x:%02x:%02x", b->ssid[0] ? b->ssid : "(hidden)", b->ch, b->dbm, secn[sec_of(b)],
                  b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5]);
        add("%s", line);
    }
    if (!sim) {                                              /* per channel: frames (beacons) heard, and the radio's own channel */
        char l[200]; int o = 0;
        for (int ch = 1; ch <= 13; ch++) o += ksnprintf(l + o, sizeof l - o, " %d:%d(%d)@%d", ch, ch_frames[ch], ch_beacons[ch], ch_rf[ch]);
        add("per channel, frames(beacons)@radio's channel:%s", l);
        add("receive ring: next slot %d, re-synchronised %d times so far", rx_next, rx_resyncs);
    }
    if (!scan_frames && !sim) add("Nothing at all was received: the radio isn't hearing yet. (RCR %08x, RX DMA %02x, CR %04x, RF 0x18 %05x)", r32(REG_RCR), r8(0x0286), r16(REG_CR), rf_read(0x18));
    if (show) kprintf("%s", report);
    vfs_write("/home/Documents/wifi-scan.txt", report, (uint64_t)rlen);
    if (show) kprintf("(Saved to Documents/wifi-scan.txt)\n");
}
/* 'wifi dump': the card's registers into Documents/wifi-regs.txt, laid out as Linux's rtlwifi debugfs prints them
 * (mac_0..mac_17, bb_8..bb_1f: 16 bytes a line; rf_a: four RF registers a line), so the two can be compared on
 * a computer where Linux's driver runs the same card */
void wifi_dump(void) {
    if (!started) wifi_start();
    if (!mm) { kprintf("No Wi-Fi card to read.\n"); return; }
    uint64_t cap = 64 * 1024; char *o = kmalloc(cap); if (!o) return;
    uint64_t n = (uint64_t)ksnprintf(o, cap, "# AXIOM wifi dump: RTL8821AE registers (MAC/BB 0x0000-0x1FFF, RF path A 0x00-0xFF)\n# radio %s, firmware %s\n",
                                     radio_ok ? "set up" : "not set up", fw_ok ? "running" : "not running");
    for (int a = 0; a < 0x2000 && n < cap - 100; a += 16) {
        n += (uint64_t)ksnprintf(o + n, cap - n, "\n%8.8x  ", a);
        for (int i = 0; i < 4; i++) n += (uint64_t)ksnprintf(o + n, cap - n, "%8.8x    ", r32(a + 4 * i));
    }
    n += (uint64_t)ksnprintf(o + n, cap - n, "\n\n# rf_a");
    for (int r = 0; r <= 0xFF && n < cap - 100; r += 4) {
        n += (uint64_t)ksnprintf(o + n, cap - n, "\n%8.8x  ", r);
        for (int i = 0; i < 4; i++) n += (uint64_t)ksnprintf(o + n, cap - n, "%8.8x    ", rf_read(r + i));
    }
    n += (uint64_t)ksnprintf(o + n, cap - n, "\n");
    vfs_write("/home/Documents/wifi-regs.txt", o, n);
    kfree(o);
    kprintf("The Wi-Fi card's registers are saved to Documents/wifi-regs.txt (%lu bytes).\n", (unsigned long)n);
}
void wifi_simulate(void) {
    sim = !sim;
    kprintf(sim ? "Simulated Wi-Fi card on (for trying the Network app; it doesn't connect to anything). 'wifi simulate' again turns it off.\n"
                : "Simulated Wi-Fi card off.\n");
}

/* ---- joining a network (common/wifi_sta.c does the protocol) ----
 * The card is told who it is (MACID, so it acknowledges frames to us), the access point (BSSID) and that it's a
 * station on a network (MSR); then the station code's frames go out through tx_frame and what arrives goes to it.
 * The connection is a network adapter: AXIOM's network stack polls it (net_thread) and runs DHCP on it as it
 * does on a cable. */
static struct wifi_sta sta;
static struct netif wnif;
static bool wnif_added, joining;
static char join_msg[200];
static uint64_t join_started;
static int rx_frames_joined, ap_beacons, ap_auth, ap_deauth, ap_assoc, ap_other, ap_to_us;
static int tx_done(int q) {                                  /* frames the card has taken back (sent, or given up on) */
    int n = 0; struct txq *t = &txq[q];
    if (!t->d) return 0;
    for (int i = 0; i < NTXD; i++) if (t->d[i * 16 + 7] && !(t->d[i * 16] & (1u << 31))) n++;
    return n;
}
static void sta_send(void *ctx, const uint8_t *f, int len) { (void)ctx; tx_frame((f[0] & 0x0C) == 0x08 ? TQ_DATA : TQ_MGMT, f, len); }
static void sta_deliver(void *ctx, const uint8_t *eth, int len) { (void)ctx; net_rx(&wnif, eth, len); }
static int wnif_send(struct netif *n, const void *frame, int len) { (void)n; return sta.state == WS_CONNECTED ? wsta_send_eth(&sta, frame, len) : -1; }
static bool wnif_link(struct netif *n) { (void)n; return sta.state == WS_CONNECTED; }
static void wnif_poll(struct netif *n) {
    (void)n;
    if (!joining) return;
    int len, dbm, rate; uint8_t *f;
    for (int k = 0; k < 32 && (f = rx_take(&len, &dbm, &rate)); k++)
        if (len >= 24) {
            rx_frames_joined++;
            if (!memcmp(f + 10, sta.bssid, 6)) {              /* from the access point: what (for the log) */
                int ty = (f[0] >> 2) & 3, st = f[0] >> 4;
                if (ty == 0 && st == 8) ap_beacons++;
                else if (ty == 0 && st == 11) ap_auth++;
                else if (ty == 0 && (st == 12 || st == 10)) ap_deauth++;
                else if (ty == 0 && st == 1) ap_assoc++;
                else ap_other++;
                if (!memcmp(f + 4, mac, 6)) ap_to_us++;
            }
            wsta_rx(&sta, f, len - 4, uptime_ms());           /* (minus the FCS) */
        }
    int was = sta.state;
    wsta_tick(&sta, uptime_ms());
    if (sta.state != was || (sta.state == WS_FAILED && joining)) {
        static const char *names[] = { "idle", "authenticating", "associating", "checking the password", "connected", "failed" };
        if (sta.state == WS_CONNECTED) { ksnprintf(join_msg, sizeof join_msg, "Connected to \"%s\".", sta.ssid); net_prefer(&wnif); }
        else if (sta.state == WS_FAILED) {
            ksnprintf(join_msg, sizeof join_msg, "Couldn't join \"%s\": %s (card sent %d of %d; router: %d beacons, %d replies)", sta.ssid,
                      sta.why[0] ? sta.why : "no answer", tx_done(TQ_MGMT) + tx_done(TQ_DATA), tx_sent, ap_beacons, ap_auth + ap_assoc + ap_deauth);
            joining = false;
        } else ksnprintf(join_msg, sizeof join_msg, "Joining \"%s\": %s...", sta.ssid, names[sta.state]);
        LOG_INFO("wifi: %s (%d frames heard, %d sent, %d dropped)\n", join_msg, rx_frames_joined, tx_sent, tx_full);
        if (sta.state == WS_FAILED || sta.state == WS_CONNECTED)
            LOG_INFO("wifi: the card took back %d of the frames sent; TXDMA_STATUS %08x TXPAUSE %02x FIFOPAGE %08x CR %04x TCR %08x; "
                     "from the access point: %d beacons, %d auth, %d deauth/disassoc, %d assoc replies, %d other, %d to us\n",
                     tx_done(TQ_MGMT) + tx_done(TQ_DATA), r32(0x0210), r8(0x0522), r32(0x0204), r16(REG_CR), r32(0x0604),
                     ap_beacons, ap_auth, ap_deauth, ap_assoc, ap_other, ap_to_us);
    }
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* join the access point found[i] (pmk: NULL for an open network) */
static void wifi_join_bss(int i, const uint8_t *pmk) {
    struct bss *b = &found[i];
    if (joining) wsta_leave(&sta);
    set_channel(b->ch);
    for (int k = 0; k < 6; k++) { w8(0x0610 + k, mac[k]); w8(0x0618 + k, b->bssid[k]); }   /* MACID, BSSID */
    w8(0x0102, (uint8_t)((r8(0x0102) & ~3) | 2));             /* MSR: a station on a network */
    memset(&sta, 0, sizeof sta);
    sta.send = sta_send; sta.deliver = sta_deliver;
    uint8_t snonce[32]; random_bytes(snonce, 32);
    rx_frames_joined = 0; tx_sent = tx_full = 0; ap_beacons = ap_auth = ap_deauth = ap_assoc = ap_other = ap_to_us = 0;
    for (int q = 0; q < 2; q++) if (txq[q].d) for (int i = 0; i < NTXD; i++) txq[q].d[i * 16 + 7] = 0;   /* (for tx_done) */
    joining = true; join_started = uptime_ms();
    wsta_join(&sta, mac, b->bssid, b->ssid, pmk, snonce, uptime_ms());
    if (!wnif_added) {
        strncpy0(wnif.adapter, "Realtek RTL8821AE (Wi-Fi)", sizeof wnif.adapter); strncpy0(wnif.driver, "rtl8821ae", sizeof wnif.driver);
        memcpy(wnif.mac, mac, 6); wnif.send = wnif_send; wnif.poll = wnif_poll; wnif.tick = wnif_poll; wnif.link_up = wnif_link;
        net_add_netif(&wnif); wnif_added = true;
    }
    ksnprintf(join_msg, sizeof join_msg, "Joining \"%s\" on channel %d...", b->ssid, b->ch);
    LOG_INFO("wifi: %s\n", join_msg);
}
/* join the saved network if the last scan found it: -1 not found, 0 started */
static int wifi_join_saved(void) {
    const char *ssid = settings_get("wifi.ssid"), *hx = settings_get("wifi.pmk");
    if (!ssid || !ssid[0] || !radio_ok) return -1;
    int best = -1;
    for (int i = 0; i < nfound; i++) if (!strcmp(found[i].ssid, ssid) && (best < 0 || found[i].dbm > found[best].dbm)) best = i;
    if (best < 0) { ksnprintf(join_msg, sizeof join_msg, "\"%s\" wasn't heard in the last scan: scan again.", ssid); return -1; }
    uint8_t pmk[32]; bool secure = hx && strlen(hx) == 64;
    for (int k = 0; secure && k < 32; k++) { int h = hexval(hx[2 * k]), l = hexval(hx[2 * k + 1]); if (h < 0 || l < 0) secure = false; else pmk[k] = (uint8_t)(h << 4 | l); }
    if (sec_of(&found[best]) != WSEC_OPEN && !secure) { strncpy0(join_msg, "This network needs its password: click it to type it.", sizeof join_msg); return -1; }
    wifi_join_bss(best, secure ? pmk : 0);
    memset(pmk, 0, 32);
    return 0;
}

/* ---- for apps (SYS_WIFI) ---- */
static void hex(const uint8_t *b, int n, char *out) { static const char h[] = "0123456789abcdef"; for (int i = 0; i < n; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; } out[2 * n] = 0; }
int64_t wifi_syscall(int op, uint64_t buf, int64_t n) {
    if (op == WIFI_STATUS) {
        struct wifi_status *st = (struct wifi_status *)buf;
        memset(st, 0, sizeof *st);
        st->simulated = sim;
        strncpy0(st->saved, settings_get("wifi.ssid"), sizeof st->saved);
        if (!dev && !sim) { st->state = WIFI_NONE; return 0; }
        strncpy0(st->adapter, sim ? "Simulated Wi-Fi card" : "Realtek RTL8821AE", sizeof st->adapter);
        if (sim) { st->state = WIFI_READY; static const uint8_t m[6] = { 0x02, 0xAC, 0x51, 0x00, 0x00, 0x01 }; memcpy(st->mac, m, 6); }
        else { memcpy(st->mac, mac, 6); st->state = !started ? WIFI_OFF : !radio_ok ? WIFI_FAILED : sta.state == WS_CONNECTED ? WIFI_CONNECTED : joining ? WIFI_CONNECTING : WIFI_READY; }
        if (!sim && (joining || sta.state == WS_CONNECTED)) strncpy0(st->ssid, sta.ssid, sizeof st->ssid);
        if (st->state == WIFI_FAILED) {                          /* the last line of the report says why */
            int e = rlen - 1; while (e > 0 && report[e - 1] != '\n') e--;
            int s0 = e - 1; while (s0 > 0 && report[s0 - 1] != '\n') s0--;
            strncpy0(st->msg, report + (s0 > 0 ? s0 : 0), sizeof st->msg);
        } else strncpy0(st->msg, join_msg, sizeof st->msg);
        return 0;
    }
    if (op == WIFI_START) { if (sim) return 0; if (!dev) return -1; if (!started) wifi_start(); return radio_ok ? 0 : -1; }
    if (op == WIFI_SCAN) {
        if (!sim && !radio_ok) return -1;
        if (joining || sta.state == WS_CONNECTED) return nfound;     /* (connected: the radio stays on its channel) */
        do_scan(); scan_report(false); return nfound;
    }
    if (op == WIFI_RESULTS) {
        struct wifi_net *o = (struct wifi_net *)buf; int k = 0;
        const char *saved = settings_get("wifi.ssid");
        for (int i = 0; i < nfound && k < n; i++, k++) {
            memset(&o[k], 0, sizeof o[k]);
            strncpy0(o[k].ssid, found[i].ssid, sizeof o[k].ssid); memcpy(o[k].bssid, found[i].bssid, 6);
            o[k].ch = found[i].ch; o[k].dbm = found[i].dbm; o[k].sec = sec_of(&found[i]);
            o[k].saved = found[i].ssid[0] && !strcmp(found[i].ssid, saved);
        }
        return k;
    }
    if (op == WIFI_CONNECT) {
        struct wifi_join j; memcpy(&j, (void *)buf, sizeof j);
        j.ssid[32] = 0; j.pass[63] = 0;
        int pl = (int)strlen(j.pass);
        if (!j.ssid[0]) return -1;
        if (pl && (pl < 8 || pl > 63)) { strncpy0(join_msg, "A Wi-Fi password is 8 to 63 characters.", sizeof join_msg); return -1; }
        /* keep the key derived from the password (not the password itself); no password for the saved network:
         * use its saved key */
        if (pl || strcmp(j.ssid, settings_get("wifi.ssid"))) {
            char hx[65] = "";
            if (pl) { uint8_t pmk[32]; wpa_pmk(j.pass, (const uint8_t *)j.ssid, (int)strlen(j.ssid), pmk); hex(pmk, 32, hx); memset(pmk, 0, 32); }
            settings_set("wifi.ssid", j.ssid); settings_set("wifi.pmk", hx);
        }
        memset(j.pass, 0, sizeof j.pass);
        if (sim) { ksnprintf(join_msg, sizeof join_msg, "Saved \"%s\" (the simulated card doesn't connect).", j.ssid); return 1; }
        if (wifi_join_saved() < 0) return -1;
        return 1;                                                /* joining (the status says how it goes) */
    }
    if (op == WIFI_FORGET) {
        if (joining || sta.state == WS_CONNECTED) { wsta_leave(&sta); joining = false; w8(0x0102, r8(0x0102) & ~3); }
        settings_set("wifi.ssid", ""); settings_set("wifi.pmk", ""); join_msg[0] = 0; return 0;
    }
    return -1;
}
