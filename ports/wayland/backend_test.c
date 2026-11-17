/* A test backend for running axwl on Linux: windows are memory; a control FIFO ($AXWL_CONTROL) takes lines
 *   snap <window#> <file.ppm>   save a window's pixels        click <x> <y> [button]   in the newest window
 *   move <x> <y>                pointer motion                key <char> / keycode <AXWIN_KEY_...>   a key
 *   type <text>                 keys for each character       resize <w> <h>   ask the newest window to resize
 *   close                       the close button              list   print the windows to stderr
 *   clip <text>                 another app put text on the clipboard
 * and windows are numbered in creation order. Events go out as AXIOM events, as on AXIOM. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "backend.h"
#define QN 256
struct bwin { int id, w, h, rw, rh; uint32_t *px; char title[64]; struct axwin_event q[QN]; int qh, qt; int pipe[2]; };
static struct bwin *wins[64]; static int nwins, ctl = -1;
static char clip[65536]; static long cliplen; static uint64_t clipseq;
int be_init(void) {
    const char *p = getenv("AXWL_CONTROL");
    if (p) { unlink(p); mkfifo(p, 0600); ctl = open(p, O_RDWR | O_NONBLOCK); }
    return 0;
}
void be_screen(int *w, int *h) { *w = 1920; *h = 1080; }
struct bwin *be_create(int w, int h, const char *title) {
    struct bwin *b = calloc(1, sizeof *b);
    b->w = w; b->h = h; b->px = calloc((size_t)w * h, 4); b->id = nwins;
    strncpy(b->title, title ? title : "", 63);
    pipe(b->pipe); fcntl(b->pipe[0], F_SETFL, O_NONBLOCK);
    wins[nwins++] = b;
    fprintf(stderr, "[test] window %d created %dx%d '%s'\n", b->id, w, h, b->title);
    return b;
}
void be_destroy(struct bwin *b) {
    fprintf(stderr, "[test] window %d closed\n", b->id);
    for (int i = 0; i < nwins; i++) if (wins[i] == b) wins[i] = 0;
    close(b->pipe[0]); close(b->pipe[1]); free(b->px); free(b);
}
uint32_t *be_pixels(struct bwin *b, int *w, int *h, int *stride) { *w = b->w; *h = b->h; *stride = b->w; return b->px; }
void be_flush(struct bwin *b, int x, int y, int w, int h) {}
void be_title(struct bwin *b, const char *t) { strncpy(b->title, t, 63); fprintf(stderr, "[test] window %d title '%s'\n", b->id, t); }
int be_fd(struct bwin *b) { return b->pipe[0]; }
static void push(struct bwin *b, int type, int x, int y, int key, int button) {
    struct axwin_event e = { type, b->id, x, y, key, button };
    b->q[b->qt] = e; b->qt = (b->qt + 1) % QN;
    char c = 1; write(b->pipe[1], &c, 1);
}
int be_event(struct bwin *b, struct axwin_event *e) {
    char c; if (read(b->pipe[0], &c, 1) != 1) return 0;
    if (b->qh == b->qt) return 0;
    *e = b->q[b->qh]; b->qh = (b->qh + 1) % QN; return 1;
}
int be_resize(struct bwin *b) {
    if (!b->rw) return -1;
    free(b->px); b->w = b->rw; b->h = b->rh; b->px = calloc((size_t)b->w * b->h, 4); b->rw = 0;
    return 0;
}
long be_clip_get(char *buf, long max, uint64_t *seq) { if (seq) *seq = clipseq; if (buf) memcpy(buf, clip, cliplen < max ? cliplen : max); return cliplen; }
void be_clip_set(const char *s, long n) { if (n > (long)sizeof clip) n = sizeof clip; memcpy(clip, s, n); cliplen = n; clipseq++; fprintf(stderr, "[test] clipboard: %.*s\n", (int)n, s); }
int be_control_fd(void) { return ctl; }
static struct bwin *newest(void) { for (int i = nwins - 1; i >= 0; i--) if (wins[i]) return wins[i]; return 0; }
static void snap(int id, const char *file) {
    if (id < 0 || id >= nwins || !wins[id]) { fprintf(stderr, "[test] no window %d\n", id); return; }
    struct bwin *b = wins[id];
    FILE *f = fopen(file, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
    for (int i = 0; i < b->w * b->h; i++) { uint32_t p = b->px[i]; fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f); }
    fclose(f);
    fprintf(stderr, "[test] snap %d -> %s\n", id, file);
}
void be_control(void) {
    static char line[4096]; static int ll;
    char c;
    while (read(ctl, &c, 1) == 1) {
        if (c != '\n') { if (ll < (int)sizeof line - 1) line[ll++] = c; continue; }
        line[ll] = 0; ll = 0;
        struct bwin *b = newest();
        int x, y, k, btn = 1; char s[1024];
        if (sscanf(line, "snap %d %1023s", &k, s) == 2) snap(k, s);
        else if (!strncmp(line, "clip ", 5)) { cliplen = (long)strlen(line + 5); memcpy(clip, line + 5, cliplen); clipseq++; }   /* another AXIOM app copied */
        else if (!b) fprintf(stderr, "[test] no window for '%s'\n", line);
        else if (sscanf(line, "click %d %d %d", &x, &y, &btn) >= 2) { push(b, AXWIN_EV_MOUSE_MOVE, x, y, 0, 0); push(b, AXWIN_EV_MOUSE_DOWN, x, y, 0, btn); push(b, AXWIN_EV_MOUSE_UP, x, y, 0, btn); }
        else if (sscanf(line, "move %d %d", &x, &y) == 2) push(b, AXWIN_EV_MOUSE_MOVE, x, y, 0, 0);
        else if (sscanf(line, "wheel %d %d %d", &x, &y, &k) == 3) push(b, AXWIN_EV_WHEEL, x, y, k, 0);
        else if (sscanf(line, "keycode %d", &k) == 1) push(b, AXWIN_EV_KEY, 0, 0, k, 0);
        else if (!strncmp(line, "key ", 4)) push(b, AXWIN_EV_KEY, 0, 0, (unsigned char)line[4], 0);
        else if (!strncmp(line, "type ", 5)) { for (char *p = line + 5; *p; p++) push(b, AXWIN_EV_KEY, 0, 0, (unsigned char)*p, 0); }
        else if (!strcmp(line, "enter")) push(b, AXWIN_EV_KEY, 0, 0, '\n', 0);
        else if (sscanf(line, "resize %d %d", &x, &y) == 2) { b->rw = x; b->rh = y; push(b, AXWIN_EV_RESIZE, x, y, 0, 0); }
        else if (!strcmp(line, "close")) push(b, AXWIN_EV_CLOSE, 0, 0, 0, 0);
        else if (!strcmp(line, "list")) for (int i = 0; i < nwins; i++) if (wins[i]) fprintf(stderr, "[test] window %d %dx%d '%s'\n", i, wins[i]->w, wins[i]->h, wins[i]->title);
        else fprintf(stderr, "[test] ? %s\n", line);
    }
}
