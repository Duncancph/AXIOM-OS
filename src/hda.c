/* Sound: Intel High Definition Audio (PCI class 04:03), as found in nearly every PC since 2004.
 * Finds the codec's speaker / headphone / line-out pins, routes them to a DAC, and plays
 * 48 kHz 16-bit stereo from one buffer. Volume (Settings > Sound) is applied in software. */
#include "kernel.h"

#define RATE 48000
#define MAX_SECONDS 6
#define BUF_BYTES (RATE * 4 * MAX_SECONDS)
#define NBDL 16

static volatile uint8_t *r;                  /* controller registers */
static uint32_t *corb; static uint64_t *rirb; static uint16_t rirb_rp;
static int cad = -1, afg, sd_off;            /* codec address, audio function group, output stream registers */
static uint8_t *buf; static uint64_t *bdl;
static uint64_t play_until;
static bool streaming, paused;
static uint64_t last_write;            /* a stream nobody writes to for 10 s is stopped (its app may have quit) */
static bool ready;
static int dacs[8], ndacs;

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
    if (pins) LOG_OK("Sound: Intel HD Audio, codec %x, %d output%s\n", param(0, 0x00) >> 16, pins, pins == 1 ? "" : "s");
    ready = pins > 0 && ndacs > 0;
}

static void stream_stop(void) {
    w8(sd_off, r8(sd_off) & ~2);               /* RUN off */
    for (int i = 0; i < 1000 && (r8(sd_off) & 2); i++) udelay(10);
    w8(sd_off + 3, 0x1C);                      /* clear status */
}

static void hda_init(struct pci_dev *pd) {
    uint64_t sz, phys = pci_bar(pd, 0, &sz);
    if (!phys) return;
    pci_write16(pd->bus, pd->dev, pd->fn, 4, pci_read16(pd->bus, pd->dev, pd->fn, 4) | 6 | (1 << 10));
    r = map_mmio(phys, sz ? sz : 0x4000);
    w32(0x08, r32(0x08) & ~1u);                /* controller reset */
    for (int i = 0; i < 1000 && (r32(0x08) & 1); i++) udelay(10);
    udelay(100);
    w32(0x08, r32(0x08) | 1);
    for (int i = 0; i < 1000 && !(r32(0x08) & 1); i++) udelay(10);
    sleep_ms(2);                               /* codecs announce themselves */
    uint16_t st = r16(0x0E);
    for (int i = 0; i < 15; i++) if (st & (1 << i)) { cad = i; break; }
    if (cad < 0) { LOG_INFO("Sound: HD Audio controller without a codec\n"); return; }
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
    codec_setup();
    if (!ready) { LOG_INFO("Sound: no usable speaker or headphone output found\n"); return; }
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

void hda_init_all(void) {
    for (int i = 0; i < pci_count && !ready; i++)
        if (pci_devs[i].class_code == 0x04 && pci_devs[i].subclass == 0x03) hda_init(&pci_devs[i]);
}

/* play 16-bit stereo 48 kHz samples (frames = sample pairs) */
static int play(const int16_t *pcm, uint32_t frames) {
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
static void stream_end(void) { if (!streaming) return; streaming = false; stream_stop(); set_format(RATE); }

void hda_tick(void) {
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

long sound_syscall(int op, uint64_t a, uint64_t b) {
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
