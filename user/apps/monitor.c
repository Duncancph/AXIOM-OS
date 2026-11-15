/* System Monitor: live CPU and memory use, and every running process. */
#include "../lib/dunc.h"

static int WW = 680, WH = 600;
#define HIST 60
static struct app a;
static struct procinfo procs[96], raw[96], prev[96];   /* raw: as the kernel lists them (prev: the last sample) */
static int nprocs, nraw, nprev, sel_pid = -1, hover_btn, hover_row = -1, list_off;
static uint64_t prev_ticks, last_sample;
static int cpu_hist[HIST], mem_hist[HIST], cpu_now, proc_cpu[96];
static struct sysinfo si;

static void sample(void) {
    sys_sysinfo(&si);
    memcpy(prev, raw, sizeof raw); nprev = nraw;
    nraw = nprocs = sys_procs(raw, 96);
    memcpy(procs, raw, sizeof raw);
    uint64_t dt = si.cpu_clock_us - prev_ticks;
    prev_ticks = si.cpu_clock_us;
    /* with several cores, percentages are of the whole machine (all cores together) */
    int ncores = si.ncpus_on > 0 ? si.ncpus_on : 1;
    uint64_t idle_us = 0;
    for (int i = 0; i < nprocs; i++) {
        uint64_t before = 0;
        for (int j = 0; j < nprev; j++) if (prev[j].pid == procs[i].pid) before = prev[j].cpu_us;
        uint64_t used = procs[i].cpu_us - before;
        proc_cpu[i] = dt ? (int)(used * 100 / (dt * ncores)) : 0;
        if (proc_cpu[i] > 100) proc_cpu[i] = 100;
        if (!strncmp(procs[i].name, "Idle", 4)) idle_us += used;
    }
    int idle_pct = dt ? (int)(idle_us * 100 / (dt * ncores)) : 0;
    if (idle_pct > 100) idle_pct = 100;
    cpu_now = dt ? 100 - idle_pct : 0;
    /* one "Idle" row for all cores together; the per-core ones are left out of the list */
    int k = 0;
    for (int i = 0; i < nprocs; i++) {
        if (!strncmp(procs[i].name, "Idle (core", 10)) continue;
        if (!strncmp(procs[i].name, "Idle", 4)) proc_cpu[i] = idle_pct;
        procs[k] = procs[i]; proc_cpu[k] = proc_cpu[i]; k++;
    }
    nprocs = k;
    /* apps first (what you're usually looking for), then the kernel's own tasks; each in pid order */
    for (int i = 1; i < nprocs; i++) {
        struct procinfo t = procs[i]; int tc = proc_cpu[i], j = i;
        while (j > 0 && (procs[j - 1].user < t.user || (procs[j - 1].user == t.user && procs[j - 1].pid > t.pid))) { procs[j] = procs[j - 1]; proc_cpu[j] = proc_cpu[j - 1]; j--; }
        procs[j] = t; proc_cpu[j] = tc;
    }
    memmove(cpu_hist, cpu_hist + 1, sizeof(int) * (HIST - 1)); cpu_hist[HIST - 1] = cpu_now;
    memmove(mem_hist, mem_hist + 1, sizeof(int) * (HIST - 1));
    mem_hist[HIST - 1] = si.mem_total ? (int)(si.mem_used * 100 / si.mem_total) : 0;
}

static void graph(struct canvas *c, int x, int y, int w, int h, int *hist, uint32_t col) {
    gfx_fill(c, x, y, w, h, HL_TERM_BG);
    for (int g = 1; g < 4; g++) gfx_fill(c, x, y + h * g / 4, w, 1, UI_PANEL);
    int px = -1, py = 0;
    for (int i = 0; i < HIST; i++) {
        int gx = x + i * (w - 1) / (HIST - 1), gy = y + h - 1 - hist[i] * (h - 2) / 100;
        for (int yy = gy; yy < y + h; yy++) gfx_blend(c, gx, yy, (w + HIST - 1) / HIST, 1, col, 40);
        if (px >= 0) gfx_line(c, px, py, gx, gy, col, 2);
        px = gx; py = gy;
    }
}

static void stat_card(struct canvas *c, int x, int y, int w, const char *label, const char *value, const char *sub, int *hist, uint32_t col) {
    ui_card(c, x, y, w, 150);
    gfx_text(c, &font_ui, x + 16, y + 26, UI_MUTED, label);
    gfx_text(c, &font_title, x + 16, y + 54, UI_TEXT, value);
    gfx_text(c, &font_ui, x + 16 + gfx_text_width(&font_title, value) + 8, y + 54, UI_MUTED, sub);
    graph(c, x + 16, y + 68, w - 32, 66, hist, col);
}

static int row_y(int i) { return 262 + (i - list_off) * 30; }
static int rows_fit(void) { int n = (WH - 50 - 262) / 30 + 1; return n < 1 ? 1 : n; }
static void clamp_off(void) { int mx = nprocs - rows_fit(); if (list_off > mx) list_off = mx; if (list_off < 0) list_off = 0; }
static bool row_shown(int i) { return i >= list_off && i < list_off + rows_fit(); }
static int sel_index(void) { for (int i = 0; i < nprocs; i++) if (procs[i].pid == sel_pid) return i; return -1; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    char v[32], s[64];
    ksnprintf(v, sizeof v, "%d%%", cpu_now);
    int cw = (WW - 56) / 2;
    stat_card(c, 20, 18, cw, "CPU", v, "in use", cpu_hist, UI_ACCENT);
    ksnprintf(v, sizeof v, "%lu MiB", si.mem_used >> 20);
    ksnprintf(s, sizeof s, "of %lu MiB", si.mem_total >> 20);
    char ml[80] = "Memory";
    if (si.cache_size) {                                      /* the disk cache: spare memory holding recently used files */
        uint64_t cu = si.cache_used >> 20;
        if (cu >= 1024) ksnprintf(ml, sizeof ml, "Memory   -   %lu.%lu GB of files cached", cu / 1024, cu % 1024 * 10 / 1024);
        else ksnprintf(ml, sizeof ml, "Memory   -   %lu MB of files cached", cu);
    }
    stat_card(c, 36 + cw, 18, cw, ml, v, s, mem_hist, 0xD8D3B8);

    gfx_text(c, &font_ui_bold, 20, 198, UI_TEXT, "Processes");
    uint64_t up = si.uptime_ms / 1000;
    ksnprintf(s, sizeof s, "%d running  -  up %lu:%02lu:%02lu", nprocs, up / 3600, (up / 60) % 60, up % 60);
    gfx_text(c, &font_ui, 20 + gfx_text_width(&font_ui_bold, "Processes") + 12, 198, UI_MUTED, s);
    bool can_kill = false;
    int si_ = sel_index();
    if (si_ >= 0 && procs[si_].user) can_kill = true;
    ui_button(c, WW - 20 - 110, 178, 110, 30, "End task", BTN_DANGER, hover_btn, !can_kill);

    ui_card(c, 20, 216, WW - 40, WH - 236);
    int extra = WW - 680 > 0 ? WW - 680 : 0;
    int colx[] = { 36, 92, 300 + extra / 2, 380 + extra * 2 / 3, 470 + extra * 5 / 6, 560 + extra };
    static const char *hdr[] = { "PID", "Name", "Type", "State", "CPU", "Memory" };
    for (int i = 0; i < 6; i++) gfx_text(c, &font_ui_bold, colx[i], 242, UI_MUTED, hdr[i]);
    gfx_fill(c, 30, 250, WW - 60, 1, UI_BORDER);
    static const char *st[] = { "", "Ready", "Running", "Sleeping", "Waiting", "Exiting" };
    clamp_off();
    if (nprocs > rows_fit()) {                           /* not all fit: say so (the list scrolls) */
        char m[64]; int below = nprocs - list_off - rows_fit();
        if (below > 0) ksnprintf(m, sizeof m, "%d more below - scroll the list", below); else ksnprintf(m, sizeof m, "%d more above - scroll the list", list_off);
        gfx_text(c, &font_ui, WW - 40 - gfx_text_width(&font_ui, m), WH - 26, UI_MUTED, m);
    }
    for (int i = list_off; i < nprocs && row_shown(i); i++) {
        int y = row_y(i);
        if (procs[i].pid == sel_pid) gfx_rrect(c, 28, y, WW - 56, 28, 6, 0x2A2A28, 255);
        else if (i == hover_row) gfx_rrect(c, 28, y, WW - 56, 28, 6, UI_PANEL2, 255);
        char b[48];
        ksnprintf(b, sizeof b, "%d", procs[i].pid);
        gfx_text(c, &font_ui, colx[0], y + 19, UI_TEXT, b);
        gfx_text(c, &font_ui_bold, colx[1], y + 19, UI_TEXT, procs[i].name);
        gfx_text(c, &font_ui, colx[2], y + 19, procs[i].user ? UI_ACCENT : UI_GOLD, procs[i].user ? "App" : "Kernel");
        gfx_text(c, &font_ui, colx[3], y + 19, UI_MUTED, procs[i].state <= 5 ? st[procs[i].state] : "?");
        ksnprintf(b, sizeof b, "%d%%", proc_cpu[i]);
        gfx_text(c, &font_ui, colx[4], y + 19, proc_cpu[i] > 50 && procs[i].user ? UI_RED : procs[i].user ? UI_TEXT : UI_MUTED, b);
        ksnprintf(b, sizeof b, "%lu KiB", procs[i].mem_kb);
        gfx_text(c, &font_ui, colx[5], y + 19, UI_TEXT, b);
    }
    app_flush(&a);
}

int main(void) {
    if (!app_open_ex(&a, WW, WH, "System Monitor", WIN_RESIZABLE)) return 1;
    sample(); sys_sleep(250); sample();
    last_sample = sys_time();
    draw();
    for (;;) {
        struct dunc_event e;
        uint64_t now = sys_time();
        long wait = 1000 - (long)(now - last_sample);
        int r = wait > 0 ? sys_event(&e, wait) : 0;
        if (r == 0) { sample(); last_sample = sys_time(); draw(); continue; }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_RESIZE) { if (app_resize(&a, &e)) { WW = a.wi.w; WH = a.wi.h; draw(); } continue; }

        if (e.type == EV_WHEEL) { list_off += e.key; clamp_off(); hover_row = -1; draw(); continue; }
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int hb = e.type == EV_MOUSE_MOVE && ui_in(e.x, e.y, WW - 130, 178, 110, 30);
            int hr = -1;
            for (int i = 0; i < nprocs; i++) if (e.type == EV_MOUSE_MOVE && row_shown(i) && ui_in(e.x, e.y, 28, row_y(i), WW - 56, 28)) hr = i;
            if (hb != hover_btn || hr != hover_row) { hover_btn = hb; hover_row = hr; draw(); }
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            for (int i = 0; i < nprocs; i++) if (row_shown(i) && ui_in(e.x, e.y, 28, row_y(i), WW - 56, 28)) { sel_pid = procs[i].pid; draw(); }
            if (ui_in(e.x, e.y, WW - 130, 178, 110, 30)) {
                int k = sel_index();
                if (k >= 0 && procs[k].user) {
                    char m[96]; ksnprintf(m, sizeof m, "%s (pid %d) was ended from System Monitor.", procs[k].name, procs[k].pid);
                    if (sys_kill(procs[k].pid) == 0) sys_notify("Task ended", m);
                    sel_pid = -1; sample(); draw();
                }
            }
        }
    }
    return 0;
}
