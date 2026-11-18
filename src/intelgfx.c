/* Intel graphics (Haswell and similar), step 1: look only. Reads how the firmware set up the screen
 * (which pipe, transcoder, port, clock and timings) and asks the monitor for its EDID (its name and
 * its own best resolution) over the HDMI/DVI wire (GMBUS) or over DisplayPort (AUX). Nothing is
 * changed. The report is shown in Settings > Display and written to the log; with it, the next step
 * (switching the screen to the monitor's own resolution) can be written for the exact setup. */
#include "kernel.h"

static volatile uint8_t *mmio;
/* what the probe found, for the mode switch (step 2) */
static int cur_trans = -1, cur_port = -1, cur_mode = -1, cur_pipe = -1;
static uint8_t edid[256]; static int edid_len;
struct dmode { int w, h, hz; uint32_t clk; int hss, hse, ht, vss, vse, vt; bool phs, pvs; };
static struct dmode want_mode; static bool have_want;
static bool native_on;
static uint16_t gdev;
static bool native_gave_up;                                 /* the last start with it didn't finish: turned off */
bool intelgfx_native_active(void) { return native_on; }
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
static bool gmbus_read(int pin, int offset, int len, uint8_t *e) {
    wr(GMBUS0, pin);                                     /* 100 kHz, this pin pair */
    wr(GMBUS1, (1u << 30) | (1u << 26) | (1u << 25) | ((uint32_t)offset << 8) | ((uint32_t)len << 16) | (0x50 << 1) | 1);   /* read from 0x50 at offset */
    bool ok = true;
    for (int i = 0; i < len && ok; i += 4) {
        if (!gmbus_wait(1 << 11)) { ok = false; break; }
        uint32_t v = rd(GMBUS3);
        for (int k = 0; k < 4 && i + k < len; k++) e[i + k] = v >> (8 * k);
    }
    wr(GMBUS1, (1u << 30) | (4u << 25));                 /* stop */
    for (int i = 0; i < 100 && (rd(GMBUS2) & (1 << 9)); i++) udelay(100);
    if (!ok) { wr(GMBUS1, 1u << 31); wr(GMBUS1, 0); }   /* clear the error */
    wr(GMBUS0, 0);
    return ok;
}
static bool gmbus_edid(int pin, uint8_t *e) { return gmbus_read(pin, 0, 128, e); }

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

static void find_modes(void);
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
    gdev = g->device;
    uint64_t sz, bar0 = pci_bar(g, 0, &sz), sz2, bar2 = pci_bar(g, 2, &sz2);
    ksnprintf(summary, sizeof summary, "Intel graphics, device %04x", g->device);
    dev_claim(g, "intelgfx", DEV_OK, "Intel HD Graphics", "Screen as the firmware set it up; AXIOM reads the monitor's details and sends HDMI sound");
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
        if (cur_trans < 0) { cur_trans = t; cur_port = port; cur_mode = mode; cur_pipe = pipe; }
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
            if (gmbus_edid(pins[i], e) && edid_ok(e)) {
                got = true; edid_describe(e, pn[i]);
                memcpy(edid, e, 128); edid_len = 128;
                if (e[126] >= 1 && gmbus_read(pins[i], 128, 128, edid + 128)) {   /* the extension (CEA-861: TV/HDMI modes) */
                    uint8_t sum = 0; for (int k = 128; k < 256; k++) sum += edid[k];
                    if (!sum) edid_len = 256; else add("  EDID extension block: bad checksum");
                }
            }
        }
    for (int p = 0; p < 4 && !got; p++) {
        if (aux_edid(p, e) && edid_ok(e)) { char how[40]; ksnprintf(how, sizeof how, "DisplayPort AUX of port %c", 'A' + p); got = true; edid_describe(e, how); }
    }
    if (!got) add("monitor: didn't answer (no EDID on any port)");
    if (edid_len) find_modes();
    ksnprintf(summary, sizeof summary, "Intel graphics (%04x)%s%s", g->device,
              used_port >= 0 ? ", screen on port " : "", used_port >= 0 ? (const char[]){ (char)('A' + used_port), 0 } : "");
}

/* ---------------- step 2: switching the screen to the monitor's full size ----------------
 * Only for the case the probe has seen: Haswell, one HDMI/DVI port driven by transcoder A / pipe A
 * with WRPLL1 as its clock. The firmware's framebuffer stays where it is (graphics memory offset 0);
 * only the timings, the pixel clock and the plane are changed. */
static void dtd_mode(const uint8_t *t, struct dmode *m) {
    m->clk = (t[0] | t[1] << 8) * 10;
    int ha = t[2] | (t[4] & 0xF0) << 4, hb = t[3] | (t[4] & 15) << 8;
    int va = t[5] | (t[7] & 0xF0) << 4, vb = t[6] | (t[7] & 15) << 8;
    int hso = t[8] | (t[11] & 0xC0) << 2, hsw = t[9] | (t[11] & 0x30) << 4;
    int vso = t[10] >> 4 | (t[11] & 0x0C) << 2, vsw = (t[10] & 15) | (t[11] & 3) << 4;
    m->w = ha; m->h = va; m->ht = ha + hb; m->vt = va + vb;
    m->hss = ha + hso; m->hse = ha + hso + hsw; m->vss = va + vso; m->vse = va + vso + vsw;
    m->phs = (t[17] & 0x18) == 0x18 && (t[17] & 2); m->pvs = (t[17] & 0x18) == 0x18 && (t[17] & 4);
    m->hz = m->ht && m->vt ? (int)((uint64_t)m->clk * 1000 / ((uint64_t)m->ht * m->vt)) : 0;
}
static bool mode_ok(const struct dmode *m) {           /* progressive, sizes we can draw, a clock the PLL can make */
    return m->clk >= 25000 && m->clk <= 300000 && m->w >= 1024 && m->w <= 2560 && m->h >= 600 && m->h <= 1600 && m->hz >= 50 && m->hz <= 75;
}
static void consider(const struct dmode *m, const char *from) {
    if (!mode_ok(m)) return;
    add("  mode %dx%d at %d Hz (%u kHz) - %s", m->w, m->h, m->hz, m->clk, from);
    /* the most pixels wins; at equal size the one nearest 60 Hz */
    if (!have_want || (int64_t)m->w * m->h > (int64_t)want_mode.w * want_mode.h ||
        ((int64_t)m->w * m->h == (int64_t)want_mode.w * want_mode.h && (m->hz - 60) * (m->hz - 60) < (want_mode.hz - 60) * (want_mode.hz - 60))) {
        want_mode = *m; have_want = true;
    }
}
static void find_modes(void) {
    add("modes the monitor lists:");
    for (int d = 0; d < 4; d++) { const uint8_t *t = edid + 54 + d * 18; if (t[0] || t[1]) { struct dmode m; dtd_mode(t, &m); consider(&m, "EDID detailed timing"); } }
    if (edid_len < 256 || edid[128] != 2) return;        /* CEA-861 extension */
    const uint8_t *x = edid + 128; int dtd = x[2];
    for (int i = 4; i < dtd && i < 127;) {               /* data blocks: the video block lists standard TV modes by number */
        int tag = x[i] >> 5, len = x[i] & 31;
        if (tag == 2) for (int k = 1; k <= len && i + k < 127; k++) {
            int vic = x[i + k] & 127;
            static const struct dmode cea[] = {           /* VIC 16, 31, 4, 19: 1080p60, 1080p50, 720p60, 720p50 */
                { 1920, 1080, 60, 148500, 2008, 2052, 2200, 1084, 1089, 1125, true, true },
                { 1920, 1080, 50, 148500, 2448, 2492, 2640, 1084, 1089, 1125, true, true },
                { 1280, 720, 60, 74250, 1390, 1430, 1650, 725, 730, 750, true, true },
                { 1280, 720, 50, 74250, 1720, 1760, 1980, 725, 730, 750, true, true } };
            int ix = vic == 16 ? 0 : vic == 31 ? 1 : vic == 4 ? 2 : vic == 19 ? 3 : -1;
            if (ix >= 0) { char f[40]; ksnprintf(f, sizeof f, "HDMI standard mode %d", vic); consider(&cea[ix], f); }
        }
        i += 1 + len;
    }
    for (int i = dtd; i >= 4 && i + 18 <= 127; i += 18) { if (!x[i] && !x[i + 1]) break; struct dmode m; dtd_mode(x + i, &m); consider(&m, "EDID extension timing"); }
}

/* WRPLL dividers for a pixel clock (Linux's search, simplified): VCO = 2700 MHz * n2 / r2 in 2400..4800 MHz,
 * reference 2700*2/r2 in 48..400 MHz, pixel clock = VCO / (p * 5) */
static bool wrpll_for(uint32_t khz, uint32_t *val) {
    uint64_t best_err = ~0ULL; int br = 0, bn = 0, bp = 0;
    for (int r2 = 2700 * 2 / 400 + 1; r2 <= 2700 * 2 / 48; r2++)
        for (int n2 = 2400 * r2 / 2700 + 1; n2 <= 4800 * r2 / 2700; n2++)
            for (int p = 2; p <= 64; p += 2) {
                uint64_t f = 2700000ULL * n2 / ((uint64_t)r2 * p * 5);          /* kHz */
                uint64_t err = f > khz ? f - khz : khz - f;
                if (err < best_err) { best_err = err; br = r2; bn = n2; bp = p; }
            }
    if (!br || best_err * 1000 > (uint64_t)khz * 1) return false;          /* within 0.1% */
    *val = (1u << 31) | (3u << 28) | ((uint32_t)bn << 16) | ((uint32_t)bp << 8) | (uint32_t)br;   /* enable, LCPLL reference */
    return true;
}
static bool wait_bit(uint32_t reg, uint32_t bit, bool set, int ms) {
    for (int i = 0; i < ms * 10; i++) { if (!!(rd(reg) & bit) == set) return true; udelay(100); }
    return false;
}
static bool do_switch(void) {
    const struct dmode *m = &want_mode;
    if (cur_trans != 0 || cur_pipe != 0 || cur_port < 1 || cur_port > 3 || (cur_mode != 0 && cur_mode != 1)) { add("switch: not an HDMI/DVI port on transcoder A - not switching"); return false; }
    if ((rd(0x46100 + 4 * cur_port) >> 29) != 4) { add("switch: the port isn't clocked by WRPLL1 - not switching"); return false; }
    uint32_t pll; if (!wrpll_for(m->clk, &pll)) { add("switch: no clock setting for %u kHz", m->clk); return false; }
    uint32_t stride = (uint32_t)m->w * 4, need = stride * (uint32_t)m->h;
    /* the graphics memory behind the framebuffer: the firmware's map (GTT) must cover the bigger picture */
    volatile uint32_t *gtt = (volatile uint32_t *)(mmio + 0x200000);
    uint32_t pte0 = gtt[0], pages = (need + 4095) / 4096, mapped = 0;
    for (uint32_t i = 0; i < pages; i++) { uint32_t e = gtt[i]; if (!(e & 1) || (e & ~0xFFFu) != (pte0 & ~0xFFFu) + i * 4096) break; mapped++; }
    add("switch: GTT maps %u of the %u pages needed (first entry %08x)", mapped, pages, pte0);
    if (mapped < pages) { add("switch: graphics memory too small - not switching"); return false; }
    uint32_t ddi = rd(0x60400), bufc = rd(0x64000 + 0x100 * cur_port), conf = rd(0x70008), plane = rd(0x70180);
    add("switch: to %dx%d at %d Hz, WRPLL %08x", m->w, m->h, m->hz, pll);
    /* off: plane, pipe, panel fitter, transcoder, port, clock */
    wr(0x70180, plane & ~(1u << 31)); wr(0x7019C, rd(0x7019C));
    wr(0x70008, conf & ~(1u << 31)); wait_bit(0x70008, 1u << 30, false, 100);
    wr(0x68080, 0); wr(0x68074, 0);
    wr(0x60400, ddi & ~((1u << 31) | (7u << 28)));
    wr(0x64000 + 0x100 * cur_port, bufc & ~(1u << 31)); wait_bit(0x64000 + 0x100 * cur_port, 1u << 7, true, 20);
    wr(0x46100 + 4 * cur_port, 7u << 29);
    wr(0x46040, rd(0x46040) & ~(1u << 31)); udelay(200);
    /* on: clock, timings, transcoder, port, pipe, plane */
    wr(0x46040, pll); udelay(500);
    wr(0x46100 + 4 * cur_port, 4u << 29);
    wr(0x60000, (uint32_t)(m->w - 1) | (uint32_t)(m->ht - 1) << 16);
    wr(0x60004, (uint32_t)(m->w - 1) | (uint32_t)(m->ht - 1) << 16);
    wr(0x60008, (uint32_t)(m->hss - 1) | (uint32_t)(m->hse - 1) << 16);
    wr(0x6000C, (uint32_t)(m->h - 1) | (uint32_t)(m->vt - 1) << 16);
    wr(0x60010, (uint32_t)(m->h - 1) | (uint32_t)(m->vt - 1) << 16);
    wr(0x60014, (uint32_t)(m->vss - 1) | (uint32_t)(m->vse - 1) << 16);
    wr(0x6001C, (uint32_t)(m->w - 1) << 16 | (uint32_t)(m->h - 1));
    uint32_t nddi = (ddi & ~(3u << 16)) | (1u << 31) | ((uint32_t)cur_port << 28) | (m->phs ? 1u << 16 : 0) | (m->pvs ? 1u << 17 : 0);
    wr(0x60400, nddi);
    wr(0x64000 + 0x100 * cur_port, bufc | (1u << 31)); udelay(600);
    wr(0x70008, conf | (1u << 31));
    bool on = wait_bit(0x70008, 1u << 30, true, 100);
    wr(0x70188, stride); wr(0x70184, 0); wr(0x701A4, 0);
    wr(0x70180, plane | (1u << 31)); wr(0x7019C, 0);
    add("switch: done, pipe %s", on ? "running" : "NOT running");
    bootinfo.fb_w = m->w; bootinfo.fb_h = m->h; bootinfo.fb_pitch = stride;
    return true;
}

/* Settings: display.native = off | try | on. "try" switches once and asks (desktop) whether to keep it;
 * display.native_check is set while an attempt hasn't been confirmed or settled: if it's still set at
 * the next start, that start went wrong, and the switch is turned off. */
/* sleep: the pipe stops sending a picture, so the monitor goes into standby; and back */
void intelgfx_display(bool on) {
    static bool off; static uint32_t plane, conf;
    if (!native_on || on == !off) return;
    if (!on) {
        plane = rd(0x70180); conf = rd(0x70008);
        wr(0x70180, plane & ~(1u << 31)); wr(0x7019C, rd(0x7019C));
        wr(0x70008, conf & ~(1u << 31)); wait_bit(0x70008, 1u << 30, false, 100);
        off = true;
    } else {
        wr(0x70008, conf | (1u << 31)); wait_bit(0x70008, 1u << 30, true, 100);
        wr(0x70180, plane | (1u << 31)); wr(0x7019C, rd(0x7019C));
        off = false;
    }
}

void intelgfx_maybe_switch(void) {
    const char *v = settings_get("display.native"), *chk = settings_get("display.native_check");
    if (!v || !strcmp(v, "off") || !*v) return;
    if (chk && !strcmp(chk, "1")) {
        add("switch: the last start with it didn't finish - turned off");
        native_gave_up = true;
        settings_set("display.native", "off"); settings_set("display.native_check", "0");
        return;
    }
    if (!mmio || !have_want) return;
    if ((uint32_t)want_mode.w == bootinfo.fb_w && (uint32_t)want_mode.h == bootinfo.fb_h) return;   /* already there */
    settings_set("display.native_check", "1");
    if (!strcmp(v, "try")) settings_set("display.native", "asking");
    native_on = do_switch();
    if (!native_on) { settings_set("display.native", "off"); settings_set("display.native_check", "0"); }
}
bool intelgfx_native_asking(void) {
#ifdef GFX_FAKE
    return true;
#endif
    const char *v = settings_get("display.native"); return native_on && v && !strcmp(v, "asking"); }
void intelgfx_native_answer(bool keep) {
    if (keep) { settings_set("display.native", "on"); settings_set("display.native_check", "0"); return; }
    settings_set("display.native", "off"); settings_set("display.native_check", "0");
    acpi_reboot();
}
void intelgfx_native_settled(void) {                       /* 20 s after a start with "on" */
    if (native_gave_up) desktop_notify("Screen size", "AXIOM went back to the computer's own screen size because the last start with the "
                                       "monitor's full size didn't finish. Turn it on again in Settings > Display.");
    const char *v = settings_get("display.native");
    if (native_on && v && !strcmp(v, "on")) settings_set("display.native_check", "0");
}
const char *intelgfx_offer(void) {                          /* for Settings: the size it would switch to */
    static char o[48]; o[0] = 0;
    if (mmio && have_want && ((uint32_t)want_mode.w != bootinfo.fb_w || (uint32_t)want_mode.h != bootinfo.fb_h || native_on))
        ksnprintf(o, sizeof o, "%dx%d", want_mode.w, want_mode.h);
    return o;
}

const char *intelgfx_summary(void) { return summary; }
const char *intelgfx_monitor(void) { return monitor; }
const char *intelgfx_report(void) { return report; }

/* ---------------- sound over HDMI (Haswell) ----------------
 * The HDMI sound chip (the HD Audio controller at 00:03.0) only plays through the monitor when the
 * graphics side is ready for it: the display power well on (it powers the sound link), the port in
 * HDMI mode (DVI carries no sound), the monitor's sound abilities handed over as an "ELD" (made from
 * its EDID), the audio clock matching the pixel clock, and audio output turned on for the transcoder.
 * Called by the sound driver before it starts that controller. */
static bool audio_on;
static uint32_t port_khz(void) {                         /* the pixel clock of the port in use, from its PLL */
    if (cur_port < 0) return 0;
    uint32_t sel = rd(0x46100 + 4 * cur_port) >> 29;
    if (sel != 4 && sel != 5) return 0;
    uint32_t v = rd(sel == 4 ? 0x46040 : 0x46060);
    uint32_t r2 = v & 0xFF, p = (v >> 8) & 0x3F, n2 = (v >> 16) & 0xFF, ref = ((v >> 28) & 3) == 1 ? 135000 : 2700000;
    if (!r2 || !p) return 0;
    return (uint32_t)((uint64_t)ref * n2 / ((uint64_t)r2 * p * 5));
}
static int build_eld(uint8_t *eld) {                     /* ELD version 2, from the EDID (CEA-861 audio blocks) */
    memset(eld, 0, 84);
    char name[16] = ""; int mnl = 0;
    if (edid_len >= 128)
        for (int d = 0; d < 4; d++) {
            const uint8_t *b = edid + 54 + d * 18;
            if (!b[0] && !b[1] && b[3] == 0xFC) { mnl = 0; for (int i = 5; i < 18 && b[i] != '\n' && mnl < 15; i++) name[mnl++] = (char)b[i]; }
        }
    while (mnl && name[mnl - 1] == ' ') mnl--;
    uint8_t sad[45]; int nsad = 0, spk = 0, cea_ver = 3;
    if (edid_len == 256 && edid[128] == 2) {
        cea_ver = edid[129] >= 3 ? 3 : edid[129];
        int dend = edid[130];
        if (dend > 127) dend = 127;
        for (int i = 132; i < 128 + dend;) {
            int tag = edid[i] >> 5, len = edid[i] & 31;
            if (i + 1 + len > 128 + dend) break;
            if (tag == 1) for (int k = 0; k + 2 < len && nsad < 15; k += 3) { memcpy(sad + nsad * 3, edid + i + 1 + k, 3); nsad++; }
            if (tag == 4 && len >= 1) spk = edid[i + 1];
            i += len + 1;
        }
    }
    if (!nsad) { sad[0] = 0x09; sad[1] = 0x07; sad[2] = 0x07; nsad = 1; }   /* stereo PCM, 32/44.1/48 kHz, 16/20/24 bit */
    if (!spk) spk = 1;                                                       /* front left and right */
    eld[0] = 2 << 3;
    eld[4] = (uint8_t)(cea_ver << 5 | mnl);
    eld[5] = (uint8_t)(nsad << 4);                                           /* connection type 0: HDMI */
    eld[7] = (uint8_t)spk;
    if (edid_len >= 128) { eld[16] = edid[8]; eld[17] = edid[9]; eld[18] = edid[10]; eld[19] = edid[11]; }
    memcpy(eld + 20, name, mnl);
    memcpy(eld + 20 + mnl, sad, nsad * 3);
    int total = 20 + mnl + nsad * 3;
    eld[2] = (uint8_t)((total - 4 + 3) / 4);                                 /* baseline length in dwords */
    return (total + 3) & ~3;
}
static void audio_config(void) {                          /* the audio clock follows the pixel clock */
    static const uint32_t clk[14] = { 25175, 25200, 27000, 27027, 54000, 54054, 74176, 74250, 148352, 148500, 296703, 297000, 593407, 594000 };
    uint32_t khz = port_khz();
    int idx = 1;
    for (int i = 0; i < 14; i++) { uint32_t d = khz > clk[i] ? khz - clk[i] : clk[i] - khz; if (d * 500 <= clk[i]) { idx = i; break; } }
    uint32_t reg = 0x65000 + 0x100 * cur_trans, ac = rd(reg);
    ac &= ~((1u << 29) | (0xFu << 16) | (1u << 28));        /* HDMI, pixel clock index, automatic N */
    ac |= (uint32_t)idx << 16;
    wr(reg, ac);
    add("audio: pixel clock %u kHz -> audio clock setting %d (%u kHz)%s", khz, idx, clk[idx],
        idx == 1 && khz != 25200 ? " - not a standard HDMI clock, the sound may not play at this screen size" : "");
}
int intelgfx_audio_enable(void) {                        /* returns the port (1 = B, 2 = C, 3 = D) or -1 */
    if (!mmio || cur_trans < 0 || cur_trans > 2 || cur_port < 1 || cur_port > 3) {
        if (mmio) add("audio: the screen isn't on an HDMI port driven by transcoder A, B or C - no sound over HDMI");
        return -1;
    }
    uint32_t pw = rd(0x45404);
    if (!(pw & (1u << 30))) { wr(0x45404, pw | (1u << 31)); wait_bit(0x45404, 1u << 30, true, 20); }
    add("audio: display power well %08x -> %08x", pw, rd(0x45404));
    uint32_t freg = 0x60400 + 0x1000 * cur_trans, fn = rd(freg);
    int mode = (fn >> 24) & 7;
    if (mode == 1) { wr(freg, fn & ~(7u << 24)); cur_mode = 0; add("audio: port was in DVI mode (no sound); switched to HDMI mode"); }
    else if (mode != 0) { add("audio: port mode %d isn't HDMI - no sound over it", mode); return -1; }
    int t = cur_trans;
    uint32_t v = rd(0x650C0);
    v |= 1u << (t * 4 + 2); v &= ~(1u << (t * 4));          /* audio output on, ELD not valid while it's written */
    wr(0x650C0, v);
    uint8_t eld[84]; int len = build_eld(eld);
    uint32_t ctl = 0x650B4 + 0x100 * t;
    wr(ctl, rd(ctl) & ~(0x1Fu << 5));                        /* write from the start of the ELD buffer */
    for (int i = 0; i < len / 4; i++) { uint32_t d; memcpy(&d, eld + i * 4, 4); wr(0x65050 + 0x100 * t, d); }
    wr(0x650C0, rd(0x650C0) | 1u << (t * 4));               /* ELD valid: the sound chip sees the monitor */
    audio_config();
    audio_on = true;
    add("audio: transcoder %c, port %c, ELD %d bytes (%d sound format%s), pin/ELD bits %08x, video DIP %08x",
        'A' + t, 'A' + cur_port, len, eld[5] >> 4, (eld[5] >> 4) == 1 ? "" : "s", rd(0x650C0), rd(0x60200 + 0x1000 * t));
    return cur_port;
}
uint32_t intelgfx_cdclk_khz(void) {
    if (!mmio) return 450000;
    uint32_t lc = rd(0x130040), fz = rd(0xC2014), f = (lc >> 26) & 3;
    if (lc & (1u << 21)) return 800000;
    if (fz & (1u << 24)) return 450000;
    if (f == 0) return 450000;
    if (gdev >> 8 == 0x0A) return 337500;                  /* Haswell ULT */
    return 540000;
}
