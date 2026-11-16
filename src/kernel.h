#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "../common/abi.h"

#define OS_NAME "AXIOM"
#define KERNEL_NAME "Axiom Core"
#define OS_VERSION "0.2"
#ifndef OS_BUILD
#define OS_BUILD 0                       /* set by the Makefile: the number of commits */
#endif
/* where Settings looks for updates (a signed manifest; see tools/make-release.py); "" = not set up */
#define UPDATE_URL "https://raw.githubusercontent.com/Duncancph/AXIOM-OS/updates/latest.txt"

typedef int bool;
#define true 1
#define false 0

/* ---- port I/O and CPU helpers ---- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

static inline uint32_t mmio_r32(volatile void *a) { return *(volatile uint32_t *)a; }
static inline void mmio_w32(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void mmio_w64(volatile void *a, uint64_t v) {
    ((volatile uint32_t *)a)[0] = (uint32_t)v;
    ((volatile uint32_t *)a)[1] = (uint32_t)(v >> 32);
}

/* ---- common/str.c + lib.c ---- */
#include "../common/str.h"
void kprintf(const char *fmt, ...);
void panic(const char *msg) __attribute__((noreturn));

/* ---- serial.c ---- */
void serial_init(void);
void serial_putc(char c);
int serial_getc(void);   /* -1 if none */

/* ---- console.c ---- */
enum { C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
       C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE };
void console_init(void);
void console_putc(char c);
void console_write(const char *s);
void console_set_color(int fg);
void console_clear(void);
void console_cursor(bool on);
void console_info(char *buf, size_t n);
void console_gui_mode(void);
void console_log_read(char *buf, size_t n);
extern bool console_has_fb;

/* ---- multiboot / boot info ---- */
struct mmap_ent { uint64_t base, len; uint32_t type; };
struct bootinfo {
    uint64_t fb_addr; uint32_t fb_pitch, fb_w, fb_h; uint8_t fb_bpp, fb_type;
    uint8_t r_pos, r_size, g_pos, g_size, b_pos, b_size;
    struct mmap_ent mmap[64]; int mmap_count;
    uint64_t rsdp; /* physical address of a copy of the ACPI RSDP */
    uint8_t rsdp_copy[64];
    char bootloader[64];
    uint64_t mod_start, mod_end;      /* initial files (tar), loaded by GRUB */
    struct { uint64_t start, end; char name[24]; } mods[8]; int mod_count;   /* every GRUB module, by name */
    char cmdline[128];
    bool uefi;
};
extern struct bootinfo bootinfo;

/* ---- mem.c ---- */
void mem_init(void);
void *page_alloc(size_t pages);      /* zeroed, 4 KiB aligned, below 4 GiB; NULL if out of memory */
void *page_alloc_or_panic(size_t pages);
int mem_high_regions(uint64_t *base, uint64_t *len, int max);
void mem_high_init(uint64_t skip);          /* after the disk cache has taken its part */
uint64_t upage_alloc(void);                 /* a program's page (physical; may be above 4 GiB) */
void upage_free(uint64_t pa);
uint64_t mem_low_free(void);
void page_free(void *page);          /* single 4 KiB page */
/* Device memory above 4 GiB is mapped into the kernel-only top half of the address space
 * (shared by every program's page tables); below 4 GiB it is identity mapped. */
#define HHDM 0xFFFF800000000000ULL
static inline void *mmio_virt(uint64_t phys) { return (void *)(phys < 0x100000000ULL ? phys : HHDM + phys); }
void *map_mmio(uint64_t phys, uint64_t size);
void map_mmio_flags(uint64_t phys, uint64_t size, uint64_t cache);
uint64_t mem_total(void);
uint64_t mem_used(void);

/* ---- idt.c / timer.c ---- */
void idt_init(void);
void timer_init(void);
uint64_t uptime_ms(void);
uint64_t uptime_us(void);
uint64_t uptime_ns(void);
void sleep_ms(uint64_t ms);
void udelay(uint64_t us);
/* keymap.c */
void kbd_hid_key(uint8_t usage, uint8_t mods);
void hid_kbd_report(uint8_t *last, const uint8_t *r, int len);   /* boot keyboard report (USB and Bluetooth) */
void desktop_emergency(const char *const *lines, int n);
void desktop_size(int *w, int *h, bool *scaled);    /* the desktop's size (smaller than the screen when stretched) */
void kbd_super(bool down);
void kbd_ps2_scancode(uint8_t sc);
void kbd_set_layout(const char *id);
const char *kbd_layout(void);
int  kbd_layouts(char *out, int n);
/* settings.c */
const char *settings_get(const char *key);
int  settings_int(const char *key);
int  settings_set(const char *key, const char *val);
int  settings_syscall_get(const char *key, char *out, int n);
void settings_load(void);
bool settings_check_password(const char *pw);
void rtc_read_local(struct rtc_time *t);
long sound_syscall(int op, uint64_t a, uint64_t b);
void hda_init_all(void);
void hda_tick(void);
void sound_notify(void);
void desktop_lock(void);                  /* busy-wait */
const uint8_t *bootmod(const char *name, uint64_t *len);   /* a GRUB module by its name, or 0 */
uint64_t tsc_hz(void);

/* ---- input (kbd.c) ---- */
void ps2_init(void);
void ps2_poll(void);
void input_push(int c);
int input_pop(void);        /* -1 if empty (does not poll hardware) */
void mouse_push(int dx, int dy, int buttons, int wheel);
bool mouse_pop(int *dx, int *dy, int *buttons, int *wheel);
extern bool mouse_present;
int input_poll(void);    /* non-blocking, -1 if none; also services USB */
int input_getc(void);    /* blocking */
extern bool ps2_present;

/* ---- pci.c ---- */
struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, rev;
};
extern struct pci_dev pci_devs[256];
extern int pci_count;
void pci_scan(void);
void intelgfx_probe(void);
const char *intelgfx_summary(void);
const char *intelgfx_monitor(void);
const char *intelgfx_report(void);
void intelgfx_maybe_switch(void);
bool intelgfx_native_active(void);
bool intelgfx_native_asking(void);
void intelgfx_native_answer(bool keep);
void intelgfx_native_settled(void);
const char *intelgfx_offer(void);
int intelgfx_audio_enable(void);
uint32_t intelgfx_cdclk_khz(void);
const char *hda_report(void);
void hda_choose(void);
int hda_outputs(char *out, int n);
const char *hda_current(void);
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off);
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off);
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v);
uint64_t pci_bar(struct pci_dev *p, int bar, uint64_t *size);
const char *pci_class_name(uint8_t cls, uint8_t sub, uint8_t pif);
/* the Device Manager (devices.c) */
enum { DEV_OK = 1, DEV_FAILED, DEV_OFF };
void dev_claim(const struct pci_dev *pd, const char *driver, int status, const char *name, const char *note);
void dev_line(const char *kind, const char *name, const char *driver, const char *status, const char *where, const char *ids, const char *detail);
int devices_report(char *out, int cap);
void usb_devices(void);       /* xhci.c: a dev_line for each USB device */
void disk_devices(void);      /* disk.c: a dev_line for each drive */
extern bool mouse_present;
extern int ncpus_on, ncpus_found;

/* ---- acpi.c ---- */
void acpi_init(void);
void acpi_poweroff(void);
void acpi_reboot(void);

/* ---- usb ---- */
void xhci_init_all(void);
void xhci_poll(void);
void xhci_service(void);
void usb_list(void);

/* ---- bluetooth (bt.c) ---- */
struct usb_dev;
void bt_attach(struct usb_dev *dev, int iface, int evt_dci);
void bt_info(void);
void bt_scan(int seconds);
void bt_list(void);
bool bt_present(void);

/* ---- shell.c ---- */
void shell_run(void);
void shell_execute(char *line);

/* ---- cpu.c ---- */
void gdt_init(void);
void tss_set_rsp0(uint64_t rsp);
void idt_load(void);
void lapic_ap_init(void);
uint32_t lapic_id(void);
void lapic_ipi(uint32_t apic_id, uint32_t icr_low);
void lapic_timer_start(uint32_t hz);
void lapic_init(uint32_t hz);
void lapic_eoi(void);
void fb_write_combining(uint64_t addr, uint64_t size);

/* ---- rtc.c ---- */
void rtc_read(struct rtc_time *t);

/* ---- vm.c ---- */
void vm_init(void);
uint64_t *vm_create(void);
bool vm_map(uint64_t *pml4, uint64_t va, uint64_t pa, bool writable, bool owned);
long vm_alloc(uint64_t *pml4, uint64_t va, uint64_t len);
uint64_t vm_phys(uint64_t *pml4, uint64_t va);
bool user_ok(uint64_t *pml4, const void *p, uint64_t len, bool write);
void vm_destroy(uint64_t *pml4);
void vm_unmap(uint64_t *pml4, uint64_t va, uint64_t pages);
/* a program's memory: shared by its threads; SYS_MMAP's areas (vmas) are given pages when first touched */
struct vma { uint64_t s, e; int prot; };
struct mm {
    uint64_t *pml4;
    int refs;                          /* threads using it */
    struct vma *v; int nv, cap;
    volatile uint64_t tlb_gen;         /* bumped when mappings are taken away or reduced */
};
struct mm *mm_new(void);
void mm_put(struct mm *m);
long mm_mmap(struct mm *m, uint64_t addr, uint64_t len, int prot, int flags, uint64_t *pages);
long mm_munmap(struct mm *m, uint64_t addr, uint64_t len, uint64_t *pages);
long mm_mprotect(struct mm *m, uint64_t addr, uint64_t len, int prot);
long mm_dontneed(struct mm *m, uint64_t addr, uint64_t len, uint64_t *pages);
struct frame;
bool mm_fault(struct mm *m, uint64_t addr, bool write, bool exec, uint64_t *pages);
int mm_prot_at(struct mm *m, uint64_t addr);
bool mm_map_shared(struct mm *m, uint64_t va, uint64_t pa, int prot);   /* a page owned elsewhere (not freed with the program) */
void tlb_sync(void);                   /* before going back to an app: drop stale translations */
void tlb_ipi(void);                    /* the shootdown interrupt (no kernel lock) */
long futex_wait(uint64_t addr, uint32_t val, int64_t timeout_ms);
long futex_wake(struct mm *m, uint64_t addr, int n);
long task_thread(uint64_t rip, uint64_t rsp, uint64_t fs, uint64_t clear_tid);
struct task *task_new_user(const char *name, struct mm *mm, struct task *proc, uint64_t rip, uint64_t rsp);
long task_clone(const struct frame *f, uint64_t rsp, uint64_t fs, uint64_t clear_tid);   /* a thread starting as a copy of f (rax = 0) */
struct lxproc;
void linux_cpu_init(void);                 /* the syscall instruction (each core) */
uint64_t linux_syscall(struct frame *f);
int linux_spawn(const char *path, int argc, char **argv, struct task *out);   /* -> pid */
void linux_task_exit(struct task *t);
bool task_alive(int pid);
void task_thread_exit(void) __attribute__((noreturn));
long elf_load(uint64_t *pml4, const uint8_t *img, uint64_t size, uint64_t *entry);

/* ---- sched.c ---- */
#define MAX_TASKS 256
#define EVQ_SIZE 64
struct task {
    int pid, state;            /* state: 0 = free slot, else PS_* */
    bool user, killed, waiting_event;
    int busy;                  /* inside a long kernel operation: defer kills */
    char name[32];
    const char *app;
    uint64_t rsp, kstack_top;
    uint8_t *kstack;
    uint64_t *pml4;
    uint64_t wake_at, ticks, pages;
    uint64_t heap_top;         /* end of the app's heap (starts at USER_HEAP) */
    struct dunc_event evq[EVQ_SIZE]; int evh, evt;
    int nwin;
    uint8_t fx[512] __attribute__((aligned(16)));   /* the app's floating-point / SSE registers (FXSAVE) */
    char *capture; size_t cap_len, cap_max;
    char arg[256];             /* start-up argument (e.g. a file to open) */
    char appid[24];
    bool idle;                 /* a core's idle task */
    uint32_t alarm_ticks;      /* SYS_ALARM: timer ticks of the app's own running time left, 0 = none */
    uint64_t alarm_fn;
    int cpu;                   /* the core it last ran on */
    /* threads (SYS_THREAD): every thread of a program shares its memory (mm); proc is the program's first thread */
    struct mm *mm;
    struct task *proc;
    uint64_t fs_base;          /* thread-local storage (%fs) */
    uint64_t clear_tid;        /* when the thread ends: *(u32 *)clear_tid = 0, futex woken */
    struct lxproc *lx;         /* a Linux program (linux.c): its files etc., shared by its threads */
    struct mm *futex_mm; uint64_t futex_addr; bool futex_woken;   /* waiting in SYS_FUTEX */
};
struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err, rip, cs, rflags, rsp, ss;
};
extern struct task tasks[MAX_TASKS];
/* ---- cores (smp.c) ----
 * One core runs kernel code at a time (the "big kernel lock", taken on every way into the kernel and
 * dropped on the way back to an app); apps run on every core at once. Kernel threads stay on core 0. */
#define MAX_CPUS 64
struct tss64 { uint32_t r0; uint64_t rsp0, rsp1, rsp2, r1; uint64_t ist[7]; uint64_t r2; uint16_t r3, iomap; } __attribute__((packed));
struct cpu {
    struct cpu *self;                  /* %gs:0 */
    struct task *cur;                  /* %gs:8 */
    uint64_t sc_rsp;                   /* %gs:16 the app's stack pointer during a Linux syscall's entry */
    uint64_t sc_kstack;                /* %gs:24 the running task's kernel stack top (as in the TSS) */
    struct task *idle;
    int id; uint32_t apic_id;
    volatile bool bkl_held, started;
    uint64_t last_switch_tsc;
    uint64_t gdt[7] __attribute__((aligned(16)));
    struct tss64 tss;
    uint8_t *df_stack;
    volatile int in_user;              /* running an app's code (TLB shootdowns are sent to it) */
    uint64_t tlb_gen;                  /* the generation of the running program's page tables it has loaded */
    volatile uint64_t tlb_ack;
};
extern struct cpu cpus[MAX_CPUS];
extern int ncpus_on, ncpus_found;
static inline struct cpu *cpu_self(void) { struct cpu *c; __asm__ volatile("mov %%gs:0, %0" : "=r"(c)); return c; }
#ifdef HOST_TEST
extern struct task *current;
#else
#define current (cpu_self()->cur)
#endif
void bkl_lock(void);
void bkl_unlock(void);
struct blockdev;
void bcache_init(void);
void bcache_attach(struct blockdev *bd);
void bcache_detach(struct blockdev *bd);
uint64_t bcache_bytes(void);
uint64_t bcache_capacity(void);
void smp_init(void);
void smp_settled(void);                /* start-up went fine: forget the safety marker */                   /* find the cores; start them if the setting says so */
void cpu_setup(struct cpu *c);         /* GDT, TSS and %gs for a core */
extern volatile uint64_t sched_ticks;
extern bool sched_running;
void sched_init(void);
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg);
int task_spawn(const char *app);
int task_spawn_arg(const char *app, const char *arg);
void sched_start(void) __attribute__((noreturn));
void schedule(void);
void task_yield(void);
void task_sleep(uint64_t ms);
void task_exit(int code) __attribute__((noreturn));
bool task_kill(int pid);
void task_reap(void);
void event_push(struct task *t, const struct dunc_event *ev);
void sched_tick(struct frame *f);
void alarm_deliver(struct frame *f);
void kyield(void);
uint64_t cpu_time_us(void);   /* also brings the running task's CPU time up to date */         /* yield if the scheduler runs, else spin */
struct app_image { const char *id, *title; const uint8_t *start, *end; };
/* ---- apps.c: registry of built-in and installed (clan) apps ---- */
struct app_entry { char id[24], name[32], icon[16], version[12]; const struct app_image *builtin; bool installed; };
void apps_refresh(void);
int apps_count(void);
int apps_visible(void);                      /* the first ones: shown in the dock and menu (Protection Test isn't) */
const struct app_entry *apps_get(int i);
const struct app_entry *apps_find(const char *id);
void clan_command(int argc, char **argv);
const struct app_image *app_find(const char *id);
extern const struct app_image app_images[];
extern const int app_count;

void *kmalloc(size_t n);
void kfree(void *p);

/* ---- random.c ---- */
void random_init(void);
void random_bytes(void *out, size_t n);
void random_event(uint64_t x);

/* ---- syscall.c ---- */
uint64_t syscall_dispatch(struct frame *f);

/* ---- desktop.c (window manager + compositor) ---- */
struct window;
bool desktop_available(void);
void desktop_init(void);
void desktop_thread(void *arg);
struct window *wm_create(struct task *t, int w, int h, const char *title, uint32_t **buf, int *pages, uint64_t uva, int flags);
bool wm_resize(struct task *t, int id, int *nw, int *nh);
int wm_id(struct window *w);
bool wm_flush(struct task *t, int id, int x, int y, int w, int h);
bool wm_set_title(struct task *t, int id, const char *title);
bool wm_close(struct task *t, int id);
void wm_task_exit(struct task *t);
uint32_t *wm_buffer(struct task *t, int id, int *pages, int *w, int *h);
void wm_screen(int *w, int *h);
bool linux_window_event(struct task *t, const struct dunc_event *ev);   /* linux.c: true if it went to a Linux window */
int wm_count(struct task *t);
void desktop_notify(const char *title, const char *msg);
void boot_progress(int percent);

/* ---- bt.c (GUI API) ---- */
void bt_get_adapter(struct bt_adapter *a);
int bt_get_results(struct bt_device *out, int max);
int  bt_pair_start(const uint8_t *addr);
/* Bluetooth audio: sound goes to a paired speaker while bta_active() */
int  bt_audio_connect(const uint8_t *addr);
void bt_audio_stop(void);
void bt_audio_status(struct bt_audio *out);
bool bta_active(void);
long bta_write(const int16_t *pcm, long frames, int in_rate);   /* frames taken */
long bta_queued_ms(void);
void bta_flush(void);
void bta_pause(bool on);
void bt_pair_run(const uint8_t *addr);
void bt_pair_confirm(int yes);
void bt_pair_status(struct bt_pair *out);
int  bt_paired(struct bt_paired *out, int max);
void bt_forget(const uint8_t *addr);

#define LOG_OK(...)   do { console_set_color(C_LGREEN); kprintf("[ ok ] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_WARN(...) do { console_set_color(C_YELLOW); kprintf("[warn] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_INFO(...) do { console_set_color(C_LCYAN); kprintf("[info] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
