/* Preemptive multitasking. User programs are switched by the local APIC timer
 * (250 Hz); kernel threads (the desktop) run until they sleep or yield. */
#include "kernel.h"
#include "vfs.h"
#include "net.h"
void kfree(void *p);

struct task tasks[MAX_TASKS];
volatile uint64_t sched_ticks;
bool sched_running;
static struct task *idle_task;
static int next_pid = 1;
static uint64_t rq_seq;          /* stamps tasks as they become ready (first in, first out: see schedule) */
extern uint64_t boot_pml4[];
void switch_to(uint64_t *save, uint64_t rsp);
void thread_entry(void);
void user_entry(void);

#define KSTACK_PAGES 8

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }

static uint8_t fx_init[512] __attribute__((aligned(16)));   /* a clean FPU/SSE state for new apps */

static struct task *alloc_task(const char *name) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (!tasks[i].state) {
            struct task *t = &tasks[i];
            memset(t, 0, sizeof *t);
            t->pid = next_pid++;
            strncpy0(t->name, name, sizeof t->name);
            t->kstack = page_alloc(KSTACK_PAGES);
            if (!t->kstack) return 0;
            t->kstack_top = (uint64_t)t->kstack + KSTACK_PAGES * 4096;
            t->pml4 = boot_pml4;
            return t;
        }
    return 0;
}

struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg) {
    struct task *t = alloc_task(name);
    if (!t) return 0;
    uint64_t *sp = (uint64_t *)(t->kstack_top - 24);
    *sp = (uint64_t)thread_entry;
    *--sp = (uint64_t)fn;     /* rbx */
    *--sp = 0;                /* rbp */
    *--sp = (uint64_t)arg;    /* r12 */
    *--sp = 0; *--sp = 0; *--sp = 0;   /* r13 r14 r15 */
    t->rsp = (uint64_t)sp;
    t->state = PS_READY; t->rq = ++rq_seq;
    return t;
}

const struct app_image *app_find(const char *id) {
    for (int i = 0; i < app_count; i++) if (!strcmp(app_images[i].id, id)) return &app_images[i];
    return 0;
}

int task_spawn(const char *app) { return task_spawn_arg(app, ""); }

int task_spawn_arg(const char *app, const char *arg) {
    const struct app_entry *ae = apps_find(app);
    if (!ae) return -1;
    if (ae->linux_cmd[0]) {                                     /* a Linux program (an installed package) */
        char cmd[400]; ksnprintf(cmd, sizeof cmd, "%s%s%s", ae->linux_cmd, arg && arg[0] ? " " : "", arg ? arg : "");
        char *argv[24]; int argc = 0;
        for (char *p = cmd; *p && argc < 23;) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (!argc) return -1;
        argv[argc] = 0;
        int pid = linux_spawn(argv[0], argc, argv, 0);
        if (pid < 0) return -1;
        for (int i = 0; i < MAX_TASKS; i++)                     /* the dock shows it as this app */
            if (tasks[i].state && tasks[i].pid == pid) { strncpy0(tasks[i].appid, ae->id, sizeof tasks[i].appid); tasks[i].app = tasks[i].appid; strncpy0(tasks[i].name, ae->name, sizeof tasks[i].name); }
        return pid;
    }
    const uint8_t *img = 0; uint64_t img_len = 0; void *loaded = 0;
    if (ae->builtin) { img = ae->builtin->start; img_len = ae->builtin->end - ae->builtin->start; }
    else {
        char path[64]; ksnprintf(path, sizeof path, "/apps/%s.elf", ae->id);
        loaded = vfs_read_all(path, &img_len);
        img = loaded;
        if (!img) return -1;
    }
    struct task *t = alloc_task(ae->name);
    if (!t) { kfree(loaded); return -1; }
    t->user = true;
    memcpy(t->fx, fx_init, sizeof t->fx);
    strncpy0(t->appid, ae->id, sizeof t->appid);
    t->app = t->appid;
    strncpy0(t->arg, arg ? arg : "", sizeof t->arg);
    t->mm = mm_new();
    t->pml4 = t->mm ? t->mm->pml4 : 0;
    t->proc = t;
    uint64_t entry;
    long pages = t->pml4 ? elf_load(t->pml4, img, img_len, &entry) : -1;
    long sp = pages >= 0 ? vm_alloc(t->pml4, USER_STACK_TOP - 256 * 1024, 256 * 1024) : -1;
    kfree(loaded);
    if (pages < 0 || sp < 0) {
        if (t->mm) mm_put(t->mm);
        t->mm = 0;
        for (int i = 0; i < KSTACK_PAGES; i++) page_free(t->kstack + i * 4096);
        t->state = 0;
        return -1;
    }
    t->pages = pages + sp;
    uint64_t *s = (uint64_t *)t->kstack_top;
    *--s = 0x1B;                     /* ss: user data */
    *--s = USER_STACK_TOP - 8;       /* rsp */
    *--s = 0x202;                    /* rflags: interrupts on */
    *--s = 0x23;                     /* cs: user code */
    *--s = entry;                    /* rip */
    *--s = (uint64_t)user_entry;
    for (int i = 0; i < 6; i++) *--s = 0;
    t->rsp = (uint64_t)s;
    t->state = PS_READY; t->rq = ++rq_seq;
    kprintf("[sched] started '%s' (pid %d, %lu KiB)\n", t->name, t->pid, t->pages * 4);
    return t->pid;
}

/* Another thread of the running program (SYS_THREAD): same memory, its own registers, stack and %fs. */
long task_thread(uint64_t rip, uint64_t rsp, uint64_t fs, uint64_t clear_tid) {
    struct task *p = current->proc;
    if (!current->user || !p || !current->mm) return -DERR_INVAL;
    if (rip < USER_BASE || rip >= USER_END || rsp < USER_BASE || rsp > USER_END) return -DERR_FAULT;
    char nm[32]; ksnprintf(nm, sizeof nm, "%s thread", p->name);
    struct task *t = alloc_task(nm);
    if (!t) return -DERR_AGAIN;
    t->user = true;
    memcpy(t->fx, fx_init, sizeof t->fx);
    strncpy0(t->appid, p->appid, sizeof t->appid);
    t->app = t->appid;
    t->mm = current->mm; t->mm->refs++;
    t->pml4 = t->mm->pml4;
    t->proc = p;
    t->fs_base = fs; t->clear_tid = clear_tid;
    uint64_t *s = (uint64_t *)t->kstack_top;
    *--s = 0x1B; *--s = rsp; *--s = 0x202; *--s = 0x23; *--s = rip;
    *--s = (uint64_t)user_entry;
    for (int i = 0; i < 6; i++) *--s = 0;
    t->rsp = (uint64_t)s;
    t->state = PS_READY; t->rq = ++rq_seq;
    return t->pid;
}

/* a new user task in mm (proc: its program's first thread, 0: it is one) starting at rip with stack rsp */
struct task *task_new_user(const char *name, struct mm *mm, struct task *proc, uint64_t rip, uint64_t rsp) {
    struct task *t = alloc_task(name);
    if (!t) return 0;
    t->user = true;
    memcpy(t->fx, fx_init, sizeof t->fx);
    t->mm = mm; mm->refs++;
    t->pml4 = mm->pml4;
    t->proc = proc ? proc : t;
    if (proc) { strncpy0(t->appid, proc->appid, sizeof t->appid); t->app = t->appid; t->lx = proc->lx; }
    uint64_t *s = (uint64_t *)t->kstack_top;
    *--s = 0x1B; *--s = rsp; *--s = 0x202; *--s = 0x23; *--s = rip;
    *--s = (uint64_t)user_entry;
    for (int i = 0; i < 6; i++) *--s = 0;
    t->rsp = (uint64_t)s;
    t->state = PS_READY; t->rq = ++rq_seq;
    return t;
}
void frame_entry(void);
long task_clone(const struct frame *f, uint64_t rsp, uint64_t fs, uint64_t clear_tid) {
    struct task *p = current->proc;
    char nm[32]; ksnprintf(nm, sizeof nm, "%s thread", p->name);
    struct task *t = alloc_task(nm);
    if (!t) return -DERR_AGAIN;
    t->user = true;
    __asm__ volatile("fxsave64 %0" : "=m"(t->fx) :: "memory");     /* (its parent's floating-point state) */
    t->mm = current->mm; t->mm->refs++;
    t->pml4 = t->mm->pml4; t->proc = p; t->lx = current->lx;
    strncpy0(t->appid, p->appid, sizeof t->appid); t->app = t->appid;
    t->fs_base = fs; t->clear_tid = clear_tid;
    struct frame *nf = (struct frame *)(t->kstack_top - sizeof *nf);
    *nf = *f; nf->rax = 0; nf->rsp = rsp;
    uint64_t *s = (uint64_t *)nf;
    *--s = (uint64_t)frame_entry;
    for (int i = 0; i < 6; i++) *--s = 0;
    t->rsp = (uint64_t)s;
    t->state = PS_READY; t->rq = ++rq_seq;
    return t->pid;
}
bool task_alive(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].state && tasks[i].state != PS_ZOMBIE && tasks[i].pid == pid) return true;
    return false;
}

/* ---- futexes: threads waiting for a word of their program's memory to change ---- */
long futex_wait(uint64_t addr, uint32_t val, int64_t timeout_ms) {
    struct task *t = current;
    if ((addr & 3) || !user_ok(t->pml4, (const void *)addr, 4, false)) return -DERR_FAULT;
    if (__atomic_load_n((volatile uint32_t *)addr, __ATOMIC_SEQ_CST) != val) return -DERR_AGAIN;
    if (timeout_ms == 0) return -DERR_TIMEDOUT;
    t->futex_mm = t->mm; t->futex_addr = addr; t->futex_woken = false;
    t->wake_at = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms + 1 : 0;   /* (+1: whole ms count, never wake early) */
    t->state = PS_BLOCKED;
    schedule();
    bool woken = t->futex_woken;
    t->futex_mm = 0; t->futex_addr = 0; t->futex_woken = false;
    if (t->killed) task_exit(-1);
    return woken ? 0 : -DERR_TIMEDOUT;
}
long futex_wake(struct mm *m, uint64_t addr, int n) {
    int k = 0;
    for (int i = 0; i < MAX_TASKS && k < n; i++) {
        struct task *t = &tasks[i];
        if (t->state != PS_BLOCKED || t->futex_mm != m || t->futex_addr != addr || !m) continue;
        t->futex_woken = true; t->futex_mm = 0;
        t->state = PS_READY; t->wake_at = 0; t->rq = ++rq_seq;
        k++;
    }
    return k;
}

/* A core with nothing to do: let go of the kernel lock and sleep until the next interrupt. */
static void idle(void *arg) {
    for (;;) {
        __asm__ volatile("cli");
        bkl_unlock();
        __asm__ volatile("sti; hlt; cli");
        bkl_lock();
        schedule();
    }
}
struct task *sched_make_idle(const char *name) {
    struct task *t = task_create_kernel(name, idle, 0);
    if (t) t->idle = true;
    return t;
}

/* another core joins (smp.c): it runs its idle task, which takes work from the shared task list */
void sched_ap_enter(struct cpu *c) {
    static uint64_t dummy;
    bkl_lock();
    c->last_switch_tsc = rdtsc();
    current = c->idle;
    current->state = PS_RUNNING;
    tss_set_rsp0(current->kstack_top);
    switch_to(&dummy, current->rsp);
    for (;;) __asm__ volatile("hlt");
}

/* Apps may use floating point (SSE): the kernel itself never does (-mgeneral-regs-only), so the registers
 * always hold the state of the last app that ran; it is saved and restored around each switch between apps. */
static void fpu_init(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2); cr0 |= 1ull << 1;                      /* EM off, MP on */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);                          /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("fninit; ldmxcsr %0; fxsave64 %1" :: "m"(mxcsr), "m"(fx_init) : "memory");
}

void sched_init(void) {
    fpu_init();
    idle_task = sched_make_idle("Idle (unused time)");
    cpus[0].idle = idle_task;
}

void sched_start(void) {
    static uint64_t boot_rsp;
    bkl_lock();
    lapic_timer_start(250);
    sched_running = true;
    current = idle_task;
    current->state = PS_RUNNING;
    tss_set_rsp0(current->kstack_top);
    switch_to(&boot_rsp, current->rsp);
    for (;;) __asm__ volatile("hlt");
}

static void account(void) {
    struct cpu *c = cpu_self();
    uint64_t now = rdtsc();
    if (c->last_switch_tsc && c->cur) c->cur->ticks += (now - c->last_switch_tsc) / (tsc_hz() / 1000000);
    c->last_switch_tsc = now;
}
uint64_t cpu_time_us(void) { account(); return uptime_us(); }

static volatile uint64_t last_sched_ms;
static bool stall_reported;

void schedule(void) {
    __asm__ volatile("cli");
    struct cpu *me = cpu_self();
    uint64_t now = uptime_ms();
    if (me->id == 0) { last_sched_ms = now; stall_reported = false; }
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if ((t->state == PS_SLEEPING || t->state == PS_BLOCKED) && t->wake_at && now >= t->wake_at) {
            t->state = PS_READY; t->wake_at = 0; t->rq = ++rq_seq;
        }
    }
    int cur = current - tasks;
    /* first in, first out: the ready task that was woken (or stopped) longest ago, so threads run in the order
     * they were woken, as Linux programs expect (a thread woken by a message runs before one woken after it:
     * Chromium's shutdown relies on that); the one running now goes after every ready one */
    struct task *next = 0;
    uint64_t best = ~0ULL;
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = &tasks[(cur + i) % MAX_TASKS];
        if (t->idle || (me->id && !t->user)) continue;      /* kernel threads stay on core 0 */
        if (t->state == PS_READY && t->rq < best) { best = t->rq; next = t; }
    }
    if (!next && current->state == PS_RUNNING && !current->idle && !(me->id && !current->user)) next = current;
    if (!next) next = me->idle;
    account();
    if (next == current) { current->state = PS_RUNNING; if (!current->user) __asm__ volatile("sti"); return; }
    struct task *prev = current;
    if (prev->user && prev->state != PS_ZOMBIE) __asm__ volatile("fxsave64 %0" : "=m"(prev->fx) :: "memory");
    if (next->user) {
        __asm__ volatile("fxrstor64 %0" :: "m"(next->fx) : "memory");
        __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)next->fs_base), "d"((uint32_t)(next->fs_base >> 32)));   /* its %fs (thread-local storage) */
    }
    if (prev->state == PS_RUNNING) { prev->state = PS_READY; prev->rq = ++rq_seq; }
    next->state = PS_RUNNING;
    next->cpu = me->id;
    current = next;
    tss_set_rsp0(next->kstack_top);
    if (read_cr3() != (uint64_t)next->pml4) { write_cr3((uint64_t)next->pml4); if (next->mm) me->tlb_gen = next->mm->tlb_gen; }
    switch_to(&prev->rsp, next->rsp);
    if (!current->user) __asm__ volatile("sti");     /* see thread_entry */
}

void task_yield(void) { schedule(); }
/* Waiting inside the kernel for something (often for a kernel thread, e.g. network data). On core 0
 * that means letting the other tasks run. On another core the kernel threads can't run here, so it lets
 * go of the kernel lock for a moment and wakes core 0, which runs them. */
void kyield(void) {
    if (!sched_running) { cpu_relax(); return; }
    if (cpu_self()->id != 0) {
        bkl_unlock();
        if (cpus[0].cur && cpus[0].cur->idle) lapic_ipi(cpus[0].apic_id, 0xF0);   /* wake it if it sleeps */
        __asm__ volatile("sti");
        for (int i = 0; i < 300; i++) cpu_relax();
        __asm__ volatile("cli");
        bkl_lock();
    }
    schedule();
}

void task_sleep(uint64_t ms) {
    if (!sched_running) { sleep_ms(ms); return; }
    current->wake_at = uptime_ms() + (ms ? ms : 1) + 1;   /* (+1: the current ms is part gone; a sleep never ends early) */
    current->state = PS_SLEEPING;
    schedule();
}

static void kill_one(struct task *t) {                    /* (not current) */
    if (t->busy || t->state == PS_RUNNING) { t->killed = true; return; }   /* running on another core: it stops at its next tick */
    wm_task_exit(t);
    net_task_exit(t);
    t->state = PS_ZOMBIE;
}
static void kill_threads(struct task *except) {           /* the other threads of except's program */
    struct task *p = except->proc;
    if (!p) return;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t == except || !t->state || t->state == PS_ZOMBIE || t->proc != p) continue;
        kill_one(t);
    }
}
/* the end of one thread: its clear_tid word is cleared and its waiters woken (how threads are joined) */
void task_thread_exit(void) {
    struct task *t = current;
    if (t->proc == t) task_exit(0);                       /* the program's first thread: the program ends */
    if (t->clear_tid && user_ok(t->pml4, (const void *)t->clear_tid, 4, true)) {
        *(volatile uint32_t *)t->clear_tid = 0;
        futex_wake(t->mm, t->clear_tid, 1 << 30);
    }
    wm_task_exit(t);
    net_task_exit(t);
    t->state = PS_ZOMBIE;
    schedule();
    panic("zombie thread was scheduled");
}

void task_exit(int code) {
    struct task *t = current;
    if (t->user) kprintf("[sched] '%s' (pid %d) exited with code %d\n", t->name, t->pid, code);
    if (t->user) kill_threads(t);                         /* a program ends with all its threads */
    wm_task_exit(t);
    net_task_exit(t);
    t->state = PS_ZOMBIE;
    schedule();
    panic("zombie task was scheduled");
}

bool task_kill(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->state || t->state == PS_ZOMBIE || t->pid != pid) continue;
        if (!t->user) return false;            /* kernel threads can't be killed */
        if (t->proc && t->proc->state && t->proc->state != PS_ZOMBIE) t = t->proc;   /* a thread: its whole program */
        if (t == current || (current && current->proc == t->proc && current->proc)) task_exit(-1);
        kprintf("[sched] killed '%s' (pid %d)\n", t->name, t->pid);
        kill_threads(t);
        kill_one(t);
        return true;
    }
    return false;
}

void task_reap(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state != PS_ZOMBIE || t == current) continue;
        bool threads_left = false;                        /* a program's first thread goes last (the others point to it) */
        for (int j = 0; j < MAX_TASKS && t->user && t->proc == t; j++) if (&tasks[j] != t && tasks[j].state && tasks[j].proc == t) threads_left = true;
        if (threads_left) continue;
        if (t->user && t->proc == t && t->lx) linux_task_exit(t);
        if (t->user && t->mm) mm_put(t->mm);             /* the memory goes with its last thread */
        t->mm = 0;
        for (int k = 0; k < KSTACK_PAGES; k++) page_free(t->kstack + k * 4096);
        t->state = 0;
    }
}

void event_push(struct task *t, const struct dunc_event *ev) {
    if (!t || !t->state || t->state == PS_ZOMBIE) return;
    if (t->lx && linux_window_event(t, ev)) return;           /* a Linux program's window: its file has the events */
    if (ev->type == EV_MOUSE_MOVE && t->evh != t->evt) {   /* coalesce mouse motion */
        struct dunc_event *last = &t->evq[(t->evh + EVQ_SIZE - 1) % EVQ_SIZE];
        if (last->type == EV_MOUSE_MOVE && last->win == ev->win) { *last = *ev; return; }
    }
    int nh = (t->evh + 1) % EVQ_SIZE;
    if (nh == t->evt) return;   /* full: drop */
    t->evq[t->evh] = *ev;
    t->evh = nh;
    if (t->state == PS_BLOCKED && t->waiting_event) { t->state = PS_READY; t->wake_at = 0; t->rq = ++rq_seq; }
}

/* Watchdog: kernel code isn't preempted, so a kernel thread that never yields freezes everything.
 * After 5 s without a task switch, show where it is stuck (on screen and in the log) to help find it. */
static void report_stall(struct frame *f) {
    char l[6][96]; const char *lines[6]; int n = 0;
    ksnprintf(l[n++], 96, "AXIOM is stuck: '%s' has been running for %lu s", current ? current->name : "?", (uptime_ms() - last_sched_ms) / 1000);
    ksnprintf(l[n++], 96, "at %lx, build %d (please photograph this for Claude)", f->rip, OS_BUILD);
    uint64_t *bp = (uint64_t *)f->rbp; char b[96]; int k = ksnprintf(b, sizeof b, "called from");
    for (int i = 0; i < 6 && current && current->kstack && (uint8_t *)bp >= current->kstack && (uint64_t)bp + 16 <= current->kstack_top; i++) {
        k += ksnprintf(b + k, sizeof b - k, " %lx", bp[1]);
        bp = (uint64_t *)bp[0];
    }
    strncpy0(l[n++], b, 96);
    for (int i = 0; i < n; i++) { lines[i] = l[i]; kprintf("[stuck] %s\n", l[i]); }
    desktop_emergency(lines, n);
}

void sched_tick(struct frame *f) {
    bool core0 = cpu_self()->id == 0;
    if (core0) sched_ticks++;
    lapic_eoi();
    if (core0 && (f->cs & 3) == 0 && last_sched_ms && !stall_reported && uptime_ms() - last_sched_ms > 5000) { stall_reported = true; report_stall(f); }
    if ((f->cs & 3) == 3) {
        if (current->alarm_ticks && --current->alarm_ticks == 0) alarm_deliver(f);   /* counts only the app's own running time */
        schedule();
        if (current->killed) task_exit(-1);
    }
}
