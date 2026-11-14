/* DuncOS kernel <-> application interface (shared by the Thane kernel and user programs). */
#pragma once
#include <stdint.h>

#define USER_BASE      0x0000008000000000ULL   /* programs are loaded here */
#define USER_STACK_TOP 0x0000008040000000ULL
#define USER_WIN_BASE  0x0000008080000000ULL   /* window pixel buffers are mapped here */
#define USER_HEAP      0x0000008100000000ULL   /* grows upwards with SYS_MEM_GROW */
#define USER_END       0x0000010000000000ULL

enum {
    SYS_EXIT, SYS_YIELD, SYS_SLEEP, SYS_TIME, SYS_LOG,
    SYS_WIN_CREATE, SYS_WIN_FLUSH, SYS_EVENT, SYS_WIN_TITLE, SYS_WIN_CLOSE,
    SYS_SPAWN, SYS_PROCS, SYS_KILL, SYS_KCMD, SYS_BT, SYS_SYSINFO, SYS_POWER, SYS_RTC, SYS_NOTIFY,
    SYS_FS_LIST, SYS_FS_READ, SYS_FS_WRITE, SYS_FS_MKDIR, SYS_FS_DELETE, SYS_FS_RENAME, SYS_FS_STAT,
    SYS_FS_VOLUMES, SYS_GETARG, SYS_WIN_RESIZE, SYS_APPS, SYS_FS_COPY,
    SYS_RANDOM, SYS_FS_APPEND, SYS_NET_INFO, SYS_NET_CONFIG, SYS_NET_PING, SYS_NET_RESOLVE, SYS_TCP_CONNECT, SYS_TCP_SEND, SYS_TCP_RECV, SYS_TCP_CLOSE,
    SYS_DISK_LIST, SYS_DISK_INSTALL, SYS_DISK_STATUS, SYS_MEM_GROW, SYS_SETTING_GET, SYS_SETTING_SET, SYS_SOUND, SYS_LOCK,
    SYS_COUNT
};

/* keys (besides printable ASCII, '\n', '\b', '\t', 27 = Esc) */
/* Keys: characters are 1..255 (Windows-1252; Ctrl+A..Z = 1..26); special keys from 0x100.
 * Shift/Ctrl/Alt/Super held with a special key, or Ctrl+Shift / Alt / Super with a character, add KEYMOD_* bits. */
enum { KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN, KEY_INS, KEY_PRTSC,
       KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SUPER };
#define KEYMOD_SHIFT 0x1000
#define KEYMOD_CTRL  0x2000
#define KEYMOD_ALT   0x4000
#define KEYMOD_SUPER 0x8000
#define KEY_CODE(k)  ((k) & 0xFFF)

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

struct rtc_time { int year, mon, day, hour, min, sec, utc_offset_min; };   /* SYS_RTC: local time; UTC = local - offset */

/* ---- files ---- */
#define PATH_MAX_ 256
struct dunc_dirent { char name[64]; int is_dir; uint64_t size; };
struct dunc_stat { int is_dir, volume; uint64_t size; };
struct dunc_volume { char path[16], label[32], kind[16]; uint64_t total, free; int writable; };
/* ---- disks (SATA, NVMe, USB) and the installer ---- */
/* SYS_SOUND operations */
enum { SOUND_TEST, SOUND_PLAY_PCM, SOUND_INFO, SOUND_EVENT };

enum { DISK_SATA = 1, DISK_NVME, DISK_USB };
struct dunc_disk {
    int id, kind, nparts;
    char model[48];
    uint64_t bytes; uint32_t bsize;
    int duncos;                      /* has a DuncOS partition */
    int system;                      /* it holds /home right now */
    struct { uint64_t bytes; char fs[12], label[24]; } part[8];
};
enum { INSTALL_ERASE = 1, INSTALL_UPDATE };   /* update: new system files, keep the user's files */
struct dunc_install { int state, percent; char msg[120]; };   /* state: 0 idle, 1 working, 2 done, 3 failed */
enum { FS_OK = 0, FS_ENOENT = -1, FS_EEXIST = -2, FS_ENOTDIR = -3, FS_EISDIR = -4, FS_ENOSPC = -5,
       FS_EIO = -6, FS_ENOTEMPTY = -7, FS_EROFS = -8, FS_EINVAL = -9 };

/* ---- registered apps (built-in + installed with clan) ---- */
struct dunc_app { char id[24], name[32]; int installed; };

/* window flags */
#define WIN_RESIZABLE 1

/* ---- networking ---- */
#include "inet.h"
/* IPv4 addresses are kept in network byte order: a.b.c.d == a | b<<8 | c<<16 | d<<24 */
#define IP4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
struct dunc_netinfo {
    int present, link, configured, dns_manual;   /* configured: has an IPv4 address from DHCP */
    char adapter[48], driver[24];
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns, lease_s;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    /* IPv6 */
    int v6_configured;               /* has a global IPv6 address and a router */
    int v6_count;
    struct ipaddr v6[4];             /* v6[0] is the link-local address; .prefix is the prefix length */
    uint8_t v6_state[4];             /* V6_* */
    uint32_t v6_valid_s[4];          /* seconds left; 0xFFFFFFFF = forever */
    struct ipaddr router6;           /* IPv6 default router (all zero: none) */
    struct ipaddr dns6;              /* IPv6 DNS server from the network (all zero: none) */
    struct ipaddr dns_user;          /* DNS server set by the user (when dns_manual) */
    struct ipaddr dns_now;           /* the DNS server lookups use now */
    uint32_t mtu6;
};
enum { V6_TENTATIVE = 1, V6_PREFERRED, V6_DEPRECATED, V6_DUPLICATE };
enum { NET_OK = 0, NET_ENODEV = -1, NET_ENOTCONN = -2, NET_ETIMEDOUT = -3, NET_EREFUSED = -4, NET_ERESET = -5,
       NET_ENOTFOUND = -6, NET_EINVAL = -7, NET_ENOMEM = -8, NET_EAGAIN = -9, NET_EUNREACH = -10 };
enum { NETCFG_DNS, NETCFG_RENEW };
