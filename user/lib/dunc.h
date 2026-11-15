/* libdunc: the AXIOM application runtime. */
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

/* HTTP and HTTPS. Returns the body length (truncated to max), or a negative error.
 * Follows redirects. status/ctype/final_url may be NULL. */
#define HTTP_EHTTPS -20
#define HTTP_EURL -21
#define HTTP_EBAD -22
#define HTTP_ECLOSED -23        /* the server closed the connection without answering */
#define HTTP_EHEADERS -24       /* the answer's headers were too big */
#define HTTP_EREDIRECTS -25     /* too many redirects (a loop) */
#define HTTP_EGZIP -26          /* the compressed page couldn't be unpacked */
#define HTTP_ENOTHTTP -27       /* the answer wasn't HTTP/1.x */
long http_get(const char *url, char *body, long max, int *status, char *ctype, int ctlen, char *final_url, int fulen);
/* Streaming version: sink() receives the body piece by piece (return false to stop).
 * *total is set to Content-Length when the server sends it, else -1. Returns bytes received or an error. */
typedef bool (*http_sink)(const char *data, long len, void *ctx);
long http_fetch(const char *url, http_sink sink, void *ctx, int *status, long *total, char *ctype, int ctlen, char *final_url, int fulen);
/* General request: method ("GET"/"POST"/...), optional body with its Content-Type. rq may be NULL (GET).
 * After a POST, 301/302/303 redirects continue as GET; 307/308 repeat the request. */
struct http_req { const char *method; const char *body; long body_len; const char *content_type; const char *referer;
                  const char *extra_headers;   /* optional "Name: value\r\n" lines */
                  long timeout_ms; };          /* optional: how long to wait for data (default 15 s) */
extern bool (*http_wait_hook)(void);   /* optional: called ~4x a second while waiting for a server; false = give up */
long http_request(const char *url, const struct http_req *rq, http_sink sink, void *ctx, int *status, long *total,
                  char *ctype, int ctlen, char *final_url, int fulen);
/* Options for this program (default 0): HTTP_OPT_COOKIES keeps a cookie jar (persistent cookies are saved in
 * /home/.web/cookies); HTTP_OPT_GZIP asks for gzip-compressed responses and decodes them. */
#define HTTP_OPT_COOKIES 1
#define HTTP_OPT_GZIP 2
extern int http_options;
int http_cookie_count(void);
void http_cookies_clear(void);
/* "name=value; ..." for a listing (about:cookies style): returns lines "domain path name" for each cookie */
int http_cookie_list(char *out, int max);
const char *http_strerror(long e);
/* security of the last http_fetch/http_get: secure = 1 for a verified https:// connection */
struct http_security { int secure; char cipher[48], subject[96], issuer[96]; char addr[IP_STRLEN]; };   /* addr: the server address used */
extern struct http_security http_sec;
int http_trusted_roots(void);
int http_trust_pem(const char *pem, long n);   /* trust these CA certificates in this program too */
static inline void sys_random(void *b, long n) { sys_call(SYS_RANDOM, (long)b, n, 0, 0, 0); }
static inline int sys_setting_get(const char *key, char *out, int n) { return sys_call(SYS_SETTING_GET, (long)key, (long)out, n, 0, 0); }
static inline int sys_setting_set(const char *key, const char *val) { return sys_call(SYS_SETTING_SET, (long)key, (long)val, 0, 0, 0); }
/* the shared clipboard: plain text */
static inline void clip_set(const char *s, long n) { sys_call(SYS_CLIP_SET, (long)s, n, 0, 0, 0); }
static inline long clip_get(char *buf, long max) { return sys_call(SYS_CLIP_GET, (long)buf, max, 0, 0, 0); }
/* paste the clipboard's first line onto the end of a text field; returns how many characters were added */
int clip_paste_line(char *buf, int *len, int max);
/* which app opens a file: "viewer", "web", "editor", or 0 if none can */
const char *app_for_file(const char *path);
/* open a file with its app (installing it with clan first if needed); 0, or a message why not */
const char *open_file(const char *path);
static inline int sys_disks(struct dunc_disk *out, int max) { return sys_call(SYS_DISK_LIST, (long)out, max, 0, 0, 0); }
static inline int sys_install(int disk_id, int mode) { return sys_call(SYS_DISK_INSTALL, disk_id, mode, 0, 0, 0); }
static inline void sys_install_status(struct dunc_install *st) { sys_call(SYS_DISK_STATUS, (long)st, 0, 0, 0, 0); }
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

/* Alarms: after ms of the app's own running time (waiting doesn't count), fn(frame) is called wherever the
 * app is; it can jump out (dunc_longjmp) or carry on where it was (sys_sigreturn(frame)). 0 cancels. */
static inline void sys_alarm(long ms, void (*fn)(void *frame)) { sys_call(SYS_ALARM, ms, (long)fn, 0, 0, 0); }
static inline void sys_sigreturn(void *frame) { sys_call(SYS_SIGRETURN, (long)frame, 0, 0, 0, 0); }
typedef uint64_t dunc_jmp[8];
int dunc_setjmp(dunc_jmp b) __attribute__((returns_twice));
void dunc_longjmp(dunc_jmp b, int v) __attribute__((noreturn));
extern volatile int malloc_busy;
void app_flush_rect(struct app *a, int x, int y, int w, int h);
static inline void app_title(struct app *a, const char *t) { sys_call(SYS_WIN_TITLE, a->wi.id, (long)t, 0, 0, 0); }

/* memory */
/* a key event that types a character (Windows-1252) */
static inline bool key_is_char(int k) { return k >= 32 && k <= 255 && k != 127; }
uint32_t accent_from_setting(const char *v);
void *malloc(size_t n);
void free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t a, size_t b);

/* theme */
/* Highland theme (common/theme_highland.h) */
#include "../../common/theme_highland.h"
#define UI_BG      HL_WIN_BG
#define UI_PANEL   0x1A1A19
#define UI_PANEL2  0x232321
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
