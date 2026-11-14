/* Host test for the Web browser's engine (user/apps/web/{html,css,layout}.c): parses pages, applies style sheets
 * and lays them out, checks a few results, then feeds it mangled input to make sure nothing crashes or hangs.
 * Build: see the Makefile's test target. Usage: test_web file.html... */
#include <stdio.h>
#include <stdlib.h>
#include "../user/apps/web/web.h"

long sys_call(long n, long a, long b, long c, long d, long e) { return -1; }
struct link *links; int nlinks, links_cap;
int add_link(const char *href, struct node *n) { return -1; }
struct imgent *img_get(const char *u) { return 0; }
void form_init_control(struct doc *d, struct node *n) {}
char *web_fetch_text(const char *url, long *len, long cap) { return 0; }
void resolve_url(const char *base, const char *href, char *out, int n) { strncpy0(out, href, n); }

static struct layout lay;
static int run(char *s, long n, int width, bool quiet, const char *name) {
    struct doc *d = calloc(1, sizeof *d);
    n = utf8_lenient_to_cp1252(s, n);
    html_parse(d, s, n);
    css_reset(&d->ar);
    /* style elements, in order */
    struct node *stack[300]; int sp = 0; stack[sp++] = d->root;
    while (sp) {
        struct node *e = stack[--sp];
        if (e->type == N_ELEM && e->tag == T_STYLE && e->text) css_add_sheet(e->text, e->tlen, "", 0);
        struct node *kids[4096]; int k = 0;
        for (struct node *c = e->first; c && k < 4096; c = c->next) kids[k++] = c;
        while (k && sp < 299) stack[sp++] = kids[--k];
    }
    css_cascade(d, width);
    layout_doc(d, &lay, width);
    int texts = 0;
    for (int i = 0; i < lay.nit; i++) if (lay.it[i].kind == DI_TEXT || lay.it[i].kind == DI_MONO) texts++;
    if (!quiet) printf("%-28s nodes %6d  items %6d (text %5d)  height %6d  title \"%s\"\n", name, d->nnodes, lay.nit, texts, lay.doc_h, d->title);
    int h = lay.doc_h;
    ar_reset(&d->ar); free(d);
    return h;
}

/* find the text item containing word; returns its index or -1 */
static int find_text(const char *word) {
    int wl = strlen(word);
    for (int i = 0; i < lay.nit; i++) {
        struct ditem *it = &lay.it[i];
        if (it->kind != DI_TEXT && it->kind != DI_MONO) continue;
        for (uint32_t k = 0; k + wl <= it->len; k++) if (!memcmp(lay.tp + it->off + k, word, wl)) return i;
    }
    return -1;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void unit_checks(void) {
    uint32_t c;
    CHECK(css_parse_color("#f00", 4, &c) && c == 0xFFFF0000, "#f00");
    CHECK(css_parse_color("rgb(0, 128, 255)", 16, &c) && c == 0xFF0080FF, "rgb()");
    CHECK(css_parse_color("rgba(0,0,0,0.2)", 15, &c) && c == 0, "rgba transparent");
    CHECK(css_parse_color("rebeccapurple", 13, &c) && c == 0xFF663399, "named");
    CHECK(css_parse_color("hsl(120, 100%, 25%)", 19, &c) && c == 0xFF008000, "hsl %08x", c);
    CHECK(!css_parse_color("nonsense", 8, &c), "bad colour");
    char e[4]; CHECK(html_entity("&eacute;", 8, e) == 8 && (unsigned char)e[0] == 0xE9, "&eacute;");
    CHECK(html_entity("&#8364;", 7, e) == 7 && (unsigned char)e[0] == 0x80, "euro");
    CHECK(html_entity("&#x263A;", 8, e) == 8 && e[0] == '?', "unmappable");
    char u[] = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"; long n = utf8_lenient_to_cp1252(u, strlen(u));
    CHECK(n == 8 && !memcmp(u, "caf\xE9 \x80 ?", 8), "utf8 %ld", n);
    CHECK(css_media_matches("screen and (max-width: 600px)", 29, 500) && !css_media_matches("screen and (max-width: 600px)", 29, 800), "media width");
    CHECK(!css_media_matches("print", 5, 800) && css_media_matches("all", 3, 800), "media print");

    /* cascade and layout */
    char page[] = "<style>p{color:red} .b{color:#00f} #c{color:green} div p{margin-left:100px} div > p.b{font-weight:bold}"
                  "@media (max-width:500px){div p{margin-left:0}} span{display:none} .w{width:300px;margin:0 auto}</style>"
                  "<div><p class=b id=c>Alpha</p><p class=b>Beta <span>Hidden</span></p><p style=\"color:#123456 !important\" class=b>Gamma</p></div>"
                  "<div class=w><p>Centred</p></div><table><tr><td>One</td><td>Two</td></tr></table><ul><li>Item</li></ul>";
    struct doc *d = calloc(1, sizeof *d);
    html_parse(d, page, strlen(page));
    css_reset(&d->ar);
    for (struct node *e = d->head->first; e; e = e->next) if (e->tag == T_STYLE) css_add_sheet(e->text, e->tlen, "", 0);
    css_cascade(d, 800);
    layout_doc(d, &lay, 800);
    int a = find_text("Alpha"), b = find_text("Beta"), g = find_text("Gamma"), cn = find_text("Centred"), one = find_text("One"), two = find_text("Two");
    CHECK(a >= 0 && lay.it[a].col == 0x008000, "id beats class");
    CHECK(b >= 0 && lay.it[b].col == 0x0000FF, "class beats type");
    CHECK(g >= 0 && lay.it[g].col == 0x123456, "inline style");
    CHECK(find_text("Hidden") < 0, "display none");
    CHECK(a >= 0 && lay.it[a].x >= 100, "descendant margin %d", a >= 0 ? lay.it[a].x : -1);
    CHECK(cn >= 0 && lay.it[cn].x >= 250, "auto margins centre %d", cn >= 0 ? lay.it[cn].x : -1);
    CHECK(one >= 0 && two >= 0 && lay.it[one].y == lay.it[two].y && lay.it[two].x > lay.it[one].x, "table row");
    int marks = 0; for (int i = 0; i < lay.nit; i++) if (lay.it[i].kind == DI_MARK) marks++;
    CHECK(marks == 1, "list bullet");
    css_cascade(d, 400);
    layout_doc(d, &lay, 400);
    a = find_text("Alpha");
    CHECK(a >= 0 && lay.it[a].x < 100, "media query on narrow window");
    ar_reset(&d->ar); free(d);
}

static unsigned long rng = 12345;
static unsigned rnd(void) { rng = rng * 6364136223846793005ul + 1442695040888963407ul; return (unsigned)(rng >> 33); }

int main(int argc, char **argv) {
    css_init();
    unit_checks();
    static char buf[4 << 20];
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("can't open %s\n", argv[i]); return 1; }
        long n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        const char *nm = argv[i]; for (const char *q = argv[i]; *q; q++) if (*q == '/') nm = q + 1;
        static char copy[4 << 20];
        memcpy(copy, buf, n);
        if (run(copy, n, 860, false, nm) <= 0) { printf("FAIL: %s has no height\n", nm); fails++; }
        /* mangled copies: cut, shuffled bytes, inserted markup */
        static const char *junk[] = { "<", ">", "</div>", "<table>", "<td>", "<li>", "{", "}", "<style>", "&#", "\"", "<!--", "<p", "/*", "@media", ":", ";",
                                      "<b><i></b></i>", "<tr><td colspan=999>", "<select><option>", "<pre>", "\xC3", "<img src=x width=99999 height=0>" };
        for (int round = 0; round < 60; round++) {
            long m = 0;
            for (long k = 0; k < n && m < (long)sizeof copy - 64; k++) {
                unsigned r = rnd() % 1000;
                if (r < 3) { const char *j = junk[rnd() % (sizeof junk / sizeof junk[0])]; long l = strlen(j); memcpy(copy + m, j, l); m += l; }
                else if (r < 5) continue;
                else if (r < 6) copy[m++] = (char)rnd();
                else copy[m++] = buf[k];
            }
            if (round % 10 == 9) m = rnd() % (m + 1);
            run(copy, m, 200 + rnd() % 1200, true, nm);
        }
    }
    /* deep nesting and huge inputs */
    static char deep[3 << 20]; long m = 0;
    for (int k = 0; k < 20000; k++) m += sprintf(deep + m, k % 3 ? "<div style='padding:1px'>" : k % 2 ? "<table><tr><td>" : "<span>");
    m += sprintf(deep + m, "deep");
    run(deep, m, 860, false, "(20000 nested elements)");
    m = 0;
    static const char *kinds[] = { "<div style='display:flex'>", "<table><tr><td>", "<ul><li>", "<span style='display:inline-block'>", "<b>",
                                   "<div style='display:inline-flex'>", "<a href=x>", "<blockquote>", "<img style='display:block' width=5>" };
    for (int k = 0; k < 3000; k++) m += sprintf(deep + m, "%s", kinds[k % 9]);
    m += sprintf(deep + m, "deep");
    run(deep, m, 860, false, "(3000 mixed nested elements)");
    m = 0;
    for (int k = 0; k < 2000; k++) m += sprintf(deep + m, "<style>.c%d .d%d > p + a ~ b{margin:%dpx}</style><p class='c%d d%d'>x</p>", k, k, k, k, k);
    run(deep, m, 860, false, "(2000 style blocks)");
    printf(fails ? "test_web: %d FAILED\n" : "test_web: all passed\n", fails);
    return fails != 0;
}
