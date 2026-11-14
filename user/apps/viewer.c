/* Viewer: shows pictures (PNG, JPEG, GIF, BMP). Opened from Files or with "open photo.jpg".
 * Arrow keys: previous / next picture in the folder. + / - zoom, 0 fit to the window. Drag to move a zoomed picture. */
#include "../lib/dunc.h"
#include "../lib/img/img.h"

static int WW = 800, WH = 560;
#define BAR 34
static struct app a;
static char path[256], err[160];
static struct image img;                         /* decoded picture */
static uint32_t *scaled; static int sw, sh;      /* picture at the current zoom */
static int zoom_pct;                             /* 0 = fit */
static int panx, pany, drag_x, drag_y; static bool dragging;
static int hover = -1;

static void *al(void *c, size_t n) { return malloc(n); }
static void fr(void *c, void *p) { free(p); }
static const struct img_alloc A = { 0, al, fr };

static const char *base(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

static void load(void) {
    free(img.px); img.px = 0; img.w = img.h = 0;
    free(scaled); scaled = 0; err[0] = 0; zoom_pct = 0; panx = pany = 0;
    struct dunc_stat st;
    if (fs_stat(path, &st) < 0) { ksnprintf(err, sizeof err, "Couldn't open %s.", base(path)); return; }
    if (st.size > (64u << 20)) { strncpy0(err, "This file is too big to open.", sizeof err); return; }
    uint8_t *data = malloc(st.size ? st.size : 1);
    if (!data) { strncpy0(err, "Not enough memory.", sizeof err); return; }
    long n = fs_read(path, data, 0, st.size);
    int r = n < 0 ? -1 : img_decode(data, (size_t)n, &img, &A);
    free(data);
    if (r) { ksnprintf(err, sizeof err, "Can't show this picture: %s.", n < 0 ? "it couldn't be read" : img_strerror(r)); img.px = 0; }
    char t[300]; ksnprintf(t, sizeof t, "Viewer - %s", base(path)); app_title(&a, t);
}

/* scale with area averaging when shrinking, nearest neighbour when enlarging */
static void rescale(void) {
    free(scaled); scaled = 0;
    if (!img.px) return;
    int aw = WW, ah = WH - BAR;
    int pct = zoom_pct;
    if (!pct) { pct = 100; if (img.w * 100 / aw > 100 || img.h * 100 / ah > 100) { int px = aw * 100 / img.w, py = ah * 100 / img.h; pct = px < py ? px : py; } if (pct < 1) pct = 1; }
    sw = img.w * pct / 100; sh = img.h * pct / 100;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    scaled = malloc((size_t)sw * sh * 4);
    if (!scaled) return;
    for (int y = 0; y < sh; y++) {
        int y0 = y * img.h / sh, y1 = (y + 1) * img.h / sh; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < sw; x++) {
            int x0 = x * img.w / sw, x1 = (x + 1) * img.w / sw; if (x1 <= x0) x1 = x0 + 1;
            uint32_t ra = 0, ga = 0, ba = 0, aa = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < img.h; yy += (y1 - y0 > 4 ? (y1 - y0) / 4 : 1))
                for (int xx = x0; xx < x1 && xx < img.w; xx += (x1 - x0 > 4 ? (x1 - x0) / 4 : 1)) {
                    uint32_t p = img.px[yy * img.w + xx];
                    ra += p >> 16 & 255; ga += p >> 8 & 255; ba += p & 255; aa += p >> 24; n++;
                }
            if (!n) n = 1;
            /* blend over a dark checker for transparent pictures */
            uint32_t alpha = aa / n, bg = ((x / 8 + y / 8) & 1) ? 0x2A2A28 : 0x1C1C1B;
            uint32_t r = (ra / n * alpha + (bg >> 16 & 255) * (255 - alpha)) / 255;
            uint32_t g = (ga / n * alpha + (bg >> 8 & 255) * (255 - alpha)) / 255;
            uint32_t b = (ba / n * alpha + (bg & 255) * (255 - alpha)) / 255;
            scaled[y * sw + x] = r << 16 | g << 8 | b;
        }
    }
}

static void clamp_pan(void) {
    int aw = WW, ah = WH - BAR;
    int mx = sw > aw ? (sw - aw) / 2 : 0, my = sh > ah ? (sh - ah) / 2 : 0;
    if (panx > mx) panx = mx;
    if (panx < -mx) panx = -mx;
    if (pany > my) pany = my;
    if (pany < -my) pany = -my;
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, 0x050505);
    if (scaled) {
        clamp_pan();
        int ox = (WW - sw) / 2 + panx, oy = (WH - BAR - sh) / 2 + pany;
        gfx_clip(c, 0, 0, WW, WH - BAR);
        gfx_blit(c, ox, oy, scaled, sw, sh, sw);
        gfx_clip(c, 0, 0, WW, WH);
    } else gfx_text_center(c, &font_ui, 0, WW, (WH - BAR) / 2, err[0] ? UI_RED : UI_MUTED, err[0] ? err : "No picture");
    gfx_fill(c, 0, WH - BAR, WW, BAR, UI_PANEL);
    char st[160];
    if (img.px) ksnprintf(st, sizeof st, "%s   %d x %d   %d%%", base(path), img.w, img.h, sw * 100 / (img.w ? img.w : 1));
    else strncpy0(st, base(path), sizeof st);
    gfx_text(c, &font_ui, 12, WH - 12, UI_TEXT, st);
    const char *help = "<  >  next   +  -  zoom   0  fit";
    gfx_text(c, &font_ui, WW - 12 - gfx_text_width(&font_ui, help), WH - 12, UI_MUTED, help);
    app_flush(&a);
}

/* previous / next picture in the same folder */
static void step(int d) {
    char dir[256]; strncpy0(dir, path, sizeof dir);
    char *sl = dir; for (char *p = dir; *p; p++) if (*p == '/') sl = p;
    *sl = 0;
    static struct dunc_dirent e[256];
    int n = fs_list(dir[0] ? dir : "/", e, 256), cur = -1, cnt = 0;
    int idx[256];
    for (int i = 0; i < n; i++) {
        char p[256]; ksnprintf(p, sizeof p, "%s/%s", dir, e[i].name);
        if (e[i].is_dir || strcmp(app_for_file(p) ? app_for_file(p) : "", "viewer")) continue;
        if (!strcmp(e[i].name, base(path))) cur = cnt;
        idx[cnt++] = i;
    }
    if (cnt < 2) return;
    int k = ((cur < 0 ? 0 : cur) + d + cnt) % cnt;
    ksnprintf(path, sizeof path, "%s/%s", dir, e[idx[k]].name);
    load(); rescale();
}

static void set_zoom(int pct) {
    if (pct > 800) pct = 800;
    if (pct && pct < 5) pct = 5;
    zoom_pct = pct; rescale();
}

int main(void) {
    sys_getarg(path, sizeof path);
    if (!app_open_ex(&a, WW, WH, "Viewer", WIN_RESIZABLE)) return 1;
    if (path[0]) load(); else strncpy0(err, "Open a picture from Files.", sizeof err);
    rescale(); draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; rescale(); draw(); } continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) { dragging = true; drag_x = e.x; drag_y = e.y; continue; }
        if (e.type == EV_MOUSE_UP) { dragging = false; continue; }
        if (e.type == EV_MOUSE_MOVE && dragging) { panx += e.x - drag_x; pany += e.y - drag_y; drag_x = e.x; drag_y = e.y; draw(); continue; }
        if (e.type == EV_WHEEL) { int cur = sw * 100 / (img.w ? img.w : 1); set_zoom(e.key < 0 ? cur * 5 / 4 + 1 : cur * 4 / 5); draw(); continue; }
        if (e.type != EV_KEY) continue;
        int k = KEY_CODE(e.key);
        int cur = sw * 100 / (img.w ? img.w : 1);
        if (k == KEY_RIGHT || k == ' ' || k == KEY_PGDN) step(1);
        else if (k == KEY_LEFT || k == '\b' || k == KEY_PGUP) step(-1);
        else if (k == '+' || k == '=') set_zoom(cur * 5 / 4 + 1);
        else if (k == '-') set_zoom(cur * 4 / 5);
        else if (k == '0') set_zoom(0);
        else if (k == '1') set_zoom(100);
        else if (k == 27) break;
        (void)hover;
        draw();
    }
    return 0;
}
