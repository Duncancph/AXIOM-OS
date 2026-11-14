/* libdunc: the DuncOS application runtime. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../../common/abi.h"
#include "../../common/str.h"
#include "../../common/gfx.h"

typedef int bool;
#define true 1
#define false 0

/* system calls */
long sys_call(long n, long a, long b, long c, long d, long e);
static inline void sys_exit(int code) { sys_call(SYS_EXIT, code, 0, 0, 0, 0); }
static inline void sys_sleep(uint64_t ms) { sys_call(SYS_SLEEP, ms, 0, 0, 0, 0); }
static inline uint64_t sys_time(void) { return sys_call(SYS_TIME, 0, 0, 0, 0, 0); }
static inline void sys_log(const char *s) { sys_call(SYS_LOG, (long)s, 0, 0, 0, 0); }
static inline int sys_event(struct dunc_event *e, long timeout_ms) { return sys_call(SYS_EVENT, (long)e, timeout_ms, 0, 0, 0); }
static inline int sys_spawn(const char *app) { return sys_call(SYS_SPAWN, (long)app, 0, 0, 0, 0); }
static inline int sys_procs(struct procinfo *p, int max) { return sys_call(SYS_PROCS, (long)p, max, 0, 0, 0); }
static inline int sys_kill(int pid) { return sys_call(SYS_KILL, pid, 0, 0, 0, 0); }
static inline long sys_kcmd(const char *cmd, char *buf, long len) { return sys_call(SYS_KCMD, (long)cmd, (long)buf, len, 0, 0); }
static inline long sys_bt(int op, void *buf, long n) { return sys_call(SYS_BT, op, (long)buf, n, 0, 0); }
static inline void sys_sysinfo(struct sysinfo *s) { sys_call(SYS_SYSINFO, (long)s, 0, 0, 0, 0); }
static inline void sys_power(int reboot) { sys_call(SYS_POWER, reboot, 0, 0, 0, 0); }
static inline void sys_rtc(struct rtc_time *t) { sys_call(SYS_RTC, (long)t, 0, 0, 0, 0); }
static inline void sys_notify(const char *t, const char *m) { sys_call(SYS_NOTIFY, (long)t, (long)m, 0, 0, 0); }

/* windows */
struct app { struct win_info wi; struct canvas c; };
bool app_open(struct app *a, int w, int h, const char *title);
void app_flush(struct app *a);
void app_flush_rect(struct app *a, int x, int y, int w, int h);
static inline void app_title(struct app *a, const char *t) { sys_call(SYS_WIN_TITLE, a->wi.id, (long)t, 0, 0, 0); }

/* memory */
void *malloc(size_t n);

/* theme */
#define UI_BG      0x141821
#define UI_PANEL   0x1C2230
#define UI_PANEL2  0x232A3A
#define UI_BORDER  0x2E3648
#define UI_TEXT    0xE8EBF2
#define UI_MUTED   0x8E97AA
#define UI_ACCENT  0x4FD1E8
#define UI_GOLD    0xFFC83D
#define UI_GREEN   0x34D399
#define UI_RED     0xE5484D

enum { BTN_NORMAL, BTN_PRIMARY, BTN_DANGER };
void ui_button(struct canvas *c, int x, int y, int w, int h, const char *label, int style, bool hover, bool disabled);
void ui_card(struct canvas *c, int x, int y, int w, int h);
static inline bool ui_in(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
