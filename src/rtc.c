/* CMOS real-time clock. */
#include "kernel.h"

static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }

void rtc_read(struct rtc_time *t) {
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++) cpu_relax();
    uint8_t s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), b = cmos(0x0B);
    bool pm = h & 0x80; h &= 0x7F;
    if (!(b & 4)) {
#define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        s = BCD(s); m = BCD(m); h = BCD(h); d = BCD(d); mo = BCD(mo); y = BCD(y);
    }
    if (!(b & 2)) { if (h == 12) h = 0; if (pm) h += 12; }
    t->sec = s; t->min = m; t->hour = h; t->day = d; t->mon = mo; t->year = 2000 + y;
}
