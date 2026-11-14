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
static inline int sys_spawn_arg(const char *app, const char *arg) { return sys_call(SYS_SPAWN, (long)app, (long)arg, 0, 0, 0); }
static inline int sys_getarg(char *buf, long n) { return sys_call(SYS_GETARG, (long)buf, n, 0, 0, 0); }
static inline int sys_apps(struct dunc_app *a, int max) { return sys_call(SYS_APPS, (long)a, max, 0, 0, 0); }
static inline int fs_list(const char *p, struct dunc_dirent *d, int max) { return sys_call(SYS_FS_LIST, (long)p, (long)d, max, 0, 0); }
static inline long fs_read(const char *p, void *buf, uint64_t off, uint64_t len) { return sys_call(SYS_FS_READ, (long)p, (long)buf, off, len, 0); }
static inline long fs_write(const char *p, const void *buf, uint64_t len) { return sys_call(SYS_FS_WRITE, (long)p, (long)buf, len, 0, 0); }
static inline int fs_mkdir(const char *p) { return sys_call(SYS_FS_MKDIR, (long)p, 0, 0, 0, 0); }
static inline int fs_delete(const char *p) { return sys_call(SYS_FS_DELETE, (long)p, 0, 0, 0, 0); }
static inline int fs_rename(const char *a, const char *b) { return sys_call(SYS_FS_RENAME, (long)a, (long)b, 0, 0, 0); }
static inline int fs_copy(const char *a, const char *b) { return sys_call(SYS_FS_COPY, (long)a, (long)b, 0, 0, 0); }
static inline int fs_stat(const char *p, struct dunc_stat *st) { return sys_call(SYS_FS_STAT, (long)p, (long)st, 0, 0, 0); }
static inline int fs_volumes(struct dunc_volume *v, int max) { return sys_call(SYS_FS_VOLUMES, (long)v, max, 0, 0, 0); }
const char *fs_strerror(int e);
/* join a directory and a name / resolve a user path (supports ~, ., ..) into out */
void path_resolve(const char *cwd, const char *in, char *out, int n);
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
bool app_open_ex(struct app *a, int w, int h, const char *title, int flags);   /* flags: WIN_RESIZABLE */
bool app_resize(struct app *a, const struct dunc_event *e);   /* handle EV_RESIZE; true if resized */
void app_flush(struct app *a);
void app_flush_rect(struct app *a, int x, int y, int w, int h);
static inline void app_title(struct app *a, const char *t) { sys_call(SYS_WIN_TITLE, a->wi.id, (long)t, 0, 0, 0); }

/* memory */
void *malloc(size_t n);

/* theme */
/* Highland theme (common/theme_highland.h) */
#include "../../common/theme_highland.h"
#define UI_BG      HL_WIN_BG
#define UI_PANEL   0x152039
#define UI_PANEL2  0x1B2945
#define UI_BORDER  HL_WIN_BORDER
#define UI_TEXT    HL_TEXT
#define UI_MUTED   HL_TEXT_FAINT
#define UI_ACCENT  HL_ACCENT
#define UI_GOLD    HL_GOLD
#define UI_GREEN   HL_GREEN
#define UI_RED     HL_RED

enum { BTN_NORMAL, BTN_PRIMARY, BTN_DANGER };
void ui_button(struct canvas *c, int x, int y, int w, int h, const char *label, int style, bool hover, bool disabled);
void ui_card(struct canvas *c, int x, int y, int w, int h);
static inline bool ui_in(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
