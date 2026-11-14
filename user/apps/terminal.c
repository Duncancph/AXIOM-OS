/* Terminal: a command line in a window. Built-in commands manage apps and
 * processes; everything else is passed to the Thane kernel's command shell. */
#include "../lib/dunc.h"
#include "../lib/logo_draw.h"

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
static int view;                                   /* lines scrolled back from the bottom */
static int sel_l0 = -1, sel_c0, sel_l1, sel_c1;    /* selection (line, column), -1: none */
static bool dragging;
static uint32_t pal[] = { HL_TEXT, HL_CREAM, HL_GOLD, HL_GREEN, HL_RED, HL_TEXT_FAINT, HL_TEXT_DIM };
enum { C_TEXT, C_CYAN, C_GOLD, C_GREEN, C_RED, C_DIM, C_WHITE };

static void newline(void) {
    if (nlines == LINES) {
        if (sel_l0 >= 0) { sel_l0--; sel_l1--; if (sel_l0 < 0 || sel_l1 < 0) sel_l0 = -1; }
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

static void fetch(void) {
    struct sysinfo s; sys_sysinfo(&s);
    char v[6][48];
    ksnprintf(v[0], 48, "AXIOM %s, build %ld", strncmp(s.kernel, "Thane ", 6) ? s.kernel : s.kernel + 6, sys_call(SYS_UPDATE, UPDATE_BUILD, 0, 0, 0, 0));
    ksnprintf(v[1], 48, "%s", s.kernel);
    ksnprintf(v[2], 48, "castle");
    ksnprintf(v[3], 48, "%lu min", s.uptime_ms / 60000);
    ksnprintf(v[4], 48, "%lu / %lu MiB", s.mem_used >> 20, s.mem_total >> 20);
    ksnprintf(v[5], 48, "font8x16");
    static const char *k[] = { "OS", "Kernel", "Shell", "Uptime", "Memory", "Font" };
    if (cx) putch('\n');
    fetch_line = nlines - 1;
    puts_c("            ", C_TEXT); { char un[40] = ""; sys_setting_get("user.name", un, sizeof un); puts_c(un[0] ? un : "bryan", C_CYAN); } puts_c("@", C_TEXT); puts_c("axiom\n", C_CYAN);
    puts_c("            -------------\n", C_DIM);
    for (int i = 0; i < 6; i++) { puts_c("            ", C_TEXT); print("%s", ""); cur_col = C_GOLD; print("%-9s", k[i]); cur_col = C_TEXT; print("%s\n", v[i]); }
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
    if (view > first) view = first;
    first -= view;
    int a0l = sel_l0, a0c = sel_c0, a1l = sel_l1, a1c = sel_c1;           /* selection in reading order */
    if (a0l > a1l || (a0l == a1l && a0c > a1c)) { a0l = sel_l1; a0c = sel_c1; a1l = sel_l0; a1c = sel_c0; }
    for (int r = 0; r < ROWS && first + r < nlines; r++) {
        int ln = first + r;
        if (sel_l0 >= 0 && ln >= a0l && ln <= a1l) {
            int c0 = ln == a0l ? a0c : 0, c1 = ln == a1l ? a1c : COLS;
            if (c1 > c0) gfx_fill(c, PAD + c0 * 8, PAD + r * 16, (c1 - c0) * 8, 16, 0x34332E);
        }
        for (int k = 0; k < COLS && text[ln][k]; k++)
            gfx_mono(c, PAD + k * 8, PAD + r * 16, text[ln][k], pal[color[ln][k]]);
    }
    if (fetch_line >= first - 5 && fetch_line < first + ROWS)   /* the AXIOM mark beside the fetch info */
        draw_logo(c, PAD + 2, PAD + (fetch_line - first) * 16 + 14, &logo_mark_term, HL_CREAM);
    if (view) {                                         /* "scrolled back" hint */
        const char *h = "Scrolled back - press any key or scroll down";
        int w = (int)strlen(h) * 8 + 16;
        gfx_rrect(c, WW - w - 10, 8, w, 22, 6, 0x232321, 235);
        gfx_mono_text(c, WW - w - 2, 11, h, HL_GOLD);
    }
    if (cursor_on && !view) {
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
    n = utf8_to_cp1252(buf, n);
    for (long i = 0; i < n; i++) { unsigned char c = buf[i]; if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) putch(c); else putch('.'); }
    if (n == (long)sizeof buf - 1) puts_c("\n... (truncated; open it with 'edit')", C_DIM);
    if (cx) putch('\n');
}

static const char *strchr_(const char *s, char c) { for (; *s; s++) if (*s == c) return s; return 0; }

static const char *v6_state_name(int st) {
    return st == V6_TENTATIVE ? "  (checking...)" : st == V6_DEPRECATED ? "  (being replaced)" : st == V6_DUPLICATE ? "  (in use by another device)" : "";
}

static void cmd_ip(void) {
    struct dunc_netinfo n; sys_netinfo(&n);
    if (!n.present) { puts_c("No supported network adapter. AXIOM supports Intel and Realtek wired Ethernet.\n", C_GOLD); return; }
    char a[IP_STRLEN], m[16], g[16];
    bool any = n.configured || n.v6_configured;
    print("  %s (%s)  ", n.adapter, n.driver);
    puts_c(n.link ? (any ? "connected\n" : "cable connected, getting an address...\n") : "no cable\n", any ? C_GREEN : C_GOLD);
    print("  MAC      %02x:%02x:%02x:%02x:%02x:%02x\n", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    if (n.configured) {
        ip_str(n.ip, a); ip_str(n.mask, m); ip_str(n.gw, g);
        if (n.lease_s == 0xFFFFFFFFu) print("  IPv4     %s / %s, gateway %s, permanent lease\n", a, m, g);
        else print("  IPv4     %s / %s, gateway %s, lease %u min\n", a, m, g, n.lease_s / 60);
    } else if (n.link) puts_c("  IPv4     no address (no DHCP server answered yet)\n", C_DIM);
    for (int i = 0; i < n.v6_count; i++) {
        ip_format(&n.v6[i], a);
        print("  IPv6     %s/%u%s%s\n", a, n.v6[i].prefix, i == 0 ? "  (link-local)" : "", v6_state_name(n.v6_state[i]));
    }
    if (n.router6.v6) { ip_format(&n.router6, a); print("  Router   %s, MTU %u\n", a, n.mtu6); }
    else if (n.link && n.v6_count) puts_c("  IPv6     no router on this network: local IPv6 only\n", C_DIM);
    if (n.dns_now.v6 || n.dns_now.b[0]) { ip_format(&n.dns_now, a); print("  DNS      %s%s\n", a, n.dns_manual ? " (set by you)" : ""); }
    print("  Traffic  %lu packets in (%lu KB), %lu out (%lu KB)\n", n.rx_packets, n.rx_bytes / 1024, n.tx_packets, n.tx_bytes / 1024);
}

/* "-4" / "-6" choose the IP version; returns the rest of the arguments */
static const char *family_opt(const char *arg, int *fam) {
    *fam = 0;
    while (arg[0] == '-' && (arg[1] == '4' || arg[1] == '6') && (arg[2] == ' ' || !arg[2])) {
        *fam = arg[1] - '0'; arg += 2; while (*arg == ' ') arg++;
    }
    return arg;
}

/* resolve and pick an address of the wanted family (0: the first, which prefers IPv6 when it works) */
static int resolve_one(const char *name, int fam, struct ipaddr *out) {
    struct ipaddr a[8];
    int r = sys_resolve(name, a, 8);
    if (r < 0) return r;
    for (int i = 0; i < r; i++) if (!fam || (fam == 6) == (a[i].v6 != 0)) { *out = a[i]; return 0; }
    return NET_ENOTFOUND;
}

static void cmd_ping(const char *arg) {
    int fam; arg = family_opt(arg, &fam);
    if (!*arg) { puts_c("usage: ping [-4|-6] <host>\n", C_RED); return; }
    struct ipaddr ip; int r = resolve_one(arg, fam, &ip);
    if (r) { puts_c(r == NET_ENOTFOUND && fam ? (fam == 6 ? "That name has no IPv6 address" : "That name has no IPv4 address") : net_strerror(r), C_RED); putch('\n'); return; }
    char a[IP_STRLEN]; ip_format(&ip, a);
    print("Pinging %s (%s):\n", arg, a);
    draw();
    int ok = 0; long best = -1, worst = 0, sum = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t t0 = sys_time();
        long us = sys_ping(&ip, 2000);
        if (us >= 0) {
            ok++; sum += us; if (best < 0 || us < best) best = us; if (us > worst) worst = us;
            print("  Reply from %s: time=%ld.%ld ms\n", a, us / 1000, (us / 100) % 10);
        } else if (us == NET_ETIMEDOUT) puts_c("  No reply (timed out)\n", C_GOLD);
        else { print("  "); puts_c(net_strerror((int)us), C_RED); putch('\n'); break; }
        draw();
        uint64_t el = sys_time() - t0;
        if (i < 3 && el < 1000) sys_sleep(1000 - el);
    }
    print("%d of 4 replies", ok);
    if (ok) print(", time min %ld.%ld / avg %ld.%ld / max %ld.%ld ms", best / 1000, best / 100 % 10, sum / ok / 1000, sum / ok / 100 % 10, worst / 1000, worst / 100 % 10);
    putch('\n');
}

static void cmd_host(const char *arg) {
    if (!*arg) { puts_c("usage: host <name>\n", C_RED); return; }
    struct ipaddr a[8];
    int r = sys_resolve(arg, a, 8);
    if (r <= 0) { print("%s: ", arg); puts_c(net_strerror(r ? r : NET_ENOTFOUND), C_RED); putch('\n'); return; }
    for (int i = 0; i < r; i++) { char b[IP_STRLEN]; ip_format(&a[i], b); print("%s has %s address %s\n", arg, a[i].v6 ? "IPv6" : "IPv4", b); }
}

struct dl { char path[256]; long got, total, next_report; int err; bool first; };
static bool dl_sink(const char *d, long n, void *ctx) {
    struct dl *x = ctx;
    long r = x->first ? fs_write(x->path, d, n) : fs_append(x->path, d, n);
    x->first = false;
    if (r < 0) { x->err = (int)r; return false; }
    x->got += n;
    if (x->got >= x->next_report) {
        x->next_report = x->got + 512 * 1024;
        if (x->total > 0) print("  %ld KB of %ld KB (%ld%%)\n", x->got / 1024, x->total / 1024, x->got * 100 / x->total);
        else print("  %ld KB\n", x->got / 1024);
        draw();
    }
    return true;
}

static void cmd_get(const char *arg, bool save) {
    char url[300], file[200] = "";
    int k = 0; while (*arg && *arg != ' ' && k < 299) url[k++] = *arg++; url[k] = 0;
    while (*arg == ' ') arg++;
    if (!url[0]) { puts_c(save ? "usage: wget <url> [file]\n" : "usage: curl <url>\n", C_RED); return; }
    int status = 0; char ct[64];
    print("Connecting to %s ...\n", url); draw();
    if (!save) {
        static char body[16384];
        long n = http_get(url, body, sizeof body, &status, ct, sizeof ct, 0, 0);
        if (n < 0) { puts_c(http_strerror(n), C_RED); putch('\n'); return; }
        n = utf8_to_cp1252(body, n);
        for (long i = 0; i < n; i++) { unsigned char c = body[i]; if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) putch(c); }
        if (cx) putch('\n');
        print("-- HTTP %d, %ld bytes%s\n", status, n, n == (long)sizeof body ? " (cut short; use wget to save it)" : "");
        if (http_sec.addr[0]) print("-- server address %s\n", http_sec.addr);
        if (http_sec.secure) { puts_c("-- secure: ", C_GREEN); print("%s; certificate for %s, from %s\n", http_sec.cipher, http_sec.subject, http_sec.issuer); }
        return;
    }
    if (*arg) strncpy0(file, arg, sizeof file);
    else {   /* name the file after the last part of the URL */
        const char *s = url; for (const char *p = url; *p; p++) if (*p == '/') s = p + 1;
        if (!*s || strchr_(url + 7, '/') == 0) strncpy0(file, "index.html", sizeof file);
        else { int j = 0; while (*s && *s != '?' && j < 60) file[j++] = *s++; file[j] = 0; }
    }
    static struct dl x;
    memset(&x, 0, sizeof x);
    path_resolve(cwd, file, x.path, sizeof x.path);
    x.first = true; x.next_report = 512 * 1024;
    uint64_t t0 = sys_time();
    long n = http_fetch(url, dl_sink, &x, &status, &x.total, ct, sizeof ct, 0, 0);
    if (n < 0) { puts_c(http_strerror(n), C_RED); putch('\n'); return; }
    if (x.err) { puts_c(fs_strerror(x.err), C_RED); putch('\n'); return; }
    if (x.first) fs_write(x.path, "", 0);
    uint64_t ms = sys_time() - t0; if (!ms) ms = 1;
    print("HTTP %d - saved %ld bytes to %s (%lu KB/s)%s\n", status, x.got, file, (uint64_t)x.got * 1000 / 1024 / ms, http_sec.secure ? " - secure" : "");
    if (x.total > 0 && x.got < x.total) puts_c("Warning: the download stopped early.\n", C_GOLD);
}

static void report(int r) { if (r < 0) { puts_c(fs_strerror(r), C_RED); putch('\n'); } }

static bool app_installed(const char *id) {
    struct dunc_app a[24]; int n = sys_apps(a, 24);
    for (int i = 0; i < n; i++) if (!strcmp(a[i].id, id)) return true;
    return false;
}

static void cmd_help(void) {
    puts_c("Terminal commands\n", C_GOLD);
    static const char *h[][2] = {
        {"ls [folder]", "list files"}, {"cd <folder>", "change folder (~ is your home)"}, {"cat <file>", "show a file"},
        {"mkdir <name>", "make a folder"}, {"rm <path>", "delete a file or folder"}, {"mv <a> <b>", "move or rename"},
        {"cp <a> <b>", "copy"}, {"touch <file>", "create an empty file"}, {"echo hi > f", "write text to a file"},
        {"edit <file>", "open a file in Editor"}, {"df", "drives and free space"},
        {"ip", "network status (IPv4 and IPv6)"}, {"ping [-4|-6] <host>", "test a connection"}, {"host <name>", "look up an address"},
        {"wget <url> [f]", "download (http:// or https://)"}, {"trust [file]", "list or add trusted authorities"}, {"curl <url>", "show a web page"}, {"dns <ip|auto>", "choose a DNS server"},
        {"apps", "list installed apps"}, {"open <app>", "start an app, e.g. 'open calc'"},
        {"ps", "list running processes"}, {"kill <pid>", "stop a process"},
        {"date", "show the date and time"}, {"fetch", "system summary"}, {"clear", "clear the screen"}, {"exit", "close this terminal"} };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) { puts_c("  ", C_TEXT); print("%-20s", h[i][0]); puts_c(h[i][1], C_DIM); putch('\n'); }
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
        if (arg[k] == ' ') {
            strncpy0(rest, arg + k + 1, sizeof rest);
            bool is_url = false; for (const char *s = rest; *s; s++) if (s[0] == ':' && s[1] == '/' && s[2] == '/') is_url = true;
            if (is_url) strncpy0(full, rest, sizeof full); else path_resolve(cwd, rest, full, sizeof full);
        }
        char fp[256]; struct dunc_stat st;
        path_resolve(cwd, app, fp, sizeof fp);
        if (!full[0] && fs_stat(fp, &st) == 0 && !st.is_dir) {      /* "open photo.png": the file's own app */
            const char *why = open_file(fp);
            if (why) { puts_c(why, C_RED); putch('\n'); } else print("Opened %s\n", app);
            return;
        }
        int pid = sys_spawn_arg(app, full);
        if (pid < 0) { puts_c("No such app or file. Type 'apps' for the list.\n", C_RED); }
        else print("Started %s (pid %d)\n", app, pid);
    }
    else if (!strcmp(line, "pwd")) print("%s\n", cwd);
    else if (!strcmp(line, "ip") || !strcmp(line, "ifconfig")) cmd_ip();
    else if (!strcmp(line, "ping")) cmd_ping(arg);
    else if (!strcmp(line, "ping6")) { char a6[200]; ksnprintf(a6, sizeof a6, "-6 %s", arg); cmd_ping(a6); }
    else if (!strcmp(line, "host") || !strcmp(line, "nslookup")) cmd_host(arg);
    else if (!strcmp(line, "wget")) cmd_get(arg, true);
    else if (!strcmp(line, "curl")) cmd_get(arg, false);
    else if (!strcmp(line, "trust")) {
        if (!*arg) {
            print("%d trusted certificate authorities. Ones you added:\n", http_trusted_roots());
            static struct dunc_dirent d[32]; int n = fs_list("/system/certs/user", d, 32);
            if (n <= 0) puts_c("  (none)\n", C_DIM);
            for (int i = 0; i < n; i++) print("  %s\n", d[i].name);
            puts_c("To add one: trust <file.pem>\n", C_DIM);
        } else {
            char p[256], to[256]; path_resolve(cwd, arg, p, sizeof p);
            static char pem[65536];
            long n = fs_read(p, pem, 0, sizeof pem - 1);
            if (n < 0) { puts_c(fs_strerror((int)n), C_RED); putch('\n'); }
            else {
                pem[n] = 0;
                bool ok = false; for (long i = 0; i + 27 < n; i++) if (!strncmp(pem + i, "-----BEGIN CERTIFICATE-----", 27)) ok = true;
                const char *b = p; for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
                ksnprintf(to, sizeof to, "/system/certs/user/%s", b);
                if (!ok) puts_c("That file doesn't contain a PEM certificate.\n", C_RED);
                else if (fs_write(to, pem, n) < 0) puts_c("Couldn't save it.\n", C_RED);
                else {
                    int k = http_trust_pem(pem, n);
                    print("Added %s (%d certificate%s) as a trusted authority until restart.\n", b, k, k == 1 ? "" : "s");
                    puts_c("Only add authorities you know and trust. Apps already open pick it up when reopened.\n", C_GOLD);
                }
            }
        }
    }
    else if (!strcmp(line, "settings")) {
        static const char *keys[] = { "keyboard.layout", "time.zone", "time.rtc_local", "time.24h", "display.mode", "display.current",
                                      "sound.volume", "lock.password", "lock.idle_min", "screen.off_min" };
        for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) { char v[112]; sys_setting_get(keys[i], v, sizeof v); print("  %-16s %s\n", keys[i], v); }
    }
    else if (!strcmp(line, "set")) {
        char key[32]; int k = 0; const char *v = arg;
        while (*v && *v != ' ' && k < 31) key[k++] = *v++;
        key[k] = 0; while (*v == ' ') v++;
        if (!k) puts_c("usage: set <setting> <value>   (see 'settings')\n", C_RED);
        else if (sys_setting_set(key, v)) puts_c("No such setting.\n", C_RED);
        else print("%s = %s\n", key, !strcmp(key, "lock.password") ? (*v ? "(set)" : "(none)") : v);
    }
    else if (!strcmp(line, "disks")) {
        struct dunc_disk d[8]; int n = sys_disks(d, 8);
        if (n <= 0) puts_c("No disks found.\n", C_DIM);
        for (int i = 0; i < n; i++) {
            bool gb = d[i].bytes >= (10ull << 30);
            print("  %d  %-5s %-28s %6lu %s%s%s\n", d[i].id, d[i].kind == DISK_SATA ? "SATA" : d[i].kind == DISK_NVME ? "NVMe" : "USB", d[i].model,
                  gb ? d[i].bytes >> 30 : d[i].bytes >> 20, gb ? "GB" : "MB", d[i].duncos ? "  AXIOM" : "", d[i].system ? " (your files)" : "");
            for (int k = 0; k < d[i].nparts; k++) print("        partition %d: %lu MB %s %s\n", k + 1, d[i].part[k].bytes >> 20, d[i].part[k].fs, d[i].part[k].label);
        }
    }
    else if (!strcmp(line, "install")) {
        int id = (int)katoi(arg); const char *m = arg; while (*m && *m != ' ') m++; while (*m == ' ') m++;
        int mode = !strcmp(m, "update") ? INSTALL_UPDATE : !strcmp(m, "erase") ? INSTALL_ERASE : 0;
        if (!id || !mode) { puts_c("usage: install <disk number> erase|update   (see 'disks'; erase wipes the whole disk)\n", C_RED); }
        else {
            int r = sys_install(id, mode);
            if (r) { puts_c(fs_strerror(r), C_RED); putch('\n'); }
            else {
                struct dunc_install st; int last = -1;
                do {
                    sys_sleep(250); sys_install_status(&st);
                    if (st.percent != last) { print("  %3d%%  %s\n", st.percent, st.msg); last = st.percent; draw(); }
                } while (st.state == 1);
                puts_c(st.msg, st.state == 2 ? C_GREEN : C_RED); putch('\n');
            }
        }
    }
    else if (!strcmp(line, "dns")) {
        struct ipaddr ip;
        if (!strcmp(arg, "auto")) { sys_set_dns(0); puts_c("DNS server: from the network\n", C_TEXT); }
        else if (ip_parse(arg, &ip)) { sys_set_dns(&ip); print("DNS server set to %s\n", arg); }
        else puts_c("usage: dns <IPv4 or IPv6 address> | dns auto\n", C_RED);
    }
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

static void cell_at(int x, int y, int *l, int *c) {
    int first = (nlines > ROWS ? nlines - ROWS : 0) - view;
    if (first < 0) first = 0;
    int r = (y - PAD) / 16; if (y < PAD) r = 0;
    *l = first + r; if (*l >= nlines) *l = nlines - 1;
    *c = (x - PAD + 4) / 8; if (*c < 0) *c = 0; if (*c > COLS) *c = COLS;
}

static void copy_sel(void) {
    if (sel_l0 < 0) return;
    int a0l = sel_l0, a0c = sel_c0, a1l = sel_l1, a1c = sel_c1;
    if (a0l > a1l || (a0l == a1l && a0c > a1c)) { a0l = sel_l1; a0c = sel_c1; a1l = sel_l0; a1c = sel_c0; }
    static char out[LINES * (MAXCOLS + 1)]; int n = 0;
    for (int l = a0l; l <= a1l; l++) {
        int c0 = l == a0l ? a0c : 0, c1 = l == a1l ? a1c : COLS, ll = (int)strlen(text[l]);
        if (c1 > ll) c1 = ll;
        int st = n;
        for (int k = c0; k < c1; k++) out[n++] = text[l][k];
        while (n > st && out[n - 1] == ' ') n--;            /* no trailing spaces */
        if (l < a1l) out[n++] = '\n';
    }
    static char u[sizeof out * 3];
    long ul = cp1252_to_utf8(out, n, u, sizeof u);
    clip_set(u, ul);
}

static void paste(void) {
    static char b[4096];
    long n = clip_get(b, sizeof b);
    if (n <= 0) return;
    n = utf8_to_cp1252(b, n);
    for (long i = 0; i < n && b[i] != '\n' && b[i] != '\r'; i++) {
        int k = (unsigned char)b[i];
        if (k == '\t') k = ' ';
        if (key_is_char(k) && ilen < (int)sizeof input - 1 && prompt_len + ilen < COLS - 1) input[ilen++] = (char)k;
    }
    show_input();
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "castle - ~", WIN_RESIZABLE)) return 1;
    pal[1] = HL_ACCENT;                                  /* the accent colour from Settings > Personalise */
    fetch();
    puts_c("Type 'help' to see the available commands.\n", C_DIM);
    prompt();
    draw();
    for (;;) {
        struct dunc_event e;
        int r = sys_event(&e, 530);
        if (r == 0) {   /* blink: repaint only the cursor cell */
            if (view) continue;                 /* scrolled back: the cursor isn't on screen */
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
        if (e.type == EV_WHEEL) { view -= e.key * 3; if (view < 0) view = 0; draw(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) { cell_at(e.x, e.y, &sel_l0, &sel_c0); sel_l1 = sel_l0; sel_c1 = sel_c0; dragging = true; draw(); continue; }
        if (e.type == EV_MOUSE_MOVE && dragging) { cell_at(e.x, e.y, &sel_l1, &sel_c1); draw(); continue; }
        if (e.type == EV_MOUSE_UP) { dragging = false; if (sel_l0 == sel_l1 && sel_c0 == sel_c1) sel_l0 = -1; draw(); continue; }
        if (e.type == EV_MOUSE_DOWN && e.button == 2) { paste(); view = 0; draw(); continue; }   /* right click pastes */
        if (e.type != EV_KEY) continue;
        int k = e.key;
        cursor_on = true;
        if (KEY_CODE(k) == KEY_PGUP || KEY_CODE(k) == KEY_PGDN) {
            view += KEY_CODE(k) == KEY_PGUP ? ROWS - 1 : -(ROWS - 1); if (view < 0) view = 0;
            draw(); continue;
        }
        if (k == (KEYMOD_CTRL | KEYMOD_SHIFT | 'c') || (k == 3 && sel_l0 >= 0)) { copy_sel(); sel_l0 = -1; draw(); continue; }
        if (k == 22 || k == (KEYMOD_CTRL | KEYMOD_SHIFT | 'v')) { sel_l0 = -1; view = 0; paste(); draw(); continue; }
        if (k == 3) { putch('^'); putch('C'); putch('\n'); ilen = 0; prompt(); draw(); continue; }   /* Ctrl+C: drop the line */
        view = 0; sel_l0 = -1;
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
        else if (key_is_char(k) && ilen < (int)sizeof input - 1 && prompt_len + ilen < COLS - 1) { input[ilen++] = k; show_input(); }
        draw();
    }
    return 0;
}
