/* Terminal: a command line in a window. Built-in commands manage apps and
 * processes; everything else is passed to the Thane kernel's command shell. */
#include "../lib/dunc.h"

static int WW = 780, WH = 470;
#define PAD 12
#define MAXCOLS 240
#define COLS ((WW - 2 * PAD) / 8 > MAXCOLS ? MAXCOLS : (WW - 2 * PAD) / 8)
#define ROWS ((WH - 2 * PAD) / 16)
#define LINES 400

static struct app a;
static char text[LINES][MAXCOLS + 1];
static uint8_t color[LINES][MAXCOLS];
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
static char cwd[256] = "/home";
static void short_cwd(char *out, int n) {
    if (!strncmp(cwd, "/home", 5) && (cwd[5] == 0 || cwd[5] == '/')) ksnprintf(out, n, "~%s", cwd + 5);
    else strncpy0(out, cwd, n);
}
static void prompt(void) {
    char s[64]; short_cwd(s, sizeof s);
    int o = cur_col; cur_col = C_GREEN; print("%s > ", s); cur_col = o;
    prompt_len = cx;
}

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

static void human(uint64_t b, char *out, int n) {
    if (b < 1024) ksnprintf(out, n, "%lu B", b);
    else if (b < 1024 * 1024) ksnprintf(out, n, "%lu KB", b / 1024);
    else ksnprintf(out, n, "%lu.%lu MB", b >> 20, ((b >> 10) & 1023) * 10 / 1024);
}

static void cmd_ls(const char *arg) {
    char path[256]; path_resolve(cwd, *arg ? arg : ".", path, sizeof path);
    static struct dunc_dirent d[256];
    int n = fs_list(path, d, 256);
    if (n < 0) { puts_c(fs_strerror(n), C_RED); putch('\n'); return; }
    if (!n) { puts_c("(empty)\n", C_DIM); return; }
    for (int i = 0; i < n; i++) {
        if (d[i].is_dir) { puts_c("  ", C_TEXT); puts_c(d[i].name, C_CYAN); puts_c("/\n", C_CYAN); }
        else { char s[24]; human(d[i].size, s, sizeof s); print("  %-40s ", d[i].name); puts_c(s, C_DIM); putch('\n'); }
    }
}

static void cmd_cat(const char *arg) {
    char path[256]; path_resolve(cwd, arg, path, sizeof path);
    static char buf[16384];
    long n = fs_read(path, buf, 0, sizeof buf - 1);
    if (n < 0) { puts_c(fs_strerror(n), C_RED); putch('\n'); return; }
    buf[n] = 0;
    for (long i = 0; i < n; i++) if (buf[i] == '\n' || (buf[i] >= 32 && buf[i] < 127) || buf[i] == '\t') putch(buf[i]); else putch('.');
    if (n == (long)sizeof buf - 1) puts_c("\n... (truncated; open it with 'edit')", C_DIM);
    if (cx) putch('\n');
}

static void report(int r) { if (r < 0) { puts_c(fs_strerror(r), C_RED); putch('\n'); } }

static bool app_installed(const char *id) {
    struct dunc_app a[24]; int n = sys_apps(a, 24);
    for (int i = 0; i < n; i++) if (!strcmp(a[i].id, id)) return true;
    return false;
}

static const char *strchr_(const char *s, char c) { for (; *s; s++) if (*s == c) return s; return 0; }

static void cmd_help(void) {
    puts_c("Terminal commands\n", C_GOLD);
    static const char *h[][2] = {
        {"ls [folder]", "list files"}, {"cd <folder>", "change folder (~ is your home)"}, {"cat <file>", "show a file"},
        {"mkdir <name>", "make a folder"}, {"rm <path>", "delete a file or folder"}, {"mv <a> <b>", "move or rename"},
        {"cp <a> <b>", "copy"}, {"touch <file>", "create an empty file"}, {"echo hi > f", "write text to a file"},
        {"edit <file>", "open a file in Editor"}, {"df", "drives and free space"},
        {"apps", "list installed apps"}, {"open <app>", "start an app, e.g. 'open calc'"},
        {"ps", "list running processes"}, {"kill <pid>", "stop a process"},
        {"date", "show the date and time"}, {"fetch", "system summary"}, {"clear", "clear the screen"}, {"exit", "close this terminal"} };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) { puts_c("  ", C_TEXT); print("%-14s", h[i][0]); puts_c(h[i][1], C_DIM); putch('\n'); }
    puts_c("Kernel commands\n", C_GOLD);
    static const char *k[][2] = {
        {"sysinfo", "CPU, memory, display"}, {"pci", "hardware on the PCI bus"}, {"usb", "USB devices"},
        {"bt", "Bluetooth adapter"}, {"bt scan [secs]", "find nearby Bluetooth devices"}, {"log", "kernel messages"},
        {"uptime", "time since boot"}, {"clan", "package manager (clan list / clan install)"},
        {"reboot / poweroff", "restart or shut down"} };
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
    else if (!strcmp(line, "apps")) {
        struct dunc_app ap[24]; int n = sys_apps(ap, 24);
        for (int i = 0; i < n; i++) { print("  %-10s ", ap[i].id); puts_c(ap[i].name, C_DIM); if (ap[i].installed) puts_c("  (clan)", C_GOLD); putch('\n'); }
    }
    else if (!strcmp(line, "open") || !strcmp(line, "run") || !strcmp(line, "start")) {
        char app[32], rest[200] = "", full[256] = "";
        int k = 0; while (arg[k] && arg[k] != ' ' && k < 31) { app[k] = arg[k]; k++; } app[k] = 0;
        if (arg[k] == ' ') { strncpy0(rest, arg + k + 1, sizeof rest); path_resolve(cwd, rest, full, sizeof full); }
        int pid = sys_spawn_arg(app, full);
        if (pid < 0) { puts_c("No such app. Type 'apps' for the list.\n", C_RED); }
        else print("Started %s (pid %d)\n", app, pid);
    }
    else if (!strcmp(line, "pwd")) print("%s\n", cwd);
    else if (!strcmp(line, "cd")) {
        char path[256]; path_resolve(cwd, *arg ? arg : "~", path, sizeof path);
        struct dunc_stat st;
        int r = fs_stat(path, &st);
        if (r < 0) report(r); else if (!st.is_dir) report(FS_ENOTDIR); else strncpy0(cwd, path, sizeof cwd);
    }
    else if (!strcmp(line, "ls") || !strcmp(line, "dir")) cmd_ls(arg);
    else if (!strcmp(line, "cat")) cmd_cat(arg);
    else if (!strcmp(line, "mkdir")) { char p[256]; path_resolve(cwd, arg, p, sizeof p); report(fs_mkdir(p)); }
    else if (!strcmp(line, "rm") || !strcmp(line, "del")) { char p[256]; path_resolve(cwd, arg, p, sizeof p); report(fs_delete(p)); }
    else if (!strcmp(line, "touch")) {
        char p[256]; path_resolve(cwd, arg, p, sizeof p);
        struct dunc_stat st;
        if (fs_stat(p, &st) < 0) report((int)fs_write(p, "", 0));
    }
    else if (!strcmp(line, "mv") || !strcmp(line, "cp")) {
        char a1[128], *b1 = arg; int k = 0;
        while (*b1 && *b1 != ' ' && k < 127) a1[k++] = *b1++;
        a1[k] = 0; while (*b1 == ' ') b1++;
        if (!*a1 || !*b1) { puts_c("usage: mv|cp <from> <to>\n", C_RED); }
        else {
            char pa[256], pb[256]; path_resolve(cwd, a1, pa, sizeof pa); path_resolve(cwd, b1, pb, sizeof pb);
            struct dunc_stat st;
            if (fs_stat(pb, &st) == 0 && st.is_dir) {      /* into a folder */
                const char *base = pa; for (const char *s = pa; *s; s++) if (*s == '/') base = s + 1;
                char t[256]; ksnprintf(t, sizeof t, "%s/%s", pb, base); strncpy0(pb, t, sizeof pb);
            }
            report(!strcmp(line, "mv") ? fs_rename(pa, pb) : fs_copy(pa, pb));
        }
    }
    else if (!strcmp(line, "echo") && strchr_(arg, '>')) {
        char text[200]; int k = 0;
        const char *s = arg;
        while (*s && *s != '>' && k < 199) text[k++] = *s++;
        while (k && text[k - 1] == ' ') k--;
        text[k++] = '\n'; text[k] = 0;
        s++; while (*s == ' ') s++;
        char p[256]; path_resolve(cwd, s, p, sizeof p);
        report((int)fs_write(p, text, k));
    }
    else if (!strcmp(line, "edit")) {
        if (!app_installed("editor")) puts_c("Editor isn't installed yet. Run: clan install editor\n", C_GOLD);
        else {
            char p[256] = ""; if (*arg) path_resolve(cwd, arg, p, sizeof p);
            if (sys_spawn_arg("editor", p) < 0) puts_c("Couldn't start Editor.\n", C_RED);
        }
    }
    else if (!strcmp(line, "df")) {
        struct dunc_volume v[8]; int n = fs_volumes(v, 8);
        puts_c("  DRIVE    NAME               TYPE      SIZE        FREE\n", C_GOLD);
        for (int i = 0; i < n; i++) {
            char t[24], f[24]; human(v[i].total, t, sizeof t); human(v[i].free, f, sizeof f);
            print("  %-8s %-18s %-9s %-11s %s\n", v[i].path, v[i].label, v[i].kind, t, f);
        }
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
    if (!app_open_ex(&a, WW, WH, "castle - ~", WIN_RESIZABLE)) return 1;
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
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }
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
