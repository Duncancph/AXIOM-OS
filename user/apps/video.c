/* Video: plays MP4 files (H.264 video, AAC sound).
 * Decoding: user/lib/video (H.264) and user/lib/media (MP4 reader, AAC); sound: the kernel's HD Audio
 * stream (SOUND_STREAM_*), which is also the clock pictures are shown by. */
#include "../lib/dunc.h"
#include "../lib/video/h264.h"
#include "../lib/media/mp4.h"
#include "../lib/media/aac.h"

static int WW = 960, WH = 600;
#define CTRL_H 56
static struct app a;
static long snd(int op, long x, long y) { return sys_call(SYS_SOUND, op, x, y, 0, 0); }

/* ---------------- the list of videos (when opened without a file) ---------------- */
struct vid { char path[160], title[64]; };
static struct vid vids[200]; static int nvids, list_off, hover = -1;
static bool is_video(const char *n) {
    int l = strlen(n); if (l < 5) return false;
    const char *e = n + l - 4;
    return (e[0] == '.' && (e[1] | 32) == 'm' && (e[2] | 32) == 'p' && e[3] == '4') ||
           (e[0] == '.' && (e[1] | 32) == 'm' && (e[2] | 32) == '4' && (e[3] | 32) == 'v') ||
           (e[0] == '.' && (e[1] | 32) == 'm' && (e[2] | 32) == 'o' && (e[3] | 32) == 'v');
}
static void add_dir(const char *dir, int depth) {
    static struct dunc_dirent e[128];
    int n = fs_list(dir, e, 128);
    for (int i = 0; i < n && nvids < 200; i++) {
        char p[160]; ksnprintf(p, sizeof p, "%s/%s", dir, e[i].name);
        if (e[i].is_dir) { if (depth > 0 && e[i].name[0] != '.') { char sub[160]; strncpy0(sub, p, sizeof sub); add_dir(sub, depth - 1); } continue; }
        if (!is_video(e[i].name)) continue;
        struct vid *v = &vids[nvids++];
        strncpy0(v->path, p, sizeof v->path);
        strncpy0(v->title, e[i].name, sizeof v->title);
    }
}
static void scan(void) {
    nvids = 0;
    add_dir("/home/Videos", 2); add_dir("/home/Downloads", 1); add_dir("/home", 0);
    struct dunc_volume v[8]; int nv = fs_volumes(v, 8);
    for (int i = 0; i < nv; i++) if (!strncmp(v[i].path, "/usb", 4)) add_dir(v[i].path, 1);
}

/* ---------------- playback state ---------------- */
static uint8_t *file; static int64_t flen;
static mp4 *mv; static int vt = -1, at = -1;
static h264dec *vd; static aacdec *ad;
static long vs, as_;                        /* next video / audio sample */
static bool playing, paused, ended;
static char title[80], msg[160];
static long dur_ms;
/* the clock: from the sound card when there is sound, else from the system time */
static int out_rate; static int64_t audio_base_ms, audio_frames_out;   /* frames written since audio_base_ms */
static uint64_t wall_start; static int64_t wall_base_ms, paused_at_ms;
static int16_t pcm[AAC_MAX_SAMPLES * 2], pend[AAC_MAX_SAMPLES * 2 * 7 + 64];   /* room for 8 kHz -> 48 kHz */
static int npend, ppos, src_rate, src_ch; static uint32_t rs_pos; static int16_t rs_last[2];
/* a decoded picture waiting for its time (the decoder's buffers are kept until it is shown) */
static struct h264_frame held; static bool have_held; static int64_t held_ms;
static int64_t seek_floor_ms = -1;          /* after a seek: skip pictures before this */
static int64_t shown_ms;
static long frames_shown, frames_dropped;

static int64_t clock_ms(void) {
    if (at >= 0 && out_rate) {
        int64_t played = audio_frames_out * 1000 / out_rate - snd(SOUND_STREAM_STATUS, 0, 0);
        if (played < 0) played = 0;
        return audio_base_ms + played;
    }
    if (paused) return paused_at_ms;
    return wall_base_ms + (int64_t)(sys_time() - wall_start);
}

static void stop(void) {
    if (playing && at >= 0) snd(SOUND_STREAM_STOP, 0, 0);
    if (vd) { h264_close(vd); vd = 0; }
    if (ad) { aac_close(ad); ad = 0; }
    if (mv) { mp4_close(mv); mv = 0; }
    free(file); file = 0;
    playing = paused = ended = have_held = false; vt = at = -1;
}

static void restart_audio(int64_t from_ms) {
    npend = ppos = 0; rs_pos = 0; rs_last[0] = rs_last[1] = 0;
    audio_base_ms = from_ms; audio_frames_out = 0;
    if (at >= 0) { snd(SOUND_STREAM_START, out_rate, 0); if (paused) snd(SOUND_STREAM_PAUSE, 1, 0); }
}

/* decode one AAC frame into pend[] (stereo at out_rate); false when the sound track is over */
static bool audio_more(void) {
    struct mp4_sample s;
    for (int guard = 0; guard < 8; guard++) {
        if (!mp4_sample(mv, at, as_, &s)) return false;
        as_++;
        if (s.offset < 0 || s.offset + s.size > flen) continue;
        int n = aac_decode(ad, file + s.offset, (int)s.size, pcm);
        if (n <= 0) continue;
        int ch = aac_channels(ad) == 1 ? 1 : 2, rate = aac_sample_rate(ad);
        if (rate != src_rate) { src_rate = rate; rs_pos = 0; }
        src_ch = ch;
        npend = 0; ppos = 0;
        if (rate == out_rate) {
            for (int i = 0; i < n; i++) { pend[2 * i] = pcm[i * ch]; pend[2 * i + 1] = pcm[i * ch + ch - 1]; }
            npend = n;
        } else {                                          /* linear resampling to 48 kHz */
            uint32_t step = (uint32_t)((uint64_t)rate * 65536 / out_rate);
            while ((rs_pos >> 16) < (uint32_t)n && npend < (int)(sizeof pend / 4) - 2) {
                int k = rs_pos >> 16; uint32_t f = rs_pos & 0xFFFF;
                for (int c = 0; c < 2; c++) {
                    int a0 = k == 0 ? rs_last[c] : pcm[(k - 1) * ch + (c && ch == 2)], a1 = pcm[k * ch + (c && ch == 2)];
                    pend[2 * npend + c] = (int16_t)(a0 + (int)(((int64_t)(a1 - a0) * f) >> 16));
                }
                npend++; rs_pos += step;
            }
            rs_pos -= (uint32_t)n << 16;
            rs_last[0] = pcm[(n - 1) * ch]; rs_last[1] = pcm[(n - 1) * ch + ch - 1];
        }
        return true;
    }
    return true;
}
static bool audio_done;
static void feed_audio(void) {
    if (at < 0 || paused || audio_done) return;
    for (int guard = 0; guard < 64; guard++) {
        if (ppos >= npend && !audio_more()) { audio_done = true; return; }
        if (ppos >= npend) continue;
        long took = snd(SOUND_STREAM_WRITE, (long)(pend + ppos * 2), (long)(npend - ppos) * 4);
        if (took <= 0) return;                            /* the card's buffer is full for now */
        ppos += (int)(took / 4); audio_frames_out += took / 4;
    }
}

/* ---------------- pictures: YUV to the screen ---------------- */
static int vx, vy, vw, vh;                  /* where the picture goes in the window */
static int xmap[4096];
static int16_t tY[256], tRV[256], tGU[256], tGV[256], tBU[256];
static int tab_matrix = -1, tab_full = -1;
static void make_tables(int matrix, int full) {
    if (matrix == tab_matrix && full == tab_full) return;
    tab_matrix = matrix; tab_full = full;
    bool b709 = matrix == 1 || (matrix != 5 && matrix != 6 && held.height >= 720);   /* HD is BT.709 unless it says otherwise */
    /* coefficients * 64 */
    double kr = b709 ? 0.2126 : 0.299, kb = b709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    double ys = full ? 1.0 : 255.0 / 219.0, cs = full ? 1.0 : 255.0 / 224.0;
    for (int i = 0; i < 256; i++) {
        double y = full ? i : (i - 16) * ys, c = (i - 128) * cs;
        tY[i] = (int16_t)(y * 64 + 0.5);
        tRV[i] = (int16_t)(c * 2 * (1 - kr) * 64);
        tBU[i] = (int16_t)(c * 2 * (1 - kb) * 64);
        tGU[i] = (int16_t)(-c * 2 * (1 - kb) * kb / kg * 64);
        tGV[i] = (int16_t)(-c * 2 * (1 - kr) * kr / kg * 64);
    }
}
static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 * 64 ? 255 : (uint32_t)(v >> 6); }
static void layout(int fw, int fh) {
    int aw = WW, ah = WH - CTRL_H;
    if (fw <= 0 || fh <= 0) { vx = vy = 0; vw = aw; vh = ah; return; }
    vw = aw; vh = (int)((int64_t)fh * aw / fw);
    if (vh > ah) { vh = ah; vw = (int)((int64_t)fw * ah / fh); }
    if (vw > 4096) vw = 4096;
    vx = (aw - vw) / 2; vy = (ah - vh) / 2;
}
static int xfr[4096];
/* scale the picture to the window smoothly: brightness (Y) bilinear, colour (U, V) from the nearest sample */
static void blit(const struct h264_frame *f) {
    struct canvas *c = &a.c;
    layout(f->width, f->height);
    make_tables(f->matrix, f->full_range);
    int fw = f->width, fh = f->height;
    for (int x = 0; x < vw; x++) {                       /* source position of each column, in 1/256 pixels (pixel centres) */
        int p = (int)(((int64_t)(2 * x + 1) * fw * 128) / vw) - 128; if (p < 0) p = 0;
        if (p > (fw - 1) * 256) p = (fw - 1) * 256;
        xmap[x] = p >> 8; xfr[x] = p & 255;
    }
    for (int y = 0; y < vh; y++) {
        int q = (int)(((int64_t)(2 * y + 1) * fh * 128) / vh) - 128; if (q < 0) q = 0;
        if (q > (fh - 1) * 256) q = (fh - 1) * 256;
        int sy = q >> 8, fy = q & 255, sy2 = sy + 1 < fh ? sy + 1 : sy;
        const uint8_t *Y0 = f->y + (long)sy * f->stride_y, *Y1 = f->y + (long)sy2 * f->stride_y;
        int cy = (sy + (fy >= 128)) >> 1; if (cy > (fh - 1) >> 1) cy = (fh - 1) >> 1;
        const uint8_t *U = f->u + (long)cy * f->stride_uv, *V = f->v + (long)cy * f->stride_uv;
        uint32_t *d = c->px + (long)(vy + y) * c->stride + vx;
        for (int x = 0; x < vw; x++) {
            int sx = xmap[x], fx = xfr[x], sx2 = sx + 1 < fw ? sx + 1 : sx;
            int top = Y0[sx] * (256 - fx) + Y0[sx2] * fx, bot = Y1[sx] * (256 - fx) + Y1[sx2] * fx;
            int yv = (top * (256 - fy) + bot * fy + 32768) >> 16;
            int cx = (sx + (fx >= 128)) >> 1; if (cx > (fw - 1) >> 1) cx = (fw - 1) >> 1;
            int yy = tY[yv], u = U[cx], v = V[cx];
            d[x] = clamp8(yy + tRV[v]) << 16 | clamp8(yy + tGU[u] + tGV[v]) << 8 | clamp8(yy + tBU[u]);
        }
    }
}

/* ---------------- drawing ---------------- */
static void fmt_time(long ms, char *o, int n) { long s = ms / 1000; if (s >= 3600) ksnprintf(o, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60); else ksnprintf(o, n, "%ld:%02ld", s / 60, s % 60); }
static int bar_x0(void) { return 140; }
static int bar_x1(void) { return WW - 130; }
static void draw_controls(void) {
    struct canvas *c = &a.c;
    int y = WH - CTRL_H;
    gfx_fill(c, 0, y, WW, CTRL_H, 0x0C0C0B);
    ui_button(c, 14, y + 11, 104, 34, !playing || paused ? "Play" : "Pause", BTN_PRIMARY, hover == 1, !mv);
    long pos = mv ? (long)(shown_ms > 0 ? shown_ms : 0) : 0;
    if (pos > dur_ms) pos = dur_ms;
    int x0 = bar_x0(), x1 = bar_x1();
    gfx_rrect(c, x0, y + 25, x1 - x0, 6, 3, 0x2A2A28, 255);
    if (dur_ms > 0) {
        int px = x0 + (int)((int64_t)(x1 - x0) * pos / dur_ms);
        gfx_rrect(c, x0, y + 25, px - x0, 6, 3, UI_ACCENT, 255);
        gfx_circle(c, px, y + 28, 7, UI_TEXT, 255);
    }
    char t1[16], t2[16], t[40]; fmt_time(pos, t1, sizeof t1); fmt_time(dur_ms, t2, sizeof t2);
    ksnprintf(t, sizeof t, "%s / %s", t1, t2);
    gfx_text(c, &font_ui, x1 + 14, y + 34, UI_MUTED, t);
}
static void draw_list(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH - CTRL_H, UI_BG);
    gfx_text(c, &font_title, 24, 42, UI_TEXT, "Video");
    char sub[96]; ksnprintf(sub, sizeof sub, "%d video%s  -  MP4 files (H.264 with AAC sound)", nvids, nvids == 1 ? "" : "s");
    gfx_text(c, &font_ui, 24, 66, UI_MUTED, sub);
    ui_button(c, WW - 130, 18, 110, 32, "Look again", BTN_NORMAL, hover == 10, false);
    int y = 84, rows = (WH - CTRL_H - 20 - y) / 40;
    if (!nvids) {
        gfx_text(c, &font_ui, 24, y + 30, UI_TEXT, "No videos found.");
        gfx_text(c, &font_ui, 24, y + 54, UI_MUTED, "Put MP4 files in the Videos folder (Files > Home > Videos), Downloads, or on a USB drive,");
        gfx_text(c, &font_ui, 24, y + 74, UI_MUTED, "then press \"Look again\". You can also double-click an MP4 file in Files.");
    }
    for (int i = list_off; i < nvids && i < list_off + rows; i++, y += 40) {
        if (hover == 100 + i) gfx_rrect(c, 16, y, WW - 32, 36, 9, UI_PANEL, 255);
        gfx_text(c, &font_ui, 32, y + 23, UI_TEXT, vids[i].title);
    }
    if (msg[0]) gfx_text(c, &font_ui, 24, WH - CTRL_H - 16, UI_RED, msg);
}
static void draw_all(void) {
    struct canvas *c = &a.c;
    if (!mv) draw_list();
    else {
        gfx_fill(c, 0, 0, WW, WH - CTRL_H, 0);
        if (have_held) blit(&held);
        else if (msg[0]) gfx_text_center(c, &font_ui, 0, WW, (WH - CTRL_H) / 2, UI_MUTED, msg);
    }
    draw_controls();
    app_flush(&a);
}

/* ---------------- opening and seeking ---------------- */
static bool open_video(const char *path) {
    stop(); msg[0] = 0;
    const char *nm = path; for (const char *q = path; *q; q++) if (*q == '/') nm = q + 1;
    strncpy0(title, nm, sizeof title);
    struct dunc_stat st;
    if (fs_stat(path, &st) < 0) { ksnprintf(msg, sizeof msg, "\"%s\" can't be read.", title); return false; }
    if (st.size > (1500l << 20)) { ksnprintf(msg, sizeof msg, "\"%s\" is too big (over 1.5 GB) for Video for now.", title); return false; }
    file = malloc(st.size + 16);
    if (!file) { ksnprintf(msg, sizeof msg, "Not enough memory to open \"%s\".", title); return false; }
    flen = fs_read(path, file, 0, st.size);
    mv = flen > 0 ? mp4_open(file, flen) : 0;
    if (!mv) { free(file); file = 0; ksnprintf(msg, sizeof msg, "\"%s\" isn't an MP4 video Video can read.", title); return false; }
    vt = mp4_find(mv, MP4_VIDEO); at = mp4_find(mv, MP4_AUDIO);
    const struct mp4_track *v = vt >= 0 ? mp4_track(mv, vt) : 0, *s = at >= 0 ? mp4_track(mv, at) : 0;
    if (v && strcmp(v->codec, "avc1") && strcmp(v->codec, "avc3")) {
        ksnprintf(msg, sizeof msg, "This video is in a format Video can't play yet (%s). It plays H.264 (avc1).", v->codec);
        vt = -1;
    }
    if (s && (strcmp(s->codec, "mp4a") || (s->object_type && s->object_type != 0x40))) at = -1;
    if (vt < 0 && at < 0) { if (!msg[0]) ksnprintf(msg, sizeof msg, "\"%s\" has no video or sound Video can play.", title); mp4_close(mv); mv = 0; free(file); file = 0; return false; }
    if (vt >= 0) { vd = h264_open(v->config, v->config_len); if (!vd) { ksnprintf(msg, sizeof msg, "The video in \"%s\" can't be decoded.", title); vt = -1; } }
    if (at >= 0) { ad = aac_open(s->config, s->config_len); if (!ad) at = -1; }
    if (at >= 0) {
        int r = s->sample_rate ? s->sample_rate : 48000;
        out_rate = r == 44100 ? 44100 : 48000; src_rate = 0;
        if (snd(SOUND_STREAM_START, out_rate, 0) < 0) { at = -1; aac_close(ad); ad = 0; }
    }
    dur_ms = mp4_duration_ms(mv);
    vs = as_ = 0; playing = true; paused = ended = audio_done = false;
    restart_audio(0);
    wall_start = sys_time(); wall_base_ms = 0;
    shown_ms = 0; seek_floor_ms = -1; frames_shown = frames_dropped = 0;
    char t[100]; ksnprintf(t, sizeof t, "%s - Video", title); app_title(&a, t);
    return true;
}
static void seek_to(int64_t ms) {
    if (!mv) return;
    if (ms < 0) ms = 0;
    if (dur_ms > 0 && ms > dur_ms - 500) ms = dur_ms > 500 ? dur_ms - 500 : 0;
    int64_t start = ms;
    if (vt >= 0) {
        vs = mp4_sample_at(mv, vt, ms, 1); if (vs < 0) vs = 0;
        struct mp4_sample s; if (mp4_sample(mv, vt, vs, &s)) start = mp4_ms(mv, vt, s.pts);
        h264_reset(vd); have_held = false;
    }
    seek_floor_ms = ms;
    if (at >= 0) {
        as_ = mp4_sample_at(mv, at, ms, 0); if (as_ < 0) as_ = 0;
        struct mp4_sample s; int64_t a0 = ms; if (mp4_sample(mv, at, as_, &s)) a0 = mp4_ms(mv, at, s.pts);
        aac_reset(ad); audio_done = false;
        restart_audio(a0);
    }
    (void)start;
    wall_start = sys_time(); wall_base_ms = ms; paused_at_ms = ms;
    ended = false; shown_ms = ms;
}
static void toggle_pause(void) {
    if (!mv) return;
    if (ended) { seek_to(0); paused = false; if (at >= 0) snd(SOUND_STREAM_PAUSE, 0, 0); return; }
    paused = !paused;
    if (at >= 0) snd(SOUND_STREAM_PAUSE, paused, 0);
    if (paused) paused_at_ms = clock_ms(); else { wall_start = sys_time(); wall_base_ms = paused_at_ms; }
}

/* decode until a picture comes out: 1 = got one, 0 = not yet (try again next turn), -1 = the video track ended */
static int next_picture(void) {
    if (vt < 0) return -1;
    for (int guard = 0; guard < 64; guard++) {
        if (h264_get_frame(vd, &held)) {
            held_ms = held.pts;
            if (seek_floor_ms >= 0 && held_ms < seek_floor_ms - 20) continue;   /* catching up after a seek: not shown */
            have_held = true; return 1;
        }
        struct mp4_sample s;
        if (!mp4_sample(mv, vt, vs, &s)) {
            static bool flushed;
            if (!flushed) { h264_flush(vd); flushed = true; continue; }
            flushed = false; return -1;
        }
        vs++;
        if (s.offset < 0 || s.offset + s.size > flen) continue;
        h264_decode(vd, file + s.offset, (int)s.size, mp4_ms(mv, vt, s.pts));
    }
    return 0;
}

/* one step of playback; returns how long the main loop may wait */
static long play_step(void) {
    if (!mv || !playing) return -1;
    feed_audio();
    if (paused || ended) return 100;
    int64_t now = clock_ms();
    if (vt < 0) {                                         /* sound only */
        shown_ms = now;
        if (audio_done && snd(SOUND_STREAM_STATUS, 0, 0) <= 20) { ended = true; draw_all(); }
        static uint64_t last; if (sys_time() - last > 250) { last = sys_time(); draw_controls(); app_flush_rect(&a, 0, WH - CTRL_H, WW, CTRL_H); }
        return 20;
    }
    if (!have_held) {
        int np = next_picture();
        if (np == 0) return 0;                            /* still catching up (e.g. after a seek): carry on next turn */
        if (np < 0) {                                     /* no more pictures */
            if (at < 0 || audio_done) { ended = true; draw_all(); }
            return 20;
        }
    }
    bool force = false;
    if (seek_floor_ms >= 0) {                             /* first picture after a seek: show it and line the clock up */
        seek_floor_ms = -1; force = true;
        if (at < 0) { wall_start = sys_time(); wall_base_ms = held_ms; }
        now = clock_ms();
    }
    static uint64_t last_shown;
    if (sys_time() - last_shown > 250) force = true;      /* never more than a quarter second without a new picture */
    if (!force && held_ms > now + 4) { long w = (long)(held_ms - now); return w > 20 ? 20 : w; }
    if (!force && now - held_ms > 120 && !held.keyframe) {   /* too late: skip it to catch up */
        frames_dropped++; have_held = false; return 0;
    }
    last_shown = sys_time();
    shown_ms = held_ms;
    blit(&held);
    app_flush_rect(&a, vx, vy, vw, vh);                   /* just the picture; the controls only when the time shown changes */
    static long last_sec = -1;
    if (shown_ms / 1000 != last_sec) { last_sec = shown_ms / 1000; draw_controls(); app_flush_rect(&a, 0, WH - CTRL_H, WW, CTRL_H); }
    frames_shown++;
    have_held = false;
    return 0;
}

static int hit(int x, int y) {
    if (y >= WH - CTRL_H) {
        if (ui_in(x, y, 14, WH - CTRL_H + 11, 104, 34)) return 1;
        if (x >= bar_x0() - 8 && x <= bar_x1() + 8) return 2;
        return -1;
    }
    if (mv) return 3;
    if (ui_in(x, y, WW - 130, 18, 110, 32)) return 10;
    int yy = 84, rows = (WH - CTRL_H - 20 - yy) / 40;
    for (int i = list_off; i < nvids && i < list_off + rows; i++, yy += 40) if (ui_in(x, y, 16, yy, WW - 32, 36)) return 100 + i;
    return -1;
}

int main(void) {
    char arg[200] = ""; sys_getarg(arg, sizeof arg);
    if (!app_open_ex(&a, WW, WH, "Video", WIN_RESIZABLE)) return 1;
    fs_mkdir("/home/Videos");
    scan();
    if (arg[0]) open_video(arg);
    draw_all();
    for (;;) {
        long wait = play_step();
        struct dunc_event e;
        if (sys_event(&e, wait) <= 0) continue;
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw_all(); } continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1;
            if (h != hover) { hover = h; if (!mv) draw_all(); else { draw_controls(); app_flush_rect(&a, 0, WH - CTRL_H, WW, CTRL_H); } }
            continue;
        }
        if (e.type == EV_WHEEL && !mv) { list_off += e.key > 0 ? 3 : -3; if (list_off > nvids - 3) list_off = nvids - 3; if (list_off < 0) list_off = 0; draw_all(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 1) { if (mv) toggle_pause(); }
            else if (h == 2 && mv && dur_ms > 0) {
                int x = e.x < bar_x0() ? bar_x0() : e.x > bar_x1() ? bar_x1() : e.x;
                seek_to((int64_t)dur_ms * (x - bar_x0()) / (bar_x1() - bar_x0()));
            } else if (h == 3) toggle_pause();
            else if (h == 10) { scan(); }
            else if (h >= 100 && h - 100 < nvids) open_video(vids[h - 100].path);
            draw_all();
            continue;
        }
        if (e.type != EV_KEY) continue;
        int k = e.key, code = KEY_CODE(k);
        if (k == ' ') toggle_pause();
        else if (code == KEY_RIGHT && mv) seek_to(shown_ms + 10000);
        else if (code == KEY_LEFT && mv) seek_to(shown_ms - 10000);
        else if (k == 27 && mv && !arg[0]) { stop(); app_title(&a, "Video"); msg[0] = 0; }
        else continue;
        draw_all();
    }
    stop();
    return 0;
}
