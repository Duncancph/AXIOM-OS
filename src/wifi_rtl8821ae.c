// SPDX-License-Identifier: GPL-2.0
/* Realtek RTL8821AE Wi-Fi (PCIe, 802.11ac, with Bluetooth on the same card).
 *
 * This file follows Linux's rtlwifi/rtl8821ae driver (Copyright Realtek Corporation, GPL-2.0): the power-on
 * steps, register numbers and the order of the start-up are taken from it, so this file is under the GPL v2.
 *
 * Stage 1 (being tried on the real card): find the card, read its built-in settings (eFuse: the MAC address
 * among them), power it up, set up its packet memory, and load and start Realtek's firmware
 * (/system/firmware/rtlwifi/rtl8821aefw_29.bin). Nothing here runs at start-up: 'wifi start' does it, and
 * 'wifi' shows what happened, so a surprise on new hardware can't stop AXIOM from starting. */
#include "kernel.h"
#include "vfs.h"

static struct pci_dev *dev;
static volatile uint8_t *mm;
static char report[3000]; static int rlen;
static bool started, fw_ok;
static uint8_t efuse[512], mac[6];

static void add(const char *f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    char line[200]; kvsnprintf(line, sizeof line, f, ap); __builtin_va_end(ap);
    int n = ksnprintf(report + rlen, sizeof report - rlen, "%s\n", line);
    if (n > 0 && rlen + n < (int)sizeof report) rlen += n;
}
static uint8_t r8(int o) { return mm[o]; }
static uint16_t r16(int o) { return *(volatile uint16_t *)(mm + o); }
static uint32_t r32(int o) { return *(volatile uint32_t *)(mm + o); }
static void w8(int o, uint8_t v) { mm[o] = v; }
static void w16(int o, uint16_t v) { *(volatile uint16_t *)(mm + o) = v; }
static void w32(int o, uint32_t v) { *(volatile uint32_t *)(mm + o) = v; }

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
    return true;
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
    mm = map_mmio(base, 0x4000);
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
    add(fw_ok ? "Stage 1 done: the card is on and its firmware is running. (Scanning for networks comes next.)"
              : "Stage 1 stopped: see above.");
    kprintf("%s", report);
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
