/* Calculator (fixed-point, 6 decimal places). */
#include "../lib/dunc.h"

#define WW 340
#define WH 500
#define SCALE 1000000LL
static struct app a;
static int64_t acc, cur;          /* scaled by SCALE */
static int op;                    /* 0, '+', '-', 'x', '/' */
static bool entering, err;
static const char *strchr_(const char *s, char c) { for (; *s; s++) if (*s == c) return s; return 0; }
static int hover = -1, pressed = -1;
static char expr[64];

static const char *keys[] = { "C", "+/-", "%", "/", "7", "8", "9", "x", "4", "5", "6", "-", "1", "2", "3", "+", "0", ".", "<", "=" };

static void fmt(int64_t v, char *out, int n) {
    bool neg = v < 0; uint64_t u = neg ? -(uint64_t)v : (uint64_t)v;
    uint64_t ip = u / SCALE, fp = u % SCALE;
    char f[8]; ksnprintf(f, sizeof f, "%06lu", fp);
    int k = 5; while (k >= 0 && f[k] == '0') f[k--] = 0;
    if (f[0]) ksnprintf(out, n, "%s%lu.%s", neg ? "-" : "", ip, f);
    else ksnprintf(out, n, "%s%lu", neg ? "-" : "", ip);
}

static int64_t apply(int64_t x, int64_t y, int o) {
    __int128 r;
    switch (o) {
    case '+': return x + y;
    case '-': return x - y;
    case 'x': r = (__int128)x * y / SCALE; return (int64_t)r;
    case '/': if (!y) { err = true; return 0; } r = (__int128)x * SCALE / y; return (int64_t)r;
    }
    return y;
}

static char entry[24];            /* digits being typed */

static int64_t parse(const char *e) {
    bool neg = *e == '-'; if (neg) e++;
    int64_t ip = 0, fp = 0, p = SCALE;
    for (; *e && *e != '.'; e++) ip = ip * 10 + (*e - '0');
    if (*e == '.') for (e++; *e && p > 1; e++) { p /= 10; fp += (*e - '0') * p; }
    int64_t v = ip * SCALE + fp;
    return neg ? -v : v;
}

static void key(const char *k) {
    if (err && strcmp(k, "C")) return;
    char c = k[0];
    int l = strlen(entry);
    if ((c >= '0' && c <= '9') || (c == '.' && !k[1])) {
        if (!entering) { entry[0] = 0; l = 0; entering = true; }
        if (c == '.' && strchr_(entry, '.')) return;
        if (l >= 16) return;
        if (c == '.' && (l == 0 || (l == 1 && entry[0] == '-'))) { entry[l++] = '0'; }
        if (l == 1 && entry[0] == '0' && c != '.') l = 0;
        entry[l++] = c; entry[l] = 0;
        cur = parse(entry);
    } else if (!strcmp(k, "C")) { acc = cur = 0; op = 0; entering = err = false; expr[0] = entry[0] = 0; }
    else if (!strcmp(k, "+/-")) {
        if (entering) {
            if (entry[0] == '-') memmove(entry, entry + 1, l);
            else if (l < 16) { memmove(entry + 1, entry, l + 1); entry[0] = '-'; }
            cur = parse(entry);
        } else cur = -cur;
    }
    else if (c == '%') { cur = cur / 100; entering = false; }
    else if (c == '<') { if (entering && l) { entry[--l] = 0; cur = parse(entry); } }
    else if (c == '=') {
        if (op) {
            char x[32], y[32]; fmt(acc, x, 32); fmt(cur, y, 32);
            ksnprintf(expr, sizeof expr, "%s %c %s =", x, op, y);
            cur = apply(acc, cur, op); op = 0;
        }
        entering = false;
    } else {   /* operator */
        if (op && entering) cur = apply(acc, cur, op);
        acc = cur; op = c; entering = false;
        char x[32]; fmt(acc, x, 32);
        ksnprintf(expr, sizeof expr, "%s %c", x, op);
    }
}

static void btn_rect(int i, int *x, int *y, int *w, int *h) {
    int col = i % 4, row = i / 4, bw = (WW - 24 - 3 * 8) / 4, bh = 58;
    *x = 12 + col * (bw + 8); *y = 168 + row * (bh + 8); *w = bw; *h = bh;
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    gfx_text(c, &font_ui, WW - 18 - gfx_text_width(&font_ui, expr), 70, UI_MUTED, expr);
    char d[48];
    if (err) strncpy0(d, "Can't divide by 0", sizeof d);
    else {
        if (entering) strncpy0(d, entry[0] ? entry : "0", sizeof d);
        else fmt(cur, d, sizeof d);
    }
    const struct font *f = gfx_text_width(&font_big, d) > WW - 36 ? &font_title : &font_big;
    gfx_text(c, f, WW - 18 - gfx_text_width(f, d), 136, UI_TEXT, d);
    for (int i = 0; i < 20; i++) {
        int x, y, w, h; btn_rect(i, &x, &y, &w, &h);
        char k = keys[i][0];
        bool opk = (i % 4 == 3);
        uint32_t bg = opk ? (op == k && !entering ? 0xFFFFFF : UI_GOLD) : (i < 3 ? 0x22345A : UI_PANEL2);
        if (i == hover) bg = gfx_mix(bg, 0xFFFFFF, 30);
        if (i == pressed) bg = gfx_mix(bg, 0x000000, 50);
        gfx_rrect(c, x, y, w, h, 12, bg, 255);
        uint32_t fg = opk ? HL_NAVY : UI_TEXT;
        gfx_text_center(c, &font_title, x, w, y + 37, fg, keys[i]);
    }
    app_flush(&a);
}

static int hit(int px, int py) {
    for (int i = 0; i < 20; i++) { int x, y, w, h; btn_rect(i, &x, &y, &w, &h); if (ui_in(px, py, x, y, w, h)) return i; }
    return -1;
}

int main(void) {
    if (!app_open(&a, WW, WH, "Calculator")) return 1;
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } }
        if (e.type == EV_MOUSE_DOWN) { pressed = hit(e.x, e.y); draw(); }
        if (e.type == EV_MOUSE_UP) { int h = hit(e.x, e.y); if (h >= 0 && h == pressed) key(keys[h]); pressed = -1; draw(); }
        if (e.type == EV_KEY) {
            int k = e.key; char s[2] = { (char)k, 0 };
            if (k == '*') s[0] = 'x';
            if (k == '\n') s[0] = '=';
            if (k == '\b') s[0] = '<';
            if (k == 27 || k == 'c' || k == 'C') { key("C"); draw(); continue; }
            for (int i = 0; i < 20; i++) if (keys[i][0] == s[0] && !keys[i][1]) { key(keys[i]); break; }
            draw();
        }
    }
    return 0;
}
