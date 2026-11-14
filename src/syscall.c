/* System calls: every pointer coming from a program is checked against that program's
 * own page tables before the kernel touches it. */
#include "kernel.h"

#define UOK(p, n, w) user_ok(current->pml4, (const void *)(p), (n), (w))

static bool copy_str(char *dst, uint64_t src, size_t max) {
    size_t i = 0;
    for (; i + 1 < max; i++) {
        if ((i == 0 || ((src + i) & 0xFFF) == 0) && !UOK(src + i, 1, false)) return false;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return true;
    }
    dst[i] = 0;
    return true;
}

static uint64_t sys_event(uint64_t uev, int64_t timeout) {
    if (!UOK(uev, sizeof(struct dunc_event), true)) return (uint64_t)-1;
    uint64_t deadline = timeout > 0 ? uptime_ms() + timeout : 0;
    for (;;) {
        struct task *t = current;
        if (t->evh != t->evt) {
            *(struct dunc_event *)uev = t->evq[t->evt];
            t->evt = (t->evt + 1) % EVQ_SIZE;
            return 1;
        }
        if (timeout == 0 || (deadline && uptime_ms() >= deadline)) return 0;
        t->waiting_event = true;
        t->wake_at = deadline;
        t->state = PS_BLOCKED;
        schedule();
        t->waiting_event = false;
        if (t->killed) task_exit(-1);
    }
}

static uint64_t sys_kcmd(uint64_t ucmd, uint64_t ubuf, uint64_t len) {
    char cmd[128];
    if (!copy_str(cmd, ucmd, sizeof cmd) || !len || !UOK(ubuf, len, true)) return (uint64_t)-1;
    if (len > 16384) len = 16384;
    char *buf = page_alloc(4);
    if (!buf) return (uint64_t)-1;
    current->capture = buf; current->cap_len = 0; current->cap_max = 16384 - 1;
    current->busy++;
    shell_execute(cmd);
    current->busy--;
    current->capture = 0;
    buf[current->cap_len] = 0;
    size_t n = current->cap_len + 1 < len ? current->cap_len + 1 : len;
    memcpy((void *)ubuf, buf, n);
    ((char *)ubuf)[n - 1] = 0;
    for (int i = 0; i < 4; i++) page_free(buf + i * 4096);
    return n - 1;
}

uint64_t syscall_dispatch(struct frame *f) {
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->rcx, e = f->r8;
    switch (f->rax) {
    case SYS_EXIT: task_exit((int)a);
    case SYS_YIELD: schedule(); return 0;
    case SYS_SLEEP: task_sleep(a); return 0;
    case SYS_TIME: return uptime_ms();
    case SYS_LOG: {
        char s[256];
        if (!copy_str(s, a, sizeof s)) return (uint64_t)-1;
        kprintf("[%s] %s\n", current->name, s);
        return 0;
    }
    case SYS_WIN_CREATE: {
        char title[64];
        if (!copy_str(title, c, sizeof title) || !UOK(d, sizeof(struct win_info), true)) return (uint64_t)-1;
        if (current->nwin >= 16) return (uint64_t)-1;
        uint32_t *buf; int pages;
        uint64_t va = USER_WIN_BASE + (uint64_t)current->nwin * 0x4000000ULL;
        struct window *w = wm_create(current, (int)a, (int)b, title, &buf, &pages, va);
        if (!w) return (uint64_t)-1;
        current->nwin++;
        for (int i = 0; i < pages; i++) vm_map(current->pml4, va + i * 4096ULL, (uint64_t)buf + i * 4096ULL, true, false);
        struct win_info *wi = (struct win_info *)d;
        wi->id = wm_id(w); wi->buf = (uint32_t *)va; wi->w = (int)a; wi->h = (int)b;
        return wi->id;
    }
    case SYS_WIN_FLUSH: return wm_flush(current, (int)a, (int)b, (int)c, (int)d, (int)e) ? 0 : (uint64_t)-1;
    case SYS_EVENT: return sys_event(a, (int64_t)b);
    case SYS_WIN_TITLE: {
        char title[64];
        if (!copy_str(title, b, sizeof title)) return (uint64_t)-1;
        return wm_set_title(current, (int)a, title) ? 0 : (uint64_t)-1;
    }
    case SYS_WIN_CLOSE: return wm_close(current, (int)a) ? 0 : (uint64_t)-1;
    case SYS_SPAWN: {
        char app[32];
        if (!copy_str(app, a, sizeof app)) return (uint64_t)-1;
        return (uint64_t)(int64_t)task_spawn(app);
    }
    case SYS_PROCS: {
        int max = (int)b, n = 0;
        if (max < 0 || max > MAX_TASKS || !UOK(a, (uint64_t)max * sizeof(struct procinfo), true)) return (uint64_t)-1;
        struct procinfo *p = (struct procinfo *)a;
        cpu_time_us();
        for (int i = 0; i < MAX_TASKS && n < max; i++) {
            struct task *t = &tasks[i];
            if (!t->state || t->state == PS_ZOMBIE) continue;
            p[n].pid = t->pid; p[n].state = t->state; p[n].user = t->user;
            p[n].windows = wm_count(t);
            p[n].cpu_us = t->ticks; p[n].mem_kb = t->user ? t->pages * 4 + 32 : 32;
            strncpy0(p[n].name, t->name, sizeof p[n].name);
            n++;
        }
        return n;
    }
    case SYS_KILL: return task_kill((int)a) ? 0 : (uint64_t)-1;
    case SYS_KCMD: return sys_kcmd(a, b, c);
    case SYS_BT:
        if (a == BT_INFO) {
            if (!UOK(b, sizeof(struct bt_adapter), true)) return (uint64_t)-1;
            bt_get_adapter((struct bt_adapter *)b);
            return 0;
        }
        if (a == BT_SCAN) {
            current->busy++;
            int64_t r = bt_present() ? (bt_scan((int)b), 0) : -1;
            current->busy--;
            return (uint64_t)r;
        }
        if (a == BT_RESULTS) {
            int max = (int)c;
            if (max < 0 || max > 64 || !UOK(b, (uint64_t)max * sizeof(struct bt_device), true)) return (uint64_t)-1;
            return bt_get_results((struct bt_device *)b, max);
        }
        return (uint64_t)-1;
    case SYS_SYSINFO: {
        if (!UOK(a, sizeof(struct sysinfo), true)) return (uint64_t)-1;
        struct sysinfo *s = (struct sysinfo *)a;
        memset(s, 0, sizeof *s);
        s->mem_total = mem_total(); s->mem_used = mem_used(); s->uptime_ms = uptime_ms(); s->cpu_clock_us = cpu_time_us();
        s->fb_w = bootinfo.fb_w; s->fb_h = bootinfo.fb_h; s->uefi = bootinfo.uefi;
        for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].state && tasks[i].state != PS_ZOMBIE) s->nprocs++;
        uint32_t r[12], x, y, z, w;
        cpuid(0x80000000, 0, &x, &y, &z, &w);
        if (x >= 0x80000004) {
            for (int i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
            char brand[49]; memcpy(brand, r, 48); brand[48] = 0;
            char *bp = brand; while (*bp == ' ') bp++;
            strncpy0(s->cpu, bp, sizeof s->cpu);
        }
        ksnprintf(s->kernel, sizeof s->kernel, "%s %s", KERNEL_NAME, OS_VERSION);
        return 0;
    }
    case SYS_POWER:
        if (a == 0) acpi_poweroff(); else acpi_reboot();
        return (uint64_t)-1;
    case SYS_RTC:
        if (!UOK(a, sizeof(struct rtc_time), true)) return (uint64_t)-1;
        rtc_read((struct rtc_time *)a);
        return 0;
    case SYS_NOTIFY: {
        char t[64], m[160];
        if (!copy_str(t, a, sizeof t) || !copy_str(m, b, sizeof m)) return (uint64_t)-1;
        desktop_notify(t, m);
        return 0;
    }
    }
    return (uint64_t)-1;
}
