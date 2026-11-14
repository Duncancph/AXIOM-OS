/* System Monitor: live CPU and memory use, and every running process. */
#include "../lib/dunc.h"

#define WW 680
#define WH 520
#define HIST 60
static struct app a;
static struct procinfo procs[32], prev[32];
static int nprocs, nprev, sel_pid = -1, hover_btn, hover_row = -1;
static uint64_t prev_ticks, last_sample;
static int cpu_hist[HIST], mem_hist[HIST], cpu_now, proc_cpu[32];
static struct sysinfo si;

static void sample(void) {
    sys_sysinfo(&si);
    memcpy(prev, procs, sizeof procs); nprev = nprocs;
    nprocs = sys_procs(procs, 32);
    uint64_t dt = si.cpu_clock_us - prev_ticks;
    prev_ticks = si.cpu_clock_us;
    int idle_pct = 0;
    for (int i = 0; i < nprocs; i++) {
        uint64_t before = 0;
        for (int j = 0; j < nprev; j++) if (prev[j].pid == procs[i].pid) before = prev[j].cpu_us;
        proc_cpu[i] = dt ? (int)((procs[i].cpu_us - before) * 100 / dt) : 0;
        if (proc_cpu[i] > 100) proc_cpu[i] = 100;
        if (!strcmp(procs[i].name, "Idle")) idle_pct = proc_cpu[i];
        if (!strcmp(procs[i].name, "Idle") && i < 32) {}
    }
    cpu_now = dt ? 100 - idle_pct : 0;
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

static int row_y(int i) { return 262 + i * 30; }
static int sel_index(void) { for (int i = 0; i < nprocs; i++) if (procs[i].pid == sel_pid) return i; return -1; }

static void draw(void) {
    struct canvas *c = &a.c;
    gfx_fill(c, 0, 0, WW, WH, UI_BG);
    char v[32], s[64];
    ksnprintf(v, sizeof v, "%d%%", cpu_now);
    stat_card(c, 20, 18, 312, "CPU", v, "in use", cpu_hist, UI_ACCENT);
    ksnprintf(v, sizeof v, "%lu MiB", si.mem_used >> 20);
    ksnprintf(s, sizeof s, "of %lu MiB", si.mem_total >> 20);
    stat_card(c, 348, 18, 312, "Memory", v, s, mem_hist, 0xA78BFA);

    gfx_text(c, &font_ui_bold, 20, 198, UI_TEXT, "Processes");
    uint64_t up = si.uptime_ms / 1000;
    ksnprintf(s, sizeof s, "%d running  -  up %lu:%02lu:%02lu", nprocs, up / 3600, (up / 60) % 60, up % 60);
    gfx_text(c, &font_ui, 20 + gfx_text_width(&font_ui_bold, "Processes") + 12, 198, UI_MUTED, s);
    bool can_kill = false;
    int si_ = sel_index();
    if (si_ >= 0 && procs[si_].user) can_kill = true;
    ui_button(c, WW - 20 - 110, 178, 110, 30, "End task", BTN_DANGER, hover_btn, !can_kill);

    ui_card(c, 20, 216, WW - 40, WH - 236);
    static const int colx[] = { 36, 92, 300, 380, 470, 560 };
    static const char *hdr[] = { "PID", "Name", "Type", "State", "CPU", "Memory" };
    for (int i = 0; i < 6; i++) gfx_text(c, &font_ui_bold, colx[i], 242, UI_MUTED, hdr[i]);
    gfx_fill(c, 30, 250, WW - 60, 1, UI_BORDER);
    static const char *st[] = { "", "Ready", "Running", "Sleeping", "Waiting", "Exiting" };
    for (int i = 0; i < nprocs && row_y(i) + 26 < WH - 24; i++) {
        int y = row_y(i);
        if (procs[i].pid == sel_pid) gfx_rrect(c, 28, y, WW - 56, 28, 6, 0x173A55, 255);
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
    if (!app_open(&a, WW, WH, "System Monitor")) return 1;
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
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int hb = e.type == EV_MOUSE_MOVE && ui_in(e.x, e.y, WW - 130, 178, 110, 30);
            int hr = -1;
            for (int i = 0; i < nprocs; i++) if (e.type == EV_MOUSE_MOVE && ui_in(e.x, e.y, 28, row_y(i), WW - 56, 28)) hr = i;
            if (hb != hover_btn || hr != hover_row) { hover_btn = hb; hover_row = hr; draw(); }
        }
        if (e.type == EV_MOUSE_DOWN && e.button == 1) {
            for (int i = 0; i < nprocs; i++) if (ui_in(e.x, e.y, 28, row_y(i), WW - 56, 28)) { sel_pid = procs[i].pid; draw(); }
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
