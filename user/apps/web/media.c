/* Web: <video> and <audio> on pages.
 *
 * A media element is shown like a form control (a box laid out in the page). Pressing play (or a page
 * script calling play()) downloads the whole file, then plays it in the box: MP4 (H.264 video, AAC
 * sound), MP3 and WAV. The sound goes to the same sound stream the Music and Video apps use, and the
 * pictures follow the sound's clock. One element plays at a time; starting another pauses it.
 *
 * Downloads and decoding happen from the main loop (media_tick), never inside a script, and events
 * for scripts (play, pause, ended, timeupdate...) are queued and sent from the main loop too.
 * Compiled with floating point (the decoders and the colour tables use it). */
#include "web.h"
#include "../../lib/video/h264.h"
#include "../../lib/media/mp4.h"
#include "../../lib/media/aac.h"
#include "../../lib/audio/mp3.h"

#define BAR_H_ 36                        /* the controls bar */
#define MAX_BYTES (400l << 20)
enum { MS_IDLE, MS_LOADING, MS_READY, MS_PLAYING, MS_PAUSED, MS_ENDED, MS_ERROR };
enum { F_NONE, F_MP4, F_MP3, F_WAV };

struct wmedia {
    struct node *n; bool video, controls, loop, muted, want_play, orphan, autoplaying;
    int state; char msg[140]; char url[600];
    uint8_t *file; long flen;
    int fmt;
    mp4 *mv; h264dec *vd; aacdec *ad; mp3dec *md; struct mp3_info mi; int vt, at;
    long vs, as_;                          /* next video / audio sample */
    long wav_data, wav_len, wav_pos; int wav_rate, wav_ch, wav_bits;
    int vw, vh;                            /* the video's picture size */
    long dur_ms; int64_t shown_ms, pend_seek;
    double volume;
    uint32_t *pic; int pw, ph;             /* the last picture, scaled to fit the box */
    int bw, bh;                            /* the box size it was last drawn at */
    struct wmedia *next;
};
static struct wmedia *all, *active, *loading_m;
static long snd(int op, long x, long y) { return sys_call(SYS_SOUND, op, x, y, 0, 0); }

/* ---------------- events for scripts, sent from the main loop ---------------- */
static struct { struct node *n; const char *type; } evq[32]; static int nev;
static void fire(struct wmedia *m, const char *type) {
    if (nev < 32 && !m->orphan) { evq[nev].n = m->n; evq[nev].type = type; nev++; }
}

/* ---------------- attaching to elements ---------------- */
static struct wmedia *M(struct node *n) { return n && n->fc && n->fc->type == FC_MEDIA ? (struct wmedia *)n->fc->media : 0; }
void media_attach(struct doc *d, struct node *n) {
    if (n->fc) return;
    struct fctl *fc = ar_alloc(&d->ar, sizeof *fc);
    struct wmedia *m = calloc(1, sizeof *m);
    if (!fc || !m) { free(m); return; }
    fc->type = FC_MEDIA; fc->cap = 2; fc->val = ar_alloc(&d->ar, 2); fc->media = m;
    if (!fc->val) { free(m); return; }
    m->n = n; m->video = n->tag == T_VIDEO; m->volume = 1.0; m->vt = m->at = -1; m->pend_seek = -1;
    m->controls = node_attr(n, "controls") != 0 || !m->video;
    m->loop = node_attr(n, "loop") != 0; m->muted = node_attr(n, "muted") != 0;
    m->next = all; all = m;
    n->fc = fc;
    /* autoplay: like other browsers, only without sound (muted background videos), and only small files */
    if (node_attr(n, "autoplay") && m->muted && m->video) { m->want_play = true; m->autoplaying = true; }
}

static void close_decoders(struct wmedia *m) {
    if (m == active) { snd(SOUND_STREAM_STOP, 0, 0); active = 0; }
    if (m->vd) h264_close(m->vd);
    if (m->ad) aac_close(m->ad);
    if (m->md) mp3_close(m->md);
    if (m->mv) mp4_close(m->mv);
    m->vd = 0; m->ad = 0; m->md = 0; m->mv = 0; m->vt = m->at = -1;
    free(m->file); m->file = 0; m->flen = 0; m->fmt = F_NONE;
}
static void media_free(struct wmedia *m) { close_decoders(m); free(m->pic); free(m); }
/* the page goes away: stop and forget everything (the one being downloaded is freed when that ends) */
void media_page_end(void) {
    for (struct wmedia *m = all, *nx; m; m = nx) {
        nx = m->next;
        if (m == loading_m) { m->orphan = true; m->n = 0; continue; }
        media_free(m);
    }
    all = 0; active = 0; nev = 0;
}

/* ---------------- the box size (layout) ---------------- */
void media_box(struct node *n, int *w, int *h) {
    struct wmedia *m = M(n);
    if (!m || !m->video) { *w = 300; *h = BAR_H_ + 4; return; }
    const char *aw = node_attr(n, "width"), *ah = node_attr(n, "height");
    int W = aw ? katoi(aw) : 0, H = ah ? katoi(ah) : 0;
    int iw = m->vw > 0 ? m->vw : 300, ih = m->vw > 0 ? m->vh : 150;
    if (W > 0 && H <= 0) H = (int)((long)ih * W / iw);
    else if (H > 0 && W <= 0) W = (int)((long)iw * H / ih);
    else if (W <= 0) { W = iw; H = ih; }
    if (W > 4096) W = 4096;
    if (H > 4096) H = 4096;
    *w = W; *h = H;
}

/* ---------------- which file: src, or the first <source> we can play ---------------- */
static bool has(const char *h, const char *n) { for (; *h; h++) { int i = 0; while (n[i] && h[i] == n[i]) i++; if (!n[i]) return true; } return false; }
static bool type_ok(const char *t) {
    if (!t || !t[0]) return true;
    char l[80]; int k = 0; for (; t[k] && k < 79; k++) l[k] = (char)(t[k] >= 'A' && t[k] <= 'Z' ? t[k] + 32 : t[k]); l[k] = 0;
    return !has(l, "webm") && !has(l, "ogg") && !has(l, "opus") && !has(l, "vp8") && !has(l, "vp9") && !has(l, "av01") &&
           !has(l, "hev") && !has(l, "hvc") && !has(l, "flac") && !has(l, "mpegurl") && !has(l, "dash");
}
static bool pick_source(struct wmedia *m) {
    const char *src = node_attr(m->n, "src");
    if (!src || !src[0]) {
        src = 0;
        for (struct node *c = m->n->first; c && !src; c = c->next)
            if (c->type == N_ELEM && c->tag == T_SOURCE && node_attr(c, "src") && type_ok(node_attr(c, "type"))) src = node_attr(c, "src");
    }
    if (!src || !src[0]) return false;
    char base[600]; web_page_url(base, sizeof base);
    resolve_url(web_doc ? web_doc->base : base, src, m->url, sizeof m->url);
    return true;
}

/* ---------------- sound: decode, resample to 44.1/48 kHz stereo, feed the stream ---------------- */
static int out_rate; static int64_t audio_base_ms, audio_frames_out;
static uint64_t wall_start; static int64_t wall_base_ms;
static int16_t pcm[4096 * 2], pend_pcm[4096 * 2 * 7 + 64];
static int npend, ppos, src_rate; static uint32_t rs_pos; static int16_t rs_last[2];
static bool audio_done;
static bool has_audio(struct wmedia *m) { return m->at >= 0 || m->md || m->fmt == F_WAV; }
static int64_t clock_ms(struct wmedia *m) {
    if (has_audio(m) && out_rate) {
        long q = snd(SOUND_STREAM_STATUS, 0, 0); if (q < 0) q = 0;
        int64_t played = audio_frames_out * 1000 / out_rate - q;
        if (played < 0) played = 0;
        return audio_base_ms + played;
    }
    if (m->state != MS_PLAYING) return wall_base_ms;
    return wall_base_ms + (int64_t)(sys_time() - wall_start);
}
static void restart_audio(struct wmedia *m, int64_t from_ms) {
    npend = ppos = 0; rs_pos = 0; rs_last[0] = rs_last[1] = 0; src_rate = 0; audio_done = false;
    audio_base_ms = from_ms; audio_frames_out = 0;
    if (has_audio(m)) { snd(SOUND_STREAM_START, out_rate, 0); if (m->state != MS_PLAYING) snd(SOUND_STREAM_PAUSE, 1, 0); }
}
/* n frames of ch channels at rate -> pend_pcm (stereo, out_rate), with the volume applied */
static void take(struct wmedia *m, const int16_t *in, int n, int ch, int rate) {
    if (rate != src_rate) { src_rate = rate; rs_pos = 0; }
    npend = ppos = 0;
    int vol = m->muted ? 0 : (int)(m->volume * 256 + 0.5);
    if (rate == out_rate) {
        for (int i = 0; i < n; i++) { pend_pcm[2 * i] = (int16_t)(in[i * ch] * vol >> 8); pend_pcm[2 * i + 1] = (int16_t)(in[i * ch + ch - 1] * vol >> 8); }
        npend = n;
        return;
    }
    uint32_t step = (uint32_t)((uint64_t)rate * 65536 / out_rate);
    while ((rs_pos >> 16) < (uint32_t)n && npend < (int)(sizeof pend_pcm / 4) - 2) {
        int k = rs_pos >> 16; uint32_t f = rs_pos & 0xFFFF;
        for (int c = 0; c < 2; c++) {
            int ci = c && ch == 2;
            int a0 = k == 0 ? rs_last[c] : in[(k - 1) * ch + ci], a1 = in[k * ch + ci];
            int v = a0 + (int)(((int64_t)(a1 - a0) * f) >> 16);
            pend_pcm[2 * npend + c] = (int16_t)(v * vol >> 8);
        }
        npend++; rs_pos += step;
    }
    rs_pos -= (uint32_t)n << 16;
    rs_last[0] = in[(n - 1) * ch]; rs_last[1] = in[(n - 1) * ch + ch - 1];
}
static bool audio_more(struct wmedia *m) {           /* false: the sound is over */
    for (int guard = 0; guard < 8; guard++) {
        if (m->fmt == F_MP3) {
            int n = mp3_decode(m->md, pcm);
            if (n <= 0) return false;
            take(m, pcm, n, m->mi.channels == 1 ? 1 : 2, m->mi.sample_rate);
            return true;
        }
        if (m->fmt == F_WAV) {
            int bpf = m->wav_ch * (m->wav_bits / 8);
            long left = (m->wav_len - m->wav_pos) / bpf;
            if (left <= 0) return false;
            int n = left > 1024 ? 1024 : (int)left;
            const uint8_t *p = m->file + m->wav_data + m->wav_pos;
            int ch = m->wav_ch >= 2 ? 2 : 1;
            for (int i = 0; i < n; i++)
                for (int c = 0; c < ch; c++) {
                    const uint8_t *s = p + (long)i * bpf + c * (m->wav_bits / 8);
                    pcm[i * ch + c] = m->wav_bits == 8 ? (int16_t)((s[0] - 128) << 8) : (int16_t)(s[0] | s[1] << 8);
                }
            m->wav_pos += (long)n * bpf;
            take(m, pcm, n, ch, m->wav_rate);
            return true;
        }
        struct mp4_sample s;
        if (!mp4_sample(m->mv, m->at, m->as_, &s)) return false;
        m->as_++;
        if (s.offset < 0 || s.offset + s.size > m->flen) continue;
        int n = aac_decode(m->ad, m->file + s.offset, (int)s.size, pcm);
        if (n <= 0) continue;
        take(m, pcm, n, aac_channels(m->ad) == 1 ? 1 : 2, aac_sample_rate(m->ad));
        return true;
    }
    return true;
}
static void feed_audio(struct wmedia *m) {
    if (!has_audio(m) || m->state != MS_PLAYING || audio_done) return;
    for (int guard = 0; guard < 64; guard++) {
        if (ppos >= npend && !audio_more(m)) { audio_done = true; return; }
        if (ppos >= npend) continue;
        long took = snd(SOUND_STREAM_WRITE, (long)(pend_pcm + ppos * 2), (long)(npend - ppos) * 4);
        if (took <= 0) return;
        ppos += (int)(took / 4); audio_frames_out += took / 4;
    }
}

/* ---------------- pictures ---------------- */
static int16_t tY[256], tRV[256], tGU[256], tGV[256], tBU[256];
static int tab_matrix = -1, tab_full = -1, tab_hd = -1;
static void make_tables(int matrix, int full, bool hd) {
    if (matrix == tab_matrix && full == tab_full && hd == tab_hd) return;
    tab_matrix = matrix; tab_full = full; tab_hd = hd;
    bool b709 = matrix == 1 || (matrix != 5 && matrix != 6 && hd);
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
static void fit(int fw, int fh, int bw, int bh, int *x, int *y, int *w, int *h) {   /* letterbox fw x fh into bw x bh */
    if (fw <= 0 || fh <= 0 || bw <= 0 || bh <= 0) { *x = *y = 0; *w = bw; *h = bh; return; }
    int W = bw, H = (int)((int64_t)fh * bw / fw);
    if (H > bh) { H = bh; W = (int)((int64_t)fw * bh / fh); }
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    *x = (bw - W) / 2; *y = (bh - H) / 2; *w = W; *h = H;
}
static int xmap[4096], xfr[4096];
static void to_pic(struct wmedia *m, const struct h264_frame *f) {
    int px, py, W, H; fit(f->width, f->height, m->bw > 0 ? m->bw : f->width, m->bh > 0 ? m->bh : f->height, &px, &py, &W, &H);
    if (W > 4096) W = 4096;
    if (W != m->pw || H != m->ph) { free(m->pic); m->pic = malloc((long)W * H * 4); m->pw = m->pic ? W : 0; m->ph = m->pic ? H : 0; }
    if (!m->pic) return;
    make_tables(f->matrix, f->full_range, f->height >= 720);
    int fw = f->width, fh = f->height;
    for (int x = 0; x < W; x++) {
        int p = (int)(((int64_t)(2 * x + 1) * fw * 128) / W) - 128; if (p < 0) p = 0;
        if (p > (fw - 1) * 256) p = (fw - 1) * 256;
        xmap[x] = p >> 8; xfr[x] = p & 255;
    }
    for (int y = 0; y < H; y++) {
        int q = (int)(((int64_t)(2 * y + 1) * fh * 128) / H) - 128; if (q < 0) q = 0;
        if (q > (fh - 1) * 256) q = (fh - 1) * 256;
        int sy = q >> 8, fy = q & 255, sy2 = sy + 1 < fh ? sy + 1 : sy;
        const uint8_t *Y0 = f->y + (long)sy * f->stride_y, *Y1 = f->y + (long)sy2 * f->stride_y;
        int cy = (sy + (fy >= 128)) >> 1; if (cy > (fh - 1) >> 1) cy = (fh - 1) >> 1;
        const uint8_t *U = f->u + (long)cy * f->stride_uv, *V = f->v + (long)cy * f->stride_uv;
        uint32_t *d = m->pic + (long)y * W;
        for (int x = 0; x < W; x++) {
            int sx = xmap[x], fx = xfr[x], sx2 = sx + 1 < fw ? sx + 1 : sx;
            int top = Y0[sx] * (256 - fx) + Y0[sx2] * fx, bot = Y1[sx] * (256 - fx) + Y1[sx2] * fx;
            int yv = (top * (256 - fy) + bot * fy + 32768) >> 16;
            int cx = (sx + (fx >= 128)) >> 1; if (cx > (fw - 1) >> 1) cx = (fw - 1) >> 1;
            int yy = tY[yv], u = U[cx], v = V[cx];
            d[x] = clamp8(yy + tRV[v]) << 16 | clamp8(yy + tGU[u] + tGV[v]) << 8 | clamp8(yy + tBU[u]);
        }
    }
}
static struct h264_frame held; static bool have_held; static int64_t held_ms, seek_floor_ms = -1;
static bool flushed;
static int next_picture(struct wmedia *m) {            /* 1 got one, 0 not yet, -1 the video is over */
    if (m->vt < 0) return -1;
    for (int guard = 0; guard < 64; guard++) {
        if (h264_get_frame(m->vd, &held)) {
            held_ms = held.pts;
            if (seek_floor_ms >= 0 && held_ms < seek_floor_ms - 20) continue;
            have_held = true; return 1;
        }
        struct mp4_sample s;
        if (!mp4_sample(m->mv, m->vt, m->vs, &s)) {
            if (!flushed) { h264_flush(m->vd); flushed = true; continue; }
            return -1;
        }
        m->vs++;
        if (s.offset < 0 || s.offset + s.size > m->flen) continue;
        h264_decode(m->vd, m->file + s.offset, (int)s.size, mp4_ms(m->mv, m->vt, s.pts));
    }
    return 0;
}

/* ---------------- drawing ---------------- */
static void fmt_time(long ms, char *o, int n) { if (ms < 0) ms = 0; long s = ms / 1000; if (s >= 3600) ksnprintf(o, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60); else ksnprintf(o, n, "%ld:%02ld", s / 60, s % 60); }
static void bar_geom(int w, int *sx0, int *sx1) { *sx0 = 44; *sx1 = w - 110; if (*sx1 < *sx0 + 20) *sx1 = *sx0 + 20; }
void media_draw(struct canvas *c, struct node *n, int x, int y, int w, int h) {
    struct wmedia *m = M(n);
    if (!m) return;
    m->bw = w; m->bh = h;
    const struct font *f = web_font(13, false, false), *fb = web_font(14, true, false);
    bool playing = m->state == MS_PLAYING;
    if (m->video) {
        gfx_fill(c, x, y, w, h, 0x000000);
        if (m->pic) {
            int px, py, pw, ph; fit(m->pw, m->ph, w, h, &px, &py, &pw, &ph);
            if (pw == m->pw && ph == m->ph) {
                for (int r = 0; r < ph; r++) {
                    int yy = y + py + r; if (yy < c->cy0 || yy >= c->cy1) continue;
                    for (int k = 0; k < pw; k++) { int xx = x + px + k; if (xx >= c->cx0 && xx < c->cx1) c->px[(long)yy * c->stride + xx] = m->pic[(long)r * m->pw + k]; }
                }
            } else                                     /* the box changed size: nearest picture until the next frame */
                for (int r = 0; r < ph; r++) {
                    int yy = y + py + r; if (yy < c->cy0 || yy >= c->cy1) continue;
                    const uint32_t *srow = m->pic + (long)(r * m->ph / ph) * m->pw;
                    for (int k = 0; k < pw; k++) { int xx = x + px + k; if (xx >= c->cx0 && xx < c->cx1) c->px[(long)yy * c->stride + xx] = srow[k * m->pw / pw]; }
                }
        }
        if (!playing && !(m->state == MS_PAUSED && m->pic)) {     /* the big play button */
            int r = h < 120 ? h / 4 : 30;
            gfx_circle(c, x + w / 2, y + (h - (m->controls ? BAR_H_ : 0)) / 2, r, 0x000000, 150);
            int cx = x + w / 2 - r / 3, cy = y + (h - (m->controls ? BAR_H_ : 0)) / 2;
            for (int k = 0; k < r; k++) { int hh = r * 3 / 5 - k * 3 / 5; if (hh > 0) gfx_fill(c, cx + k, cy - hh, 1, 2 * hh, 0xFFFFFF); }
        }
        if (m->msg[0]) {                                /* the message, wrapped to the box */
            char lines[4][140]; int nl = 0; const char *p = m->msg;
            while (*p && nl < 4) {
                int best = 0, k = 0; char t[140];
                while (p[k] && k < 139) {
                    t[k] = p[k]; t[k + 1] = 0;
                    if (gfx_text_width(f, t) > w - 16) break;
                    k++;
                    if (!p[k] || p[k] == ' ') best = k;
                }
                if (!best) best = k > 0 ? k : 1;
                memcpy(lines[nl], p, best); lines[nl][best] = 0; nl++;
                p += best; while (*p == ' ') p++;
            }
            int ty = y + 20;
            for (int i = 0; i < nl; i++, ty += 18) {
                int tw = gfx_text_width(f, lines[i]);
                gfx_fill(c, x + (w - tw) / 2 - 4, ty - 14, tw + 8, 18, 0x000000);
                gfx_text(c, f, x + (w - tw) / 2, ty, 0xFFFFFF, lines[i]);
            }
        }
        if (!m->controls) return;
    }
    /* the controls bar */
    int by = y + h - BAR_H_;
    if (m->video) gfx_blend(c, x, by, w, BAR_H_, 0x000000, 170);
    else { gfx_rrect(c, x, y + 2, w, BAR_H_, 18, 0xF1F1F1, 255); gfx_rrect_outline(c, x, y + 2, w, BAR_H_, 18, 0xC8C8C8, 255); by = y + 2; }
    uint32_t fg = m->video ? 0xFFFFFF : 0x1E1E1E, dim = m->video ? 0x8A8A8A : 0xB5B5B5;
    int cx = x + 22, cy = by + BAR_H_ / 2;
    if (playing) { gfx_fill(c, cx - 5, cy - 7, 4, 14, fg); gfx_fill(c, cx + 2, cy - 7, 4, 14, fg); }
    else for (int k = 0; k < 12; k++) { int hh = 7 - k * 7 / 12; if (hh > 0) gfx_fill(c, cx - 4 + k, cy - hh, 1, 2 * hh, fg); }
    int sx0, sx1; bar_geom(w, &sx0, &sx1);
    gfx_rrect(c, x + sx0, cy - 2, sx1 - sx0, 4, 2, dim, 255);
    long pos = (long)m->shown_ms;
    if (m->dur_ms > 0) {
        if (pos > m->dur_ms) pos = m->dur_ms;
        int px = x + sx0 + (int)((int64_t)(sx1 - sx0) * pos / m->dur_ms);
        gfx_rrect(c, x + sx0, cy - 2, px - x - sx0, 4, 2, m->video ? HL_ACCENT : 0x2F6FDF, 255);
        gfx_circle(c, px, cy, 6, fg, 255);
    }
    char t1[16], t2[16], t[40]; fmt_time(pos, t1, sizeof t1); fmt_time(m->dur_ms, t2, sizeof t2);
    if (m->state == MS_LOADING || m->state == MS_ERROR || (!m->video && m->msg[0])) strncpy0(t, m->state == MS_LOADING ? "loading" : "", sizeof t);
    else ksnprintf(t, sizeof t, "%s / %s", t1, t2);
    gfx_text(c, f, x + sx1 + 10, cy + 5, fg, t);
    if (!m->video && m->msg[0]) { gfx_fill(c, x + sx0, by + 4, sx1 - sx0, BAR_H_ - 8, 0xF1F1F1); gfx_text(c, f, x + sx0, cy + 5, 0x8A2A2A, m->msg); }
    (void)fb;
}

/* ---------------- loading ---------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool open_wav(struct wmedia *m) {
    const uint8_t *b = m->file; long n = m->flen, p = 12;
    if (n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    int fmt = 0;
    while (p + 8 <= n) {
        uint32_t id = rd32(b + p), sz = rd32(b + p + 4);
        if (!memcmp(b + p, "fmt ", 4) && sz >= 16) { fmt = rd16(b + p + 8); m->wav_ch = rd16(b + p + 10); m->wav_rate = (int)rd32(b + p + 12); m->wav_bits = rd16(b + p + 22); }
        else if (!memcmp(b + p, "data", 4)) { m->wav_data = p + 8; m->wav_len = sz > (uint32_t)(n - p - 8) ? n - p - 8 : sz; break; }
        (void)id;
        p += 8 + sz + (sz & 1);
    }
    if ((fmt != 1 && fmt != 0xFFFE) || !m->wav_data || (m->wav_bits != 8 && m->wav_bits != 16) || m->wav_ch < 1 || m->wav_rate < 4000) return false;
    m->dur_ms = (long)((int64_t)m->wav_len * 1000 / ((int64_t)m->wav_rate * m->wav_ch * (m->wav_bits / 8)));
    return true;
}
static void set_error(struct wmedia *m, const char *s) { strncpy0(m->msg, s, sizeof m->msg); m->state = MS_ERROR; fire(m, "error"); }
static void redraw(struct wmedia *m) { if (!m->orphan && m->n) web_redraw_node(m->n); }

static void load(struct wmedia *m) {
    close_decoders(m);
    m->msg[0] = 0;
    if (!pick_source(m)) { set_error(m, "No video or sound file given"); return; }
    m->state = MS_LOADING; strncpy0(m->msg, "Downloading...", sizeof m->msg); redraw(m);
    fire(m, "loadstart");
    char *b = 0; char ct[96] = ""; int st = 0;
    loading_m = m;
    bool autop = m->autoplaying; m->autoplaying = false;
    long n = web_download(m->url, autop ? (30l << 20) : MAX_BYTES, &b, &st, ct, sizeof ct, m->msg, sizeof m->msg, m->n);
    loading_m = 0;
    if (m->orphan) { free(b); media_free(m); return; }
    if (autop && n == -2) { m->state = MS_IDLE; m->msg[0] = 0; redraw(m); return; }   /* big: wait for a click */
    if (n < 0 || !b || st >= 400) {
        free(b);
        char e[140]; ksnprintf(e, sizeof e, n == -2 ? "Too big to play here (over 400 MB)" : st >= 400 ? "Couldn't download it (error %d)" : "Couldn't download it", st);
        set_error(m, e); redraw(m); return;
    }
    m->file = (uint8_t *)b; m->flen = n; m->msg[0] = 0;
    const uint8_t *p = m->file;
    if (n >= 12 && !memcmp(p + 4, "ftyp", 4)) m->fmt = F_MP4;
    else if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4)) m->fmt = F_WAV;
    else if (n >= 4 && (!memcmp(p, "ID3", 3) || (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0))) m->fmt = F_MP3;
    else if (n >= 4 && p[0] == 0x1A && p[1] == 0x45 && p[2] == 0xDF && p[3] == 0xA3) { set_error(m, "This is WebM, which Arc can't play yet (it plays MP4, MP3 and WAV)"); redraw(m); return; }
    else if (n >= 4 && !memcmp(p, "OggS", 4)) { set_error(m, "This is Ogg, which Arc can't play yet (it plays MP4, MP3 and WAV)"); redraw(m); return; }
    else if (n >= 7 && !memcmp(p, "#EXTM3U", 7)) { set_error(m, "This is a live stream (HLS), which Arc can't play yet"); redraw(m); return; }
    if (m->fmt == F_MP4) {
        m->mv = mp4_open(m->file, m->flen);
        if (!m->mv) { set_error(m, "This MP4 file can't be read"); redraw(m); return; }
        m->vt = m->video ? mp4_find(m->mv, MP4_VIDEO) : -1; m->at = mp4_find(m->mv, MP4_AUDIO);
        const struct mp4_track *v = m->vt >= 0 ? mp4_track(m->mv, m->vt) : 0, *s = m->at >= 0 ? mp4_track(m->mv, m->at) : 0;
        if (v && strcmp(v->codec, "avc1") && strcmp(v->codec, "avc3")) { char e[140]; ksnprintf(e, sizeof e, "The video is %s, which Arc can't play yet (it plays H.264)", v->codec); strncpy0(m->msg, e, sizeof m->msg); m->vt = -1; }
        if (s && (strcmp(s->codec, "mp4a") || (s->object_type && s->object_type != 0x40))) m->at = -1;
        if (m->vt >= 0) { m->vd = h264_open(v->config, v->config_len); if (!m->vd) m->vt = -1; else { m->vw = v->disp_w > 0 ? v->disp_w : v->width; m->vh = v->disp_h > 0 ? v->disp_h : v->height; } }
        if (m->at >= 0) { m->ad = aac_open(s->config, s->config_len); if (!m->ad) m->at = -1; }
        if (m->vt < 0 && m->at < 0) { if (!m->msg[0]) strncpy0(m->msg, "Nothing in this file Arc can play", sizeof m->msg); m->state = MS_ERROR; fire(m, "error"); redraw(m); return; }
        m->dur_ms = mp4_duration_ms(m->mv);
        out_rate = 48000;
        if (m->at >= 0) { int r = s->sample_rate; out_rate = r == 44100 || r == 22050 || r == 11025 ? 44100 : 48000; }
    } else if (m->fmt == F_MP3) {
        m->md = mp3_open(m->file, m->flen, &m->mi);
        if (!m->md) { set_error(m, "This sound file can't be read"); redraw(m); return; }
        m->dur_ms = m->mi.duration_ms;
        out_rate = m->mi.sample_rate % 11025 == 0 ? 44100 : 48000;
    } else if (m->fmt == F_WAV) {
        if (!open_wav(m)) { set_error(m, "This WAV file isn't plain PCM sound"); redraw(m); return; }
        out_rate = m->wav_rate % 11025 == 0 ? 44100 : 48000;
    } else { set_error(m, "Arc can't play this kind of file (it plays MP4, MP3 and WAV)"); redraw(m); return; }
    m->state = MS_READY; m->vs = m->as_ = 0; m->shown_ms = 0; m->wav_pos = 0;
    fire(m, "durationchange"); fire(m, "loadedmetadata"); fire(m, "loadeddata"); fire(m, "canplay"); fire(m, "canplaythrough");
    web_media_resized();                         /* the video's own size may change the layout */
}

/* ---------------- playing ---------------- */
static void seek(struct wmedia *m, int64_t ms);
static void start(struct wmedia *m) {
    if (active && active != m) {                        /* one at a time */
        struct wmedia *o = active;
        if (o->state == MS_PLAYING) { o->state = MS_PAUSED; fire(o, "pause"); redraw(o); }
        snd(SOUND_STREAM_STOP, 0, 0); active = 0;
    }
    if (m->state == MS_ENDED) seek(m, 0);
    bool fresh = active != m;
    active = m;
    m->state = MS_PLAYING;
    if (fresh) {
        int64_t at = m->shown_ms;
        have_held = false; seek_floor_ms = -1; flushed = false;
        seek(m, at);
    } else if (has_audio(m)) snd(SOUND_STREAM_PAUSE, 0, 0);
    wall_start = sys_time(); wall_base_ms = m->shown_ms;
    fire(m, "play"); fire(m, "playing");
    redraw(m);
}
static void pause_m(struct wmedia *m) {
    if (m->state != MS_PLAYING) return;
    m->shown_ms = clock_ms(m);
    m->state = MS_PAUSED;
    if (m == active && has_audio(m)) snd(SOUND_STREAM_PAUSE, 1, 0);
    wall_base_ms = m->shown_ms;
    fire(m, "pause"); redraw(m);
}
static void seek(struct wmedia *m, int64_t ms) {
    if (m->state < MS_READY || m->state == MS_ERROR) { m->pend_seek = ms; return; }
    if (ms < 0) ms = 0;
    if (m->dur_ms > 0 && ms > m->dur_ms) ms = m->dur_ms;
    if (m->fmt == F_MP4) {
        if (m->vt >= 0) {
            m->vs = mp4_sample_at(m->mv, m->vt, ms, 1); if (m->vs < 0) m->vs = 0;
            h264_reset(m->vd); if (m == active) { have_held = false; flushed = false; }
        }
        if (m->at >= 0) {
            m->as_ = mp4_sample_at(m->mv, m->at, ms, 0); if (m->as_ < 0) m->as_ = 0;
            aac_reset(m->ad);
        }
    } else if (m->fmt == F_MP3) ms = mp3_seek(m->md, ms);
    else if (m->fmt == F_WAV) {
        int bpf = m->wav_ch * (m->wav_bits / 8);
        m->wav_pos = (long)((int64_t)ms * m->wav_rate / 1000) * bpf;
        if (m->wav_pos > m->wav_len) m->wav_pos = m->wav_len;
    }
    m->shown_ms = ms;
    if (m == active) {
        seek_floor_ms = m->vt >= 0 ? ms : -1;
        restart_audio(m, ms);
        wall_start = sys_time(); wall_base_ms = ms;
    }
    if (m->state == MS_ENDED) m->state = MS_PAUSED;
    fire(m, "seeked"); fire(m, "timeupdate");
}

static void finished(struct wmedia *m) {
    if (m->loop) { seek(m, 0); return; }
    m->state = MS_ENDED; m->shown_ms = m->dur_ms;
    if (has_audio(m)) snd(SOUND_STREAM_STOP, 0, 0);
    active = 0;
    fire(m, "pause"); fire(m, "ended"); redraw(m);
}

/* one step for the playing element; returns how long the main loop may wait (-1: nothing playing) */
static long play_step(struct wmedia *m) {
    feed_audio(m);
    int64_t now = clock_ms(m);
    static uint64_t last_tu;
    if (sys_time() - last_tu > 250) { last_tu = sys_time(); fire(m, "timeupdate"); }
    if (m->vt < 0) {                                    /* sound only */
        m->shown_ms = now;
        if (audio_done && snd(SOUND_STREAM_STATUS, 0, 0) <= 20) { finished(m); return -1; }
        static uint64_t last; if (sys_time() - last > 250) { last = sys_time(); redraw(m); }
        return 20;
    }
    if (!have_held) {
        int np = next_picture(m);
        if (np == 0) return 0;
        if (np < 0) {
            if (!has_audio(m) || audio_done) { finished(m); return -1; }
            m->shown_ms = now; return 20;
        }
    }
    bool force = false;
    if (seek_floor_ms >= 0) { seek_floor_ms = -1; force = true; if (!has_audio(m)) { wall_start = sys_time(); wall_base_ms = held_ms; } now = clock_ms(m); }
    static uint64_t last_shown;
    if (sys_time() - last_shown > 250) force = true;
    if (!force && held_ms > now + 4) { long w = (long)(held_ms - now); return w > 15 ? 15 : w; }
    if (!force && now - held_ms > 120 && !held.keyframe) { have_held = false; return 0; }   /* late: skip it */
    last_shown = sys_time();
    m->shown_ms = held_ms;
    to_pic(m, &held);
    have_held = false;
    redraw(m);
    return 0;
}

long media_tick(void) {
    /* events for scripts first */
    while (nev) {
        struct node *n = evq[0].n; const char *t = evq[0].type;
        for (int i = 1; i < nev; i++) evq[i - 1] = evq[i];
        nev--;
        if (n) js_event(n, t, 0, 0, 0);
    }
    for (struct wmedia *m = all; m; m = m->next)
        if (m->want_play) {
            m->want_play = false;
            if (m->state == MS_IDLE || m->state == MS_ERROR) load(m);
            if (m->state >= MS_READY && m->state != MS_ERROR) {
                if (m->pend_seek >= 0) { m->shown_ms = m->pend_seek; m->pend_seek = -1; }
                start(m);
            }
            return 0;
        }
    if (!active || active->state != MS_PLAYING) return nev ? 0 : -1;
    return play_step(active);
}

/* ---------------- clicks and keys ---------------- */
void media_toggle(struct node *n) {
    struct wmedia *m = M(n);
    if (!m) return;
    if (m->state == MS_PLAYING) pause_m(m); else m->want_play = true;
}
void media_click(struct node *n, int x, int y, int w, int h) {
    struct wmedia *m = M(n);
    if (!m) return;
    int by = m->video ? h - BAR_H_ : 2;
    if (m->controls && y >= by && y < by + BAR_H_) {
        int sx0, sx1; bar_geom(w, &sx0, &sx1);
        if (x >= sx0 - 6 && x <= sx1 + 6 && m->dur_ms > 0 && m->state >= MS_READY && m->state != MS_ERROR) {
            int64_t ms = (int64_t)(x < sx0 ? 0 : x > sx1 ? sx1 - sx0 : x - sx0) * m->dur_ms / (sx1 - sx0);
            seek(m, ms); redraw(m); return;
        }
        if (x < 44) { media_toggle(n); return; }
        return;
    }
    if (!m->controls && js_page_active() && m->state != MS_IDLE && m->state != MS_ERROR) return;   /* the page's own buttons run it */
    media_toggle(n);
}

/* ---------------- scripts: el.play(), el.currentTime... ---------------- */
double media_js(struct node *n, const char *op, const char *name, double v) {
    struct wmedia *m = M(n);
    if (!m) return 0;
    if (!strcmp(op, "play")) { if (m->state != MS_PLAYING) m->want_play = true; return 0; }
    if (!strcmp(op, "pause")) { if (m->state == MS_PLAYING) pause_m(m); else if (m->want_play) m->want_play = false; return 0; }
    if (!strcmp(op, "load")) { if (m == active) { snd(SOUND_STREAM_STOP, 0, 0); active = 0; } close_decoders(m); m->state = MS_IDLE; m->shown_ms = 0; m->msg[0] = 0; m->vw = m->vh = 0; free(m->pic); m->pic = 0; m->pw = m->ph = 0; fire(m, "emptied"); return 0; }
    if (!strcmp(op, "set")) {
        if (!strcmp(name, "currentTime")) { if (v != v || v < 0) v = 0; seek(m, (int64_t)(v * 1000)); redraw(m); }
        else if (!strcmp(name, "volume")) { if (v >= 0 && v <= 1) { m->volume = v; fire(m, "volumechange"); } }
        else if (!strcmp(name, "muted")) { m->muted = v != 0; fire(m, "volumechange"); }
        else if (!strcmp(name, "loop")) m->loop = v != 0;
        else if (!strcmp(name, "controls")) { m->controls = v != 0 || !m->video; redraw(m); }
        return 0;
    }
    /* get */
    if (!strcmp(name, "currentTime")) return (double)(m->state == MS_PLAYING && m == active ? clock_ms(m) : m->shown_ms) / 1000.0;
    if (!strcmp(name, "duration")) return m->state >= MS_READY && m->state != MS_ERROR ? (double)m->dur_ms / 1000.0 : __builtin_nan("");
    if (!strcmp(name, "paused")) return m->state != MS_PLAYING && !m->want_play;
    if (!strcmp(name, "ended")) return m->state == MS_ENDED;
    if (!strcmp(name, "volume")) return m->volume;
    if (!strcmp(name, "muted")) return m->muted;
    if (!strcmp(name, "loop")) return m->loop;
    if (!strcmp(name, "readyState")) return m->state >= MS_READY && m->state != MS_ERROR ? 4 : 0;
    if (!strcmp(name, "networkState")) return m->state == MS_LOADING ? 2 : m->state == MS_IDLE ? 0 : 1;
    if (!strcmp(name, "videoWidth")) return m->vw;
    if (!strcmp(name, "videoHeight")) return m->vh;
    if (!strcmp(name, "error")) return m->state == MS_ERROR;
    return 0;
}
const char *media_error_text(struct node *n) { struct wmedia *m = M(n); return m && m->state == MS_ERROR ? m->msg : ""; }
const char *media_src(struct node *n) { struct wmedia *m = M(n); return m ? m->url : ""; }
