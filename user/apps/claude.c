/* Claude: chat with Claude through the Anthropic API (https://api.anthropic.com/v1/messages).
 * Needs an API key from console.anthropic.com (billed by Anthropic per use), saved in /system/config/claude.key.
 * Replies stream in as they are written. Each reply's content blocks are kept exactly as received (thinking blocks
 * and their signatures included) and sent back unchanged on the next turn, so the conversation stays valid. */
#include "../lib/dunc.h"
#include "../lib/json.h"

static int WW = 780, WH = 600;
#define TOP 56
#define MODEL "claude-opus-5-5"
#define MODEL_NAME "Claude Opus 5.5"
#define KEYFILE "/system/config/claude.key"
#define URLFILE "/system/config/claude.url"     /* developers: another endpoint, for testing */
static const char *SYSTEM =
    "You are Claude, talking with the user through a chat app on DuncOS, a hobby operating system written from "
    "scratch. The app shows plain text in a proportional font: it does not render Markdown, tables, images or links. "
    "Write in plain text with short paragraphs. For lists, start lines with \"- \". Put code between lines containing "
    "only ``` and it will be shown in a monospaced font. The app can show Western European characters (including "
    "accents and the euro sign) but not emoji or other scripts.";

static struct app a;

/* ---------------- the conversation ---------------- */
struct msg { int assistant; char *disp; long dlen; char *json; int note; };   /* disp: Windows-1252; json: what is sent */
static struct msg *msgs; static int nmsgs, cap_msgs;
static int scroll_px, content_h;            /* scroll offset from the top, total height */
static bool stick_bottom = true;

static char key[256];
static char url[256] = "https://api.anthropic.com/v1/messages";
static char input[8192]; static int ilen;
static char status[160]; static uint32_t status_col;
static bool busy;
static int dialog;                          /* 1: API key */
static char kbuf[256]; static int klen;
static int hover = -1;

static char *dupn(const char *s, long n) { char *p = malloc(n + 1); if (p) { memcpy(p, s, n); p[n] = 0; } return p; }

static void push_msg(int assistant, const char *disp, long dlen, char *json) {
    if (nmsgs == cap_msgs) { cap_msgs = cap_msgs ? cap_msgs * 2 : 16; msgs = realloc(msgs, sizeof *msgs * cap_msgs); }
    struct msg *m = &msgs[nmsgs++];
    memset(m, 0, sizeof *m);
    m->assistant = assistant; m->disp = dupn(disp, dlen); m->dlen = dlen; m->json = json;
}
static void pop_msg(void) { if (!nmsgs) return; nmsgs--; free(msgs[nmsgs].disp); free(msgs[nmsgs].json); }

/* ---------------- layout: wrap text into lines ---------------- */
struct line { const char *s; int n, y, kind; };            /* kind: 0 text, 1 bold, 2 code */
static struct line *lines; static int nlines, cap_lines;
static void add_line(const char *s, int n, int y, int kind) {
    if (nlines == cap_lines) { cap_lines = cap_lines ? cap_lines * 2 : 256; lines = realloc(lines, sizeof *lines * cap_lines); }
    lines[nlines++] = (struct line){ s, n, y, kind };
}
static int width_of(const char *s, int n, const struct font *f) {
    char t[512]; if (n > 511) n = 511;
    memcpy(t, s, n); t[n] = 0;
    return gfx_text_width(f, t);
}

/* wrap one message into lines (positions relative to its top); returns its height */
static int wrap(const char *t, long len, int width, int y0, bool record) {
    int y = y0; bool code = false;
    for (long i = 0; i < len;) {
        long e = i; while (e < len && t[e] != '\n') e++;
        const char *ln = t + i; int n = (int)(e - i);
        if (n >= 3 && !strncmp(ln, "```", 3)) { code = !code; i = e + 1; continue; }
        if (code) {                                           /* monospaced, hard wrapped */
            int per = width / 8; if (per < 8) per = 8;
            if (!n) { if (record) add_line(ln, 0, y, 2); y += 16; }
            for (int k = 0; k < n; k += per) { if (record) add_line(ln + k, n - k < per ? n - k : per, y, 2); y += 16; }
        } else {
            int kind = 0;
            if (n && ln[0] == '#') { while (n && (*ln == '#' || *ln == ' ')) { ln++; n--; } kind = 1; }
            const struct font *f = kind ? &font_ui_bold : &font_ui;
            if (!n) y += 10;
            for (int k = 0; k < n;) {                          /* greedy word wrap */
                int best = 0;
                for (int j = k; j <= n; j++) {
                    if (j < n && ln[j] != ' ') continue;
                    if (width_of(ln + k, j - k, f) > width) break;
                    best = j - k;
                }
                if (!best) { best = 1; while (k + best < n && width_of(ln + k, best + 1, f) <= width) best++; }
                if (record) add_line(ln + k, best, y, kind);
                y += 20;
                k += best; while (k < n && ln[k] == ' ') k++;
            }
        }
        i = e + 1;
    }
    return y - y0;
}

/* ---------------- drawing ---------------- */
static int inp_lines(void) {
    int n = wrap(input, ilen, WW - 160, 0, false) / 20; if (n < 1) n = 1;
    return n > 6 ? 6 : n;
}
static int bottom_h(void) { return 30 + inp_lines() * 20 + 34; }

static void draw_text_line(struct canvas *c, const struct line *l, int x, int y, uint32_t col) {
    char t[512]; int n = l->n > 511 ? 511 : l->n;
    memcpy(t, l->s, n); t[n] = 0;
    if (l->kind == 2) gfx_mono_text(c, x, y - 14, t, 0xD8DEE9);
    else gfx_text(c, l->kind ? &font_ui_bold : &font_ui, x, y, col, t);
}

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    /* header */
    gfx_fill(c, 0, 0, WW, TOP - 8, 0x0C1426);
    gfx_circle(c, 30, 24, 14, 0xD97757, 255);
    gfx_text_center(c, &font_ui_bold, 16, 28, 29, 0xFFFFFF, "*");
    gfx_text(c, &font_ui_bold, 54, 22, UI_TEXT, "Claude");
    gfx_text(c, &font_ui, 54, 40, UI_MUTED, MODEL_NAME " - Anthropic API");
    ui_button(c, WW - 248, 10, 110, 30, "New chat", BTN_NORMAL, hover == 1, busy);
    ui_button(c, WW - 128, 10, 110, 30, key[0] ? "API key" : "Add API key", key[0] ? BTN_NORMAL : BTN_PRIMARY, hover == 2, busy);

    int top = TOP, bot = WH - bottom_h();
    gfx_clip(c, 0, top, WW, bot - top);
    if (!nmsgs) {
        int y = top + 60;
        gfx_text_center(c, &font_title, 0, WW, y, UI_TEXT, key[0] ? "Ask Claude anything" : "Chat with Claude on DuncOS");
        const char *l1 = key[0] ? "Type below and press Enter. Shift+Enter starts a new line." : "This app talks to Claude through Anthropic's API, which needs an API key:";
        gfx_text_center(c, &font_ui, 0, WW, y + 34, UI_MUTED, l1);
        if (!key[0]) {
            static const char *t[] = { "1. On another device, go to console.anthropic.com and sign in.",
                                       "2. Create an API key (Settings > API keys). Anthropic bills it by use;",
                                       "   a Claude.ai subscription doesn't cover it.",
                                       "3. Press \"Add API key\" above and paste or type the key." };
            for (int i = 0; i < 4; i++) gfx_text(c, &font_ui, WW / 2 - 230, y + 72 + i * 22, UI_TEXT, t[i]);
        }
    }
    /* messages */
    nlines = 0;
    int y = top + 12 - scroll_px;
    int bw = WW - 48;
    for (int i = 0; i < nmsgs; i++) {
        struct msg *m = &msgs[i];
        int inner = m->assistant ? bw - 28 : (bw * 7 / 10) - 28;
        int first = nlines;
        int h = wrap(m->disp, m->dlen, inner, 0, true) + 18;
        if (m->assistant && !m->dlen) h = 38;
        int bx = m->assistant ? 24 : WW - 24 - inner - 28, bwid = inner + 28;
        if (!m->assistant) { int mw = 0; for (int k = first; k < nlines; k++) { int w = lines[k].kind == 2 ? lines[k].n * 8 : width_of(lines[k].s, lines[k].n, lines[k].kind ? &font_ui_bold : &font_ui); if (w > mw) mw = w; } bwid = mw + 28; bx = WW - 24 - bwid; }
        if (y + h > top && y < bot) {
            gfx_rrect(c, bx, y, bwid, h, 12, m->assistant ? UI_PANEL : 0x1E4A6E, 255);
            for (int k = first; k < nlines; k++) {
                struct line *l = &lines[k];
                if (l->kind == 2) gfx_fill(c, bx + 8, y + 9 + l->y, bwid - 16, 16, 0x0A1020);
                draw_text_line(c, l, bx + 14, y + 9 + l->y + 15, UI_TEXT);
            }
            if (m->assistant && !m->dlen) gfx_text(c, &font_ui, bx + 14, y + 24, UI_MUTED, busy && i == nmsgs - 1 ? "Thinking..." : "(no reply)");
        }
        y += h + 10;
        if (m->note) { gfx_text(c, &font_ui, 30, y + 2, UI_MUTED, "Part of this reply came from a fallback model."); y += 22; }
    }
    content_h = y + scroll_px - top;
    gfx_clip(c, 0, 0, WW, WH);
    /* input */
    int iy = WH - bottom_h();
    gfx_fill(c, 0, iy, WW, WH - iy, 0x0C1426);
    if (status[0]) gfx_text(c, &font_ui, 24, iy + 20, status_col, status);
    int il = inp_lines(), ih = il * 20 + 16;
    gfx_rrect(c, 20, iy + 28, WW - 128, ih, 10, HL_TERM_BG, 255);
    gfx_rrect_outline(c, 20, iy + 28, WW - 128, ih, 10, busy ? UI_BORDER : UI_ACCENT, 200);
    if (!ilen) gfx_text(c, &font_ui, 34, iy + 28 + 22, UI_MUTED, busy ? "Claude is replying..." : key[0] ? "Message Claude..." : "Add an API key first");
    nlines = 0;
    int total = wrap(input, ilen, WW - 160, 0, true);
    int skip = total > 6 * 20 ? total - 6 * 20 : 0, last_y = 0, last_x = 34;
    for (int k = 0; k < nlines; k++) {
        if (lines[k].y < skip) continue;
        struct line l = lines[k]; if (l.kind == 1) l.kind = 0;
        draw_text_line(c, &l, 34, iy + 28 + 8 + l.y - skip + 15, UI_TEXT);
        last_y = l.y - skip; last_x = 34 + (l.kind == 2 ? l.n * 8 : width_of(l.s, l.n, &font_ui));
    }
    if (!busy && !dialog) gfx_fill(c, last_x + 1, iy + 28 + 10 + last_y, 2, 17, UI_ACCENT);
    ui_button(c, WW - 98, iy + 28, 78, 34, "Send", BTN_PRIMARY, hover == 3, busy || !ilen || !key[0]);

    if (dialog) {
        gfx_blend(c, 0, 0, WW, WH, 0x000000, 110);
        int dw = 520, dh = 250, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        gfx_rrect(c, dx, dy, dw, dh, 14, UI_PANEL, 255);
        gfx_rrect_outline(c, dx, dy, dw, dh, 14, UI_ACCENT, 140);
        gfx_text(c, &font_title, dx + 22, dy + 40, UI_TEXT, "Anthropic API key");
        gfx_text(c, &font_ui, dx + 22, dy + 68, UI_MUTED, "Create one at console.anthropic.com. It starts with sk-ant-.");
        gfx_text(c, &font_ui, dx + 22, dy + 88, UI_MUTED, "It's saved on this computer in " KEYFILE ".");
        gfx_rrect(c, dx + 22, dy + 104, dw - 44, 36, 8, HL_TERM_BG, 255);
        gfx_rrect_outline(c, dx + 22, dy + 104, dw - 44, 36, 8, UI_ACCENT, 180);
        char shown[64]; int n = 0;
        for (int i = 0; i < klen && n < 48; i++) shown[n++] = i < 10 ? kbuf[i] : '*';
        shown[n] = 0;
        gfx_mono_text(c, dx + 34, dy + 114, klen ? shown : "Paste with Ctrl+V, or type it", klen ? HL_TEXT : HL_TEXT_FAINT);
        gfx_text(c, &font_ui, dx + 22, dy + 166, UI_MUTED, "Anyone using this computer can use the key. Remove it to stop that.");
        ui_button(c, dx + dw - 120, dy + dh - 52, 98, 34, "Save", BTN_PRIMARY, hover == 10, false);
        ui_button(c, dx + dw - 228, dy + dh - 52, 98, 34, "Cancel", BTN_NORMAL, hover == 11, false);
        if (key[0]) ui_button(c, dx + 22, dy + dh - 52, 120, 34, "Remove key", BTN_DANGER, hover == 12, false);
    }
    app_flush(&a);
}

static void scroll_to_bottom(void) {
    int view = WH - bottom_h() - TOP - 12;
    scroll_px = content_h > view ? content_h - view : 0;
}

/* ---------------- one request (streaming) ---------------- */
#define MAXB 32
struct blk { char type[24]; char *t; long tl, tc; char *sig; long sl, sc; char *raw; long rl; };
static struct blk blks[MAXB]; static int nblk;
static char stop_reason[32], errmsg[300];
static char *sse; static long sse_len, sse_cap;           /* undecoded stream text */
static char *errbody; static long errlen, errcap;
static int http_status;
static bool aborted, got_fallback;
static uint64_t last_draw;

static void app_append(char **p, long *len, long *cap, const char *s, long n) {
    if (*len + n + 1 > *cap) { *cap = (*len + n + 1) * 2; *p = realloc(*p, *cap); }
    memcpy(*p + *len, s, n); *len += n; (*p)[*len] = 0;
}

static void update_display(void) {
    struct msg *m = &msgs[nmsgs - 1];
    long n = 0;
    for (int i = 0; i < nblk; i++) if (!strcmp(blks[i].type, "text")) n += blks[i].tl + 2;
    char *d = malloc(n + 1); long k = 0;
    for (int i = 0; i < nblk; i++) if (!strcmp(blks[i].type, "text")) {
        if (k) d[k++] = '\n';
        memcpy(d + k, blks[i].t, blks[i].tl); k += blks[i].tl;
    }
    k = utf8_to_cp1252(d, k);
    free(m->disp); m->disp = d; m->dlen = k;
}

static char *dec;   static long dec_cap;                  /* scratch for decoded strings */
static long decode(const char *v, const char *e) {
    long need = e - v + 1;
    if (need > dec_cap) { dec_cap = need * 2; dec = realloc(dec, dec_cap); }
    return json_str(v, e, dec, dec_cap);
}

static void on_event(const char *js, const char *e) {
    const char *type = json_get(js, e, "type");
    if (!type) return;
    if (json_is(type, e, "content_block_start")) {
        long idx = json_int(json_get(js, e, "index"), e);
        const char *cb = json_get(js, e, "content_block");
        if (idx < 0 || idx >= MAXB || !cb) return;
        while (nblk <= idx) memset(&blks[nblk++], 0, sizeof blks[0]);
        struct blk *b = &blks[idx];
        const char *bt = json_get(cb, e, "type");
        if (bt) { long n = json_str(bt, e, b->type, sizeof b->type); if (n < 0) b->type[0] = 0; }
        const char *cend = json_skip(cb, e);
        if (!cend) return;
        b->raw = dupn(cb, cend - cb); b->rl = cend - cb;           /* kept verbatim for block types we don't build */
        if (!strcmp(b->type, "fallback")) got_fallback = true;
        const char *init = json_get(cb, e, !strcmp(b->type, "thinking") ? "thinking" : "text");
        if (init && (!strcmp(b->type, "text") || !strcmp(b->type, "thinking"))) { long n = decode(init, e); if (n > 0) app_append(&b->t, &b->tl, &b->tc, dec, n); }
    } else if (json_is(type, e, "content_block_delta")) {
        long idx = json_int(json_get(js, e, "index"), e);
        if (idx < 0 || idx >= nblk) return;
        struct blk *b = &blks[idx];
        const char *dt = json_get(js, e, "delta.type");
        if (!dt) return;
        const char *field = json_is(dt, e, "text_delta") ? "delta.text" : json_is(dt, e, "thinking_delta") ? "delta.thinking" : json_is(dt, e, "signature_delta") ? "delta.signature" : 0;
        if (!field) return;
        const char *v = json_get(js, e, field);
        long n = v ? decode(v, e) : -1;
        if (n < 0) return;
        if (!strcmp(field, "delta.signature")) app_append(&b->sig, &b->sl, &b->sc, dec, n);
        else app_append(&b->t, &b->tl, &b->tc, dec, n);
        if (!strcmp(field, "delta.text")) update_display();
    } else if (json_is(type, e, "message_delta")) {
        const char *sr = json_get(js, e, "delta.stop_reason");
        if (sr) json_str(sr, e, stop_reason, sizeof stop_reason);
    } else if (json_is(type, e, "error")) {
        const char *mv = json_get(js, e, "error.message");
        if (mv) json_str(mv, e, errmsg, sizeof errmsg); else strncpy0(errmsg, "The API reported an error.", sizeof errmsg);
    }
}

static bool sink(const char *data, long n, void *ctx) {
    if (http_status != 200) { if (errlen < 65536) app_append(&errbody, &errlen, &errcap, data, n); }
    else {
        app_append(&sse, &sse_len, &sse_cap, data, n);
        long start = 0;
        for (long i = 0; i + 1 < sse_len; i++) {                 /* events end with a blank line */
            if (sse[i] != '\n' || sse[i + 1] != '\n') continue;
            for (long p = start; p < i;) {
                long q = p; while (q < i && sse[q] != '\n') q++;
                if (q - p > 5 && !strncmp(sse + p, "data:", 5)) on_event(sse + p + 5, sse + q);
                p = q + 1;
            }
            start = i + 2; i++;
        }
        if (start) { memmove(sse, sse + start, sse_len - start); sse_len -= start; }
    }
    uint64_t now = sys_time();
    if (now - last_draw > 60) {
        last_draw = now;
        strncpy0(status, msgs[nmsgs - 1].dlen ? "Claude is writing... (Esc stops)" : "Claude is thinking... (Esc stops)", sizeof status); status_col = UI_MUTED;
        if (stick_bottom) { draw(); scroll_to_bottom(); }
        draw();
        struct dunc_event ev;                                    /* Esc or closing the window stops the reply */
        while (sys_event(&ev, 0) > 0) {
            if (ev.type == EV_CLOSE) sys_exit(0);
            if (ev.type == EV_KEY && ev.key == 27) aborted = true;
            if (ev.type == EV_WHEEL) { scroll_px += ev.key * 40; stick_bottom = ev.key > 0 && scroll_px >= content_h - (WH - bottom_h() - TOP - 12); if (scroll_px < 0) scroll_px = 0; }
            if (ev.type == EV_RESIZE && app_resize(&a, &ev)) { WW = a.wi.w; WH = a.wi.h; }
        }
    }
    return !aborted;
}

/* the assistant turn as sent back next time: blocks unchanged, in order; before a mid-reply fallback block,
 * only text blocks are echoed (thinking and other model-internal blocks there are left out, as the API asks) */
static char *assistant_json(void) {
    int last_fb = -1;
    for (int i = 0; i < nblk; i++) if (!strcmp(blks[i].type, "fallback")) last_fb = i;
    long cap = 64; for (int i = 0; i < nblk; i++) cap += blks[i].tl * 6 + blks[i].sl * 6 + blks[i].rl + 64;
    char *o = malloc(cap); long k = 0;
    o[k++] = '[';
    int count = 0;
    for (int i = 0; i < nblk; i++) {
        struct blk *b = &blks[i];
        bool text = !strcmp(b->type, "text"), think = !strcmp(b->type, "thinking");
        if (i < last_fb && !text) continue;
        if (!b->type[0]) continue;
        if (count++) o[k++] = ',';
        if (text) {
            k += ksnprintf(o + k, cap - k, "{\"type\":\"text\",\"text\":\"");
            k += json_escape(b->t ? b->t : "", b->tl, o + k, cap - k);
            k += ksnprintf(o + k, cap - k, "\"}");
        } else if (think) {
            k += ksnprintf(o + k, cap - k, "{\"type\":\"thinking\",\"thinking\":\"");
            k += json_escape(b->t ? b->t : "", b->tl, o + k, cap - k);
            k += ksnprintf(o + k, cap - k, "\",\"signature\":\"");
            k += json_escape(b->sig ? b->sig : "", b->sl, o + k, cap - k);
            k += ksnprintf(o + k, cap - k, "\"}");
        } else { memcpy(o + k, b->raw, b->rl); k += b->rl; }
    }
    o[k++] = ']'; o[k] = 0;
    return o;
}

static void free_blocks(void) {
    for (int i = 0; i < nblk; i++) { free(blks[i].t); free(blks[i].sig); free(blks[i].raw); }
    memset(blks, 0, sizeof blks); nblk = 0;
}

static void send_message(void) {
    if (!ilen || busy || !key[0]) return;
    /* the user's message, as UTF-8 */
    char *u = malloc(ilen * 3 + 1); long ul = cp1252_to_utf8(input, ilen, u, ilen * 3 + 1);
    char *ue = malloc(ul * 6 + 3); ue[0] = '"'; long el = 1 + json_escape(u, ul, ue + 1, ul * 6 + 1); ue[el++] = '"'; ue[el] = 0;
    free(u);
    push_msg(0, input, ilen, ue);
    char saved[sizeof input]; memcpy(saved, input, ilen); int saved_len = ilen;
    ilen = 0; input[0] = 0;
    push_msg(1, "", 0, 0);
    stick_bottom = true;
    /* the request body */
    long cap = 1024 + strlen(SYSTEM) * 2;
    for (int i = 0; i < nmsgs - 1; i++) cap += strlen(msgs[i].json) + 48;
    char *body = malloc(cap); long k = 0;
    k += ksnprintf(body + k, cap - k, "{\"model\":\"" MODEL "\",\"max_tokens\":64000,\"stream\":true,\"fallbacks\":\"default\","
                                    "\"output_config\":{\"effort\":\"medium\"},\"system\":\"");
    k += json_escape(SYSTEM, strlen(SYSTEM), body + k, cap - k);
    k += ksnprintf(body + k, cap - k, "\",\"messages\":[");
    for (int i = 0; i < nmsgs - 1; i++)
        k += ksnprintf(body + k, cap - k, "%s{\"role\":\"%s\",\"content\":%s}", i ? "," : "", msgs[i].assistant ? "assistant" : "user", msgs[i].json);
    k += ksnprintf(body + k, cap - k, "]}");
    char hdrs[400];
    ksnprintf(hdrs, sizeof hdrs, "x-api-key: %s\r\nanthropic-version: 2026-10-21\r\nanthropic-beta: server-side-fallback-2026-07-01\r\n", key);
    struct http_req rq = { "POST", body, k, "application/json", 0, hdrs, 180000 };
    busy = true; aborted = false; got_fallback = false; stop_reason[0] = 0; errmsg[0] = 0; http_status = 0;
    sse_len = 0; errlen = 0; free_blocks();
    strncpy0(status, "Connecting...", sizeof status); status_col = UI_MUTED;
    draw(); scroll_to_bottom(); draw();
    long r = http_request(url, &rq, sink, 0, &http_status, 0, 0, 0, 0, 0);
    free(body);
    busy = false;
    /* how did it go? */
    const char *problem = 0; static char pbuf[360];
    if (aborted) problem = "Stopped.";
    else if (r < 0) { ksnprintf(pbuf, sizeof pbuf, "Couldn't reach Claude: %s.", http_strerror(r)); problem = pbuf; }
    else if (http_status != 200) {
        char m[300] = "";
        const char *mv = errbody ? json_get(errbody, errbody + errlen, "error.message") : 0;
        if (mv) json_str(mv, errbody + errlen, m, sizeof m);
        utf8_to_cp1252(m, strlen(m) + 1);
        if (http_status == 401) problem = "The API key wasn't accepted. Check it with \"API key\".";
        else if (http_status == 429) problem = "Too many requests right now (rate limit). Wait a little and try again.";
        else if (http_status == 529 || http_status >= 500) problem = "Claude is busy or unavailable at the moment. Try again shortly.";
        else { ksnprintf(pbuf, sizeof pbuf, "The API answered %d: %s", http_status, m[0] ? m : "error"); problem = pbuf; }
    }
    else if (errmsg[0]) { ksnprintf(pbuf, sizeof pbuf, "The reply stopped: %s", errmsg); problem = pbuf; }
    else if (!strcmp(stop_reason, "refusal")) problem = "Claude declined to answer this. Try asking differently.";
    else if (!stop_reason[0]) problem = "The reply was cut off (the connection closed early).";
    if (problem) {                                       /* leave the history as it was: drop this exchange */
        pop_msg(); pop_msg();
        if (!strcmp(stop_reason, "refusal") || aborted || r < 0 || http_status != 200 || errmsg[0] || !stop_reason[0]) {
            memcpy(input, saved, saved_len); ilen = saved_len; input[ilen] = 0;    /* give the text back to edit or resend */
        }
        strncpy0(status, problem, sizeof status); status_col = aborted ? UI_MUTED : UI_RED;
    } else {
        struct msg *m = &msgs[nmsgs - 1];
        update_display();
        m->json = assistant_json();
        m->note = got_fallback;
        if (!strcmp(stop_reason, "max_tokens")) { strncpy0(status, "The reply reached its length limit.", sizeof status); status_col = UI_GOLD; }
        else status[0] = 0;
    }
    free_blocks();
    draw(); if (stick_bottom) scroll_to_bottom(); draw();
}

/* ---------------- key storage ---------------- */
static void load_key(void) {
    long n = fs_read(KEYFILE, key, 0, sizeof key - 1);
    key[n > 0 ? n : 0] = 0;
    for (int i = 0; key[i]; i++) if (key[i] == '\n' || key[i] == '\r' || key[i] == ' ') { key[i] = 0; break; }
    char u[256]; n = fs_read(URLFILE, u, 0, sizeof u - 1);
    if (n > 8) { u[n] = 0; for (int i = 0; u[i]; i++) if (u[i] == '\n' || u[i] == '\r') { u[i] = 0; break; } strncpy0(url, u, sizeof url); }
}

static int hit(int x, int y) {
    if (dialog) {
        int dw = 520, dh = 250, dx = (WW - dw) / 2, dy = (WH - dh) / 2;
        if (ui_in(x, y, dx + dw - 120, dy + dh - 52, 98, 34)) return 10;
        if (ui_in(x, y, dx + dw - 228, dy + dh - 52, 98, 34)) return 11;
        if (key[0] && ui_in(x, y, dx + 22, dy + dh - 52, 120, 34)) return 12;
        return -1;
    }
    if (ui_in(x, y, WW - 248, 10, 110, 30)) return 1;
    if (ui_in(x, y, WW - 128, 10, 110, 30)) return 2;
    int iy = WH - bottom_h();
    if (ui_in(x, y, WW - 98, iy + 28, 78, 34)) return 3;
    return -1;
}

static void save_key(void) {
    int s = 0, e = klen;
    while (s < e && kbuf[s] == ' ') s++;
    while (e > s && kbuf[e - 1] == ' ') e--;
    if (e - s < 20 || strncmp(kbuf + s, "sk-ant-", 7)) { strncpy0(status, "That doesn't look like an Anthropic API key (sk-ant-...).", sizeof status); status_col = UI_RED; return; }
    memcpy(key, kbuf + s, e - s); key[e - s] = 0;
    fs_mkdir("/system/config");
    long r = fs_write(KEYFILE, key, strlen(key));
    strncpy0(status, r < 0 ? "The key is set, but couldn't be saved." : "API key saved.", sizeof status); status_col = r < 0 ? UI_GOLD : UI_GREEN;
    dialog = 0;
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "Claude", WIN_RESIZABLE)) return 1;
    load_key();
    if (!key[0]) { dialog = 0; }
    draw();
    for (;;) {
        struct dunc_event e;
        sys_event(&e, -1);
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); if (stick_bottom) scroll_to_bottom(); draw(); } continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) { int h = e.type == EV_MOUSE_MOVE ? hit(e.x, e.y) : -1; if (h != hover) { hover = h; draw(); } continue; }
        if (e.type == EV_WHEEL && !dialog) {
            scroll_px += e.key * 40;
            int view = WH - bottom_h() - TOP - 12, mx = content_h > view ? content_h - view : 0;
            if (scroll_px > mx) scroll_px = mx;
            if (scroll_px < 0) scroll_px = 0;
            stick_bottom = scroll_px >= mx;
            draw(); continue;
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            int h = hit(e.x, e.y);
            if (h == 1) { while (nmsgs) pop_msg(); status[0] = 0; scroll_px = 0; }
            else if (h == 2) { dialog = 1; klen = 0; kbuf[0] = 0; }
            else if (h == 3) send_message();
            else if (h == 10) save_key();
            else if (h == 11) dialog = 0;
            else if (h == 12) { key[0] = 0; fs_delete(KEYFILE); dialog = 0; strncpy0(status, "API key removed.", sizeof status); status_col = UI_MUTED; }
            draw(); continue;
        }
        if (e.type != EV_KEY) continue;
        int k = e.key;
        if (dialog) {
            if (k == 27) dialog = 0;
            else if (k == '\n') save_key();
            else if (k == '\b') { if (klen) kbuf[--klen] = 0; }
            else if (k == 22) clip_paste_line(kbuf, &klen, sizeof kbuf);
            else if (key_is_char(k) && klen < (int)sizeof kbuf - 1) { kbuf[klen++] = (char)k; kbuf[klen] = 0; }
            draw(); continue;
        }
        if (k == '\n') send_message();
        else if (k == (KEYMOD_SHIFT | '\n')) { if (ilen < (int)sizeof input - 1) { input[ilen++] = '\n'; input[ilen] = 0; } }
        else if (k == '\b') { if (ilen) input[--ilen] = 0; }
        else if (k == 22) {                                   /* paste, several lines allowed */
            long n = clip_get(0, 0);
            if (n > 0) {
                char *b = malloc(n + 1); n = clip_get(b, n); n = utf8_to_cp1252(b, n);
                for (long i = 0; i < n && ilen < (int)sizeof input - 1; i++) if (b[i] != '\r') input[ilen++] = b[i];
                input[ilen] = 0; free(b);
            }
        }
        else if (KEY_CODE(k) == KEY_PGUP || KEY_CODE(k) == KEY_PGDN) {
            scroll_px += KEY_CODE(k) == KEY_PGUP ? -300 : 300; if (scroll_px < 0) scroll_px = 0; stick_bottom = false;
        }
        else if (key_is_char(k) && ilen < (int)sizeof input - 1) { input[ilen++] = (char)k; input[ilen] = 0; }
        draw();
    }
    return 0;
}
