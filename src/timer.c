/* Time keeping via the CPU timestamp counter, calibrated from CPUID or the PIT. */
#include "kernel.h"

static uint64_t hz, boot_tsc;

static uint64_t calibrate_pit(void) {
    /* PIT channel 2, one-shot 10 ms, polled via port 0x61 */
    uint8_t v = inb(0x61);
    outb(0x61, (v & ~0x02) | 0x01);
    outb(0x43, 0xB0);
    uint16_t cnt = 11932;   /* 1193182 Hz / 100 */
    outb(0x42, cnt & 0xFF); outb(0x42, cnt >> 8);
    v = inb(0x61);
    outb(0x61, v & ~1); outb(0x61, v | 1);
    uint64_t t0 = rdtsc();
    while (!(inb(0x61) & 0x20))
        if (rdtsc() - t0 > 2000000000ULL) return 0;   /* no PIT on this machine */
    return (rdtsc() - t0) * 100;
}

void timer_init(void) {
    boot_tsc = rdtsc();
    uint32_t a, b, c, d, max;
    cpuid(0, 0, &max, &b, &c, &d);
    if (max >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c) hz = (uint64_t)c * b / a;
    }
    if (!hz) {
        /* take the best of a few PIT runs */
        for (int i = 0; i < 3; i++) { uint64_t h = calibrate_pit(); if (h > hz) hz = h; }
    }
    if (!hz && max >= 0x16) { cpuid(0x16, 0, &a, &b, &c, &d); hz = (uint64_t)(a & 0xFFFF) * 1000000; }
    if (!hz) hz = 2000000000ULL;
}

uint64_t tsc_hz(void) { return hz; }
/* One clock for every core: the cores' time-stamp counters may differ a little (they do under emulation), so
 * the clock is the highest any core has seen and never goes back, whichever core reads it. */
static volatile uint64_t last_tsc;
static uint64_t mono_tsc(void) {
    uint64_t t = rdtsc(), l = __atomic_load_n(&last_tsc, __ATOMIC_RELAXED);
    while (t > l) if (__atomic_compare_exchange_n(&last_tsc, &l, t, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return t;
    return l;
}
uint64_t uptime_us(void) { return (mono_tsc() - boot_tsc) / (hz / 1000000); }
uint64_t uptime_ns(void) { uint64_t t = mono_tsc() - boot_tsc; return t / hz * 1000000000ull + t % hz * 1000000000ull / hz; }
uint64_t uptime_ms(void) { return (mono_tsc() - boot_tsc) / (hz / 1000); }
void udelay(uint64_t us) {
    uint64_t end = rdtsc() + us * (hz / 1000000);
    while (rdtsc() < end) cpu_relax();
}

void sleep_ms(uint64_t ms) {
    uint64_t end = rdtsc() + ms * (hz / 1000);
    while (rdtsc() < end) cpu_relax();
}
