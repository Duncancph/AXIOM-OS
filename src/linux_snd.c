/* Sound for Linux programs: the ALSA kernel interface (what libasound talks to), on top of AXIOM's sound stream
 * (the speakers through HD Audio, or a Bluetooth speaker). Enough for libasound's "hw" device and its "plug"
 * converter in front of it, which is how Chromium and most Linux programs play sound:
 *   /dev/snd/controlC0   one card ("AXIOM"), one playback device, no mixer controls
 *   /dev/snd/pcmC0D0p    playback: 16-bit little-endian stereo at 44.1 or 48 kHz, interleaved, by write (RW) or
 *                        through the mmap'd ring (what libasound's converters use); the status/control pages
 *                        aren't mapped, so libasound uses SYNC_PTR for positions
 * Hardware parameters are refined as Linux does it (masks and intervals, the frame/byte/time relations), so
 * libasound's "plug" can convert other rates, formats and channel counts. Numbers and layouts are Linux's
 * (include/uapi/sound/asound.h), checked against the host's header.
 *
 * How positions work: appl (frames the program has written) and hw (frames played). Frames are in a ring
 * (buffer_size frames, shared memory the program can map); RW writes copy into it. Once the stream runs, new
 * frames go from the ring to the sound stream as soon as appl moves, and hw = pushed - (what the stream
 * still holds of ours). The stream never "underruns" here: AXIOM's sound
 * stream plays silence when it runs dry, and the program just carries on writing. */
#include "kernel.h"
void *lx_shm_make(uint64_t size); void lx_shm_put(void *o); void lx_shm_rw(void *o, uint64_t off, void *buf, uint64_t n, bool write);

enum { EIO_ = 5, EAGAIN_ = 11, EFAULT_ = 14, EBUSY_ = 16, ENODEV_ = 19, EINVAL_ = 22, ENOTTY_ = 25, EPIPE_ = 32, EBADFD_ = 77 };
enum { ST_OPEN, ST_SETUP, ST_PREPARED, ST_RUNNING, ST_XRUN, ST_DRAINING, ST_PAUSED };
enum { P_ACCESS = 0, P_FORMAT, P_SUBFORMAT, P_SAMPLE_BITS = 8, P_FRAME_BITS, P_CHANNELS, P_RATE, P_PERIOD_TIME, P_PERIOD_SIZE,
       P_PERIOD_BYTES, P_PERIODS, P_BUFFER_TIME, P_BUFFER_SIZE, P_BUFFER_BYTES, P_TICK_TIME };
#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define PS_MIN 32
#define PS_MAX 16384
#define BS_MIN 64
#define BS_MAX 32768                    /* ~0.7 s at 48 kHz: well inside the sound stream's buffer */

struct snd {
    int refs, kind;                     /* 0 control, 1 playback */
    int state, rate;
    uint64_t buffer, period, avail_min, start_threshold, stop_threshold, boundary;
    uint64_t appl, hw, pushed;          /* frames: written by the program, played, sent to the stream */
    void *ring;                         /* struct shm (linux.c): buffer frames */
    bool owns_stream;
    uint64_t trigger_ns;
};

void *snd_open(int kind) {
    if (kind == 1 && !sound_ready()) return 0;
    struct snd *s = kmalloc(sizeof *s);
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->refs = 1; s->kind = kind; s->state = ST_OPEN;
    return s;
}
static void stop_stream(struct snd *s) {
    if (s->owns_stream) { sound_syscall(SOUND_STREAM_STOP, 0, 0); s->owns_stream = false; }
}
void snd_put(void *o) {
    struct snd *s = o;
    if (!s || --s->refs > 0) return;
    stop_stream(s);
    if (s->ring) lx_shm_put(s->ring);
    kfree(s);
}

/* ---- positions ---- */
static void update_hw(struct snd *s) {
    if (s->state != ST_RUNNING && s->state != ST_DRAINING && s->state != ST_PAUSED) return;
    long q = sound_queued_frames(); if (q < 0) q = 0;
    uint64_t ours = s->pushed - s->hw;                  /* the stream may also hold silence it added itself */
    if ((uint64_t)q < ours) ours = (uint64_t)q;
    uint64_t hw = s->pushed - ours;
    if (hw > s->hw) s->hw = hw;
}
static uint64_t avail(struct snd *s) { update_hw(s); uint64_t used = s->appl - s->hw; return used >= s->buffer ? 0 : s->buffer - used; }
/* send the ring's frames [pushed, appl) to the sound stream */
static void push(struct snd *s) {
    static int16_t chunk[2048 * 2];
    while (s->pushed < s->appl) {
        uint64_t at = s->pushed % s->buffer, n = s->appl - s->pushed;
        if (n > s->buffer - at) n = s->buffer - at;
        if (n > 2048) n = 2048;
        lx_shm_rw(s->ring, at * 4, chunk, n * 4, false);
        uint64_t off = 0;
        while (off < n) {
            long k = sound_kwrite(chunk + off * 2, (n - off) * 4);
            if (k < 0) { s->pushed = s->appl; return; }       /* (the stream went away: drop it) */
            if (!k) { sleep_ms(2); continue; }
            off += (uint64_t)k / 4; s->pushed += (uint64_t)k / 4;
        }
    }
}
/* the program has filled the ring up to new_appl (RW write, or mmap + SYNC_PTR / FORWARD) */
static int start(struct snd *s);
static int commit(struct snd *s, uint64_t new_appl) {
    if (new_appl < s->appl || new_appl - s->hw > s->buffer) return -EINVAL_;
    s->appl = new_appl;
    if (s->state == ST_PREPARED && s->appl >= s->start_threshold) return start(s);
    if (s->state == ST_RUNNING) push(s);
    return 0;
}
static int start(struct snd *s) {
    if (s->state != ST_PREPARED) return -EBADFD_;
    if (sound_syscall(SOUND_STREAM_START, (uint64_t)s->rate, 0) < 0) return -EIO_;
    s->owns_stream = true; s->state = ST_RUNNING; s->trigger_ns = uptime_ns();
    push(s);
    return 0;
}

/* write frames from the program (user memory) */
static long pcm_write(struct snd *s, uint64_t ubuf, uint64_t frames, bool nonblock) {
    if (s->state == ST_XRUN) return -EPIPE_;
    if (s->state != ST_PREPARED && s->state != ST_RUNNING && s->state != ST_PAUSED) return -EBADFD_;
    if (frames && !user_ok(current->pml4, (const void *)ubuf, frames * 4, false)) return -EFAULT_;
    uint64_t done = 0;
    while (done < frames) {
        uint64_t a = avail(s);
        if (!a) {
            if (nonblock) return done ? (long)done : -EAGAIN_;
            if (s->state == ST_PAUSED) { if (done) return (long)done; return -EAGAIN_; }
            sleep_ms(2); continue;
        }
        uint64_t n = frames - done; if (n > a) n = a;
        uint64_t at = s->appl % s->buffer; if (n > s->buffer - at) n = s->buffer - at;
        lx_shm_rw(s->ring, at * 4, (void *)(ubuf + done * 4), n * 4, true);
        done += n;
        int r = commit(s, s->appl + n);
        if (r < 0) return done ? (long)done : r;
    }
    return (long)done;
}
long snd_write(void *o, uint64_t ubuf, uint64_t bytes, bool nonblock) {
    struct snd *s = o;
    if (s->kind != 1) return -EINVAL_;
    long r = pcm_write(s, ubuf, bytes / 4, nonblock);
    return r < 0 ? r : r * 4;
}

/* poll: writable when at least avail_min frames fit */
int snd_ready(void *o, bool *waiting) {
    struct snd *s = o;
    if (s->kind != 1) return 4;                                      /* POLLOUT */
    switch (s->state) {
    case ST_PREPARED: case ST_RUNNING: {
        bool ok = avail(s) >= (s->avail_min ? s->avail_min : 1);
        if (!ok) *waiting = true;
        return ok ? 4 : 0;
    }
    case ST_DRAINING: case ST_PAUSED: *waiting = true; return 0;
    default: return 4 | 8;                                           /* POLLOUT | POLLERR, as Linux */
    }
}

/* ---- hardware parameters: masks (256 bits) and intervals {min, max, flags: openmin 1, openmax 2, integer 4, empty 8} ---- */
struct iv { uint32_t min, max, fl; };
#define HP_MASK(p, i) ((uint32_t *)((uint8_t *)(p) + 4 + (i) * 32))
#define HP_IV(p, i) ((struct iv *)((uint8_t *)(p) + 260 + ((i) - 8) * 12))
static void iv_norm(struct iv *v) {
    if (v->fl & 1) { if (v->min < 0xFFFFFFFFu) v->min++; v->fl &= ~1u; }
    if (v->fl & 2) { if (v->max) v->max--; v->fl &= ~2u; }
}
static bool iv_cut(struct iv *v, uint64_t lo, uint64_t hi) {       /* intersect with [lo, hi]; false if empty */
    if (hi > 0xFFFFFFFFu) hi = 0xFFFFFFFFu;
    if (lo > v->min) v->min = (uint32_t)lo;
    if (hi < v->max) v->max = (uint32_t)hi;
    if (v->min > v->max) { v->fl |= 8; return false; }
    return true;
}
static uint64_t ceil_div(uint64_t a, uint64_t b) { return b ? (a + b - 1) / b : 0xFFFFFFFFu; }
/* refine in place: returns 0 or -EINVAL */
static int refine(uint8_t *p, bool choose) {
    uint32_t *acc = HP_MASK(p, P_ACCESS), *fmt = HP_MASK(p, P_FORMAT), *sub = HP_MASK(p, P_SUBFORMAT);
    acc[0] &= 1u << ACCESS_RW_INTERLEAVED | 1u << 0; fmt[0] &= 1u << FORMAT_S16_LE; sub[0] &= 1u;
    for (int i = 1; i < 8; i++) { acc[i] = fmt[i] = sub[i] = 0; }
    if (!acc[0] || !fmt[0] || !sub[0]) return -EINVAL_;
    for (int i = P_SAMPLE_BITS; i <= P_TICK_TIME; i++) iv_norm(HP_IV(p, i));
    if (!iv_cut(HP_IV(p, P_SAMPLE_BITS), 16, 16) || !iv_cut(HP_IV(p, P_FRAME_BITS), 32, 32) || !iv_cut(HP_IV(p, P_CHANNELS), 2, 2)) return -EINVAL_;
    struct iv *rate = HP_IV(p, P_RATE);
    if (!iv_cut(rate, 44100, 48000)) return -EINVAL_;
    if (rate->min > 44100) rate->min = 48000;                       /* only the two rates in between */
    if (rate->max < 48000) rate->max = 44100;
    if (rate->min > rate->max) return -EINVAL_;
    if (choose) rate->max = rate->min;
    uint64_t rmin = rate->min, rmax = rate->max;
    struct iv *ps = HP_IV(p, P_PERIOD_SIZE), *pb = HP_IV(p, P_PERIOD_BYTES), *pt = HP_IV(p, P_PERIOD_TIME);
    struct iv *bs = HP_IV(p, P_BUFFER_SIZE), *bb = HP_IV(p, P_BUFFER_BYTES), *bt = HP_IV(p, P_BUFFER_TIME), *per = HP_IV(p, P_PERIODS);
    /* everything in frames */
    if (!iv_cut(ps, PS_MIN, PS_MAX) || !iv_cut(ps, ceil_div(pb->min, 4), pb->max / 4)) return -EINVAL_;
    /* times are real numbers in Linux (open intervals); here they're whole microseconds, so round outwards */
    if (!iv_cut(ps, (uint64_t)pt->min * rmin / 1000000, pt->max == 0xFFFFFFFFu ? 0xFFFFFFFFu : ceil_div((uint64_t)pt->max * rmax, 1000000))) return -EINVAL_;
    if (!iv_cut(bs, BS_MIN, BS_MAX) || !iv_cut(bs, ceil_div(bb->min, 4), bb->max / 4)) return -EINVAL_;
    if (!iv_cut(bs, (uint64_t)bt->min * rmin / 1000000, bt->max == 0xFFFFFFFFu ? 0xFFFFFFFFu : ceil_div((uint64_t)bt->max * rmax, 1000000))) return -EINVAL_;
    if (!iv_cut(per, 2, 1024)) return -EINVAL_;
    for (int k = 0; k < 4; k++) {                                   /* buffer = period * periods */
        if (!iv_cut(bs, (uint64_t)ps->min * per->min, (uint64_t)ps->max * per->max)) return -EINVAL_;
        if (!iv_cut(per, ceil_div(bs->min, ps->max), bs->max / ps->min)) return -EINVAL_;
        if (!iv_cut(ps, ceil_div(bs->min, per->max), bs->max / per->min)) return -EINVAL_;
    }
    if (choose) {                                                   /* single values: the smallest period, then periods */
        ps->max = ps->min; per->max = per->min;
        if (!iv_cut(bs, (uint64_t)ps->min * per->min, (uint64_t)ps->min * per->min)) {
            /* the buffer wanted isn't a multiple: take the smallest buffer and the periods that fit */
            bs->max = bs->min; per->min = per->max = bs->min / ps->min; bs->min = bs->max = ps->min * per->min;
            if (per->min < 2) return -EINVAL_;
            bs->fl &= ~8u;
        }
    }
    pb->min = ps->min * 4; pb->max = ps->max * 4;
    bb->min = bs->min * 4; bb->max = bs->max * 4;
    pt->min = (uint32_t)((uint64_t)ps->min * 1000000 / rmax); pt->max = (uint32_t)ceil_div((uint64_t)ps->max * 1000000, rmin);
    bt->min = (uint32_t)((uint64_t)bs->min * 1000000 / rmax); bt->max = (uint32_t)ceil_div((uint64_t)bs->max * 1000000, rmin);
    if (choose) { pt->min = pt->max = (uint32_t)((uint64_t)ps->min * 1000000 / rmin); bt->min = bt->max = (uint32_t)((uint64_t)bs->min * 1000000 / rmin); }
    for (int i = P_SAMPLE_BITS; i <= P_TICK_TIME; i++) { struct iv *v = HP_IV(p, i); v->fl = (v->fl & ~3u) | (i != P_PERIOD_TIME && i != P_BUFFER_TIME && i != P_TICK_TIME ? 4u : 0); }
    struct iv *tick = HP_IV(p, P_TICK_TIME); tick->min = 0; tick->max = 0xFFFFFFFFu; tick->fl = 0;
    *(uint32_t *)(p + 512) = 0;                                     /* rmask */
    *(uint32_t *)(p + 516) = 0xFFFFFFFFu;                           /* cmask: (anything may have changed) */
    *(uint32_t *)(p + 520) = 1u | 2u | (1u << 8) | (1u << 19);     /* info: MMAP, MMAP_VALID, INTERLEAVED, PAUSE */
    *(uint32_t *)(p + 524) = 16;                                    /* msbits */
    *(uint32_t *)(p + 528) = rate->min == rate->max ? rate->min : 0;   /* rate_num / rate_den */
    *(uint32_t *)(p + 532) = 1;
    *(uint64_t *)(p + 536) = 0;                                     /* fifo_size */
    return 0;
}

void *snd_mmap_obj(void *o, uint64_t off) {                       /* mmap: offset 0 is the ring */
    struct snd *s = o;
    return s->kind == 1 && off == 0 ? s->ring : 0;
}
static void ts_now(uint8_t *at) { uint64_t ns = uptime_ns(); *(int64_t *)at = (int64_t)(ns / 1000000000); *(int64_t *)(at + 8) = (int64_t)(ns % 1000000000); }
static void fill_status(struct snd *s, uint8_t *st) {               /* struct snd_pcm_status (152 bytes) */
    memset(st, 0, 152);
    update_hw(s);
    *(int32_t *)st = s->state;
    uint64_t tn = s->trigger_ns; *(int64_t *)(st + 8) = (int64_t)(tn / 1000000000); *(int64_t *)(st + 16) = (int64_t)(tn % 1000000000);
    ts_now(st + 24);
    uint64_t b = s->boundary ? s->boundary : 0x4000000000000000ull;
    *(uint64_t *)(st + 40) = s->appl % b; *(uint64_t *)(st + 48) = s->hw % b;
    *(int64_t *)(st + 56) = (int64_t)(s->appl - s->hw);
    *(uint64_t *)(st + 64) = avail(s); *(uint64_t *)(st + 72) = avail(s);
    ts_now(st + 112);
}
static void str_set(uint8_t *at, int n, const char *v) { memset(at, 0, n); strncpy0((char *)at, v, n); }
static void fill_pcm_info(uint8_t *in) {                            /* struct snd_pcm_info (288): keeps device/subdevice/stream */
    int stream = *(int32_t *)(in + 8);
    memset(in + 12, 0, 288 - 12);
    (void)stream;
    *(int32_t *)(in + 12) = 0;                                      /* card */
    str_set(in + 16, 64, "AXIOM"); str_set(in + 80, 80, "AXIOM sound"); str_set(in + 160, 32, "subdevice #0");
    *(int32_t *)(in + 200) = 1; *(int32_t *)(in + 204) = 1;          /* subdevices: count, available */
}

long snd_ioctl(void *o, uint32_t cmd, uint64_t arg, bool nonblock) {
    struct snd *s = o;
    #define UARG(n, w) do { if (!user_ok(current->pml4, (const void *)arg, (n), (w))) return -EFAULT_; } while (0)
    if (s->kind == 0) {                                              /* the control device */
        switch (cmd) {
        case 0x80045500: UARG(4, true); *(int32_t *)arg = 0x00020009; return 0;          /* PVERSION */
        case 0x81785501: {                                                               /* CARD_INFO */
            UARG(376, true); uint8_t *c = (uint8_t *)arg; memset(c, 0, 376);
            str_set(c + 8, 16, "AXIOM"); str_set(c + 24, 16, "AXIOM"); str_set(c + 40, 32, "AXIOM sound");
            str_set(c + 72, 80, "AXIOM sound (speakers or a Bluetooth speaker)"); str_set(c + 168, 80, "AXIOM");
            return 0;
        }
        case 0x80045530: {                                                               /* PCM_NEXT_DEVICE */
            UARG(4, true); int32_t d = *(int32_t *)arg; *(int32_t *)arg = d < 0 ? 0 : -1; return 0;
        }
        case 0xc1205531: {                                                               /* PCM_INFO */
            UARG(288, true); uint8_t *in = (uint8_t *)arg;
            if (*(int32_t *)in != 0 || *(int32_t *)(in + 8) != 0) return -2;               /* ENOENT: device 0, playback only */
            fill_pcm_info(in); return 0;
        }
        case 0x40045532: return 0;                                                       /* PCM_PREFER_SUBDEVICE */
        case 0xc0505510: {                                                               /* ELEM_LIST: no controls */
            UARG(80, true); *(uint32_t *)(arg + 8) = 0; *(uint32_t *)(arg + 12) = 0; return 0;
        }
        case 0xc0045516: UARG(4, true); *(int32_t *)arg = 0; return 0;                    /* SUBSCRIBE_EVENTS */
        case 0x800455d1: UARG(4, true); *(int32_t *)arg = 0; return 0;                    /* POWER_STATE: D0 */
        case 0xc0045540: case 0xc0045520: case 0xc0045543: UARG(4, true); *(int32_t *)arg = -1; return 0;   /* no MIDI, hwdep, UMP */
        }
        return -ENOTTY_;
    }
    switch (cmd) {
    case 0x80044100: UARG(4, true); *(int32_t *)arg = 0x00020011; return 0;              /* PVERSION */
    case 0x81204101: UARG(288, true); memset((void *)arg, 0, 12); fill_pcm_info((uint8_t *)arg); return 0;   /* INFO */
    case 0x40044102: case 0x40044103: case 0x40044104: return 0;                        /* TSTAMP, TTSTAMP, USER_PVERSION */
    case 0xc2604110: UARG(608, true); return refine((uint8_t *)arg, false);              /* HW_REFINE */
    case 0xc2604111: {                                                                   /* HW_PARAMS */
        UARG(608, true);
        if (s->state != ST_OPEN && s->state != ST_SETUP && s->state != ST_PREPARED) return -EBADFD_;
        int r = refine((uint8_t *)arg, true);
        if (r) return r;
        uint8_t *p = (uint8_t *)arg;
        stop_stream(s);
        s->rate = (int)HP_IV(p, P_RATE)->min; s->period = HP_IV(p, P_PERIOD_SIZE)->min; s->buffer = HP_IV(p, P_BUFFER_SIZE)->min;
        if (s->ring) lx_shm_put(s->ring);
        s->ring = lx_shm_make((s->buffer * 4 + 4095) & ~4095ull);
        if (!s->ring) return -12;
        s->avail_min = s->period; s->start_threshold = 1; s->stop_threshold = s->buffer;
        s->appl = s->hw = s->pushed = 0;
        s->state = ST_SETUP;
        kprintf("[snd] %s: %d Hz, buffer %u frames, period %u\n", current->name, s->rate, (unsigned)s->buffer, (unsigned)s->period);
        return 0;
    }
    case 0x00004112: if (s->state == ST_RUNNING) return -EBADFD_; stop_stream(s); s->state = ST_OPEN; return 0;   /* HW_FREE */
    case 0xc0884113: {                                                                   /* SW_PARAMS */
        UARG(136, true);
        if (s->state == ST_OPEN) return -EBADFD_;
        uint8_t *w = (uint8_t *)arg;
        uint64_t am = *(uint64_t *)(w + 16), stt = *(uint64_t *)(w + 32), sp = *(uint64_t *)(w + 40), bd = *(uint64_t *)(w + 64);
        if (!am) am = 1;
        s->avail_min = am; s->start_threshold = stt; s->stop_threshold = sp; s->boundary = bd;
        return 0;
    }
    case 0x80984120: case 0xc0984124: UARG(152, true); fill_status(s, (uint8_t *)arg); return 0;   /* STATUS, STATUS_EXT */
    case 0x80084121: UARG(8, true); update_hw(s); *(int64_t *)arg = (int64_t)(s->appl - s->hw); return s->state == ST_XRUN ? -EPIPE_ : 0;   /* DELAY */
    case 0x00004122: update_hw(s); return s->state == ST_XRUN ? -EPIPE_ : 0;            /* HWSYNC */
    case 0xc0884123: {                                                                   /* SYNC_PTR */
        UARG(136, true);
        uint8_t *sp = (uint8_t *)arg; uint32_t fl = *(uint32_t *)sp;
        if (!(fl & 4) && *(uint64_t *)(sp + 80)) s->avail_min = *(uint64_t *)(sp + 80);   /* (AVAIL_MIN not set: take the program's) */
        uint64_t b = s->boundary ? s->boundary : 0x4000000000000000ull;
        if (!(fl & 2) && s->ring && (s->state == ST_PREPARED || s->state == ST_RUNNING || s->state == ST_PAUSED)) {   /* (APPL not set: the program moved appl_ptr) */
            uint64_t ua = *(uint64_t *)(sp + 72) % b, d = (ua + b - s->appl % b) % b;
            if (d && d <= s->buffer) { int r = commit(s, s->appl + d); if (r < 0) return r; }
        }
        update_hw(s);
        memset(sp + 8, 0, 64);
        *(int32_t *)(sp + 8) = s->state; *(uint64_t *)(sp + 16) = s->hw % b; ts_now(sp + 24);
        *(uint64_t *)(sp + 72) = s->appl % b; *(uint64_t *)(sp + 80) = s->avail_min;
        return 0;
    }
    case 0x00004140:                                                                     /* PREPARE */
        if (s->state == ST_OPEN) return -EBADFD_;
        if (s->state == ST_RUNNING || s->state == ST_PAUSED || s->state == ST_DRAINING) stop_stream(s);
        if (s->state != ST_PREPARED) { s->appl = s->hw = s->pushed = 0; }
        s->state = ST_PREPARED; return 0;
    case 0x00004141: update_hw(s); s->appl = s->hw; return 0;                            /* RESET */
    case 0x00004142: return start(s);                                                    /* START */
    case 0x00004143:                                                                     /* DROP */
        if (s->state == ST_OPEN) return -EBADFD_;
        stop_stream(s); s->state = ST_SETUP; return 0;
    case 0x00004144: {                                                                   /* DRAIN */
        if (s->state == ST_OPEN) return -EBADFD_;
        if (s->state == ST_PREPARED && s->appl > s->pushed) { int r = start(s); if (r) return r; }
        if (s->state != ST_RUNNING) { s->state = ST_SETUP; return 0; }
        s->state = ST_DRAINING;
        for (;;) {
            update_hw(s);
            if (s->hw >= s->appl) break;
            if (nonblock) return -EAGAIN_;
            sleep_ms(5);
        }
        stop_stream(s); s->state = ST_SETUP; return 0;
    }
    case 0x40044145:                                                                     /* PAUSE */
        if (arg) { if (s->state != ST_RUNNING) return -EBADFD_; sound_syscall(SOUND_STREAM_PAUSE, 1, 0); s->state = ST_PAUSED; }
        else { if (s->state != ST_PAUSED) return -EBADFD_; sound_syscall(SOUND_STREAM_PAUSE, 0, 0); s->state = ST_RUNNING; }
        return 0;
    case 0x40084146: return 0;                                                           /* REWIND: nothing taken back */
    case 0x40084149: {                                                                   /* FORWARD (mmap): the program filled n more */
        UARG(8, false); uint64_t n = *(uint64_t *)arg, a = avail(s); if (n > a) n = a;
        int r = commit(s, s->appl + n); return r < 0 ? r : (long)n;
    }
    case 0x00004147: return -ENODEV_;                                                    /* RESUME: never suspended */
    case 0x00004148: return 0;                                                           /* XRUN */
    case 0x40184150: {                                                                   /* WRITEI_FRAMES {long result; void *buf; ulong frames} */
        UARG(24, true);
        long r = pcm_write(s, *(uint64_t *)(arg + 8), *(uint64_t *)(arg + 16), nonblock);
        if (r < 0) return r;
        *(int64_t *)arg = r; return 0;
    }
    case 0x40044160: case 0x00004161: return -EINVAL_;                                   /* LINK, UNLINK */
    case 0x80184132: {                                                                   /* CHANNEL_INFO: where a channel is in the ring */
        UARG(24, true); uint32_t ch = *(uint32_t *)arg;
        if (ch > 1 || !s->ring) return -EINVAL_;
        *(int64_t *)(arg + 8) = 0; *(uint32_t *)(arg + 16) = ch * 16; *(uint32_t *)(arg + 20) = 32;   /* offset, first and step in bits */
        return 0;
    }
    }
    kprintf("[snd] unknown sound request %08x\n", cmd);
    return -ENOTTY_;
    #undef UARG
}
