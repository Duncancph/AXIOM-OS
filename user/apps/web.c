/* Web 0.2 - a simple web browser for DuncOS (installed with: clan install web).
 * Fetches pages over HTTP and HTTPS (TLS 1.3 with certificate checks) and lays out the text:
 * headings, paragraphs, bold, links, lists and preformatted text. No images, styles or scripts. */
#include "../lib/dunc.h"

static int WW = 860, WH = 620;
#define BAR_H 52
#define MARGIN 28
#define MAXPAGE (1 << 20)

static struct app a;
static char page[MAXPAGE]; static long page_len;
static char url[600] = "about:home", title[96] = "Web", status_text[160], ctype[64];
static char addr[600]; static int alen; static bool addr_focus;
static char hist[32][600]; static int nhist;
static int scroll, doc_h, hover_link = -1, hover_btn = -1;
static struct http_security sec;

/* laid-out document */
enum { ST_TEXT, ST_BOLD, ST_H1, ST_H2, ST_H3, ST_MONO, ST_SMALL };
struct span { int x, y; uint32_t off; uint16_t len; uint8_t style; int16_t link; int w; };
#define MAXSPANS 24000
static struct span spans[MAXSPANS]; static int nspans;
static char words[MAXPAGE]; static uint32_t wpos;
static char links[600][300]; static int nlinks;

static const struct font *sfont(int st) {
    switch (st) { case ST_BOLD: return &font_ui_bold; case ST_H1: return &font_big; case ST_H2: return &font_title; case ST_H3: return &font_ui_bold; }
    return &font_ui;
}
static int lineh(int st) { return st == ST_H1 ? 46 : st == ST_H2 ? 32 : st == ST_MONO ? 18 : 22; }
static int text_w(int st, const char *s, int n) {
    if (st == ST_MONO) return n * 8;
    char tmp[256]; if (n > 255) n = 255; memcpy(tmp, s, n); tmp[n] = 0;
    return gfx_text_width(sfont(st), tmp);
}

/* ---------------- URL helpers ---------------- */
static bool pre_ci(const char *s, const char *p) { for (; *p; s++, p++) { char x = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s; if (x != *p) return false; } return true; }

static void resolve_url(const char *base, const char *href, char *out, int n) {
    while (*href == ' ') href++;
    if (pre_ci(href, "http://") || pre_ci(href, "https://") || pre_ci(href, "about:")) { strncpy0(out, href, n); return; }
    if (pre_ci(href, "mailto:") || pre_ci(href, "javascript:")) { strncpy0(out, "about:unsupported", n); return; }
    const char *scheme = pre_ci(base, "https://") ? "https" : "http";
    const char *h = base; if (pre_ci(h, "http://")) h += 7; else if (pre_ci(h, "https://")) h += 8;
    char origin[300]; int k = 0;
    while (*h && *h != '/' && k < 299) origin[k++] = *h++;
    origin[k] = 0;
    if (href[0] == '/' && href[1] == '/') { ksnprintf(out, n, "%s:%s", scheme, href); return; }
    if (href[0] == '/') { ksnprintf(out, n, "%s://%s%s", scheme, origin, href); return; }
    if (href[0] == '#') { strncpy0(out, base, n); return; }
    char dir[400]; strncpy0(dir, *h ? h : "/", sizeof dir);
    char *q = dir; while (*q && *q != '?' && *q != '#') q++; *q = 0;
    char *sl = dir + strlen(dir); while (sl > dir && sl[-1] != '/') sl--; *sl = 0;
    ksnprintf(out, n, "%s://%s%s%s", scheme, origin, dir[0] ? dir : "/", href);
}

/* ---------------- layout ---------------- */
static int cx, cy, line_st, indent, cur_link = -1;
static bool at_line_start = true, pending_space;
static int right(void) { return WW - MARGIN; }

static void newline(int extra) {
    if (!at_line_start || extra) cy += lineh(line_st) + extra;
    cx = MARGIN + indent; at_line_start = true; pending_space = false;
}
static void block(int st, int gap) { if (!at_line_start) newline(0); cy += gap; line_st = st; cx = MARGIN + indent; }

static void emit(const char *w, int n, int st) {
    if (!n || nspans >= MAXSPANS || wpos + n >= sizeof words) return;
    int ww = text_w(st, w, n), sp = pending_space && !at_line_start ? text_w(st, " ", 1) : 0;
    if (!at_line_start && cx + sp + ww > right()) { newline(0); sp = 0; }
    if (lineh(st) > lineh(line_st) && at_line_start) line_st = st;
    cx += sp;
    struct span *s = &spans[nspans++];
    s->x = cx; s->y = cy; s->off = wpos; s->len = n; s->style = st; s->link = cur_link; s->w = ww;
    memcpy(words + wpos, w, n); wpos += n;
    cx += ww; at_line_start = false; pending_space = false;
}

static int entity(const char *s, long n, char *out) {   /* returns chars consumed, writes 1 char */
    static const struct { const char *e; char c; } ents[] = { {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"nbsp;", ' '}, {"copy;", 'c'}, {"mdash;", '-'}, {"ndash;", '-'}, {"hellip;", '.'}, {"rsquo;", '\''},
        {"lsquo;", '\''}, {"rdquo;", '"'}, {"ldquo;", '"'}, {"middot;", '.'}, {"raquo;", '>'}, {"laquo;", '<'} };
    if (n > 1 && s[1] == '#') {
        long v = 0; int i = 2; bool hx = n > 2 && (s[2] == 'x' || s[2] == 'X'); if (hx) i++;
        for (; i < n && i < 10 && s[i] != ';'; i++) { char c = s[i] | 32; v = v * (hx ? 16 : 10) + (c >= 'a' ? c - 'a' + 10 : c - '0'); }
        if (i < n && s[i] == ';') { *out = v >= 32 && v < 127 ? (char)v : v == 160 ? ' ' : v == 8217 || v == 8216 ? '\'' : v == 8220 || v == 8221 ? '"' : v == 8211 || v == 8212 ? '-' : '?'; return i + 1; }
        *out = '&'; return 1;
    }
    for (unsigned k = 0; k < sizeof ents / sizeof ents[0]; k++) {
        int l = strlen(ents[k].e);
        if (n > l && !memcmp(s + 1, ents[k].e, l)) { *out = ents[k].c; return l + 1; }
    }
    *out = '&'; return 1;
}

static bool tag_is(const char *t, const char *name) {
    int l = strlen(name);
    for (int i = 0; i < l; i++) if ((t[i] | 32) != name[i]) return false;
    char e = t[l]; return e == ' ' || e == '>' || e == '/' || e == '\t' || e == '\n' || e == '\r' || e == 0;
}

static bool attr(const char *tag, const char *name, char *out, int n) {
    int l = strlen(name);
    for (const char *p = tag; *p && *p != '>'; p++) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && pre_ci(p, name) && (p[l] == '=' || p[l] == ' ')) {
            p += l; while (*p == ' ') p++;
            if (*p != '=') continue;
            p++; while (*p == ' ') p++;
            char q = (*p == '"' || *p == '\'') ? *p++ : 0;
            int k = 0;
            while (*p && (q ? *p != q : (*p != ' ' && *p != '>')) && k < n - 1) out[k++] = *p++;
            out[k] = 0;
            return true;
        }
    }
    return false;
}

static void layout_text_plain(void) {
    char w[256]; int k = 0;
    for (long i = 0; i <= page_len; i++) {
        char c = i < page_len ? page[i] : '\n';
        if (c == '\n' || k == 255) { emit(w, k, ST_MONO); k = 0; newline(1); continue; }
        if (c == '\t') c = ' ';
        if (c >= 32 && c < 127) w[k++] = c;
    }
}

static void layout(void) {
    nspans = 0; wpos = 0; nlinks = 0;
    cx = MARGIN; cy = BAR_H + 20; line_st = ST_TEXT; indent = 0; cur_link = -1;
    at_line_start = true; pending_space = false;
    if (!pre_ci(ctype, "text/html") && ctype[0]) { layout_text_plain(); doc_h = cy + 40; return; }
    int bold = 0, heading = 0, pre = 0, skip = 0, list_depth = 0;
    char w[256]; int k = 0;
    title[0] = 0;
    for (long i = 0; i < page_len; i++) {
        char c = page[i];
        if (c == '<') {
            int st = heading ? heading : pre ? ST_MONO : bold ? ST_BOLD : ST_TEXT;
            if (k) { emit(w, k, st); k = 0; }
            long e = i + 1; while (e < page_len && page[e] != '>') e++;
            if (e >= page_len) break;
            char tag[512]; long tl = e - i - 1 < 511 ? e - i - 1 : 511;
            memcpy(tag, page + i + 1, tl); tag[tl] = 0;
            bool close = tag[0] == '/';
            const char *t = close ? tag + 1 : tag;
            i = e;
            if (tag[0] == '!') continue;                                    /* comments / doctype */
            if (tag_is(t, "script") || tag_is(t, "style")) { if (!close) skip++; else if (skip) skip--; continue; }
            if (tag_is(t, "title") && !close) {                             /* read the title text */
                long s = i + 1, n = 0;
                while (s < page_len && page[s] != '<' && n < 90) { char ch = page[s++]; title[n++] = ch == '\n' || ch == '\r' ? ' ' : ch; }
                title[n] = 0; i = s - 1; skip++; continue;
            }
            if (tag_is(t, "title") && close) { if (skip) skip--; continue; }
            if (skip) continue;
            if (tag_is(t, "b") || tag_is(t, "strong") || tag_is(t, "th")) bold += close ? (bold ? -1 : 0) : 1;
            else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !(t[2] >= 'a' && t[2] <= 'z')) {
                int lv = t[1] - '0';
                if (!close) { heading = lv == 1 ? ST_H1 : lv == 2 ? ST_H2 : ST_H3; block(heading, lv <= 2 ? 14 : 8); }
                else { heading = 0; block(ST_TEXT, 6); }
            }
            else if (tag_is(t, "p") || tag_is(t, "div") || tag_is(t, "section") || tag_is(t, "article") || tag_is(t, "header") ||
                     tag_is(t, "footer") || tag_is(t, "nav") || tag_is(t, "table") || tag_is(t, "form") || tag_is(t, "dl") || tag_is(t, "dt") || tag_is(t, "main"))
                block(ST_TEXT, tag_is(t, "p") ? 10 : 2);
            else if (tag_is(t, "blockquote") || tag_is(t, "dd")) { if (close) { indent -= 24; if (indent < 0) indent = 0; } else indent += 24; block(ST_TEXT, 6); }
            else if (tag_is(t, "br")) { int s = line_st; newline(at_line_start ? 1 : 0); line_st = s; }
            else if (tag_is(t, "tr")) block(ST_TEXT, 2);
            else if (tag_is(t, "td")) { if (!close && !at_line_start) { pending_space = true; emit(" |", 2, ST_TEXT); } }
            else if (tag_is(t, "hr")) { block(ST_TEXT, 8); emit("------------------------------", 30, ST_SMALL); block(ST_TEXT, 4); }
            else if (tag_is(t, "ul") || tag_is(t, "ol")) { if (close) { list_depth--; indent -= 22; if (indent < 0) indent = 0; } else { list_depth++; indent += 22; } block(ST_TEXT, 4); }
            else if (tag_is(t, "li") && !close) { block(ST_TEXT, 2); emit("-", 1, ST_BOLD); pending_space = true; }
            else if (tag_is(t, "pre")) { pre = close ? 0 : 1; block(close ? ST_TEXT : ST_MONO, 6); }
            else if (tag_is(t, "code") || tag_is(t, "tt")) { /* stays inline */ }
            else if (tag_is(t, "a")) {
                if (close) cur_link = -1;
                else { char href[300]; if (attr(tag, "href", href, sizeof href) && nlinks < 600) { resolve_url(url, href, links[nlinks], 300); cur_link = nlinks++; } }
            }
            else if (tag_is(t, "img") && !close) { char alt[80]; if (attr(tag, "alt", alt, sizeof alt) && alt[0]) { char b[100]; int n = ksnprintf(b, sizeof b, "[%s]", alt); emit(b, n, ST_SMALL); pending_space = true; } }
            continue;
        }
        if (skip) continue;
        if (c == '&') { char ch; i += entity(page + i, page_len - i, &ch) - 1; c = ch; }
        if (pre) {
            int st = ST_MONO;
            if (c == '\n') { if (k) { emit(w, k, st); k = 0; } int s = line_st; newline(at_line_start ? 1 : 0); line_st = s; continue; }
            if (c == ' ' || c == '\t') { if (k) { emit(w, k, st); k = 0; } cx += c == '\t' ? 32 : 8; continue; }
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (k) { emit(w, k, heading ? heading : bold ? ST_BOLD : ST_TEXT); k = 0; }
            pending_space = true;
            continue;
        }
        if ((unsigned char)c >= 128) { if ((unsigned char)c >= 0xC0) c = '?'; else continue; }   /* UTF-8: one '?' per character */
        if (k < 255) w[k++] = c;
    }
    if (k) emit(w, k, ST_TEXT);
    newline(0);
    doc_h = cy + 40;
}

/* ---------------- drawing ---------------- */
static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, 0xF4F1EA);                       /* paper-coloured page */
    gfx_clip(c, 0, BAR_H, WW, WH - BAR_H - 24);
    for (int i = 0; i < nspans; i++) {
        struct span *s = &spans[i];
        int y = s->y - scroll;
        if (y + 50 < BAR_H || y > WH) continue;
        char t[256]; int n = s->len < 255 ? s->len : 255; memcpy(t, words + s->off, n); t[n] = 0;
        uint32_t col = s->link >= 0 ? (s->link == hover_link ? 0x0B5CAD : 0x1A6FC4) : s->style == ST_SMALL ? 0x7A7468 : 0x23201B;
        if (s->style == ST_MONO) gfx_mono_text(c, s->x, y, t, col);
        else {
            const struct font *f = sfont(s->style);
            gfx_text(c, f, s->x, y + f->ascent, col, t);
            if (s->link >= 0) {
                int ux = s->x;   /* join the underline with the previous word of the same link */
                if (i > 0 && spans[i - 1].link == s->link && spans[i - 1].y == s->y) ux = spans[i - 1].x + spans[i - 1].w;
                gfx_fill(c, ux, y + f->ascent + 2, s->x + s->w - ux, 1, col);
            }
        }
    }
    gfx_noclip(c);
    /* toolbar (Highland) */
    gfx_fill(c, 0, 0, WW, BAR_H, HL_WIN_BG);
    gfx_fill(c, 0, BAR_H - 1, WW, 1, HL_WIN_BORDER);
    ui_button(c, 12, 10, 36, 32, "<", BTN_NORMAL, hover_btn == 0, nhist == 0);
    ui_button(c, 54, 10, 36, 32, "R", BTN_NORMAL, hover_btn == 1, false);
    int ax = 98, aw = WW - ax - 12 - 64;
    gfx_rrect(c, ax, 10, aw, 32, 8, HL_TERM_BG, 255);
    gfx_rrect_outline(c, ax, 10, aw, 32, 8, addr_focus ? HL_ACCENT : HL_WIN_BORDER, 255);
    int tx = ax + 12;
    if (!addr_focus && sec.secure) {                       /* padlock */
        gfx_rrect_outline(c, ax + 12, 17, 10, 10, 4, HL_GREEN, 255);
        gfx_fill(c, ax + 11, 22, 12, 9, HL_GREEN);
        gfx_fill(c, ax + 16, 25, 2, 3, HL_TERM_BG);
        tx = ax + 32;
    }
    gfx_clip(c, tx - 4, 10, aw - (tx - ax) - 4, 32);
    int vis = (aw - 24) / 8, start = addr_focus && alen > vis ? alen - vis : 0;
    gfx_mono_text(c, tx, 18, (addr_focus ? addr : url) + (addr_focus ? start : 0), addr_focus ? HL_TEXT : HL_TEXT_DIM);
    if (addr_focus) gfx_fill(c, tx + (alen - start) * 8, 18, 2, 16, HL_ACCENT);
    gfx_noclip(c);
    ui_button(c, WW - 12 - 56, 10, 56, 32, "Go", BTN_PRIMARY, hover_btn == 2, false);
    /* status bar */
    gfx_fill(c, 0, WH - 24, WW, 24, HL_WIN_BG);
    const char *st = hover_link >= 0 ? links[hover_link] : status_text;
    gfx_mono_text(c, 10, WH - 20, st, HL_TEXT_FAINT);
    app_flush(&a);
}

/* ---------------- pages ---------------- */
static void set_page(const char *html) {
    page_len = strlen(html); if (page_len >= MAXPAGE) page_len = MAXPAGE - 1;
    memcpy(page, html, page_len); page[page_len] = 0;
    strncpy0(ctype, "text/html", sizeof ctype);
}

static void message_page(const char *head, const char *body) {
    static char h[2048];
    ksnprintf(h, sizeof h, "<h1>%s</h1><p>%s</p><p><a href=\"about:home\">Go to the start page</a></p>", head, body);
    set_page(h);
}

static void load(const char *u, bool push) {
    if (push && strcmp(u, url) && nhist < 32) strncpy0(hist[nhist++], url, 600);
    strncpy0(url, u, sizeof url);
    scroll = 0; hover_link = -1; addr_focus = false;
    memset(&sec, 0, sizeof sec);
    if (pre_ci(url, "about:unsupported")) {
        message_page("Not supported", "That kind of link (email or script) doesn't work in Web.");
        strncpy0(status_text, "Not supported", sizeof status_text);
    } else if (pre_ci(url, "about:")) {
        set_page("<title>Start page</title><h1>Web</h1><p>A simple browser for <b>DuncOS</b>, running on the Thane kernel's own TCP/IP stack."
                 " Secure <b>https://</b> sites use TLS 1.3, and every site's certificate is checked against the trusted authorities.</p>"
                 "<h2>Sites that read well as text</h2><ul>"
                 "<li><a href=\"https://text.npr.org/\">text.npr.org</a> - news in plain text</li>"
                 "<li><a href=\"https://lite.duckduckgo.com/lite/\">lite.duckduckgo.com</a> - web search, lite edition</li>"
                 "<li><a href=\"https://news.ycombinator.com/\">news.ycombinator.com</a> - Hacker News</li>"
                 "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a></li>"
                 "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> - the world's first website (1991)</li></ul>"
                 "<h2>Not shown</h2><p>Images, styles and JavaScript. Sites that only allow TLS 1.2 or older can't be opened yet.</p>");
        strncpy0(status_text, "Start page", sizeof status_text);
    } else {
        strncpy0(status_text, "Loading...", sizeof status_text);
        nspans = 0; draw();
        int status = 0; char final[600];
        ctype[0] = 0;
        long n = http_get(url, page, MAXPAGE - 1, &status, ctype, sizeof ctype, final, sizeof final);
        strncpy0(status_text, url, sizeof status_text);
        if (n <= -40 && n >= -68) {
            char m[400];
            ksnprintf(m, sizeof m, "DuncOS couldn't set up a secure connection to <b>%s</b>: %s.</p><p>This protects you from impostor sites."
                      " If the computer's clock is wrong, fix it and try again.", url, http_strerror(n) + 26);
            message_page("Connection isn't secure", m);
        }
        else if (n < 0) { char m[260]; ksnprintf(m, sizeof m, "Couldn't open %s: %s.", url, http_strerror(n)); message_page("Can't reach this page", m); }
        else {
            sec = http_sec;
            page_len = n; page[n] = 0;
            strncpy0(url, final, sizeof url);
            if (sec.secure) ksnprintf(status_text, sizeof status_text, "Secure: %s - certificate for %s, from %s", sec.cipher, sec.subject, sec.issuer);
            else ksnprintf(status_text, sizeof status_text, "Not secure (http) - HTTP %d - %ld bytes", status, n);
            if (ctype[0] && !pre_ci(ctype, "text/")) message_page("Can't show this file", "This page isn't text (it might be an image or a download). Use <b>wget</b> in the terminal to save it.");
        }
    }
    layout();
    char t[140]; ksnprintf(t, sizeof t, "Web - %s", title[0] ? title : url);
    app_title(&a, t);
}

static int link_at(int x, int y) {
    for (int i = 0; i < nspans; i++) {
        struct span *s = &spans[i];
        if (s->link < 0) continue;
        int sy = s->y - scroll, h = lineh(s->style);
        if (x >= s->x && x < s->x + s->w && y >= sy && y < sy + h) return s->link;
    }
    return -1;
}

static int btn_at(int x, int y) {
    if (ui_in(x, y, 12, 10, 36, 32)) return 0;
    if (ui_in(x, y, 54, 10, 36, 32)) return 1;
    if (ui_in(x, y, WW - 68, 10, 56, 32)) return 2;
    if (ui_in(x, y, 98, 10, WW - 98 - 76, 32)) return 3;
    return -1;
}

static void scroll_by(int d) {
    scroll += d;
    int mx = doc_h - (WH - 24);
    if (scroll > mx) scroll = mx;
    if (scroll < 0) scroll = 0;
}

int main(void) {
    char arg[256];
    if (!app_open_ex(&a, WW, WH, "Web", WIN_RESIZABLE)) return 1;
    if (sys_getarg(arg, sizeof arg) > 0) load(arg, false); else load("about:home", false);
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; layout(); scroll_by(0); draw(); } continue; }
        if (e.type == EV_WHEEL) { scroll_by(e.key * 60); draw(); continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int l = e.type == EV_MOUSE_MOVE && e.y > BAR_H ? link_at(e.x, e.y) : -1, b = e.type == EV_MOUSE_MOVE ? btn_at(e.x, e.y) : -1;
            if (l != hover_link || b != hover_btn) { hover_link = l; hover_btn = b; draw(); }
            continue;
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int b = btn_at(e.x, e.y);
            if (b == 0 && nhist) { char u[600]; strncpy0(u, hist[--nhist], 600); load(u, false); }
            else if (b == 1) load(url, false);
            else if (b == 2) { if (addr_focus && alen) load(addr, true); else load(url, false); }
            else if (b == 3) { addr_focus = true; strncpy0(addr, url, sizeof addr); alen = strlen(addr); }
            else { addr_focus = false; int l = link_at(e.x, e.y); if (l >= 0) { char u[300]; strncpy0(u, links[l], 300); load(u, true); } }
            draw();
            continue;
        }
        if (e.type == EV_KEY) {
            int k = e.key;
            if (addr_focus) {
                if (k == '\n') { if (alen) { char u[600]; if (!strncmp(addr, "http", 4) || !strncmp(addr, "about:", 6)) strncpy0(u, addr, 600); else ksnprintf(u, 600, "https://%s", addr); load(u, true); } }
                else if (k == 27) addr_focus = false;
                else if (k == '\b') { if (alen) addr[--alen] = 0; }
                else if (key_is_char(k) && alen < 590) { addr[alen++] = k; addr[alen] = 0; }
            } else {
                if (k == KEY_DOWN) scroll_by(40); else if (k == KEY_UP) scroll_by(-40);
                else if (k == KEY_PGDN || k == ' ') scroll_by(WH - 120); else if (k == KEY_PGUP) scroll_by(-(WH - 120));
                else if (k == KEY_HOME) scroll = 0;
                else if (k == '\b' && nhist) { char u[600]; strncpy0(u, hist[--nhist], 600); load(u, false); }
                else if (k == 12 || k == '/') { addr_focus = true; addr[0] = 0; alen = 0; }   /* Ctrl+L */
            }
            draw();
        }
    }
    return 0;
}
