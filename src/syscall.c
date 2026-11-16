/* System calls: every pointer coming from a program is checked against that program's
 * own page tables before the kernel touches it. */
#include "kernel.h"
#include "vfs.h"
#include "net.h"
#include "disk.h"

static char *clip; static uint64_t clip_len;          /* the shared clipboard */

#define UOK(p, n, w) user_ok(current->pml4, (const void *)(p), (n), (w))
long update_syscall(int op, const void *a, long b, bool (*user_ok_fn)(const void *, size_t));
static bool upd_user_ok(const void *p, size_t n) { return UOK(p, n, false); }

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
    char cmd[512];
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

/* An app's alarm (SYS_ALARM) went off while it was running its own code: save its registers on its stack
 * and make it continue in the alarm function, which can jump elsewhere or come back (SYS_SIGRETURN). */
void alarm_deliver(struct frame *f) {
    uint64_t fn = current->alarm_fn;
    current->alarm_ticks = 0;
    uint64_t sp = (f->rsp - 128 - sizeof(struct frame)) & ~15ULL;   /* below the red zone */
    if (!fn || !UOK(sp - 16, sizeof(struct frame) + 16, true)) return;
    memcpy((void *)sp, f, sizeof *f);
    *(uint64_t *)(sp - 8) = 0;                       /* no return address */
    f->rdi = sp; f->rsp = sp - 8; f->rip = fn;
}

uint64_t syscall_dispatch(struct frame *f) {
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->rcx, e = f->r8;
    switch (f->rax) {
    case SYS_EXIT: task_exit((int)a);
    case SYS_YIELD: schedule(); return 0;
    case SYS_SLEEP: task_sleep(a); return 0;
    case SYS_TIME: return uptime_ms();
    case SYS_ALARM:                                  /* after a ms of the app's own running time, call fn (0 = cancel) */
        current->alarm_ticks = a ? (uint32_t)((a + 3) / 4) : 0; current->alarm_fn = b;
        return 0;
    case SYS_SIGRETURN: {                            /* back from the alarm function to where the app was */
        if (!UOK(a, sizeof(struct frame), false)) return (uint64_t)-1;
        struct frame s; memcpy(&s, (void *)a, sizeof s);
        s.cs = 0x23; s.ss = 0x1B; s.rflags = (s.rflags & 0xCD5) | 0x202;   /* user mode, interrupts on, nothing privileged */
        *f = s;
        return f->rax;                               /* (the caller stores this in rax) */
    }
    case SYS_LOG: {
        char s[256];
        if (!copy_str(s, a, sizeof s)) return (uint64_t)-1;
        kprintf("[%s] %s\n", current->name, s);
        return 0;
    }
    case SYS_WIN_CREATE: {
        char title[64];
        if (!copy_str(title, c, sizeof title) || !UOK(d, sizeof(struct win_info), true)) return (uint64_t)-1;
        struct task *P = current->proc ? current->proc : current;   /* (a program's threads share its window area) */
        if (P->nwin >= 16) return (uint64_t)-1;
        uint32_t *buf; int pages;
        uint64_t va = USER_WIN_BASE + (uint64_t)P->nwin * 0x4000000ULL;
        struct window *w = wm_create(current, (int)a, (int)b, title, &buf, &pages, va, (int)e);
        if (!w) return (uint64_t)-1;
        P->nwin++;
        for (int i = 0; i < pages; i++) vm_map(current->pml4, va + i * 4096ULL, (uint64_t)buf + i * 4096ULL, true, false);
        struct win_info *wi = (struct win_info *)d;
        wi->id = wm_id(w); wi->buf = (uint32_t *)va; wi->w = (int)a; wi->h = (int)b;
        return wi->id;
    }
    case SYS_WIN_RESIZE: {
        if (!UOK(b, sizeof(struct win_info), true)) return (uint64_t)-1;
        int nw, nh;
        if (!wm_resize(current, (int)a, &nw, &nh)) return (uint64_t)-1;
        struct win_info *wi = (struct win_info *)b;
        wi->w = nw; wi->h = nh;
        return 0;
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
        char app[32], arg[256] = "";
        if (!copy_str(app, a, sizeof app) || (b && !copy_str(arg, b, sizeof arg))) return (uint64_t)-1;
        return (uint64_t)(int64_t)task_spawn_arg(app, arg);
    }
    case SYS_GETARG:
        if (!b || !UOK(a, b, true)) return (uint64_t)-1;
        strncpy0((char *)a, current->arg, b);
        return strlen(current->arg);
    case SYS_APPS: {
        int max = (int)b, n = apps_count();
        if (max < 0 || !UOK(a, (uint64_t)max * sizeof(struct dunc_app), true)) return (uint64_t)-1;
        struct dunc_app *o = (struct dunc_app *)a;
        for (int i = 0; i < n && i < max; i++) {
            const struct app_entry *e = apps_get(i);
            strncpy0(o[i].id, e->id, sizeof o[i].id); strncpy0(o[i].name, e->name, sizeof o[i].name);
            o[i].installed = e->installed;
        }
        return n < max ? n : max;
    }
    case SYS_FS_LIST: case SYS_FS_READ: case SYS_FS_WRITE: case SYS_FS_MKDIR: case SYS_FS_DELETE:
    case SYS_FS_RENAME: case SYS_FS_STAT: case SYS_FS_COPY: case SYS_FS_APPEND: {
        char p1[PATH_MAX_], p2[PATH_MAX_];
        if (!copy_str(p1, a, sizeof p1)) return (uint64_t)FS_EINVAL;
        int64_t r = FS_EINVAL;
        current->busy++;
        switch (f->rax) {
        case SYS_FS_LIST:
            if ((int)c > 0 && (int)c <= 1024 && UOK(b, c * sizeof(struct dunc_dirent), true)) r = vfs_list(p1, (struct dunc_dirent *)b, (int)c);
            break;
        case SYS_FS_READ:
            if (UOK(b, d, true)) r = vfs_read(p1, (void *)b, c, d);
            break;
        case SYS_FS_WRITE:
            if (c <= (64u << 20) && (c == 0 || UOK(b, c, false))) r = vfs_write(p1, (const void *)b, c);
            break;
        case SYS_FS_APPEND:
            if (c <= (64u << 20) && (c == 0 || UOK(b, c, false))) r = vfs_append(p1, (const void *)b, c);
            break;
        case SYS_FS_MKDIR: r = vfs_mkdir(p1); break;
        case SYS_FS_DELETE: r = vfs_delete(p1); break;
        case SYS_FS_RENAME: if (copy_str(p2, b, sizeof p2)) r = vfs_rename(p1, p2); break;
        case SYS_FS_COPY: if (copy_str(p2, b, sizeof p2)) r = vfs_copy(p1, p2); break;
        case SYS_FS_STAT: if (UOK(b, sizeof(struct dunc_stat), true)) r = vfs_stat(p1, (struct dunc_stat *)b); break;
        }
        current->busy--;
        if (f->rax == SYS_FS_WRITE || f->rax == SYS_FS_DELETE || f->rax == SYS_FS_RENAME) apps_refresh();
        return (uint64_t)r;
    }
    case SYS_RANDOM:
        if (!b || b > 65536 || !UOK(a, b, true)) return (uint64_t)-1;
        random_bytes((void *)a, b);
        return 0;
    case SYS_NET_INFO:
        if (!UOK(a, sizeof(struct dunc_netinfo), true)) return (uint64_t)-1;
        net_get_info((struct dunc_netinfo *)a);
        return 0;
    case SYS_NET_CONFIG:
        if (a == NETCFG_DNS) {
            if (!b) { net_set_dns(0); return 0; }
            if (!UOK(b, sizeof(struct ipaddr), false)) return (uint64_t)-1;
            struct ipaddr ip = *(struct ipaddr *)b;
            net_set_dns(&ip);
        } else if (a == NETCFG_RENEW) net_renew();
        return 0;
    case SYS_NET_PING: {
        if (!UOK(a, sizeof(struct ipaddr), false)) return (uint64_t)(int64_t)NET_EINVAL;
        struct ipaddr ip = *(struct ipaddr *)a;
        current->busy++;
        long r = net_ping(&ip, b > 10000 ? 10000 : (uint32_t)b);
        current->busy--;
        return (uint64_t)r;
    }
    case SYS_NET_RESOLVE: {
        char name[128];
        int max = c > 8 ? 8 : (int)c;
        if (!copy_str(name, a, sizeof name) || max < 1 || !UOK(b, max * sizeof(struct ipaddr), true)) return (uint64_t)(int64_t)NET_EINVAL;
        struct ipaddr out[8];
        current->busy++;
        int r = net_resolve(name, out, max);
        current->busy--;
        if (r > 0) memcpy((void *)b, out, r * sizeof out[0]);
        return (uint64_t)(int64_t)r;
    }
    case SYS_TCP_CONNECT: {
        if (!UOK(a, sizeof(struct ipaddr), false)) return (uint64_t)(int64_t)NET_EINVAL;
        struct ipaddr ip = *(struct ipaddr *)a;
        current->busy++;
        int r = tcp_connect(&ip, (uint16_t)b, (uint32_t)c);
        current->busy--;
        return (uint64_t)(int64_t)r;
    }
    case SYS_TCP_SEND: {
        if (c > (16u << 20) || (c && !UOK(b, c, false))) return (uint64_t)(int64_t)NET_EINVAL;
        current->busy++;
        long r = tcp_send((int)a, (const void *)b, (uint32_t)c);
        current->busy--;
        return (uint64_t)r;
    }
    case SYS_TCP_RECV: {
        if (!c || c > (16u << 20) || !UOK(b, c, true)) return (uint64_t)(int64_t)NET_EINVAL;
        current->busy++;
        long r = tcp_recv((int)a, (void *)b, (uint32_t)c, (int64_t)d);
        current->busy--;
        return (uint64_t)r;
    }
    case SYS_TCP_CLOSE: return (uint64_t)(int64_t)tcp_close((int)a);
    case SYS_FS_VOLUMES: {
        int max = (int)b;
        if (max <= 0 || max > 8 || !UOK(a, (uint64_t)max * sizeof(struct dunc_volume), true)) return (uint64_t)-1;
        return vfs_volumes((struct dunc_volume *)a, max);
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
        if (a == BT_PAIR || a == BT_FORGET) {
            if (!UOK(b, 6, false)) return (uint64_t)-1;
            uint8_t ad[6]; memcpy(ad, (void *)b, 6);
            if (a == BT_FORGET) { bt_forget(ad); return 0; }
            return (uint64_t)(int64_t)bt_pair_start(ad);
        }
        if (a == BT_PAIR_STATUS) {
            if (!UOK(b, sizeof(struct bt_pair), true)) return (uint64_t)-1;
            bt_pair_status((struct bt_pair *)b);
            return 0;
        }
        if (a == BT_PAIR_CONFIRM) { bt_pair_confirm((int)c); return 0; }        /* sys_bt(BT_PAIR_CONFIRM, 0, yes) */
        if (a == BT_AUDIO_CONNECT) {
            if (!UOK(b, 6, false)) return (uint64_t)-1;
            uint8_t ad[6]; memcpy(ad, (void *)b, 6);
            return (uint64_t)(int64_t)bt_audio_connect(ad);
        }
        if (a == BT_AUDIO_STOP) { bt_audio_stop(); return 0; }
        if (a == BT_AUDIO_STATUS) {
            if (!UOK(b, sizeof(struct bt_audio), true)) return (uint64_t)-1;
            bt_audio_status((struct bt_audio *)b);
            return 0;
        }
        if (a == BT_PAIRED) {
            int max = (int)c;
            if (max < 0 || max > 16 || !UOK(b, (uint64_t)max * sizeof(struct bt_paired), true)) return (uint64_t)-1;
            return bt_paired((struct bt_paired *)b, max);
        }
        return (uint64_t)-1;
    case SYS_SYSINFO: {
        if (!UOK(a, sizeof(struct sysinfo), true)) return (uint64_t)-1;
        struct sysinfo *s = (struct sysinfo *)a;
        memset(s, 0, sizeof *s);
        s->mem_total = mem_total(); s->mem_used = mem_used(); s->uptime_ms = uptime_ms(); s->cpu_clock_us = cpu_time_us();
        { int w, h; desktop_size(&w, &h, 0); s->fb_w = w; s->fb_h = h; } s->uefi = bootinfo.uefi;
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
        s->ncpus_found = ncpus_found; s->ncpus_on = ncpus_on;
        s->cache_used = bcache_bytes(); s->cache_size = bcache_capacity();
        return 0;
    }
    case SYS_DISK_LIST: {
        int max = b > 8 ? 8 : (int)b;
        if (max < 1 || !UOK(a, max * sizeof(struct dunc_disk), true)) return (uint64_t)-1;
        struct dunc_disk tmp[8];
        int n = disk_list(tmp, max);
        memcpy((void *)a, tmp, n * sizeof tmp[0]);
        return n;
    }
    case SYS_MMAP: return (uint64_t)mm_mmap(current->mm, a, b, (int)c, (int)d, &current->proc->pages);
    case SYS_MUNMAP: return (uint64_t)mm_munmap(current->mm, a, b, &current->proc->pages);
    case SYS_MPROTECT: return (uint64_t)mm_mprotect(current->mm, a, b, (int)c);
    case SYS_THREAD: return (uint64_t)task_thread(a, b, c, d);
    case SYS_THREAD_EXIT: task_thread_exit();
    case SYS_FUTEX:
        if (a == FUTEX_OP_WAIT) return (uint64_t)futex_wait(b, (uint32_t)c, (int64_t)d);
        if (a == FUTEX_OP_WAKE) return (uint64_t)futex_wake(current->mm, b, (int)c);
        return (uint64_t)-DERR_INVAL;
    case SYS_SET_FS:
        current->fs_base = a;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)a), "d"((uint32_t)(a >> 32)));
        return 0;
    case SYS_CLOCK: {
        if (a == 1) return uptime_ns();
        if (a == 2) { cpu_time_us(); return current->ticks * 1000; }
        static uint64_t boot_epoch_ns;                    /* real time: the clock chip's time at start-up, then the counter */
        if (!boot_epoch_ns) {
            struct rtc_time t; rtc_read_local(&t);
            static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
            long y = t.year, days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[(t.mon - 1) % 12] + t.day - 1;
            if (t.mon > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
            long secs = days * 86400 + t.hour * 3600 + t.min * 60 + t.sec - t.utc_offset_min * 60;
            boot_epoch_ns = (uint64_t)secs * 1000000000ull - uptime_ns();
        }
        return boot_epoch_ns + uptime_ns();
    }
    case SYS_GETTID: return (uint64_t)current->pid;
    case SYS_MEM_GROW: {                     /* a: bytes more; returns the start of the new memory, or 0 */
        struct task *P = current->proc ? current->proc : current;   /* (the program's heap, shared by its threads) */
        uint64_t len = (a + 4095) & ~4095ull;
        if (!P->heap_top) P->heap_top = USER_HEAP;
        uint64_t used = P->heap_top - USER_HEAP;
        if (!len || len > (512ull << 20) || used + len > (1024ull << 20)) return 0;     /* 1 GiB per app */
        if (mem_used() + len + (32ull << 20) > mem_total()) return 0;                 /* keep 32 MiB for the system */
        long n = vm_alloc(current->pml4, P->heap_top, len);
        if (n < 0) return 0;
        P->pages += n;
        uint64_t start = P->heap_top;
        P->heap_top += len;
        return start;
    }
    case SYS_SETTING_GET: {
        char key[32];
        if (!copy_str(key, a, sizeof key) || !c || c > 8192 || !UOK(b, c, true)) return (uint64_t)-1;
        return settings_syscall_get(key, (char *)b, (int)c);
    }
    case SYS_SETTING_SET: {
        char key[32], val[112];
        if (!copy_str(key, a, sizeof key) || !copy_str(val, b, sizeof val)) return (uint64_t)-1;
        return (uint64_t)(int64_t)settings_set(key, val);
    }
    case SYS_CLIP_SET: {
        if (b > (1u << 20) || (b && !UOK(a, b, false))) return (uint64_t)-1;
        char *n = kmalloc(b ? b : 1);
        if (!n) return (uint64_t)-1;
        memcpy(n, (void *)a, b);
        kfree(clip); clip = n; clip_len = b;
        return 0;
    }
    case SYS_CLIP_GET: {
        if (!a) return clip_len;
        uint64_t n = clip_len < b ? clip_len : b;
        if (n && !UOK(a, n, true)) return (uint64_t)-1;
        if (n) memcpy((void *)a, clip, n);
        return n;
    }
    case SYS_SOUND: return (uint64_t)(int64_t)sound_syscall((int)a, b, c);
    case SYS_LOCK: desktop_lock(); return 0;
    case SYS_UPDATE: return (uint64_t)update_syscall((int)a, (const void *)b, (long)c, upd_user_ok);
    case SYS_DISK_INSTALL: return (uint64_t)(int64_t)disk_install((int)a, (int)b, c);   /* c: MiB for AXIOM (0: the whole disk) */
    case SYS_DISK_STATUS:
        if (!UOK(a, sizeof(struct dunc_install), true)) return (uint64_t)-1;
        disk_install_status((struct dunc_install *)a);
        return 0;
    case SYS_POWER:
        if (a == 0) acpi_poweroff(); else acpi_reboot();
        return (uint64_t)-1;
    case SYS_RTC:
        if (!UOK(a, sizeof(struct rtc_time), true)) return (uint64_t)-1;
        rtc_read_local((struct rtc_time *)a);
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
