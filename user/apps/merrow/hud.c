/* Merrow Lake: the HUD and the menus (the Unity game's LakeHUD*.cs): readouts, prompts, toasts, floating numbers,
 * story dialogs, the Supply Store, the logbook, the map, the pause menu, the title screen and the crossing credits. */
#include "merrow.h"
#include "hud.h"

const uint32_t ACCENT = 0xF5A524, GREEN = 0x94C76C, AMBER = 0xF5A524;
#define INK 0xEDE6D6
#define DIM 0x9CA6AD
#define PANEL 0x15181A
#define LORE_C 0xD8CBA8

struct hud H;

/* ---------- small drawing helpers ---------- */
static void panel(struct canvas *c, int x, int y, int w, int h, int alpha) {
    gfx_rrect(c, x, y, w, h, 10, PANEL, alpha);
    gfx_rrect_outline(c, x, y, w, h, 10, 0x3A3F42, 200);
}
static void text_shadow(struct canvas *c, const struct font *f, int x, int y, uint32_t col, const char *s) {
    gfx_text(c, f, x + 1, y + 1, 0x000000, s);
    gfx_text(c, f, x, y, col, s);
}
static void text_center_shadow(struct canvas *c, const struct font *f, int x, int w, int y, uint32_t col, const char *s) {
    int tw = gfx_text_width(f, s);
    text_shadow(c, f, x + (w - tw) / 2, y, col, s);
}
/* word-wrap one paragraph into lines that fit width; draws them if draw, returns the height used */
static int wrap(struct canvas *c, const struct font *f, int x, int y, int width, uint32_t col, const char *s, int len, int draw) {
    int lh = f->size + 6, h = 0;
    char line[256];
    int i = 0;
    while (i < len) {
        int best = -1, j = i;
        while (j <= len) {
            if (j == len || s[j] == ' ' || s[j] == '\n') {
                int n = j - i; if (n > 255) n = 255;
                memcpy(line, s + i, n); line[n] = 0;
                if (gfx_text_width(f, line) > width && best >= 0) break;
                best = j;
                if (j == len || s[j] == '\n') break;
            }
            j++;
        }
        if (best < 0) best = j > len ? len : j;
        int n = best - i; if (n > 255) n = 255;
        memcpy(line, s + i, n); line[n] = 0;
        if (draw) gfx_text(c, f, x, y + h + f->ascent, col, line);
        h += lh;
        i = best;
        while (i < len && (s[i] == ' ' || s[i] == '\n')) { if (s[i] == '\n' && i + 1 < len && s[i + 1] == '\n') break; i++; }
    }
    return h;
}
/* a body of paragraphs (split on blank lines); the first byte of a paragraph may pick a style */
static int body_text(struct canvas *c, int x, int y, int width, const char *s, int draw) {
    int h = 0;
    while (*s) {
        const char *e = s;
        while (*e && !(e[0] == '\n' && e[1] == '\n')) e++;
        uint32_t col = INK; const struct font *f = &font_ui;
        const char *p = s;
        if (*p == 2) { col = GREEN; p++; } else if (*p == 3) { col = AMBER; p++; } else if (*p == 4) { col = LORE_C; p++; } else if (*p == 5) { col = DIM; p++; }
        h += wrap(c, f, x, y + h, width, col, p, (int)(e - p), draw) + 8;
        s = *e ? e + 2 : e;
    }
    return h;
}

/* ---------- toasts and floating numbers ---------- */
void hud_toast(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    kvsnprintf(H.toast, sizeof H.toast, fmt, ap);
    va_end(ap);
    H.toastT = 3.5f;
}
void hud_float(v3 at, const char *text, uint32_t col) {
    for (int i = 0; i < 16; i++) if (H.floats[i].t <= 0) {
        H.floats[i].p = at; H.floats[i].t = 1.6f; H.floats[i].col = col; strncpy0(H.floats[i].s, text, sizeof H.floats[i].s);
        return;
    }
}

/* ---------- modal dialogs ---------- */
void hud_modal(const char *title, const char *body, int nbtn, ...) {
    if (H.nmodal >= 4) return;
    struct modal *m = &H.modals[H.nmodal++];
    strncpy0(m->title, title, sizeof m->title);
    strncpy0(m->body, body, sizeof m->body);
    va_list ap; va_start(ap, nbtn);
    m->nbtn = nbtn > 3 ? 3 : nbtn;
    for (int i = 0; i < m->nbtn; i++) { strncpy0(m->btn[i], va_arg(ap, const char *), sizeof m->btn[i]); m->fn[i] = va_arg(ap, modal_fn); }
    va_end(ap);
    m->sel = 0;
}
int hud_modal_open(void) { return H.nmodal > 0; }
static void modal_choose(int i) {
    if (H.nmodal == 0) return;
    struct modal *m = &H.modals[H.nmodal - 1];
    if (i < 0 || i >= m->nbtn) return;
    modal_fn fn = m->fn[i];
    /* pop this dialog first: the callback may open the next one */
    H.nmodal--;
    if (fn) fn();
}
static int modal_rect(struct canvas *c, struct modal *m, int *x, int *y, int *w, int *h) {
    *w = 560; if (*w > c->w - 40) *w = c->w - 40;
    int bh = body_text(c, 0, 0, *w - 48, m->body, 0);
    *h = 70 + bh + 64; if (*h > c->h - 40) *h = c->h - 40;
    *x = (c->w - *w) / 2; *y = (c->h - *h) / 2;
    return bh;
}
static void draw_modal(struct canvas *c) {
    struct modal *m = &H.modals[H.nmodal - 1];
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 110);
    int x, y, w, h; modal_rect(c, m, &x, &y, &w, &h);
    panel(c, x, y, w, h, 245);
    gfx_text(c, &font_title, x + 24, y + 38, ACCENT, m->title);
    gfx_clip(c, x, y + 50, w, h - 110);
    body_text(c, x + 24, y + 58, w - 48, m->body, 1);
    gfx_noclip(c);
    int bw = (w - 48 - (m->nbtn - 1) * 10) / (m->nbtn ? m->nbtn : 1); if (bw > 170) bw = 170;
    int total = m->nbtn * bw + (m->nbtn - 1) * 10, bx = x + w - 24 - total;
    for (int i = 0; i < m->nbtn; i++) {
        int hx = bx + i * (bw + 10), hy = y + h - 52;
        int hover = ui_in(H.mx, H.my, hx, hy, bw, 36) || m->sel == i;
        ui_button(c, hx, hy, bw, 36, m->btn[i], i == 0 ? BTN_PRIMARY : BTN_NORMAL, hover, 0);
        m->bx[i] = hx; m->by[i] = hy; m->bw = bw;
    }
    char hint[48]; ksnprintf(hint, sizeof hint, m->nbtn > 1 ? "Keys 1-%d, or Enter" : "Enter", m->nbtn);
    if (bx - (x + 24) > gfx_text_width(&font_ui, hint) + 12) gfx_text(c, &font_ui, x + 24, y + h - 28, DIM, hint);
}
static int modal_key(int k) {
    struct modal *m = &H.modals[H.nmodal - 1];
    if (k >= '1' && k <= '0' + m->nbtn) { modal_choose(k - '1'); return 1; }
    if (k == '\n' || k == ' ' || k == 'e' || k == 'E') { modal_choose(m->sel); return 1; }
    if (k == KEY_LEFT || k == KEY_UP) { if (m->sel > 0) m->sel--; return 1; }
    if (k == KEY_RIGHT || k == KEY_DOWN || k == '\t') { if (m->sel < m->nbtn - 1) m->sel++; return 1; }
    if (k == 27) { modal_choose(m->nbtn - 1); return 1; }
    return 1;
}
static int modal_click(int x, int y) {
    struct modal *m = &H.modals[H.nmodal - 1];
    for (int i = 0; i < m->nbtn; i++) if (ui_in(x, y, m->bx[i], m->by[i], m->bw, 36)) { modal_choose(i); return 1; }
    return 1;
}

/* ---------- fade (going into the cave and out) ---------- */
void hud_fade(void (*mid)(void)) { H.fadeT = 1.2f; H.fadeMid = mid; H.fadeDone = 0; }

/* ---------- crossing credits ---------- */
void hud_credits(int crossings, const char *title, const char *footer) {
    H.screen = SCR_CREDITS; H.creditsT = 0;
    H.crossings = crossings;
    strncpy0(H.credTitle, title ? title : "Through the Gate", sizeof H.credTitle);
    strncpy0(H.credFooter, footer ? footer : WORLDS[world_no()].welcome, sizeof H.credFooter);
}

/* ---------- the store ---------- */
void hud_open_shop(void) { H.screen = SCR_SHOP; H.shopSel = 0; }
#define SHOP_ROWS (NPUMPS + NUPG)
static int shop_can(int i, double *cost, char *desc, int dn, char *name, int nn) {
    if (i < NPUMPS) {
        const struct pump_def *p = &PUMPS[i];
        *cost = g_pump_cost(i);
        char r[24]; fmt_num(p->rate, r);
        ksnprintf(desc, dn, "%s  %s gal/s each  " "\xB7" "  owned %d / %d", p->desc, r, G.pumps[i], p->max);
        ksnprintf(name, nn, "%s", p->name);
        return G.pumps[i] < p->max;
    }
    int u = i - NPUMPS; int n = G.upg[u];
    *cost = upg_cost(u, n);
    if (n >= UPGRADES[u].max) ksnprintf(desc, dn, "Fully upgraded (level %d)", n);
    else upg_desc(u, n, desc, dn);
    ksnprintf(name, nn, "%s  (level %d / %d)", UPGRADES[u].name, n, UPGRADES[u].max);
    return n < UPGRADES[u].max;
}
static void shop_buy(int i) {
    double cost; char d[160], nm[64];
    if (!shop_can(i, &cost, d, sizeof d, nm, sizeof nm)) return;
    if (G.cash < cost) { char m[24]; fmt_money(cost, m); hud_toast("That costs %s.", m); return; }
    if (i < NPUMPS) g_buy_pump(i); else g_buy_upgrade(i - NPUMPS);
    g_save();
}
static void draw_shop(struct canvas *c) {
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 120);
    int w = 720 < c->w - 30 ? 720 : c->w - 30, rowh = 42, h = 92 + SHOP_ROWS * rowh + 40;
    if (h > c->h - 20) { rowh = (c->h - 20 - 132) / SHOP_ROWS; h = 92 + SHOP_ROWS * rowh + 40; }
    int x = (c->w - w) / 2, y = (c->h - h) / 2;
    panel(c, x, y, w, h, 245);
    gfx_text(c, &font_title, x + 22, y + 36, ACCENT, "Supply Store");
    char m[24], t[96]; fmt_money(G.cash, m); ksnprintf(t, sizeof t, "Cash: %s", m);
    gfx_text(c, &font_ui_bold, x + w - 22 - gfx_text_width(&font_ui_bold, t), y + 34, GREEN, t);
    gfx_text(c, &font_ui, x + 22, y + 60, DIM, "Pumps fill the tank by themselves. Up/Down to choose, Enter to buy, Esc to leave.");
    for (int i = 0; i < SHOP_ROWS; i++) {
        int ry = y + 76 + i * rowh;
        double cost; char d[160], nm[64];
        int can = shop_can(i, &cost, d, sizeof d, nm, sizeof nm);
        int afford = can && G.cash >= cost;
        if (i == H.shopSel) gfx_rrect(c, x + 12, ry - 2, w - 24, rowh - 4, 8, 0x262B2E, 255);
        if (i == NPUMPS) gfx_fill(c, x + 20, ry - 4, w - 40, 1, 0x3A3F42);
        gfx_text(c, &font_ui_bold, x + 24, ry + 15, afford ? INK : DIM, nm);
        gfx_clip(c, x + 24, ry, w - 200, rowh);
        gfx_text(c, &font_ui, x + 24, ry + 33, DIM, d);
        gfx_noclip(c);
        char cs[32]; fmt_money(cost, cs);
        ui_button(c, x + w - 150, ry + 3, 128, rowh - 12, can ? cs : "Maxed", afford ? BTN_PRIMARY : BTN_NORMAL, i == H.shopSel, !afford);
    }
    gfx_text(c, &font_ui, x + 22, y + h - 16, DIM, "Tip: the pipeline sells your tank automatically. Filtration and the lighthouse raise the price.");
}
static void shop_key(int k) {
    int kc = KEY_CODE(k);
    if (kc == KEY_UP || k == 'w') { if (H.shopSel > 0) H.shopSel--; }
    else if (kc == KEY_DOWN || k == 's') { if (H.shopSel < SHOP_ROWS - 1) H.shopSel++; }
    else if (k == '\n' || k == ' ') shop_buy(H.shopSel);
    else if (k == 27 || k == 'e' || k == 'E' || k == 'q') H.screen = SCR_GAME;
}
static void shop_click(struct canvas *c, int mx, int my) {
    int w = 720 < c->w - 30 ? 720 : c->w - 30, rowh = 42, h = 92 + SHOP_ROWS * rowh + 40;
    if (h > c->h - 20) { rowh = (c->h - 20 - 132) / SHOP_ROWS; h = 92 + SHOP_ROWS * rowh + 40; }
    int x = (c->w - w) / 2, y = (c->h - h) / 2;
    if (!ui_in(mx, my, x, y, w, h)) { H.screen = SCR_GAME; return; }
    for (int i = 0; i < SHOP_ROWS; i++) {
        int ry = y + 76 + i * rowh;
        if (ui_in(mx, my, x + 12, ry - 2, w - 24, rowh - 4)) { H.shopSel = i; if (ui_in(mx, my, x + w - 150, ry + 3, 128, rowh - 12)) shop_buy(i); }
    }
}

/* ---------- logbook ---------- */
static void draw_log(struct canvas *c) {
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 120);
    int w = c->w - 60 < 820 ? c->w - 60 : 820, h = c->h - 50;
    int x = (c->w - w) / 2, y = 25;
    panel(c, x, y, w, h, 245);
    gfx_text(c, &font_title, x + 22, y + 36, ACCENT, "Logbook");
    char t[160]; char a[24], b[24];
    fmt_num(G.water, a); ksnprintf(t, sizeof t, "%s  " "\xB7" "  %s gal left", WORLDS[world_no()].name, a);
    gfx_text(c, &font_ui_bold, x + w - 22 - gfx_text_width(&font_ui_bold, t), y + 34, DIM, t);
    int cy = y + 62, colw = (w - 66) / 2;
    /* left column: missions and progress */
    int lx = x + 22;
    gfx_text(c, &font_ui_bold, lx, cy, INK, "Creeks"); cy += 22;
    for (int i = 0; i < NCREEKS; i++) {
        const struct creek_def *cr = &CREEKS[i];
        if (G.creekState[i] == 1) ksnprintf(t, sizeof t, "%s: dammed", cr->name);
        else if (G.creekState[i] == 2) { fmt_money(cr->rate * SPRING_WATER_PRICE, a); ksnprintf(t, sizeof t, "%s: bottling (+%s/s)", cr->name, a); }
        else { fmt_num(cr->rate, a); ksnprintf(t, sizeof t, "%s: %s gal/s  " "\xB7" "  %d/%d %s", cr->name, a, g_creek_have(i), cr->need, cr->item); }
        gfx_text(c, &font_ui, lx + 8, cy, G.creekState[i] ? GREEN : INK, t); cy += 20;
    }
    cy += 8;
    gfx_text(c, &font_ui_bold, lx, cy, INK, "Lakebed springs"); cy += 22;
    int any = 0;
    for (int i = 0; i < NSPRINGS; i++) {
        if (!(G.springsOpen >> i & 1)) continue;
        any = 1;
        fmt_num(SPRINGS[i].rate * g_spring_mult(), a); fmt_money(SPRINGS[i].cost, b);
        if (g_spring_flowing(i)) ksnprintf(t, sizeof t, "%s: %s gal/s, cap for %s", SPRINGS[i].name, a, b);
        else ksnprintf(t, sizeof t, "%s: %s", SPRINGS[i].name, (G.springsCapped >> i & 1) ? "capped" : "quiet");
        gfx_text(c, &font_ui, lx + 8, cy, g_spring_flowing(i) ? 0xE8836A : GREEN, t); cy += 20;
    }
    if (!any) { gfx_text(c, &font_ui, lx + 8, cy, DIM, "None open yet."); cy += 20; }
    cy += 8;
    gfx_text(c, &font_ui_bold, lx, cy, INK, "Progress"); cy += 22;
    ksnprintf(t, sizeof t, "Finds %d / %d   Secrets %d / %d   Buildings %d / %d", popcount32(G.found & 0xFFFF), NITEMS, popcount32(G.secrets), NSECRETS, popcount32(G.explored), NBUILD);
    gfx_text(c, &font_ui, lx + 8, cy, INK, t); cy += 20;
    ksnprintf(t, sizeof t, "Gate relics %d / %d found, %d set", g_relics_found(), NRELICS, popcount32(G.relicsSet));
    gfx_text(c, &font_ui, lx + 8, cy, INK, t); cy += 20;
    if (G.ngPlus > 0) { ksnprintf(t, sizeof t, "Crossings: %d   Springs push x%d.%d", G.ngPlus, (int)g_spring_mult(), (int)(g_spring_mult() * 10) % 10); gfx_text(c, &font_ui, lx + 8, cy, INK, t); cy += 20; }
    if (G.ending) { gfx_text(c, &font_ui, lx + 8, cy, AMBER, G.ending == 1 ? "Ending: you took the Keeper's chair." : "Ending: you walked home."); cy += 20; }
    cy += 8;
    gfx_text(c, &font_ui_bold, lx, cy, INK, "Journal pages (press 1-3 to read)"); cy += 22;
    for (int i = 0; i < NPAGES; i++) {
        int have = G.pages >> i & 1;
        ksnprintf(t, sizeof t, "%d. %s", i + 1, have ? PAGES[i].title : "(missing)");
        gfx_text(c, &font_ui, lx + 8, cy, have ? LORE_C : DIM, t); cy += 20;
    }
    /* right column: the log */
    int rx = x + 44 + colw, ry = y + 62;
    gfx_text(c, &font_ui_bold, rx, ry, INK, "Log"); ry += 10;
    gfx_clip(c, rx, ry, colw, y + h - 40 - ry);
    int yy = ry - H.logScroll;
    for (int i = 0; i < G.nlog; i++) {
        int hh = wrap(c, &font_ui, rx, yy, colw, i == 0 ? INK : 0xC8C0B0, G.log[i], strlen(G.log[i]), 1);
        yy += hh + 6;
    }
    gfx_noclip(c);
    gfx_text(c, &font_ui, x + 22, y + h - 16, DIM, "Up/Down scrolls the log  " "\xB7" "  L or Esc closes");
}
static void log_key(int k) {
    int kc = KEY_CODE(k);
    if (kc == KEY_UP) { H.logScroll -= 40; if (H.logScroll < 0) H.logScroll = 0; }
    else if (kc == KEY_DOWN) H.logScroll += 40;
    else if (k >= '1' && k <= '3') { int i = k - '1'; if (G.pages >> i & 1) g_read_page(i, 0); }
    else if (k == 27 || k == 'l' || k == 'L') H.screen = SCR_GAME;
}

/* ---------- map ---------- */
int w_map_xy(v3 p, int x0, int y0, int size, int *mx, int *my);
static void marker(struct canvas *c, v3 p, int x0, int y0, int size, uint32_t col, const char *label) {
    int mx, my; w_map_xy(p, x0, y0, size, &mx, &my);
    gfx_circle(c, mx, my, 4, 0x000000, 200);
    gfx_circle(c, mx, my, 3, col, 255);
    if (label) text_shadow(c, &font_ui, mx + 6, my + 5, col, label);
}
static void draw_map(struct canvas *c) {
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 140);
    int size = c->h - 90; if (size > c->w - 260) size = c->w - 260; if (size > 520) size = 520;
    int x0 = (c->w - size) / 2, y0 = (c->h - size) / 2 + 10;
    panel(c, x0 - 14, y0 - 44, size + 28, size + 70, 245);
    gfx_text(c, &font_title, x0, y0 - 14, ACCENT, "Map of the valley");
    w_draw_map(c, x0, y0, size);
    marker(c, W.tank, x0, y0, size, 0xDEF5F7, "Tank");
    marker(c, W.store, x0, y0, size, ACCENT, "Store");
    for (int i = 0; i < NBUILD; i++) marker(c, W.building[i], x0, y0, size, (G.explored >> i & 1) ? GREEN : INK, BUILDINGS[i].name);
    for (int i = 0; i < NCREEKS; i++) marker(c, W.creekHead[i], x0, y0, size, G.creekState[i] ? GREEN : 0x6FB0D8, CREEKS[i].name);
    for (int i = 0; i < NSPRINGS; i++) if (g_spring_flowing(i)) marker(c, W.spring[i], x0, y0, size, 0xE8836A, 0);
    float lv = w_level();
    for (int i = 0; i < NITEMS; i++) if (!found_item(i) && (lv <= W.item[i].y + .05f || explored("boathouse"))) marker(c, W.item[i], x0, y0, size, 0xFFE27A, 0);
    if (G.planks < 4 && G.creekState[1] == 0) marker(c, W.plankStack, x0, y0, size, 0xC09060, 0);
    for (int k = 0; k < 5; k++) if (!(G.taken >> k & 1) && G.creekState[0] == 0) marker(c, W.stone[k], x0, y0, size, 0xB8B4AA, 0);
    if (g_plug_exposed()) marker(c, w_deepest(), x0, y0, size, ACCENT, "THE PLUG");
    if (G.won) marker(c, W.caveMouth, x0, y0, size, 0x66E6FF, "CAVE");
    if (is_drowned()) { marker(c, W.church, x0, y0, size, LORE_C, "Church"); }
    /* you */
    if (W.scene == SCENE_OUTSIDE) {
        int mx, my; w_map_xy(P.pos, x0, y0, size, &mx, &my);
        int fx = mx + (int)(m_sin(P.yaw) * 10), fy = my - (int)(m_cos(P.yaw) * 10);
        gfx_line(c, mx, my, fx, fy, 0xFFFFFF, 2);
        gfx_circle(c, mx, my, 5, 0xFF4040, 255);
    }
    gfx_text(c, &font_ui, x0, y0 + size + 18, DIM, "Yellow: finds you can reach.  Red: open springs.  M or Esc closes.");
}

/* ---------- pause menu ---------- */
static const char *PAUSE_ITEMS[] = { "Resume", "Save game", "Detail", "Controls", "New game", "Quit" };
#define NPAUSE 6
static void pause_do(int i);
static void draw_pause(struct canvas *c) {
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 130);
    int w = 360, h = 70 + NPAUSE * 48, x = (c->w - w) / 2, y = (c->h - h) / 2;
    panel(c, x, y, w, h, 245);
    gfx_text_center(c, &font_title, x, w, y + 40, ACCENT, "Paused");
    for (int i = 0; i < NPAUSE; i++) {
        char t[48];
        if (i == 2) ksnprintf(t, sizeof t, "Detail: %s", H.detail == 0 ? "Fast" : H.detail == 1 ? "Balanced" : "Sharp");
        else ksnprintf(t, sizeof t, "%s", PAUSE_ITEMS[i]);
        int by = y + 60 + i * 48;
        ui_button(c, x + 30, by, w - 60, 38, t, i == 0 ? BTN_PRIMARY : i == 5 ? BTN_DANGER : BTN_NORMAL, H.pauseSel == i || ui_in(H.mx, H.my, x + 30, by, w - 60, 38), 0);
    }
}
static void pause_click(struct canvas *c, int mx, int my) {
    int w = 360, h = 70 + NPAUSE * 48, x = (c->w - w) / 2, y = (c->h - h) / 2;
    for (int i = 0; i < NPAUSE; i++) if (ui_in(mx, my, x + 30, y + 60 + i * 48, w - 60, 38)) pause_do(i);
}
static void really_new(void) { g_new_game(); H.screen = SCR_GAME; hud_toast("A new permit. Merrow Lake is full again."); }
static void pause_do(int i) {
    switch (i) {
    case 0: H.screen = SCR_GAME; break;
    case 1: hud_toast(g_save() ? "Saved to /home/.merrow" : "Couldn't save the game"); H.screen = SCR_GAME; break;
    case 2: H.detail = (H.detail + 1) % 3; H.detailChanged = 1; break;
    case 3: H.screen = SCR_GAME;
        hud_modal("Controls",
            "W / Up: start walking, tap again to stop     Shift+W: run\nS / Down: stop, or step back     A D / Left Right: turn (tap or hold)\nSpace: jump     + / -: camera distance\n\n"
            "Mouse: hold the left button to walk, and move the mouse to steer. Drag with the right button to look around. Wheel: zoom.\n\n"
            "E: use, scoop, pour, sell, pick up\nL: logbook     M: map     Esc: this menu\n\n"
            "\x05" "AXIOM tells apps when a key goes down but not when it comes back up, so walking is started and stopped with taps (or by holding the mouse button).",
            1, "Got it", (modal_fn)0);
        break;
    case 4: H.screen = SCR_GAME; hud_modal("New game", "Start over from a full Merrow Lake? Your money, upgrades, crossings and journal pages are lost.", 2, "Start over", really_new, "Cancel", (modal_fn)0); break;
    case 5: g_save(); H.quit = 1; break;
    }
}
static void pause_key(int k) {
    int kc = KEY_CODE(k);
    if (kc == KEY_UP) { if (H.pauseSel > 0) H.pauseSel--; }
    else if (kc == KEY_DOWN) { if (H.pauseSel < NPAUSE - 1) H.pauseSel++; }
    else if (k == '\n' || k == ' ') pause_do(H.pauseSel);
    else if (k == 27) H.screen = SCR_GAME;
}

/* ---------- title screen ---------- */
static void title_do(int i) {
    if (i == 0) { H.screen = SCR_GAME; }
    else { H.screen = SCR_GAME; if (H.hasSave) hud_modal("New game", "Start over from a full Merrow Lake? Your saved game is replaced.", 2, "Start over", really_new, "Cancel", (modal_fn)0); }
}
static void draw_title(struct canvas *c) {
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, 90);
    int y = c->h / 3;
    text_center_shadow(c, &font_ui_bold, 0, c->w, y - 50, 0xC8C0B0, "DUNCAN INTERACTIVE");
    text_center_shadow(c, &font_big, 0, c->w, y, INK, "Merrow Lake Drainage");
    text_center_shadow(c, &font_ui, 0, c->w, y + 30, DIM, WORLDS[world_no()].blurb);
    const char *b0 = H.hasSave ? "Continue" : "Start draining";
    for (int i = 0; i < (H.hasSave ? 2 : 1); i++) {
        int bx = c->w / 2 - 110, by = y + 70 + i * 50;
        ui_button(c, bx, by, 220, 40, i == 0 ? b0 : "New game", i == 0 ? BTN_PRIMARY : BTN_NORMAL, H.titleSel == i || ui_in(H.mx, H.my, bx, by, 220, 40), 0);
    }
    if (H.hasSave) {
        char a[24], t[96]; fmt_money(G.cash, a);
        ksnprintf(t, sizeof t, "%s  " "\xB7" "  %s  " "\xB7" "  %d%% drained", WORLDS[world_no()].name, a, (int)(g_progress() * 100));
        text_center_shadow(c, &font_ui, 0, c->w, y + 190, DIM, t);
    }
    text_center_shadow(c, &font_ui, 0, c->w, c->h - 24, 0x8A8A80, "\xA9" " 2026 Duncan Interactive  " "\xB7" "  AXIOM edition " "\xB7" " v1.0");
}

/* ---------- credits after a crossing ---------- */
static void draw_credits(struct canvas *c) {
    float t = H.creditsT;
    int a = (int)(m_clamp01(t / 1.0f) * 230);
    gfx_blend(c, 0, 0, c->w, c->h, 0x000000, a);
    int y = c->h / 3;
    char s[96];
    text_center_shadow(c, &font_ui_bold, 0, c->w, y - 40, 0xC8C0B0, "MERROW LAKE DRAINAGE");
    text_center_shadow(c, &font_big, 0, c->w, y, INK, H.credTitle);
    ksnprintf(s, sizeof s, "Crossing %d  " "\xB7" "  %s", H.crossings, WORLDS[world_no()].name);
    text_center_shadow(c, &font_ui_bold, 0, c->w, y + 36, ACCENT, s);
    int w = c->w - 160; if (w > 640) w = 640;
    wrap(c, &font_ui, (c->w - w) / 2, y + 60, w, DIM, H.credFooter, strlen(H.credFooter), 1);
    text_center_shadow(c, &font_ui, 0, c->w, y + 150, 0x8A8A80, "A game by Duncan Interactive  " "\xB7" "  rebuilt for AXIOM");
    if (t > 2) text_center_shadow(c, &font_ui, 0, c->w, c->h - 40, INK, "Press any key");
}

/* ---------- the HUD ---------- */
static void bar(struct canvas *c, int x, int y, int w, int h, float f, uint32_t col) {
    gfx_rrect(c, x, y, w, h, h / 2, 0x0A0C0D, 200);
    int fw = (int)(w * m_clamp01(f)); if (fw > 2) gfx_rrect(c, x, y, fw, h, h / 2, col, 255);
}
static void draw_hud(struct canvas *c, float fps) {
    char t[160], a[24], b[24];
    int x = 12, y = 12, w = 286;
    int lines = 6 + (g_bottling() > 0) + (g_inflow() > 0);
    panel(c, x, y, w, 30 + lines * 22, 200);
    fmt_money(G.cash, a);
    gfx_text(c, &font_title, x + 14, y + 30, GREEN, a);
    int yy = y + 54;
    fmt_num(G.water, a);
    ksnprintf(t, sizeof t, "Lake  %s gal  " "\xB7" "  %d ft down", a, (int)w_feet());
    gfx_text(c, &font_ui, x + 14, yy, INK, t); yy += 8;
    bar(c, x + 14, yy, w - 28, 6, (float)(G.water / TOTAL_GALLONS), 0x4F98B8); yy += 20;
    fmt_num(G.tank, a); fmt_num(g_capacity(), b);
    ksnprintf(t, sizeof t, "Tank  %s / %s gal", a, b);
    gfx_text(c, &font_ui, x + 14, yy, INK, t); yy += 8;
    bar(c, x + 14, yy, w - 28, 6, (float)(G.tank / g_capacity()), 0xDEF5F7); yy += 20;
    fmt_num(G.carried, a); fmt_num(g_carry_cap(), b);
    ksnprintf(t, sizeof t, "Bucket  %s / %s gal", a, b);
    gfx_text(c, &font_ui, x + 14, yy, INK, t); yy += 22;
    fmt_num(g_pump_rate(), a); fmt_money(g_price(), b);
    ksnprintf(t, sizeof t, "Pumps  %s gal/s  " "\xB7" "  %s/gal", a, b);
    gfx_text(c, &font_ui, x + 14, yy, INK, t); yy += 22;
    if (g_inflow() > 0) { fmt_num(g_inflow(), a); ksnprintf(t, sizeof t, "Flowing back in  %s gal/s", a); gfx_text(c, &font_ui, x + 14, yy, 0xE8836A, t); yy += 22; }
    if (g_bottling() > 0) { fmt_money(g_bottling(), a); ksnprintf(t, sizeof t, "Bottling  +%s/s", a); gfx_text(c, &font_ui, x + 14, yy, GREEN, t); yy += 22; }
    /* top right: where and when */
    char clk[16]; w_clock(clk);
    ksnprintf(t, sizeof t, "%s  " "\xB7" "  %s  " "\xB7" "  %s", WORLDS[world_no()].name, clk, W.scene == SCENE_OUTSIDE ? w_weather_name() : W.scene == SCENE_CAVE ? "The cave" : "The Still Room");
    int tw = gfx_text_width(&font_ui_bold, t);
    panel(c, c->w - tw - 40, 12, tw + 28, 30, 200);
    gfx_text(c, &font_ui_bold, c->w - tw - 26, 32, INK, t);
    if (H.showFps) {
        extern int w_prof[8];
        ksnprintf(t, sizeof t, "%d fps  %d tris  sky %d bd %d ter %d wat %d obj %d dyn %d  pres %d hud %d flush %d", (int)fps, R.tris,
                  w_prof[0], w_prof[1], w_prof[2], w_prof[3], w_prof[4], w_prof[5], w_prof[6], w_prof[7], H.flushMs);
        gfx_text(c, &font_ui, c->w - 20 - gfx_text_width(&font_ui, t), 60, DIM, t);
        ksnprintf(t, sizeof t, "pos %d %d %d  scene %d  yaw %d", (int)P.pos.x, (int)P.pos.y, (int)P.pos.z, W.scene, (int)(P.yaw * 57.3f));
        gfx_text(c, &font_ui, c->w - 20 - gfx_text_width(&font_ui, t), 80, DIM, t);
    }
    /* labels over notable places */
    if (W.scene == SCENE_OUTSIDE) {
        float sx, sy; int sc = H.scale;
        if (g_plug_exposed() && r_project(v_add(w_deepest(), V3(0, 5, 0)), &sx, &sy)) text_center_shadow(c, &font_ui_bold, (int)(sx * sc) - 100 + H.ox, 200, (int)(sy * sc) + H.oy, ACCENT, "THE PLUG");
        if (G.won && r_project(v_add(W.caveMouth, V3(0, 5, 0)), &sx, &sy)) text_center_shadow(c, &font_ui_bold, (int)(sx * sc) - 100 + H.ox, 200, (int)(sy * sc) + H.oy, 0x66E6FF, "CAVE");
    }
    /* floating numbers */
    for (int i = 0; i < 16; i++) {
        if (H.floats[i].t <= 0) continue;
        float sx, sy;
        v3 p = v_add(H.floats[i].p, V3(0, (1.6f - H.floats[i].t) * 1.5f, 0));
        if (r_project(p, &sx, &sy)) text_center_shadow(c, &font_ui_bold, (int)(sx * H.scale) - 150 + H.ox, 300, (int)(sy * H.scale) + H.oy, H.floats[i].col, H.floats[i].s);
    }
    /* the prompt */
    if (H.prompt[0] && !hud_modal_open()) {
        int pw = gfx_text_width(&font_ui_bold, H.prompt) + 36;
        panel(c, (c->w - pw) / 2, c->h - 96, pw, 34, 220);
        gfx_text_center(c, &font_ui_bold, 0, c->w, c->h - 73, ACCENT, H.prompt);
    }
    /* toast */
    if (H.toastT > 0) {
        int pw = gfx_text_width(&font_ui_bold, H.toast) + 40;
        int al = (int)(m_clamp01(H.toastT * 2) * 230);
        gfx_rrect(c, (c->w - pw) / 2, 56, pw, 34, 10, 0x2A2418, al);
        gfx_text_center(c, &font_ui_bold, 0, c->w, 79, 0xFFE2A0, H.toast);
    }
    /* hints */
    gfx_text(c, &font_ui, 14, c->h - 14, 0xC8C0B0, "W walk/stop  " "\xB7" "  A D turn  " "\xB7" "  hold mouse: walk & steer  " "\xB7" "  E use  " "\xB7" "  L logbook  " "\xB7" "  M map  " "\xB7" "  Esc menu");
}

void hud_update(float dt) {
    if (H.toastT > 0) H.toastT -= dt;
    for (int i = 0; i < 16; i++) if (H.floats[i].t > 0) H.floats[i].t -= dt;
    if (H.screen == SCR_CREDITS) H.creditsT += dt;
    if (H.fadeT > 0) {
        H.fadeT -= dt;
        if (H.fadeT < 0.6f && !H.fadeDone) { H.fadeDone = 1; if (H.fadeMid) H.fadeMid(); }
    }
}

void hud_draw(struct canvas *c, float fps) {
    if (H.screen == SCR_TITLE) { draw_title(c); if (hud_modal_open()) draw_modal(c); return; }
    if (H.screen == SCR_CREDITS) { draw_credits(c); return; }
    draw_hud(c, fps);
    if (H.screen == SCR_SHOP) draw_shop(c);
    else if (H.screen == SCR_LOG) draw_log(c);
    else if (H.screen == SCR_MAP) draw_map(c);
    else if (H.screen == SCR_PAUSE) draw_pause(c);
    if (hud_modal_open()) draw_modal(c);
    if (H.fadeT > 0) {
        float f = H.fadeT > 0.6f ? (1.2f - H.fadeT) / 0.6f : H.fadeT / 0.6f;
        gfx_blend(c, 0, 0, c->w, c->h, 0x000000, (int)(m_clamp01(f) * 255));
    }
}

/* returns 1 if the HUD used the key */
int hud_key(int k) {
    if (H.screen == SCR_CREDITS) { if (H.creditsT > 1.0f) H.screen = SCR_GAME; return 1; }
    if (hud_modal_open()) return modal_key(k);
    if (H.screen == SCR_TITLE) {
        int kc = KEY_CODE(k);
        if (kc == KEY_UP) H.titleSel = 0; else if (kc == KEY_DOWN && H.hasSave) H.titleSel = 1;
        else if (k == '\n' || k == ' ' || k == 'e') title_do(H.titleSel);
        return 1;
    }
    switch (H.screen) {
    case SCR_SHOP: shop_key(k); return 1;
    case SCR_LOG: log_key(k); return 1;
    case SCR_MAP: if (k == 27 || k == 'm' || k == 'M') H.screen = SCR_GAME; return 1;
    case SCR_PAUSE: pause_key(k); return 1;
    }
    if (k == 27) { H.screen = SCR_PAUSE; H.pauseSel = 0; return 1; }
    if (k == 'l' || k == 'L') { H.screen = SCR_LOG; H.logScroll = 0; return 1; }
    if (k == 'm' || k == 'M') { H.screen = SCR_MAP; return 1; }
    if (k == KEY_F3) { H.showFps = !H.showFps; return 1; }
    return 0;
}

int hud_click(struct canvas *c, int x, int y) {
    if (H.screen == SCR_CREDITS) { if (H.creditsT > 1.0f) H.screen = SCR_GAME; return 1; }
    if (hud_modal_open()) return modal_click(x, y);
    if (H.screen == SCR_TITLE) {
        int yy = c->h / 3;
        for (int i = 0; i < (H.hasSave ? 2 : 1); i++) if (ui_in(x, y, c->w / 2 - 110, yy + 70 + i * 50, 220, 40)) title_do(i);
        return 1;
    }
    if (H.screen == SCR_SHOP) { shop_click(c, x, y); return 1; }
    if (H.screen == SCR_PAUSE) { pause_click(c, x, y); return 1; }
    if (H.screen == SCR_LOG || H.screen == SCR_MAP) { H.screen = SCR_GAME; return 1; }
    return 0;
}

int hud_blocking(void) { return H.screen != SCR_GAME || hud_modal_open() || H.fadeT > 0; }
