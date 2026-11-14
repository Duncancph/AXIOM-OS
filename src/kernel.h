#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define EMBER_VERSION "0.1"

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

/* ---- lib.c ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy0(char *d, const char *s, size_t n);   /* always NUL-terminates */
int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
void kprintf(const char *fmt, ...);
long katoi(const char *s);

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
    char cmdline[128];
    bool uefi;
};
extern struct bootinfo bootinfo;

/* ---- mem.c ---- */
void mem_init(void);
void *page_alloc(size_t pages);      /* zeroed, 4 KiB aligned, below 4 GiB, never freed */
void map_mmio(uint64_t phys, uint64_t size);
uint64_t mem_total(void);
uint64_t mem_used(void);

/* ---- idt.c / timer.c ---- */
void idt_init(void);
void timer_init(void);
uint64_t uptime_ms(void);
void sleep_ms(uint64_t ms);
uint64_t tsc_hz(void);

/* ---- input (kbd.c) ---- */
enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL };
void ps2_init(void);
void input_push(int c);
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

#define LOG_OK(...)   do { console_set_color(C_LGREEN); kprintf("[ ok ] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_WARN(...) do { console_set_color(C_YELLOW); kprintf("[warn] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
#define LOG_INFO(...) do { console_set_color(C_LCYAN); kprintf("[info] "); console_set_color(C_LGRAY); kprintf(__VA_ARGS__); } while (0)
