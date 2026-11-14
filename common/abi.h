/* DuncOS kernel <-> application interface (shared by the Thane kernel and user programs). */
#pragma once
#include <stdint.h>

#define USER_BASE      0x0000008000000000ULL   /* programs are loaded here */
#define USER_STACK_TOP 0x0000008040000000ULL
#define USER_WIN_BASE  0x0000008080000000ULL   /* window pixel buffers are mapped here */
#define USER_END       0x0000010000000000ULL

enum {
    SYS_EXIT, SYS_YIELD, SYS_SLEEP, SYS_TIME, SYS_LOG,
    SYS_WIN_CREATE, SYS_WIN_FLUSH, SYS_EVENT, SYS_WIN_TITLE, SYS_WIN_CLOSE,
    SYS_SPAWN, SYS_PROCS, SYS_KILL, SYS_KCMD, SYS_BT, SYS_SYSINFO, SYS_POWER, SYS_RTC, SYS_NOTIFY,
    SYS_FS_LIST, SYS_FS_READ, SYS_FS_WRITE, SYS_FS_MKDIR, SYS_FS_DELETE, SYS_FS_RENAME, SYS_FS_STAT,
    SYS_FS_VOLUMES, SYS_GETARG, SYS_WIN_RESIZE, SYS_APPS, SYS_FS_COPY,
    SYS_RANDOM, SYS_FS_APPEND, SYS_NET_INFO, SYS_NET_CONFIG, SYS_NET_PING, SYS_NET_RESOLVE, SYS_TCP_CONNECT, SYS_TCP_SEND, SYS_TCP_RECV, SYS_TCP_CLOSE,
    SYS_COUNT
};

/* keys (besides printable ASCII, '\n', '\b', '\t', 27 = Esc) */
enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN };

enum { EV_NONE, EV_KEY, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_MOVE, EV_CLOSE, EV_FOCUS, EV_BLUR, EV_MOUSE_LEAVE, EV_WHEEL,
       EV_RESIZE /* x,y = new content size: call sys_win_resize */, EV_FILES_CHANGED };
struct dunc_event { int type, win, x, y, key, button; };

struct win_info { int id; uint32_t *buf; int w, h; };

enum { PS_READY = 1, PS_RUNNING, PS_SLEEPING, PS_BLOCKED, PS_ZOMBIE };
struct procinfo { int pid; int state; int user; int windows; uint64_t cpu_us; uint64_t mem_kb; char name[32]; };

struct sysinfo {
    uint64_t mem_total, mem_used, uptime_ms, cpu_clock_us;
    int fb_w, fb_h, nprocs, uefi;
    char cpu[64];
    char kernel[32];
};

enum { BT_INFO, BT_SCAN, BT_RESULTS };
struct bt_adapter { int present, ok, le; char maker[32], version[16], addr[18], name[64], usb[96]; };
struct bt_device { uint8_t addr[6]; int le, rssi; uint32_t cod; char name[64]; char kind[16]; };

struct rtc_time { int year, mon, day, hour, min, sec; };

/* ---- files ---- */
#define PATH_MAX_ 256
struct dunc_dirent { char name[64]; int is_dir; uint64_t size; };
struct dunc_stat { int is_dir, volume; uint64_t size; };
struct dunc_volume { char path[16], label[32], kind[16]; uint64_t total, free; int writable; };
enum { FS_OK = 0, FS_ENOENT = -1, FS_EEXIST = -2, FS_ENOTDIR = -3, FS_EISDIR = -4, FS_ENOSPC = -5,
       FS_EIO = -6, FS_ENOTEMPTY = -7, FS_EROFS = -8, FS_EINVAL = -9 };

/* ---- registered apps (built-in + installed with clan) ---- */
struct dunc_app { char id[24], name[32]; int installed; };

/* window flags */
#define WIN_RESIZABLE 1

/* ---- networking ---- */
/* IPv4 addresses are kept in network byte order: a.b.c.d == a | b<<8 | c<<16 | d<<24 */
#define IP4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
struct dunc_netinfo {
    int present, link, configured, dns_manual;
    char adapter[48], driver[24];
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns, lease_s;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};
enum { NET_OK = 0, NET_ENODEV = -1, NET_ENOTCONN = -2, NET_ETIMEDOUT = -3, NET_EREFUSED = -4, NET_ERESET = -5,
       NET_ENOTFOUND = -6, NET_EINVAL = -7, NET_ENOMEM = -8, NET_EAGAIN = -9 };
enum { NETCFG_DNS, NETCFG_RENEW };
