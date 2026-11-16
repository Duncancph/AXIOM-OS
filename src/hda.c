/* Sound: Intel High Definition Audio (PCI class 04:03), as found in nearly every PC since 2004.
 * Finds the codec's speaker / headphone / line-out pins, routes them to a DAC, and plays
 * 48 kHz 16-bit stereo from one buffer. Volume (Settings > Sound) is applied in software. */
#include "kernel.h"

#define RATE 48000
#define MAX_SECONDS 6
#define BUF_BYTES (RATE * 4 * MAX_SECONDS)
#define NBDL 16

/* One per HD Audio controller: the PC's own sound chip (headphone jack, speakers) and, on Intel
 * graphics, the one that sends sound to the monitor over HDMI. Sound plays through one of them. */
struct hdac {
    volatile uint8_t *r;                       /* controller registers */
    uint32_t *corb; uint64_t *rirb; uint16_t rirb_rp;
    int cad, afg, sd_off;                      /* codec address, audio function group, output stream registers */
    uint8_t *buf; uint64_t *bdl;
    int dacs[8], ndacs;
    bool ready, hdmi, present;                 /* present: the jack / monitor says something is plugged in */
    int pin, nodes;
    uint32_t codec;
};
static struct hdac ctls[3], none; static int nctl;
static struct hdac *C = &none;                 /* the one in use (or being set up) */
#define r       (C->r)
#define corb    (C->corb)
#define rirb    (C->rirb)
#define rirb_rp (C->rirb_rp)
#define cad     (C->cad)
#define afg     (C->afg)
#define sd_off  (C->sd_off)
#define buf     (C->buf)
#define bdl     (C->bdl)
#define dacs    (C->dacs)
#define ndacs   (C->ndacs)
#define ready   (C->ready)
static uint64_t play_until;
static bool streaming, paused;
static uint64_t last_write;            /* a stream nobody writes to for 10 s is stopped (its app may have quit) */

static char rep[2500]; static int replen;  /* what was found, for Settings > Sound > Details */
static void note(const char *f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    char line[200]; kvsnprintf(line, sizeof line, f, ap);
    __builtin_va_end(ap);
    replen += ksnprintf(rep + replen, sizeof rep - replen, "%s\n", line);
    kprintf("[sound] %s\n", line);
}

static const int16_t sine[256] = { 0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731, 23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793, -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757, -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804 };

static inline uint8_t r8(uint32_t o) { return *(volatile uint8_t *)(r + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(r + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(r + o); }
static inline void w8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(r + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(r + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(r + o) = v; }

/* send one verb through the CORB, wait for its answer in the RIRB */
static uint32_t verb(int nid, uint32_t v) {
    uint16_t wp = (r16(0x48) + 1) & 0xFF;
    corb[wp] = (uint32_t)cad << 28 | (uint32_t)nid << 20 | (v & 0xFFFFF);
    w16(0x48, wp);
    for (int i = 0; i < 2000; i++) {
        uint16_t rw = r16(0x58) & 0xFF;
        if (rw != rirb_rp) { rirb_rp = (rirb_rp + 1) & 0xFF; w8(0x5D, 0x05); return (uint32_t)rirb[rirb_rp]; }   /* ack, or some controllers pause */
        udelay(10);
    }
    return 0xFFFFFFFF;
}
static uint32_t param(int nid, int p) { return verb(nid, 0xF0000 | p); }
#define SET(nid, v12, payload) verb(nid, ((uint32_t)(v12) << 8) | (payload))

static int conn_list(int nid, int *out, int max) {
    uint32_t len = param(nid, 0x0E);
    if (len == 0xFFFFFFFF) return 0;
    bool longf = len & 0x80; int n = len & 0x7F, k = 0;
    for (int i = 0; i < n && k < max; i += longf ? 2 : 4) {
        uint32_t e = verb(nid, 0xF0200 | i);
        for (int j = 0; j < (longf ? 2 : 4) && i + j < n && k < max; j++)
            out[k++] = longf ? (e >> (16 * j)) & 0x7FFF : (e >> (8 * j)) & 0xFF;
    }
    return k;
}

static int wtype(int nid) { return (param(nid, 0x09) >> 20) & 0xF; }

static void unmute_out(int nid) {
    verb(nid, 0x3B000 | 0x7F);                 /* output amp, both sides, unmuted, gain up (clamped by the codec) */
    uint32_t oc = param(nid, 0x12); int steps = (oc >> 8) & 0x7F, zero = oc & 0x7F;
    if (steps) verb(nid, 0x3B000 | (zero + (steps - zero) * 3 / 4));   /* a little below full */
}

/* route pin -> ... -> DAC: depth-first through mixers/selectors, unmuting on the way */
static bool route(int nid, int depth) {
    int t = wtype(nid);
    if (t == 0) {                              /* audio output converter (DAC) */
        SET(nid, 0x705, 0);                    /* power D0 */
        unmute_out(nid);
        bool known = false; for (int i = 0; i < ndacs; i++) if (dacs[i] == nid) known = true;
        if (!known && ndacs < 8) dacs[ndacs++] = nid;
        return true;
    }
    if (depth > 4) return false;
    int c[16], n = conn_list(nid, c, 16);
    for (int i = 0; i < n; i++) {
        int ct = wtype(c[i]);
        if (ct != 0 && ct != 2 && ct != 3) continue;
        if (route(c[i], depth + 1)) {
            if (t != 2) SET(nid, 0x701, i);    /* selectors and pins: pick this input */
            else verb(nid, 0x37000 | (i << 8)); /* mixers: unmute this input */
            SET(nid, 0x705, 0);
            unmute_out(nid);
            return true;
        }
    }
    return false;
}

static void codec_setup(void) {
    uint32_t sub = param(0, 0x04);
    int start = (sub >> 16) & 0xFF, cnt = sub & 0xFF;
    for (int i = 0; i < cnt; i++) if ((param(start + i, 0x05) & 0xFF) == 1) { afg = start + i; break; }
    if (!afg) return;
    SET(afg, 0x705, 0);                        /* power up the function group */
    sub = param(afg, 0x04);
    start = (sub >> 16) & 0xFF; cnt = sub & 0xFF;
    int pins = 0;
    for (int nid = start; nid < start + cnt; nid++) {
            if (wtype(nid) != 4) continue;
        uint32_t caps = param(nid, 0x0C), cfg = verb(nid, 0xF1C00);
        if (!(caps & (1 << 4))) continue;      /* not an output pin */
        int conn = cfg >> 30, dev = (cfg >> 20) & 0xF;
        if (conn == 1 || dev > 2) continue;    /* nothing attached, or not line-out / speaker / headphones */
        if (!route(nid, 0)) continue;
        SET(nid, 0x707, 0x40 | (dev == 2 ? 0x80 : 0));   /* output enable (+ headphone amp) */
        if (caps & (1 << 16)) SET(nid, 0x70C, 2);         /* external amplifier on */
        unmute_out(nid);
        pins++;
    }
    note("codec %08x: %d output%s (speaker, headphone or line out)", C->codec, pins, pins == 1 ? "" : "s");
    if (pins) LOG_OK("Sound: Intel HD Audio, codec %x, %d output%s\n", C->codec >> 16, pins, pins == 1 ? "" : "s");
    ready = pins > 0 && ndacs > 0;
}

/* ---- the HDMI sound chip in Intel graphics (Haswell: codec 8086:2807 and relatives) ----
 * Its pins are the graphics ports (B, C, D in order); a pin reports the monitor (presence) and that the
 * graphics side handed over the monitor's sound abilities (ELD valid). */
static void hdmi_setup(int gport) {
    uint32_t v = verb(0x08, 0xF8100);                       /* Intel: show all pins and converters */
    if (v != 0xFFFFFFFF && !(v & 1)) { SET(0x08, 0x781, (v | 1) & 0xFF); note("HDMI codec: hidden pins turned on (%08x)", v); }
    uint32_t sub = param(0, 0x04);
    int start = (sub >> 16) & 0xFF, cnt = sub & 0xFF;
    for (int i = 0; i < cnt; i++) if ((param(start + i, 0x05) & 0xFF) == 1) { afg = start + i; break; }
    if (!afg) { note("HDMI codec: no audio function"); return; }
    SET(afg, 0x705, 0);
    sleep_ms(10);
    sub = param(afg, 0x04);
    start = (sub >> 16) & 0xFF; cnt = sub & 0xFF;
    int pins[8], np = 0, pick = -1;
    for (int nid = start; nid < start + cnt && np < 8; nid++) {
        uint32_t caps = param(nid, 0x09);
        if (((caps >> 20) & 0xF) != 4) continue;
        uint32_t pc = param(nid, 0x0C);
        if (!(pc & (1 << 4))) continue;                        /* not an output */
        SET(nid, 0x705, 0);
        uint32_t sense = verb(nid, 0xF0900);
        note("HDMI pin %02x (port %c): caps %08x, config %08x, sense %08x%s%s", nid, 'B' + np, pc, verb(nid, 0xF1C00), sense,
             sense & (1u << 31) ? " monitor present" : "", sense & (1u << 30) ? " ELD valid" : "");
        if (pick < 0 && (sense & (3u << 30)) == (3u << 30)) pick = np;
        pins[np++] = nid;
    }
    if (pick < 0) for (int i = 0; i < np; i++) if (verb(pins[i], 0xF0900) & (1u << 31)) { pick = i; break; }
    if (pick < 0 && gport >= 1 && gport - 1 < np) { pick = gport - 1; note("HDMI: no pin says a monitor is there; using port %c, the screen's port", 'A' + gport); }
    if (pick < 0) { note("HDMI: no pin to use"); return; }
    int pin = pins[pick];
    C->present = (verb(pin, 0xF0900) & (1u << 31)) != 0;
    int c[16], n = conn_list(pin, c, 16), cvt = -1, ci = 0;
    for (int i = 0; i < n; i++) if (wtype(c[i]) == 0) { cvt = c[i]; ci = i; break; }
    if (cvt < 0) { note("HDMI pin %02x: no converter connected", pin); return; }
    SET(cvt, 0x705, 0);
    SET(pin, 0x701, ci);                                       /* this pin plays from this converter */
    verb(pin, 0x3B000);                                        /* unmute */
    SET(pin, 0x707, 0x40);                                     /* output on */
    SET(cvt, 0x72D, 1);                                        /* 2 channels */
    SET(cvt, 0x70D, 1);                                        /* digital output on */
    SET(pin, 0x734, 0x00); SET(pin, 0x734, 0x11);              /* channel 0 -> slot 0, 1 -> 1 */
    /* the Audio InfoFrame: tells the monitor it's 2-channel sound, the rest from the stream */
    uint8_t ai[14] = { 0x84, 0x01, 0x0A, 0, 0x01 };
    uint8_t sum = 0; for (int i = 0; i < 14; i++) sum += ai[i];
    ai[3] = (uint8_t)(0x100 - sum);
    SET(pin, 0x730, 0); SET(pin, 0x732, 0x00);                 /* stop sending it while it changes */
    SET(pin, 0x730, 0);
    for (int i = 0; i < 14; i++) SET(pin, 0x731, ai[i]);
    SET(pin, 0x732, 0xC0);                                     /* send it continuously */
    dacs[0] = cvt; ndacs = 1; C->pin = pin; C->hdmi = true;
    ready = true;
    note("HDMI: playing through pin %02x from converter %02x%s", pin, cvt, C->present ? "" : " (the pin doesn't see the monitor)");
    LOG_OK("Sound: HDMI to the monitor (pin %x)\n", pin);
}

static void stream_stop(void) {
    w8(sd_off, r8(sd_off) & ~2);               /* RUN off */
    for (int i = 0; i < 1000 && (r8(sd_off) & 2); i++) udelay(10);
    w8(sd_off + 3, 0x1C);                      /* clear status */
}

static bool is_hdmi_ctl(struct pci_dev *pd) {
    return pd->vendor == 0x8086 && (pd->device == 0x0C0C || pd->device == 0x0D0C || pd->device == 0x0A0C || pd->device == 0x160C ||
                                    (pd->bus == 0 && pd->dev == 3));
}
static void hda_init(struct pci_dev *pd) {
    uint64_t sz, phys = pci_bar(pd, 0, &sz);
    if (!phys) return;
    bool hd = is_hdmi_ctl(pd);
    int gport = -1;
    note("controller %04x:%04x at %02x:%02x.%x%s", pd->vendor, pd->device, pd->bus, pd->dev, pd->fn, hd ? " (HDMI, in the graphics)" : "");
    if (hd) gport = intelgfx_audio_enable();                    /* the graphics side first: power, HDMI mode, ELD */
    cad = -1;
    pci_write16(pd->bus, pd->dev, pd->fn, 4, pci_read16(pd->bus, pd->dev, pd->fn, 4) | 6 | (1 << 10));
    if (pd->vendor == 0x8086) {                /* Intel: the chip must see what the processor writes (snooping on, traffic class 0) */
        uint16_t devc = pci_read16(pd->bus, pd->dev, pd->fn, 0x78);
        if (devc & 0x800) pci_write16(pd->bus, pd->dev, pd->fn, 0x78, devc & ~0x800);
        uint16_t tc = pci_read16(pd->bus, pd->dev, pd->fn, 0x44);
        if (tc & 7) pci_write16(pd->bus, pd->dev, pd->fn, 0x44, tc & ~7);
    }
    r = map_mmio(phys, sz ? sz : 0x4000);
    w32(0x08, r32(0x08) & ~1u);                /* controller reset */
    for (int i = 0; i < 1000 && (r32(0x08) & 1); i++) udelay(10);
    udelay(100);
    w32(0x08, r32(0x08) | 1);
    for (int i = 0; i < 1000 && !(r32(0x08) & 1); i++) udelay(10);
    if (hd) {                                  /* its clock from the display clock (Haswell/Broadwell) */
        uint32_t cd = intelgfx_cdclk_khz(); uint16_t m = 4, nn = 75;
        if (cd == 337500) { m = 16; nn = 225; } else if (cd == 540000) { m = 4; nn = 90; } else if (cd == 675000) { m = 8; nn = 225; }
        w16(0x100C, m); w16(0x1010, nn);
        note("HDMI controller: display clock %u kHz, link clock M/N %u/%u", cd, m, nn);
    }
    sleep_ms(hd ? 20 : 2);                     /* codecs announce themselves */
    uint16_t st = r16(0x0E);
    for (int i = 0; i < 15; i++) if (st & (1 << i)) { cad = i; break; }
    if (cad < 0) { note("no codec answered (states %04x)", st); LOG_INFO("Sound: HD Audio controller without a codec\n"); return; }
    /* command (CORB) and response (RIRB) rings, 256 entries each */
    corb = page_alloc(1); rirb = page_alloc(1);
    if (!corb || !rirb) return;
    w8(0x4C, 0); w8(0x5C, 0); udelay(100);
    w8(0x4E, 2); w8(0x5E, 2);
    w32(0x40, (uint32_t)(uintptr_t)corb); w32(0x44, 0);
    w32(0x50, (uint32_t)(uintptr_t)rirb); w32(0x54, 0);
    w16(0x4A, 1 << 15); for (int i = 0; i < 100 && !(r16(0x4A) & (1 << 15)); i++) udelay(10);
    w16(0x4A, 0);        for (int i = 0; i < 100 && (r16(0x4A) & (1 << 15)); i++) udelay(10);
    w16(0x48, 0);
    w16(0x58, 1 << 15); w16(0x5A, 0xFF); rirb_rp = 0;
    w8(0x4C, 2); w8(0x5C, 2);                  /* run the rings */
    C->codec = param(0, 0x00);
    if (hd || C->codec >> 16 == 0x8086) hdmi_setup(gport); else codec_setup();
    if (!ready) { if (!hd) LOG_INFO("Sound: no usable speaker or headphone output found\n"); return; }
    /* output stream: the first one after the input streams */
    int iss = (r16(0x00) >> 8) & 0xF;
    sd_off = 0x80 + 0x20 * iss;
    buf = page_alloc(BUF_BYTES / 4096 + 1); bdl = page_alloc(1);
    if (!buf || !bdl) { ready = false; return; }
    w8(sd_off, 1); for (int i = 0; i < 1000 && !(r8(sd_off) & 1); i++) udelay(10);   /* stream reset */
    w8(sd_off, 0); for (int i = 0; i < 1000 && (r8(sd_off) & 1); i++) udelay(10);
    uint32_t piece = BUF_BYTES / NBDL;
    for (int i = 0; i < NBDL; i++) { bdl[2 * i] = (uint64_t)(uintptr_t)(buf + i * piece); bdl[2 * i + 1] = piece; }
    w32(sd_off + 0x18, (uint32_t)(uintptr_t)bdl); w32(sd_off + 0x1C, 0);
    w32(sd_off + 0x08, BUF_BYTES);
    w16(sd_off + 0x0C, NBDL - 1);
    w16(sd_off + 0x12, 0x0011);                /* 48 kHz, 16 bits, 2 channels */
    w8(sd_off + 2, 1 << 4);                    /* stream tag 1 */
    for (int i = 0; i < ndacs; i++) { verb(dacs[i], 0x20011); SET(dacs[i], 0x706, 1 << 4); }   /* format, stream 1 channel 0 */
}

static void stream_end(void);
void hda_init_all(void) {
    for (int i = 0; i < pci_count && nctl < 3; i++)
        if (pci_devs[i].class_code == 0x04 && pci_devs[i].subclass == 0x03) {
            C = &ctls[nctl];
            hda_init(&pci_devs[i]);
            bool hd = is_hdmi_ctl(&pci_devs[i]);
            if (ready) dev_claim(&pci_devs[i], "hda", DEV_OK, hd ? "Intel HDMI sound" : "Intel HD Audio",
                                 hd ? "Sound over HDMI to the monitor's speakers" : "Headphone jack and speakers");
            else dev_claim(&pci_devs[i], "hda", DEV_FAILED, hd ? "Intel HDMI sound" : "Intel HD Audio",
                           hd ? "No HDMI sound: the monitor didn't offer speakers, or no codec answered" : "No usable speaker or headphone output was found");
            if (ready) nctl++; else memset(C, 0, sizeof *C);
        }
    C = &none;
    hda_choose();
}
/* Settings > Sound: sound.output = auto | monitor | jack. Automatic: the monitor when it's connected
 * over HDMI, else the PC's own sound chip. */
void hda_choose(void) {
    const char *want = settings_get("sound.output");
    struct hdac *h = 0, *j = 0;
    for (int i = 0; i < nctl; i++) { if (ctls[i].hdmi) { if (!h) h = &ctls[i]; } else if (!j) j = &ctls[i]; }
    struct hdac *pick = !strcmp(want, "monitor") ? (h ? h : j) : !strcmp(want, "jack") ? (j ? j : h) : (h && (h->present || !j) ? h : (j ? j : h));
    if (!pick) pick = &none;
    if (pick == C) return;
    if (streaming) stream_end();
    if (play_until) { play_until = 0; stream_stop(); }
    C = pick;
    if (C != &none) note("playing through %s", C->hdmi ? "the monitor (HDMI)" : "the PC's sound chip (jack / speaker)");
}
const char *hda_report(void) { return rep; }
/* "id=Name;..." of the outputs found, for Settings */
int hda_outputs(char *out, int n) {
    int o = 0;
    for (int i = 0; i < nctl; i++)
        o += ksnprintf(out + o, n - o, "%s%s", o ? ";" : "", ctls[i].hdmi ? "monitor=Monitor (HDMI)" : "jack=Headphone jack / speakers");
    return o;
}
const char *hda_current(void) { return C == &none ? "" : C->hdmi ? "monitor" : "jack"; }

static int vol_scale(void) { int v = settings_int("sound.volume"); if (v < 0) v = 0; if (v > 100) v = 100; return v * v; }   /* 0..10000 */
/* sound for a Bluetooth speaker (bt.c), with the volume applied */
static bool bt_streaming;
static long bt_write(const int16_t *pcm, long frames, int rate) {
    static int16_t tmp[4096];
    int scale = vol_scale(); long done = 0;
    while (done < frames) {
        long n = frames - done < 2048 ? frames - done : 2048;
        for (long i = 0; i < n * 2; i++) tmp[i] = (int16_t)((int32_t)pcm[done * 2 + i] * scale / 10000);
        long t = bta_write(tmp, n, rate);
        done += t;
        if (t < n) break;
    }
    return done;
}
/* play 16-bit stereo 48 kHz samples (frames = sample pairs) */
static int play(const int16_t *pcm, uint32_t frames) {
    if (bta_active()) { if (bt_streaming) return -1; bt_write(pcm, frames, RATE); return 0; }   /* to the Bluetooth speaker */
    if (!ready || streaming) return -1;                      /* music is playing: no chimes */
    if (frames > (uint32_t)RATE * MAX_SECONDS) frames = RATE * MAX_SECONDS;
    stream_stop();
    int vol = settings_int("sound.volume"); if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int scale = vol * vol;                     /* 0..10000: closer to how loud it sounds */
    int16_t *o = (int16_t *)buf;
    for (uint32_t i = 0; i < frames * 2; i++) o[i] = (int16_t)((int32_t)pcm[i] * scale / 10000);
    memset(buf + frames * 4, 0, BUF_BYTES - frames * 4);
    w8(sd_off, r8(sd_off) | 2);                /* run */
    play_until = uptime_ms() + (uint64_t)frames * 1000 / RATE + 60;
    return 0;
}

/* ---- streaming: the DMA buffer is a ring that the app keeps filling ahead of the sound chip ---- */
static int srate = RATE;
static uint64_t wtotal, rtotal; static uint32_t last_lpib;
static void set_format(int rate) {
    uint16_t fmt = rate == 44100 ? 0x4011 : 0x0011;          /* 44.1 or 48 kHz, 16 bits, 2 channels */
    w16(sd_off + 0x12, fmt);
    for (int i = 0; i < ndacs; i++) verb(dacs[i], 0x20000 | fmt);
}
static void update_read(void) {                              /* how far the chip has played, in bytes since the start */
    uint32_t lp = r32(sd_off + 0x04) % BUF_BYTES;
    rtotal += (lp + BUF_BYTES - last_lpib) % BUF_BYTES;
    last_lpib = lp;
}
static void stream_fill_silence(void) {                      /* never let it play old samples again */
    update_read();
    uint64_t want = rtotal + BUF_BYTES / 4;
    while (wtotal < want) {
        uint32_t at = (uint32_t)(wtotal % BUF_BYTES), n = BUF_BYTES - at;
        if (n > want - wtotal) n = (uint32_t)(want - wtotal);
        memset(buf + at, 0, n); wtotal += n;
    }
}
static int stream_start(int rate) {
    if (!ready || (rate != 44100 && rate != 48000)) return -1;
    play_until = 0;
    stream_stop();
    w8(sd_off, 1); for (int i = 0; i < 1000 && !(r8(sd_off) & 1); i++) udelay(10);    /* reset: position back to 0 */
    w8(sd_off, 0); for (int i = 0; i < 1000 && (r8(sd_off) & 1); i++) udelay(10);
    uint32_t piece = BUF_BYTES / NBDL;
    for (int i = 0; i < NBDL; i++) { bdl[2 * i] = (uint64_t)(uintptr_t)(buf + i * piece); bdl[2 * i + 1] = piece; }
    w32(sd_off + 0x18, (uint32_t)(uintptr_t)bdl); w32(sd_off + 0x1C, 0);
    w32(sd_off + 0x08, BUF_BYTES); w16(sd_off + 0x0C, NBDL - 1);
    w8(sd_off + 2, 1 << 4);
    srate = rate; set_format(rate);
    memset(buf, 0, BUF_BYTES);
    wtotal = (uint64_t)rate * 4 / 10; rtotal = 0; last_lpib = 0;    /* start with 0.1 s of silence */
    streaming = true; paused = false; last_write = uptime_ms();
    w8(sd_off, r8(sd_off) | 2);
    return 0;
}
static long stream_write(uint64_t ua, uint64_t n) {
    if (!streaming || (n && !user_ok(current->pml4, (const void *)ua, n, false))) return -1;
    update_read();
    if (wtotal < rtotal) wtotal = rtotal;                  /* it ran dry: carry on from where it is */
    uint64_t space = BUF_BYTES - (wtotal - rtotal) - 4096;
    if ((int64_t)space <= 0) return 0;
    if (n > space) n = space;
    n &= ~3ull;
    int vol = settings_int("sound.volume"); if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int scale = vol * vol;
    const int16_t *src = (const int16_t *)ua;
    for (uint64_t i = 0; i < n / 2; i++) {
        uint32_t at = (uint32_t)((wtotal + i * 2) % BUF_BYTES);
        *(int16_t *)(buf + at) = (int16_t)((int32_t)src[i] * scale / 10000);
    }
    wtotal += n; last_write = uptime_ms();
    return (long)n;
}
static void stream_end(void) { if (!streaming || C == &none) return; streaming = false; stream_stop(); set_format(RATE); }

void hda_tick(void) {
    if (C == &none) return;
    if (play_until && uptime_ms() > play_until) { play_until = 0; stream_stop(); }
    if (streaming && !paused) stream_fill_silence();
    if (streaming && !paused && uptime_ms() - last_write > 10000) stream_end();
}

/* a soft two-note chime; style 0 = test sound, 1 = notification */
static int chime(int style) {
    static int16_t pcm[RATE * 2 / 2];          /* up to 0.5 s */
    static const int notes[2][2] = { { 659, 880 }, { 784, 1047 } };
    int frames = style ? RATE * 30 / 100 : RATE * 45 / 100, half = frames / 2;
    uint32_t ph = 0;
    for (int i = 0; i < frames; i++) {
        int f = notes[style ? 1 : 0][i < half ? 0 : 1];
        ph += (uint32_t)((uint64_t)f * 65536 * 65536 / RATE);
        int pos = i < half ? i : i - half;
        int env = 4096 - pos * 4096 / half;                     /* fade out each note */
        if (pos < 240) env = env * pos / 240;                   /* soft start */
        int s = sine[ph >> 24] * env / 4096 * (style ? 3 : 5) / 10;
        pcm[2 * i] = pcm[2 * i + 1] = (int16_t)s;
    }
    return play(pcm, frames);
}

static int bt_rate = RATE;
long sound_syscall(int op, uint64_t a, uint64_t b) {
    if (bta_active() || (bt_streaming && op != SOUND_STREAM_START)) {          /* a Bluetooth speaker plays the sound */
        switch (op) {
        case SOUND_INFO: return 1;
        case SOUND_STREAM_START: if (a != 44100 && a != 48000) return -1; bt_rate = (int)a; bta_flush(); bt_streaming = true; return 0;
        case SOUND_STREAM_WRITE: {
            if (!bt_streaming || (b && !user_ok(current->pml4, (const void *)a, b, false))) return -1;
            if (!bta_active()) { bt_streaming = false; return -1; }        /* the speaker went away: the app starts again on this computer */
            return bt_write((const int16_t *)a, (long)(b / 4), bt_rate) * 4;
        }
        case SOUND_STREAM_STATUS: return bt_streaming ? bta_queued_ms() : -1;
        case SOUND_STREAM_STOP: bta_flush(); bt_streaming = false; return 0;
        case SOUND_STREAM_PAUSE: if (!bt_streaming) return -1; bta_pause(a != 0); return 0;
        default: break;                                                     /* chimes and short sounds: play() sends them */
        }
    }
    if (op == SOUND_STREAM_START) bt_streaming = false;
    switch (op) {
    case SOUND_INFO: return ready ? 1 : 0;
    case SOUND_TEST: return chime(0);
    case SOUND_EVENT: return chime(1);
    case SOUND_STREAM_START: return stream_start((int)a);
    case SOUND_STREAM_WRITE: return stream_write(a, b);
    case SOUND_STREAM_STATUS: if (!streaming) return -1; update_read(); return wtotal > rtotal ? (long)((wtotal - rtotal) * 1000 / ((uint64_t)srate * 4)) : 0;
    case SOUND_STREAM_STOP: stream_end(); return 0;
    case SOUND_STREAM_PAUSE:
        if (!streaming) return -1;
        paused = a != 0; last_write = uptime_ms();
        if (paused) w8(sd_off, r8(sd_off) & ~2); else { w8(sd_off, r8(sd_off) | 2); }
        return 0;
    case SOUND_PLAY_PCM: {
        if (!ready) return -1;
        if (b > (uint64_t)BUF_BYTES || (b && !user_ok(current->pml4, (const void *)a, b, false))) return -1;
        static int16_t tmp[BUF_BYTES / 2];
        memcpy(tmp, (const void *)a, b);
        return play(tmp, (uint32_t)(b / 4));
    }
    }
    return -1;
}

void sound_notify(void) { if (ready && uptime_ms() > 15000) chime(1); }
