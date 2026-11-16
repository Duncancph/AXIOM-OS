/* Music: plays MP3 files from Music (and Home, Downloads and USB drives), one after another.
 * Decoding: user/lib/audio (AXIOM's MP3 decoder); sound: the kernel's HD Audio stream (SOUND_STREAM_*). */
#include "../lib/dunc.h"
#include "../lib/audio/mp3.h"

static int WW = 820, WH = 560;
static struct app a;

struct song { char path[160]; char title[80], artist[64]; };
static struct song songs[400]; static int nsongs, cur = -1, list_off;
static uint8_t *file; static long flen;
static mp3dec *dec; static struct mp3_info info;
static bool playing, paused;
static int hover = -1;
static char msg[120];
static int out_rate;                    /* what the sound card is given: 44100 or 48000 */
/* samples decoded but not yet taken by the sound card (stereo 16-bit) */
static int16_t pend[8192 * 2]; static int npend, ppos;
static int16_t frame[MP3_MAX_SAMPLES_PER_FRAME * 2];
static uint32_t rs_pos;                  /* resampler position (16.16) */
static int16_t rs_last[2];

static long snd(int op, long x, long y) { return sys_call(SYS_SOUND, op, x, y, 0, 0); }

/* ---------------- finding songs ---------------- */
static bool is_mp3(const char *n) { int l = strlen(n); return l > 4 && (n[l - 4] == '.') && (n[l - 3] | 32) == 'm' && (n[l - 2] | 32) == 'p' && n[l - 1] == '3'; }
static int add_file(const char *p) {                     /* one song: title and artist from its tags, else the file name */
    if (nsongs >= 400) return -1;
    struct song *s = &songs[nsongs];
    strncpy0(s->path, p, sizeof s->path);
    const char *nm = p; for (const char *q = p; *q; q++) if (*q == '/') nm = q + 1;
    strncpy0(s->title, nm, sizeof s->title); if (is_mp3(s->title)) s->title[strlen(s->title) - 4] = 0;
    s->artist[0] = 0;
    static uint8_t head[16384]; long r = fs_read(p, head, 0, sizeof head);      /* ID3v2 is at the front */
    if (r > 10) {
        char t[80], ar[64], al[64]; mp3_tags(head, r, t, ar, al, 64);
        if (t[0]) { utf8_to_cp1252(t, strlen(t) + 1); strncpy0(s->title, t, sizeof s->title); }
        if (ar[0]) { utf8_to_cp1252(ar, strlen(ar) + 1); strncpy0(s->artist, ar, sizeof s->artist); }
    }
    return nsongs++;
}
static void add_dir(const char *dir, int depth) {
    struct dunc_dirent e[128];
    int n = fs_list(dir, e, 128);
    for (int i = 0; i < n && nsongs < 400; i++) {
        char p[160]; ksnprintf(p, sizeof p, "%s/%s", dir, e[i].name);
        if (e[i].is_dir) { if (depth < 2 && e[i].name[0] != '.') add_dir(p, depth + 1); continue; }
        if (is_mp3(e[i].name)) add_file(p);
    }
}
static void scan(void) {
    nsongs = 0;
    add_dir("/home/Music", 0);
    add_dir("/home/Downloads", 1);
    struct dunc_dirent e[64]; int n = fs_list("/home", e, 64);   /* songs right in Home */
    for (int i = 0; i < n; i++) if (!e[i].is_dir && is_mp3(e[i].name) && nsongs < 400) {
        char p[160]; ksnprintf(p, sizeof p, "/home/%s", e[i].name); add_file(p);
    }
    struct dunc_volume v[8]; int nv = sys_call(SYS_FS_VOLUMES, (long)v, 8, 0, 0, 0);      /* USB drives */
    for (int i = 0; i < nv; i++) if (!strncmp(v[i].path, "/usb", 4)) add_dir(v[i].path, 0);
}

/* ---------------- playing ---------------- */
static void stop(void) {
    if (dec) { mp3_close(dec); dec = 0; }
    free(file); file = 0;
    if (playing) snd(SOUND_STREAM_STOP, 0, 0);
    playing = paused = false; npend = ppos = 0;
}
static void play(int i) {
    stop();
    if (i < 0 || i >= nsongs) return;
    cur = i;
    struct dunc_stat st;
    if (fs_stat(songs[i].path, &st) < 0 || st.size > (64l << 20)) { strncpy0(msg, "That file can't be read.", sizeof msg); return; }
    file = malloc(st.size + 1);
    if (!file) { strncpy0(msg, "Not enough memory for that song.", sizeof msg); return; }
    flen = fs_read(songs[i].path, file, 0, st.size);
    dec = flen > 0 ? mp3_open(file, flen, &info) : 0;
    if (!dec) { free(file); file = 0; ksnprintf(msg, sizeof msg, "\"%s\" isn't an MP3 Music can play.", songs[i].title); return; }
    out_rate = info.sample_rate == 44100 ? 44100 : 48000;
    rs_pos = 0; rs_last[0] = rs_last[1] = 0;
    if (snd(SOUND_STREAM_START, out_rate, 0) < 0) { stop(); strncpy0(msg, "No sound card found (AXIOM supports Intel HD Audio).", sizeof msg); return; }
    playing = true; msg[0] = 0;
}
/* decode one frame into pend[] (stereo, at out_rate); false at the end */
static bool decode_more(void) {
    int n = mp3_decode(dec, frame);
    if (n <= 0) return false;
    int ch = info.channels == 1 ? 1 : 2;
    npend = 0; ppos = 0;
    if (info.sample_rate == out_rate) {
        for (int i = 0; i < n; i++) { pend[2 * i] = frame[i * ch]; pend[2 * i + 1] = frame[i * ch + ch - 1]; }
        npend = n;
    } else {                                              /* linear resampling to 48 kHz */
        uint32_t step = (uint32_t)((uint64_t)info.sample_rate * 65536 / out_rate);
        while ((rs_pos >> 16) < (uint32_t)n && npend < 8192) {
            int k = rs_pos >> 16; uint32_t f = rs_pos & 0xFFFF;
            for (int c = 0; c < 2; c++) {
                int a0 = k == 0 ? rs_last[c] : frame[(k - 1) * ch + (c && ch == 2)], a1 = frame[k * ch + (c && ch == 2)];
                pend[2 * npend + c] = (int16_t)(a0 + (int)(((int64_t)(a1 - a0) * f) >> 16));
            }
            npend++; rs_pos += step;
        }
        rs_pos -= (uint32_t)n << 16;
        rs_last[0] = frame[(n - 1) * ch]; rs_last[1] = frame[(n - 1) * ch + ch - 1];
    }
    return true;
}
static void next(int dir) { if (!nsongs) return; int i = cur < 0 ? 0 : (cur + dir + nsongs) % nsongs; play(i); }
static void feed(void) {                                  /* keep the sound card's buffer topped up */
    if (!playing || paused || !dec) return;
    for (int guard = 0; guard < 64; guard++) {
        if (ppos >= npend) {
            if (!decode_more()) {                             /* the song ended: let what's queued play, then the next one */
                if (snd(SOUND_STREAM_STATUS, 0, 0) <= 50) { if (cur + 1 < nsongs) next(1); else stop(); }   /* (after the last one: stop) */
                return;
            }
        }
        long took = snd(SOUND_STREAM_WRITE, (long)(pend + ppos * 2), (long)(npend - ppos) * 4);
        if (took <= 0) return;                                /* full for now */
        ppos += (int)(took / 4);
    }
}

/* ---------------- drawing ---------------- */
static void fmt_time(long ms, char *o, int n) { long s = ms / 1000; ksnprintf(o, n, "%ld:%02ld", s / 60, s % 60); }
#define BAR_Y (WH - 130)
static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_text(c, &font_title, 24, 42, UI_TEXT, "Music");
    char sub[64]; ksnprintf(sub, sizeof sub, "%d song%s", nsongs, nsongs == 1 ? "" : "s");
    gfx_text(c, &font_ui, 24, 66, UI_MUTED, sub);
    ui_button(c, WW - 130, 18, 110, 32, "Look again", BTN_NORMAL, hover == 10, false);
    int y = 84, rows = (BAR_Y - 20 - y) / 40;
    if (!nsongs) {
        gfx_text(c, &font_ui, 24, y + 30, UI_TEXT, "No MP3 files found.");
        gfx_text(c, &font_ui, 24, y + 54, UI_MUTED, "Put songs in the Music folder (Files > Home > Music), Downloads, or on a USB drive,");
        gfx_text(c, &font_ui, 24, y + 74, UI_MUTED, "then press \"Look again\".");
    }
    for (int i = list_off; i < nsongs && i < list_off + rows; i++, y += 40) {
        if (i == cur) gfx_rrect(c, 16, y, WW - 32, 36, 9, 0x26261F, 255);
        else if (hover == 100 + i) gfx_rrect(c, 16, y, WW - 32, 36, 9, UI_PANEL, 255);
        char t[80]; strncpy0(t, songs[i].title, 56);
        gfx_text(c, i == cur ? &font_ui_bold : &font_ui, 32, y + 23, i == cur ? UI_ACCENT : UI_TEXT, t);
        if (songs[i].artist[0]) { char ar[40]; strncpy0(ar, songs[i].artist, sizeof ar); gfx_text(c, &font_ui, WW - 260, y + 23, UI_MUTED, ar); }
    }
    /* now playing */
    gfx_fill(c, 0, BAR_Y - 10, WW, WH - BAR_Y + 10, 0x0C0C0B);
    if (cur >= 0 && cur < nsongs) {
        gfx_text(c, &font_ui_bold, 24, BAR_Y + 20, UI_TEXT, songs[cur].title);
        if (songs[cur].artist[0]) gfx_text(c, &font_ui, 24, BAR_Y + 40, UI_MUTED, songs[cur].artist);
    } else gfx_text(c, &font_ui, 24, BAR_Y + 20, UI_MUTED, msg[0] ? msg : "Pick a song.");
    if (msg[0] && cur >= 0) gfx_text(c, &font_ui, 24, BAR_Y + 40, UI_RED, msg);
    long pos = dec ? mp3_position_ms(dec) - snd(SOUND_STREAM_STATUS, 0, 0) : 0, dur = dec ? info.duration_ms : 0;
    if (pos < 0) pos = 0;
    int bx = 70, bw = WW - 140, by = BAR_Y + 60;
    gfx_rrect(c, bx, by, bw, 6, 3, 0x2A2A28, 255);
    if (dur > 0) gfx_rrect(c, bx, by, (int)(bw * (pos > dur ? dur : pos) / dur), 6, 3, UI_ACCENT, 255);
    char t1[16], t2[16]; fmt_time(pos, t1, sizeof t1); fmt_time(dur, t2, sizeof t2);
    gfx_text(c, &font_ui, 24, by + 8, UI_MUTED, t1);
    gfx_text(c, &font_ui, WW - 58, by + 8, UI_MUTED, t2);
    int cx = WW / 2;
    ui_button(c, cx - 130, BAR_Y + 80, 70, 36, "<<", BTN_NORMAL, hover == 1, !nsongs);
    ui_button(c, cx - 50, BAR_Y + 80, 100, 36, playing && !paused ? "Pause" : "Play", BTN_PRIMARY, hover == 2, !nsongs);
    ui_button(c, cx + 60, BAR_Y + 80, 70, 36, ">>", BTN_NORMAL, hover == 3, !nsongs);
    char vb[16]; sys_setting_get("sound.volume", vb, sizeof vb); int vol = (int)katoi(vb);
    gfx_text(c, &font_ui, WW - 220, BAR_Y + 104, UI_MUTED, "Volume");
    gfx_rrect(c, WW - 160, BAR_Y + 96, 130, 6, 3, 0x2A2A28, 255);
    gfx_rrect(c, WW - 160, BAR_Y + 96, 130 * vol / 100, 6, 3, UI_ACCENT, 255);
    gfx_circle(c, WW - 160 + 130 * vol / 100, BAR_Y + 99, 7, UI_TEXT, 255);
    app_flush(&a);
}
static int hit(int x, int y) {
    int cx = WW / 2;
    if (ui_in(x, y, WW - 130, 18, 110, 32)) return 10;
    if (ui_in(x, y, cx - 130, BAR_Y + 80, 70, 36)) return 1;
    if (ui_in(x, y, cx - 50, BAR_Y + 80, 100, 36)) return 2;
    if (ui_in(x, y, cx + 60, BAR_Y + 80, 70, 36)) return 3;
    if (ui_in(x, y, 70, BAR_Y + 52, WW - 140, 22)) return 4;
    if (ui_in(x, y, WW - 168, BAR_Y + 88, 146, 22)) return 5;
    int rows = (BAR_Y - 20 - 84) / 40;
    if (y >= 84 && y < 84 + rows * 40) { int i = list_off + (y - 84) / 40; if (i < nsongs) return 100 + i; }
    return -1;
}

int main(void) {
    char arg[200] = ""; sys_getarg(arg, sizeof arg);
    if (!app_open_ex(&a, WW, WH, "Music", WIN_RESIZABLE)) return 1;
    fs_mkdir("/home/Music");
    scan();
    if (arg[0]) {                                         /* opened with a file (from Files) */
        int found = -1;
        for (int i = 0; i < nsongs; i++) if (!strcmp(songs[i].path, arg)) found = i;
        if (found < 0) found = add_file(arg);
        play(found);
    }
    draw();
    uint64_t last_draw = 0;
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, playing && !paused ? 40 : -1);
        feed();
        if (r <= 0) { if (sys_time() - last_draw > 500) { last_draw = sys_time(); draw(); } continue; }
        if (e.type == EV_CLOSE) { stop(); break; }
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } continue; }
        if (e.type == EV_WHEEL) { list_off += e.key; if (list_off > nsongs - 1) list_off = nsongs - 1; if (list_off < 0) list_off = 0; draw(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 10) { stop(); cur = -1; scan(); }
            else if (h == 1) next(-1);
            else if (h == 3) next(1);
            else if (h == 2) {
                if (!playing) play(cur >= 0 ? cur : 0);
                else { paused = !paused; snd(SOUND_STREAM_PAUSE, paused, 0); }
            }
            else if (h == 4 && dec && info.duration_ms > 0) {   /* jump within the song */
                long ms = (long)(e.x - 70) * info.duration_ms / (WW - 140);
                snd(SOUND_STREAM_START, out_rate, 0); npend = ppos = 0; rs_pos = 0;
                mp3_seek(dec, ms < 0 ? 0 : ms);
                if (paused) { paused = false; }
            }
            else if (h == 5) { int v = (e.x - (WW - 160)) * 100 / 130; if (v < 0) v = 0; if (v > 100) v = 100; char b[8]; ksnprintf(b, sizeof b, "%d", v); sys_setting_set("sound.volume", b); }
            else if (h >= 100) play(h - 100);
            draw(); continue;
        }
        if (e.type == EV_KEY) {
            int k = e.key;
            if (k == ' ') { if (!playing) play(cur >= 0 ? cur : 0); else { paused = !paused; snd(SOUND_STREAM_PAUSE, paused, 0); } }
            else if (KEY_CODE(k) == KEY_RIGHT) next(1);
            else if (KEY_CODE(k) == KEY_LEFT) next(-1);
            draw();
        }
    }
    return 0;
}
