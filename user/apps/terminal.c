/* Terminal: a command line in a window. Built-in commands manage apps and
 * processes; everything else is passed to the Thane kernel's command shell. */
#include "../lib/dunc.h"

#define WW 780
#define WH 470
#define PAD 12
#define COLS ((WW - 2 * PAD) / 8)
#define ROWS ((WH - 2 * PAD) / 16)
#define LINES 400

static struct app a;
static char text[LINES][COLS + 1];
static uint8_t color[LINES][COLS];
static int nlines = 1, cx;
static int fetch_line = -100;   /* line where the pixel crown is drawn */              /* cursor column on the last line */
static char input[128]; static int ilen;
static char hist[16][128]; static int nhist, hpos;
static bool cursor_on = true;
static int cur_col = 0;
static const uint32_t pal[] = { HL_TEXT, HL_ACCENT, HL_GOLD, HL_GREEN, HL_RED, HL_TEXT_FAINT, HL_TEXT_DIM };
enum { C_TEXT, C_CYAN, C_GOLD, C_GREEN, C_RED, C_DIM, C_WHITE };

static void newline(void) {
    if (nlines == LINES) {
        memmove(text[0], text[1], sizeof text[0] * (LINES - 1));
        memmove(color[0], color[1], sizeof color[0] * (LINES - 1));
        nlines--;
        fetch_line--;
    }
    memset(text[nlines], 0, sizeof text[0]);
    nlines++; cx = 0;
}

static void putch(char ch) {
    if (ch == '\n') { newline(); return; }
    if (ch == '\r') return;
    if (ch == '\t') { do putch(' '); while (cx % 4); return; }
    if (cx >= COLS) newline();
    text[nlines - 1][cx] = ch; color[nlines - 1][cx] = cur_col; cx++;
}
static void puts_c(const char *s, int col) { int o = cur_col; cur_col = col; while (*s) putch(*s++); cur_col = o; }
static void print(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); kvsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    puts_c(buf, cur_col);
}

static int prompt_len;
static const char *CROWN[12] = {   /* from desktop_highland.c */
    "................", ".......##.......", ".#.....##.....#.", ".##...####...##.",
    ".###..#..#..###.", ".#.##.#..#.##.#.", ".#..###..###..#.", ".#...#....#...#.",
    ".#............#.", ".##############.", ".##############.", "................" };

static void fetch(void) {
    struct sysinfo s; sys_sysinfo(&s);
    char v[6][48];
    ksnprintf(v[0], 48, "DuncOS 0.1");
    ksnprintf(v[1], 48, "%s", s.kernel);
    ksnprintf(v[2], 48, "castle");
    ksnprintf(v[3], 48, "%lu min", s.uptime_ms / 60000);
    ksnprintf(v[4], 48, "%lu / %lu MiB", s.mem_used >> 20, s.mem_total >> 20);
    ksnprintf(v[5], 48, "font8x16");
    static const char *k[] = { "OS", "Kernel", "Shell", "Uptime", "Memory", "Font" };
    if (cx) putch('\n');
    fetch_line = nlines - 1;
    puts_c("           ", C_TEXT); puts_c("bryan", C_CYAN); puts_c("@", C_TEXT); puts_c("duncos\n", C_CYAN);
    puts_c("           -------------\n", C_DIM);
    for (int i = 0; i < 6; i++) { puts_c("           ", C_TEXT); print("%s", ""); cur_col = C_GOLD; print("%-9s", k[i]); cur_col = C_TEXT; print("%s\n", v[i]); }
    putch('\n');
}
static void prompt(void) { puts_c("~ > ", C_GREEN); prompt_len = 4; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, HL_TERM_BG);
    int first = nlines > ROWS ? nlines - ROWS : 0;
    for (int r = 0; r < ROWS && first + r < nlines; r++) {
        int ln = first + r;
        for (int k = 0; k < COLS && text[ln][k]; k++)
            gfx_mono(c, PAD + k * 8, PAD + r * 16, text[ln][k], pal[color[ln][k]]);
    }
    if (fetch_line >= first && fetch_line < first + ROWS)   /* pixel crown beside the fetch info */
        for (int r = 0; r < 12; r++)
            for (int k = 0; k < 16; k++)
                if (CROWN[r][k] == '#') gfx_fill(c, PAD + 2 + k * 4, PAD + (fetch_line - first) * 16 + 2 + r * 4, 4, 4, HL_GOLD);
    if (cursor_on) {
        int r = (nlines - 1) - first;
        gfx_fill(c, PAD + cx * 8, PAD + r * 16, 8, 16, HL_TEXT);
    }
    app_flush(&a);
}

/* redraw the input line after an edit */
static void show_input(void) {
    char *ln = text[nlines - 1];
    memset(ln + prompt_len, 0, COLS - prompt_len);
    cx = prompt_len;
    int o = cur_col; cur_col = C_WHITE;
    for (int i = 0; i < ilen && cx < COLS; i++) putch(input[i]);
    cur_col = o;
}

static const char *apps[][2] = {
    {"terminal", "Terminal"}, {"monitor", "System Monitor"}, {"bluetooth", "Bluetooth"},
    {"calc", "Calculator"}, {"sandbox", "Protection Test"}, {"about", "About DuncOS"} };

static void cmd_help(void) {
    puts_c("Terminal commands\n", C_GOLD);
    static const char *h[][2] = {
        {"apps", "list installed apps"}, {"open <app>", "start an app, e.g. 'open calc'"},
        {"ps", "list running processes"}, {"kill <pid>", "stop a process"},
        {"date", "show the date and time"}, {"fetch", "system summary"}, {"clear", "clear the screen"}, {"exit", "close this terminal"} };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) { puts_c("  ", C_TEXT); print("%-14s", h[i][0]); puts_c(h[i][1], C_DIM); putch('\n'); }
    puts_c("Kernel commands\n", C_GOLD);
    static const char *k[][2] = {
        {"sysinfo", "CPU, memory, display"}, {"pci", "hardware on the PCI bus"}, {"usb", "USB devices"},
        {"bt", "Bluetooth adapter"}, {"bt scan [secs]", "find nearby Bluetooth devices"}, {"log", "kernel messages"},
        {"uptime", "time since boot"}, {"reboot / poweroff", "restart or shut down"} };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) { puts_c("  ", C_TEXT); print("%-18s", k[i][0]); puts_c(k[i][1], C_DIM); putch('\n'); }
}

static void cmd_ps(void) {
    struct procinfo p[32];
    int n = sys_procs(p, 32);
    static const char *st[] = { "", "ready", "running", "sleeping", "waiting", "exiting" };
    puts_c("  PID  NAME                TYPE     STATE     CPU TIME   MEMORY\n", C_GOLD);
    for (int i = 0; i < n; i++)
        print("  %-4d %-19s %-8s %-9s %6lu ms  %5lu KiB\n", p[i].pid, p[i].name, p[i].user ? "app" : "kernel",
              p[i].state <= 5 ? st[p[i].state] : "?", p[i].cpu_us / 1000, p[i].mem_kb);
}

static void run(char *line) {
    while (*line == ' ') line++;
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    if (!*line) return;
    if (!strcmp(line, "help")) cmd_help();
    else if (!strcmp(line, "fetch") || !strcmp(line, "neofetch")) fetch();
    else if (!strcmp(line, "clear") || !strcmp(line, "cls")) { nlines = 1; memset(text, 0, sizeof text); cx = 0; fetch_line = -100; return; }
    else if (!strcmp(line, "exit")) sys_exit(0);
    else if (!strcmp(line, "ps")) cmd_ps();
    else if (!strcmp(line, "apps")) { for (int i = 0; i < 6; i++) { print("  %-10s ", apps[i][0]); puts_c(apps[i][1], C_DIM); putch('\n'); } }
    else if (!strcmp(line, "open") || !strcmp(line, "run") || !strcmp(line, "start")) {
        int pid = sys_spawn(arg);
        if (pid < 0) { puts_c("No such app. Type 'apps' for the list.\n", C_RED); }
        else print("Started %s (pid %d)\n", arg, pid);
    }
    else if (!strcmp(line, "kill")) {
        if (sys_kill((int)katoi(arg)) < 0) puts_c("No such app process (kernel threads can't be killed).\n", C_RED);
        else print("Stopped process %ld\n", katoi(arg));
    }
    else if (!strcmp(line, "date") || !strcmp(line, "time")) {
        struct rtc_time t; sys_rtc(&t);
        print("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.mon, t.day, t.hour, t.min, t.sec);
    }
    else {
        char full[160];
        ksnprintf(full, sizeof full, "%s%s%s", line, *arg ? " " : "", arg);
        if (!strncmp(full, "bt scan", 7)) { puts_c("Scanning for Bluetooth devices...\n", C_DIM); draw(); }
        static char out[16384];
        sys_kcmd(full, out, sizeof out);
        for (char *p = out; *p; p++) {
            if (!strncmp(p, "[ ok ]", 6)) { puts_c("[ ok ]", C_GREEN); p += 5; continue; }
            if (!strncmp(p, "[warn]", 6)) { puts_c("[warn]", C_GOLD); p += 5; continue; }
            putch(*p);
        }
    }
}

int main(void) {
    if (!app_open(&a, WW, WH, "castle - ~")) return 1;
    fetch();
    puts_c("Type 'help' to see the available commands.\n", C_DIM);
    prompt();
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, 530);
        if (r == 0) {   /* blink: repaint only the cursor cell */
            cursor_on = !cursor_on;
            int first = nlines > ROWS ? nlines - ROWS : 0, row = (nlines - 1) - first;
            int px = PAD + cx * 8, py = PAD + row * 16;
            gfx_fill(&a.c, px, py, 8, 16, HL_TERM_BG);
            if (cursor_on) gfx_fill(&a.c, px, py, 8, 16, HL_TEXT);
            else if (text[nlines - 1][cx]) gfx_mono(&a.c, px, py, text[nlines - 1][cx], pal[color[nlines - 1][cx]]);
            app_flush_rect(&a, px, py, 8, 16);
            continue;
        }
        if (e.type == EV_CLOSE) break;
        if (e.type != EV_KEY) continue;
        int k = e.key;
        cursor_on = true;
        if (k == '\n') {
            input[ilen] = 0;
            putch('\n');
            if (ilen) { strncpy0(hist[nhist % 16], input, 128); nhist++; }
            hpos = nhist;
            run(input);
            ilen = 0;
            if (cx) putch('\n');
            prompt();
        } else if (k == '\b') { if (ilen) ilen--; show_input(); }
        else if (k == KEY_UP || k == KEY_DOWN) {
            int np = k == KEY_UP ? hpos - 1 : hpos + 1;
            if (np >= 0 && np <= nhist && np >= nhist - 16) {
                hpos = np;
                if (np == nhist) ilen = 0;
                else { strncpy0(input, hist[np % 16], 128); ilen = strlen(input); }
                show_input();
            }
        } else if (k == 12) { nlines = 1; memset(text, 0, sizeof text); cx = 0; fetch_line = -100; prompt(); }   /* Ctrl+L */
        else if (k >= 32 && k < 127 && ilen < (int)sizeof input - 1 && prompt_len + ilen < COLS - 1) { input[ilen++] = k; show_input(); }
        draw();
    }
    return 0;
}
