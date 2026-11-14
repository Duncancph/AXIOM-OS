/* Intel graphics (Haswell and similar), step 1: look only. Reads how the firmware set up the screen
 * (which pipe, transcoder, port, clock and timings) and asks the monitor for its EDID (its name and
 * its own best resolution) over the HDMI/DVI wire (GMBUS) or over DisplayPort (AUX). Nothing is
 * changed. The report is shown in Settings > Display and written to the log; with it, the next step
 * (switching the screen to the monitor's own resolution) can be written for the exact setup. */
#include "kernel.h"

static volatile uint8_t *mmio;
static char report[3000]; static int rlen;
static char summary[160], monitor[160];

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static void add(const char *f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    char line[200]; kvsnprintf(line, sizeof line, f, ap);
    __builtin_va_end(ap);
    rlen += ksnprintf(report + rlen, sizeof report - rlen, "%s\n", line);
    kprintf("[gfx] %s\n", line);
}

/* ---- EDID over the HDMI/DVI/VGA wire: the PCH's GMBUS (I2C) controller ---- */
#define GMBUS0 0xC5100
#define GMBUS1 0xC5104
#define GMBUS2 0xC5108
#define GMBUS3 0xC510C
static bool gmbus_wait(uint32_t bit) {
    for (int i = 0; i < 500; i++) {
        uint32_t s = rd(GMBUS2);
        if (s & (1 << 10)) return false;                 /* NAK: nothing answered */
        if (s & bit) return true;
        udelay(100);
    }
    return false;
}
static bool gmbus_edid(int pin, uint8_t *e) {
    wr(GMBUS0, pin);                                     /* 100 kHz, this pin pair */
    wr(GMBUS1, (1u << 30) | (1u << 26) | (1u << 25) | (128u << 16) | (0x50 << 1) | 1);   /* index 0, read 128 bytes from 0x50 */
    bool ok = true;
    for (int i = 0; i < 128 && ok; i += 4) {
        if (!gmbus_wait(1 << 11)) { ok = false; break; }
        uint32_t v = rd(GMBUS3);
        for (int k = 0; k < 4 && i + k < 128; k++) e[i + k] = v >> (8 * k);
    }
    wr(GMBUS1, (1u << 30) | (4u << 25));                 /* stop */
    for (int i = 0; i < 100 && (rd(GMBUS2) & (1 << 9)); i++) udelay(100);
    if (!ok) { wr(GMBUS1, 1u << 31); wr(GMBUS1, 0); }   /* clear the error */
    wr(GMBUS0, 0);
    return ok;
}

/* ---- EDID over DisplayPort: the AUX channel, I2C-over-AUX ---- */
static bool aux_xfer(int port, const uint8_t *msg, int mlen, uint8_t *rep, int rmax, int *rlen_out) {
    uint32_t ctl = 0x64010 + port * 0x100;
    for (int tries = 0; tries < 5; tries++) {
        for (int i = 0; i < 5; i++) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) if (i * 4 + k < mlen) v |= (uint32_t)msg[i * 4 + k] << (24 - 8 * k);
            wr(ctl + 4 + i * 4, v);
        }
        uint32_t div = port == 0 ? 225 : 63;            /* 450 MHz cdclk / 2 MHz for port A, 125 MHz / 2 for the others */
        wr(ctl, (1u << 31) | (1u << 30) | (1u << 28) | (1u << 25) | (3u << 26) | ((uint32_t)mlen << 20) | (5u << 16) | div);
        int i = 0;
        while ((rd(ctl) & (1u << 31)) && i++ < 400) udelay(10);
        uint32_t s = rd(ctl);
        wr(ctl, s | (1u << 30) | (1u << 28) | (1u << 25));
        if (s & (1u << 31)) continue;                    /* still busy */
        if (s & ((1u << 28) | (1u << 25))) { udelay(400); continue; }   /* timeout / receive error: try again */
        int n = (s >> 20) & 31;
        if (n > rmax) n = rmax;
        for (int k = 0; k < n; k++) rep[k] = rd(ctl + 4 + (k / 4) * 4) >> (24 - 8 * (k % 4));
        *rlen_out = n;
        return true;
    }
    return false;
}
static bool aux_edid(int port, uint8_t *e) {
    uint8_t rep[20]; int n;
    uint8_t w[5] = { 0x40, 0x00, 0x50, 0x00, 0x00 };   /* I2C write (MOT) to 0x50: offset 0 */
    if (!aux_xfer(port, w, 5, rep, 20, &n) || n < 1 || (rep[0] & 0xF0)) return false;
    for (int off = 0; off < 128; off += 16) {
        uint8_t r[4] = { 0x50, 0x00, 0x50, 15 };        /* I2C read (MOT), 16 bytes */
        int got = 0;
        for (int t = 0; t < 7 && !got; t++) {
            if (!aux_xfer(port, r, 4, rep, 20, &n)) return false;
            if ((rep[0] & 0xF0) == 0x20 || (rep[0] & 0xF0) == 0x80 || n < 17) { udelay(500); continue; }   /* defer */
            if (rep[0] & 0xF0) return false;
            memcpy(e + off, rep + 1, 16); got = 1;
        }
        if (!got) return false;
    }
    uint8_t stop[3] = { 0x10, 0x00, 0x50 };              /* I2C read without MOT: end */
    aux_xfer(port, stop, 3, rep, 20, &n);
    return true;
}

static bool edid_ok(const uint8_t *e) {
    static const uint8_t hdr[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };
    if (memcmp(e, hdr, 8)) return false;
    uint8_t s = 0; for (int i = 0; i < 128; i++) s += e[i];
    return s == 0;
}
static void edid_describe(const uint8_t *e, const char *how) {
    char name[16] = "";
    for (int d = 0; d < 4; d++) {
        const uint8_t *b = e + 54 + d * 18;
        if (!b[0] && !b[1] && b[3] == 0xFC) {
            int k = 0; for (int i = 5; i < 18 && b[i] != '\n' && k < 15; i++) name[k++] = b[i];
            name[k] = 0;
        }
    }
    char mfg[4] = { (char)('@' + ((e[8] >> 2) & 31)), (char)('@' + (((e[8] & 3) << 3) | (e[9] >> 5))), (char)('@' + (e[9] & 31)), 0 };
    const uint8_t *t = e + 54;                           /* the first detailed timing: the monitor's own mode */
    uint32_t clk = (t[0] | t[1] << 8) * 10;              /* kHz */
    int ha = t[2] | (t[4] & 0xF0) << 4, hb = t[3] | (t[4] & 15) << 8;
    int va = t[5] | (t[7] & 0xF0) << 4, vb = t[6] | (t[7] & 15) << 8;
    int hso = t[8] | (t[11] & 0xC0) << 2, hsw = t[9] | (t[11] & 0x30) << 4;
    int vso = t[10] >> 4 | (t[11] & 0x0C) << 2, vsw = (t[10] & 15) | (t[11] & 3) << 4;
    int hz = clk && ha + hb && va + vb ? (int)((uint64_t)clk * 1000 / ((uint64_t)(ha + hb) * (va + vb))) : 0;
    ksnprintf(monitor, sizeof monitor, "%s%s%s, its own resolution %dx%d at %d Hz", mfg, name[0] ? " " : "", name, ha, va, hz);
    add("monitor (EDID via %s): %s", how, monitor);
    add("  native timing: clock %u kHz, h %d+%d (sync +%d w%d), v %d+%d (sync +%d w%d), flags %02x",
        clk, ha, hb, hso, hsw, va, vb, vso, vsw, t[17]);
    add("  EDID version %d.%d, %d extension block(s)", e[18], e[19], e[126]);
}

void intelgfx_probe(void) {
    struct pci_dev *g = 0;
    for (int i = 0; i < pci_count; i++)
        if (pci_devs[i].vendor == 0x8086 && pci_devs[i].class_code == 3) { g = &pci_devs[i]; break; }
    if (!g) {
#ifdef GFX_FAKE
        ksnprintf(summary, sizeof summary, "Intel graphics (0d22), screen on port B");
        ksnprintf(monitor, sizeof monitor, "DEL DELL U2414H, its own resolution 1920x1080 at 60 Hz");
        for (int i = 0; i < 14; i++) add("transcoder A: conf c0000000, ddi 98000000 -> port B, HDMI, bpc code 0, pipe A, timings 0x%08x and a long tail of text %d", i * 1234567, i);
#endif
        return;
    }
    uint64_t sz, bar0 = pci_bar(g, 0, &sz), sz2, bar2 = pci_bar(g, 2, &sz2);
    ksnprintf(summary, sizeof summary, "Intel graphics, device %04x", g->device);
    add("Intel graphics %04x rev %02x at %02x:%02x.%x; registers %lx (%lu KB), memory window %lx (%lu MB)",
        g->device, g->rev, g->bus, g->dev, g->fn, bar0, sz >> 10, bar2, sz2 >> 20);
    add("screen from firmware: %ux%u, pitch %u, at %lx", bootinfo.fb_w, bootinfo.fb_h, bootinfo.fb_pitch, bootinfo.fb_addr);
    uint16_t gen = g->device >> 8;
    bool hsw = gen == 0x04 || gen == 0x0A || gen == 0x0C || gen == 0x0D;
    if (!bar0 || sz < 0x100000) { add("no register window"); return; }
    if (!hsw) add("note: not a Haswell chip (only read, nothing changed)");
    mmio = map_mmio(bar0, sz > (4u << 20) ? (4u << 20) : sz);
    uint32_t cmd = pci_read16(g->bus, g->dev, g->fn, 4);
    if (!(cmd & 2)) { pci_write16(g->bus, g->dev, g->fn, 4, cmd | 6); }   /* memory decoding on */

    add("power well %08x, LCPLL %08x, fuse straps %08x", rd(0x45404), rd(0x130040), rd(0xC2014));
    static const char *tn[4] = { "A", "B", "C", "eDP" };
    static const uint32_t tb[4] = { 0x60000, 0x61000, 0x62000, 0x6F000 };
    static const char *modes[8] = { "HDMI", "DVI", "DisplayPort", "DP-MST", "FDI", "?", "?", "?" };
    static const char *clks[8] = { "LCPLL 2700", "LCPLL 1350", "LCPLL 810", "SPLL", "WRPLL1", "WRPLL2", "?", "none" };
    int used_port = -1;
    for (int t = 0; t < 4; t++) {
        uint32_t conf = rd(tb[t] == 0x6F000 ? 0x7F008 : 0x70008 + 0x1000 * t), fn = rd(tb[t] + 0x400);
        if (!(conf & (1u << 31)) && !(fn & (1u << 31))) continue;
        int port = (fn >> 28) & 7, mode = (fn >> 24) & 7, bpc = (fn >> 20) & 7;
        int pipe = t < 3 ? t : (int)((fn >> 12) & 7) == 0 ? 0 : ((fn >> 12) & 7) == 5 ? 1 : ((fn >> 12) & 7) == 6 ? 2 : 0;
        add("transcoder %s: conf %08x, ddi %08x -> port %c, %s, bpc code %d, pipe %c", tn[t], conf, fn, 'A' + port, modes[mode], bpc, 'A' + pipe);
        uint32_t ht = rd(tb[t]), hbk = rd(tb[t] + 4), hs = rd(tb[t] + 8), vt = rd(tb[t] + 12), vbk = rd(tb[t] + 16), vs = rd(tb[t] + 20);
        add("  timings: htotal %08x hblank %08x hsync %08x | vtotal %08x vblank %08x vsync %08x", ht, hbk, hs, vt, vbk, vs);
        add("  = %ux%u active, %ux%u total", (ht & 0xFFFF) + 1, (vt & 0xFFFF) + 1, (ht >> 16) + 1, (vt >> 16) + 1);
        add("  M/N: data %08x/%08x link %08x/%08x", rd(tb[t] + 0x30), rd(tb[t] + 0x34), rd(tb[t] + 0x40), rd(tb[t] + 0x44));
        uint32_t pb = 0x70000 + 0x1000 * pipe;
        add("  pipe %c: source %08x, plane ctl %08x stride %u surface %08x offset %08x", 'A' + pipe, rd(0x6001C + 0x1000 * pipe),
            rd(pb + 0x180), rd(pb + 0x188), rd(pb + 0x19C), rd(pb + 0x1A4));
        uint32_t pf = 0x68000 + 0x800 * pipe;
        add("  panel fitter: ctl %08x, window pos %08x size %08x", rd(pf + 0x80), rd(pf + 0x70), rd(pf + 0x74));
        used_port = port;
    }
    for (int p = 0; p < 5; p++) {
        uint32_t buf = rd(0x64000 + 0x100 * p), cs = rd(0x46100 + 4 * p);
        if (!(buf & (1u << 31)) && (cs >> 29) == 7) continue;
        add("port %c: buffer %08x, clock %s (%08x), DP training %08x", 'A' + p, buf, clks[cs >> 29], cs, rd(0x64040 + 0x100 * p));
    }
    add("PLLs: WRPLL1 %08x, WRPLL2 %08x, SPLL %08x", rd(0x46040), rd(0x46060), rd(0x46020));

    /* the monitor: try the wire of the port in use first, then the others */
    uint8_t e[128]; bool got = false;
    static const int pins[3] = { 5, 4, 6 };              /* GMBUS pin pairs for ports B, C, D */
    static const char *pn[3] = { "HDMI/DVI wire of port B", "HDMI/DVI wire of port C", "HDMI/DVI wire of port D" };
    for (int k = 0; k < 2 && !got; k++)
        for (int i = 0; i < 3 && !got; i++) {
            if (k == 0 && used_port != i + 1) continue;
            if (k == 1 && used_port == i + 1) continue;
            if (gmbus_edid(pins[i], e) && edid_ok(e)) { got = true; edid_describe(e, pn[i]); }
        }
    for (int p = 0; p < 4 && !got; p++) {
        if (aux_edid(p, e) && edid_ok(e)) { char how[40]; ksnprintf(how, sizeof how, "DisplayPort AUX of port %c", 'A' + p); got = true; edid_describe(e, how); }
    }
    if (!got) add("monitor: didn't answer (no EDID on any port)");
    ksnprintf(summary, sizeof summary, "Intel graphics (%04x)%s%s", g->device,
              used_port >= 0 ? ", screen on port " : "", used_port >= 0 ? (const char[]){ (char)('A' + used_port), 0 } : "");
}

const char *intelgfx_summary(void) { return summary; }
const char *intelgfx_monitor(void) { return monitor; }
const char *intelgfx_report(void) { return report; }
