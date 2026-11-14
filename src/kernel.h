#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "../common/abi.h"

#define OS_NAME "DuncOS"
#define KERNEL_NAME "Thane"
#define OS_VERSION "0.2"
#ifndef OS_BUILD
#define OS_BUILD 0                       /* set by the Makefile: the number of commits */
#endif
/* where Settings looks for updates (a signed manifest; see tools/make-release.py); "" = not set up */
#define UPDATE_URL ""

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
void sleep_ms(uint64_t ms);
void udelay(uint64_t us);
/* keymap.c */
void kbd_hid_key(uint8_t usage, uint8_t mods);
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
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off);
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off);
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v);
uint64_t pci_bar(struct pci_dev *p, int bar, uint64_t *size);
const char *pci_class_name(uint8_t cls, uint8_t sub, uint8_t pif);

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
void lapic_timer_start(uint32_t hz);
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
long elf_load(uint64_t *pml4, const uint8_t *img, uint64_t size, uint64_t *entry);

/* ---- sched.c ---- */
#define MAX_TASKS 32
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
    char *capture; size_t cap_len, cap_max;
    char arg[256];             /* start-up argument (e.g. a file to open) */
    char appid[24];
};
struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err, rip, cs, rflags, rsp, ss;
};
extern struct task tasks[MAX_TASKS];
extern struct task *current;
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
void kyield(void);
uint64_t cpu_time_us(void);   /* also brings the running task's CPU time up to date */         /* yield if the scheduler runs, else spin */
struct app_image { const char *id, *title; const uint8_t *start, *end; };
/* ---- apps.c: registry of built-in and installed (clan) apps ---- */
struct app_entry { char id[24], name[32], icon[16], version[12]; const struct app_image *builtin; bool installed; };
void apps_refresh(void);
int apps_count(void);
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
int wm_count(struct task *t);
void desktop_notify(const char *title, const char *msg);
void boot_progress(int percent);

/* ---- bt.c (GUI API) ---- */
void bt_get_adapter(struct bt_adapter *a);
int bt_get_results(struct bt_device *out, int max);
int  bt_pair_start(const uint8_t *addr);
void bt_pair_run(const uint8_t *addr);
void bt_pair_confirm(int yes);
void bt_pair_status(struct bt_pair *out);
int  bt_paired(struct bt_paired *out, int max);
void bt_forget(const uint8_t *addr);

#define LOG_OK(...)   do { console_set_color(C_LGREEN); kprintf("[ ok ] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_WARN(...) do { console_set_color(C_YELLOW); kprintf("[warn] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_INFO(...) do { console_set_color(C_LCYAN); kprintf("[info] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
