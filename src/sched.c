/* Preemptive multitasking. User programs are switched by the local APIC timer
 * (250 Hz); kernel threads (the desktop) run until they sleep or yield. */
#include "kernel.h"
#include "vfs.h"
#include "net.h"
void kfree(void *p);

struct task tasks[MAX_TASKS];
struct task *current;
volatile uint64_t sched_ticks;
bool sched_running;
static struct task *idle_task;
static int next_pid = 1;
extern uint64_t boot_pml4[];
void switch_to(uint64_t *save, uint64_t rsp);
void thread_entry(void);
void user_entry(void);

#define KSTACK_PAGES 8

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }

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
    t->state = PS_READY;
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
    strncpy0(t->appid, ae->id, sizeof t->appid);
    t->app = t->appid;
    strncpy0(t->arg, arg ? arg : "", sizeof t->arg);
    t->pml4 = vm_create();
    uint64_t entry;
    long pages = t->pml4 ? elf_load(t->pml4, img, img_len, &entry) : -1;
    long sp = pages >= 0 ? vm_alloc(t->pml4, USER_STACK_TOP - 256 * 1024, 256 * 1024) : -1;
    kfree(loaded);
    if (pages < 0 || sp < 0) {
        if (t->pml4) vm_destroy(t->pml4);
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
    t->state = PS_READY;
    kprintf("[sched] started '%s' (pid %d, %lu KiB)\n", t->name, t->pid, t->pages * 4);
    return t->pid;
}

static void idle(void *arg) {
    for (;;) { __asm__ volatile("sti; hlt; cli"); schedule(); }
}

void sched_init(void) {
    idle_task = task_create_kernel("Idle (unused time)", idle, 0);
}

void sched_start(void) {
    static uint64_t boot_rsp;
    sched_running = true;
    lapic_timer_start(250);
    current = idle_task;
    current->state = PS_RUNNING;
    tss_set_rsp0(current->kstack_top);
    switch_to(&boot_rsp, current->rsp);
    for (;;) __asm__ volatile("hlt");
}

static uint64_t last_switch_tsc;
static void account(void) {
    uint64_t now = rdtsc();
    if (last_switch_tsc && current) current->ticks += (now - last_switch_tsc) / (tsc_hz() / 1000000);
    last_switch_tsc = now;
}
uint64_t cpu_time_us(void) { account(); return uptime_us(); }

static volatile uint64_t last_sched_ms;
static bool stall_reported;

void schedule(void) {
    __asm__ volatile("cli");
    uint64_t now = uptime_ms();
    last_sched_ms = now; stall_reported = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if ((t->state == PS_SLEEPING || t->state == PS_BLOCKED) && t->wake_at && now >= t->wake_at) {
            t->state = PS_READY; t->wake_at = 0;
        }
    }
    int cur = current - tasks;
    struct task *next = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = &tasks[(cur + i) % MAX_TASKS];
        if (t != idle_task && (t->state == PS_READY || (t == current && t->state == PS_RUNNING))) { next = t; break; }
    }
    if (!next) next = idle_task;
    if (next == current) { current->state = PS_RUNNING; if (!current->user) __asm__ volatile("sti"); return; }
    struct task *prev = current;
    account();
    if (prev->state == PS_RUNNING) prev->state = PS_READY;
    next->state = PS_RUNNING;
    current = next;
    tss_set_rsp0(next->kstack_top);
    if (read_cr3() != (uint64_t)next->pml4) write_cr3((uint64_t)next->pml4);
    switch_to(&prev->rsp, next->rsp);
    if (!current->user) __asm__ volatile("sti");     /* see thread_entry */
}

void task_yield(void) { schedule(); }
void kyield(void) { if (sched_running) schedule(); else cpu_relax(); }

void task_sleep(uint64_t ms) {
    if (!sched_running) { sleep_ms(ms); return; }
    current->wake_at = uptime_ms() + (ms ? ms : 1);
    current->state = PS_SLEEPING;
    schedule();
}

void task_exit(int code) {
    struct task *t = current;
    if (t->user) kprintf("[sched] '%s' (pid %d) exited with code %d\n", t->name, t->pid, code);
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
        if (t == current) task_exit(-1);
        if (t->busy) { t->killed = true; return true; }
        kprintf("[sched] killed '%s' (pid %d)\n", t->name, t->pid);
        wm_task_exit(t);
        net_task_exit(t);
        t->state = PS_ZOMBIE;
        return true;
    }
    return false;
}

void task_reap(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state != PS_ZOMBIE || t == current) continue;
        if (t->user && t->pml4) vm_destroy(t->pml4);
        for (int k = 0; k < KSTACK_PAGES; k++) page_free(t->kstack + k * 4096);
        t->state = 0;
    }
}

void event_push(struct task *t, const struct dunc_event *ev) {
    if (!t || !t->state || t->state == PS_ZOMBIE) return;
    if (ev->type == EV_MOUSE_MOVE && t->evh != t->evt) {   /* coalesce mouse motion */
        struct dunc_event *last = &t->evq[(t->evh + EVQ_SIZE - 1) % EVQ_SIZE];
        if (last->type == EV_MOUSE_MOVE && last->win == ev->win) { *last = *ev; return; }
    }
    int nh = (t->evh + 1) % EVQ_SIZE;
    if (nh == t->evt) return;   /* full: drop */
    t->evq[t->evh] = *ev;
    t->evh = nh;
    if (t->state == PS_BLOCKED && t->waiting_event) { t->state = PS_READY; t->wake_at = 0; }
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
    sched_ticks++;
    lapic_eoi();
    if ((f->cs & 3) == 0 && last_sched_ms && !stall_reported && uptime_ms() - last_sched_ms > 5000) { stall_reported = true; report_stall(f); }
    if ((f->cs & 3) == 3) {
        schedule();
        if (current->killed) task_exit(-1);
    }
}
