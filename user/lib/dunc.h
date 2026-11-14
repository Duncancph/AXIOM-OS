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
static inline long fs_append(const char *p, const void *buf, uint64_t len) { return sys_call(SYS_FS_APPEND, (long)p, (long)buf, len, 0, 0); }
static inline int fs_copy(const char *a, const char *b) { return sys_call(SYS_FS_COPY, (long)a, (long)b, 0, 0, 0); }
static inline int fs_stat(const char *p, struct dunc_stat *st) { return sys_call(SYS_FS_STAT, (long)p, (long)st, 0, 0, 0); }
static inline int fs_volumes(struct dunc_volume *v, int max) { return sys_call(SYS_FS_VOLUMES, (long)v, max, 0, 0, 0); }
const char *fs_strerror(int e);

/* networking */
static inline void sys_netinfo(struct dunc_netinfo *n) { sys_call(SYS_NET_INFO, (long)n, 0, 0, 0, 0); }
static inline void sys_set_dns(const struct ipaddr *ip) { sys_call(SYS_NET_CONFIG, NETCFG_DNS, (long)ip, 0, 0, 0); }   /* 0: automatic */
static inline void sys_net_renew(void) { sys_call(SYS_NET_CONFIG, NETCFG_RENEW, 0, 0, 0, 0); }
static inline long sys_ping(const struct ipaddr *ip, uint32_t ms) { return sys_call(SYS_NET_PING, (long)ip, ms, 0, 0, 0); }
/* up to max addresses for a name or address literal, best first; returns how many, or an error */
static inline int sys_resolve(const char *name, struct ipaddr *out, int max) { return sys_call(SYS_NET_RESOLVE, (long)name, (long)out, max, 0, 0); }
static inline int tcp_open(const struct ipaddr *ip, int port, uint32_t timeout_ms) { return sys_call(SYS_TCP_CONNECT, (long)ip, port, timeout_ms, 0, 0); }
static inline long tcp_write(int h, const void *b, uint32_t n) { return sys_call(SYS_TCP_SEND, h, (long)b, n, 0, 0); }
static inline long tcp_read(int h, void *b, uint32_t n, long timeout_ms) { return sys_call(SYS_TCP_RECV, h, (long)b, n, timeout_ms, 0); }
static inline int tcp_shut(int h) { return sys_call(SYS_TCP_CLOSE, h, 0, 0, 0, 0); }
const char *net_strerror(int e);
void ip_str(uint32_t ip, char *out);          /* out: at least 16 bytes */

/* HTTP (http:// only for now). Returns the body length (truncated to max), or a negative error.
 * Follows redirects. status/ctype/final_url may be NULL. */
#define HTTP_EHTTPS -20
#define HTTP_EURL -21
#define HTTP_EBAD -22
long http_get(const char *url, char *body, long max, int *status, char *ctype, int ctlen, char *final_url, int fulen);
/* Streaming version: sink() receives the body piece by piece (return false to stop).
 * *total is set to Content-Length when the server sends it, else -1. Returns bytes received or an error. */
typedef bool (*http_sink)(const char *data, long len, void *ctx);
long http_fetch(const char *url, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen, char *final_url, int fulen);
const char *http_strerror(long e);
/* security of the last http_fetch/http_get: secure = 1 for a verified https:// connection */
struct http_security { int secure; char cipher[48], subject[96], issuer[96]; char addr[IP_STRLEN]; };   /* addr: the server address used */
extern struct http_security http_sec;
int http_trusted_roots(void);
int http_trust_pem(const char *pem, long n);   /* trust these CA certificates in this program too */
static inline void sys_random(void *b, long n) { sys_call(SYS_RANDOM, (long)b, n, 0, 0, 0); }
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
