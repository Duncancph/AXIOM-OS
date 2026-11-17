/* More than one core. The cores are listed in the ACPI MADT; each extra one is started with
 * INIT + start-up IPIs into ap_tramp.asm (copied below 1 MiB), which brings it to 64-bit mode on the
 * kernel's page tables and calls ap_main. Then it runs apps alongside the first core.
 *
 * The kernel was written for one core, so only one core is in the kernel at a time: the "big kernel
 * lock" (bkl) is taken on every way into the kernel (interrupts and system calls, in isr_dispatch) and
 * dropped on the way back to an app; an idle core drops it while it sleeps. Apps' own code - where the
 * heavy work happens - runs on all cores at once. Kernel threads (desktop, USB, network) stay on core 0. */
#include "kernel.h"

struct cpu cpus[MAX_CPUS];
int ncpus_on = 1, ncpus_found = 1;
static bool smp_gave_up;

/* called 20 s after start-up: all went well (or tell the user why the extra cores are off) */
void smp_settled(void) {
    if (smp_gave_up) desktop_notify("Using one core", "AXIOM turned off \"Use all processor cores\" because the last start with it didn't finish.");
    const char *t = settings_get("system.cores_trying");
    if (t && !strcmp(t, "1")) settings_set("system.cores_trying", "0");
}

/* A ticket lock: cores get it in the order they asked (a plain test-and-set lock let one core lose the race
 * for seconds while the others kept dropping and retaking it, e.g. while waiting for the disk) */
static volatile uint32_t bkl_next, bkl_serving;
void bkl_lock(void) {
    uint32_t me = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != me) cpu_relax();
    cpu_self()->bkl_held = true;
}
void bkl_unlock(void) {
    cpu_self()->bkl_held = false;
    __atomic_store_n(&bkl_serving, bkl_serving + 1, __ATOMIC_RELEASE);
}

void *acpi_table(const char *sig);
extern uint8_t ap_tramp_start[], ap_tramp_end[];
extern uint64_t boot_pml4[];
#define TRAMP 0x8000

static inline uint64_t rdmsr_(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr_(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static uint64_t bsp_pat;
static struct cpu *volatile booting;

void sched_ap_enter(struct cpu *c) __attribute__((noreturn));
struct task *sched_make_idle(const char *name);

/* a new core, on its start-up stack */
static void ap_main(void) {
    struct cpu *c = booting;
    ((volatile uint64_t *)TRAMP)[7] = 4;
    cpu_setup(c);
    ((volatile uint64_t *)TRAMP)[7] = 5;
    idt_load();
    fpu_cpu_init();                                      /* (SSE and AVX, as the first core) */
    wrmsr_(0x277, bsp_pat);                              /* same memory types (write-combining screen) */
    lapic_ap_init();
    c->started = true;
    while (!sched_running) cpu_relax();
    sched_ap_enter(c);
}

/* is the page at TRAMP free RAM (not firmware's)? */
static bool tramp_ok(void) {
    for (int i = 0; i < bootinfo.mmap_count; i++) {
        struct mmap_ent *e = &bootinfo.mmap[i];
        if (e->type == 1 && e->base <= TRAMP && e->base + e->len >= TRAMP + 0x1000) return true;
    }
    return false;
}

void smp_init(void) {
    lapic_init(250);
    cpus[0].id = 0; cpus[0].apic_id = lapic_id(); cpus[0].started = true;
    uint8_t *madt = acpi_table("APIC");
    if (!madt) { LOG_INFO("Cores: 1 (no ACPI core list)\n"); return; }
    uint32_t len = *(uint32_t *)(madt + 4), ids[MAX_CPUS]; int n = 0;
    for (uint8_t *p = madt + 44; p + 2 <= madt + len && p[1] >= 2; p += p[1]) {
        uint32_t id; bool on;
        if (p[0] == 0 && p[1] >= 8) { id = p[3]; on = *(uint32_t *)(p + 4) & 1; }          /* local APIC (enabled) */
        else if (p[0] == 9 && p[1] >= 16) { id = *(uint32_t *)(p + 4); on = *(uint32_t *)(p + 8) & 1; }   /* x2APIC */
        else continue;
        if (!on || id == cpus[0].apic_id) continue;
        bool dup = false; for (int i = 0; i < n; i++) if (ids[i] == id) dup = true;
        if (!dup && n < MAX_CPUS - 1) ids[n++] = id;
    }
    ncpus_found = n + 1;
    const char *want = settings_get("system.cores");
    bool use = want && !strcmp(want, "all");
    /* Safety: if the last start with all cores never got going (a core taking the machine down can
     * restart it), go back to one core instead of trying again and again. */
    const char *trying = settings_get("system.cores_trying");
    if (use && trying && !strcmp(trying, "1")) {
        LOG_WARN("Cores: the last start with all cores didn't finish; going back to one\n");
        settings_set("system.cores", "one");
        settings_set("system.cores_trying", "0");
        smp_gave_up = true;
        use = false;
    }
    if (!n || !use) {
        LOG_INFO("Cores: %d found, using 1%s\n", ncpus_found, n ? " (turn on the others in Settings > About)" : "");
        return;
    }
    if (!tramp_ok()) { LOG_WARN("Cores: no free low memory for start-up code; using 1\n"); return; }
    settings_set("system.cores_trying", "1");           /* cleared by smp_settled() once things run */
    memcpy((void *)TRAMP, ap_tramp_start, ap_tramp_end - ap_tramp_start);
    uint64_t cr0, cr4, cr3;
    __asm__ volatile("mov %%cr0, %0; mov %%cr4, %1; mov %%cr3, %2" : "=r"(cr0), "=r"(cr4), "=r"(cr3));
    volatile uint64_t *t = (volatile uint64_t *)TRAMP;
    t[1] = cr3; t[3] = (uint64_t)ap_main; t[4] = rdmsr_(0xC0000080) & ~(1ULL << 10); t[5] = cr4; t[6] = cr0;
    bsp_pat = rdmsr_(0x277);
    for (int i = 0; i < n; i++) {
        struct cpu *c = &cpus[ncpus_on];
        memset(c, 0, sizeof *c);
        c->id = ncpus_on; c->apic_id = ids[i];
        c->df_stack = page_alloc(2);
        uint8_t *stack = page_alloc(4);
        char nm[32]; ksnprintf(nm, sizeof nm, "Idle (core %d)", c->id + 1);
        c->idle = sched_make_idle(nm);
        if (!stack || !c->df_stack || !c->idle) break;
        c->idle->cpu = c->id;
        t[2] = (uint64_t)stack + 4 * 4096;
        booting = c; t[7] = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        lapic_ipi(c->apic_id, 0x4500);                   /* INIT */
        sleep_ms(10);
        for (int k = 0; k < 2 && !c->started; k++) {
            lapic_ipi(c->apic_id, 0x4600 | (TRAMP >> 12));   /* start-up, at TRAMP */
            for (int w = 0; w < 200 && !c->started; w++) udelay(100);
        }
        if (!c->started) { LOG_WARN("Cores: core with APIC id %u didn't start (got to step %lu)\n", c->apic_id, t[7]); c->idle->state = 0; continue; }
        ncpus_on++;
    }
    LOG_OK("Cores: %d found, using %d\n", ncpus_found, ncpus_on);
}
