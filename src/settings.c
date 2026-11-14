/* System settings: a small key=value store saved in /system/config/settings.ini (on the DuncOS disk
 * when installed), plus time zones. Settings is the app that changes them; the kernel applies them.
 *   keyboard.layout   us | uk | de | fr | es
 *   time.zone         e.g. Europe/London (see zones[] below)
 *   time.rtc_local    1 if the PC's clock keeps local time (as Windows sets it), else it's UTC
 *   time.24h          1 for a 24-hour clock
 *   display.mode      e.g. 1920x1080 (used from the next start; empty = automatic)
 *   sound.volume      0..100
 *   lock.password     (stored as a salted hash; apps only see "set" or "")
 *   lock.idle_min     lock after this many idle minutes (0 = never)
 *   screen.off_min    blank the screen after this many idle minutes (0 = never) */
#include "kernel.h"
#include "vfs.h"
#include "disk.h"
#include "../user/lib/crypto/crypto.h"

#define NSET 32
static struct { char key[32], val[112]; } set[NSET];
static int nset;
static const char *PATH = "/system/config/settings.ini";

static const char *defaults[][2] = {
    { "keyboard.layout", "us" }, { "time.zone", "UTC" }, { "time.rtc_local", "0" }, { "time.24h", "0" },
    { "display.mode", "" }, { "sound.volume", "70" }, { "lock.password", "" }, { "lock.idle_min", "0" }, { "screen.off_min", "10" },
};

const char *settings_get(const char *key) {
    for (int i = 0; i < nset; i++) if (!strcmp(set[i].key, key)) return set[i].val;
    for (unsigned i = 0; i < sizeof defaults / sizeof defaults[0]; i++) if (!strcmp(defaults[i][0], key)) return defaults[i][1];
    return "";
}
int settings_int(const char *key) { return (int)katoi(settings_get(key)); }

static void store(const char *key, const char *val) {
    for (int i = 0; i < nset; i++) if (!strcmp(set[i].key, key)) { strncpy0(set[i].val, val, sizeof set[i].val); return; }
    if (nset < NSET) { strncpy0(set[nset].key, key, sizeof set[nset].key); strncpy0(set[nset].val, val, sizeof set[nset].val); nset++; }
}

static void apply(const char *key) {
    if (!strcmp(key, "keyboard.layout")) kbd_set_layout(settings_get(key));
    if (!strcmp(key, "display.mode")) {
        char m[40] = "";
        if (settings_get(key)[0]) ksnprintf(m, sizeof m, "%sx32", settings_get(key));
        disk_set_boot_option("gfxmode", m);
    }
}

static void save(void) {
    static char buf[NSET * 150];
    int k = ksnprintf(buf, sizeof buf, "# DuncOS settings (change them in Settings)\n");
    for (int i = 0; i < nset; i++) k += ksnprintf(buf + k, sizeof buf - k, "%s=%s\n", set[i].key, set[i].val);
    vfs_mkdir("/system/config");
    vfs_write(PATH, buf, k);
}

void settings_load(void) {
    char buf[NSET * 150];
    long n = vfs_read(PATH, buf, 0, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; *line;) {
        char *nl = line; while (*nl && *nl != '\n') nl++;
        char save_c = *nl; *nl = 0;
        char *eq = line; while (*eq && *eq != '=') eq++;
        if (*line != '#' && *eq) { *eq = 0; char *v = eq + 1; int l = strlen(v); if (l && v[l - 1] == '\r') v[l - 1] = 0; store(line, v); }
        if (!save_c) break;
        line = nl + 1;
    }
    for (int i = 0; i < nset; i++) apply(set[i].key);
    LOG_OK("Settings loaded (keyboard %s, time zone %s)\n", settings_get("keyboard.layout"), settings_get("time.zone"));
}

/* ---- the lock screen password: 16-byte salt, 20000 rounds of SHA-256 ---- */
static void hash_pw(const char *pw, const uint8_t *salt, uint8_t out[32]) {
    uint8_t h[32]; sha256_ctx c;
    sha256_init(&c); sha256_update(&c, salt, 16); sha256_update(&c, pw, strlen(pw)); sha256_final(&c, h);
    for (int i = 0; i < 20000; i++) { sha256_init(&c); sha256_update(&c, h, 32); sha256_update(&c, salt, 16); sha256_final(&c, h); }
    memcpy(out, h, 32);
}
static void hex(const uint8_t *b, int n, char *out) { for (int i = 0; i < n; i++) ksnprintf(out + 2 * i, 3, "%02x", b[i]); }
static int unhex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = s[2 * i + k]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1000); }
        if (v < 0) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

bool settings_check_password(const char *pw) {
    const char *s = settings_get("lock.password");
    if (!s[0]) return true;
    uint8_t salt[16], want[32], got[32];
    if (strlen(s) != 32 + 1 + 64 || s[32] != '$' || unhex(s, salt, 16) || unhex(s + 33, want, 32)) return false;
    hash_pw(pw, salt, got);
    uint8_t d = 0; for (int i = 0; i < 32; i++) d |= got[i] ^ want[i];
    return d == 0;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d);
void civil_from_days(int64_t z, int *y, int *m, int *d);
int tz_offset_zone(const char *z, int64_t utc);
int tz_list(char *out, int n);
static int tz_offset(int64_t utc) { return tz_offset_zone(settings_get("time.zone"), utc); }

/* the clock in local time, with the offset from UTC filled in */
void rtc_read_local(struct rtc_time *t) {
    struct rtc_time r; rtc_read(&r);
    int64_t s = days_from_civil(r.year, r.mon, r.day) * 86400 + r.hour * 3600 + r.min * 60 + r.sec;
    int64_t utc = s;
    if (settings_int("time.rtc_local")) utc = s - tz_offset(s) * 60;   /* close enough at the changeover hour */
    int off = tz_offset(utc);
    int64_t local = utc + off * 60;
    int y, m, d; civil_from_days(local / 86400, &y, &m, &d);
    int64_t sec = local % 86400; if (sec < 0) sec += 86400;
    t->year = y; t->mon = m; t->day = d; t->hour = (int)(sec / 3600); t->min = (int)(sec / 60 % 60); t->sec = (int)(sec % 60);
    t->utc_offset_min = off;
}

/* ---- the system call ---- */
int settings_syscall_get(const char *key, char *out, int n) {
    if (!strcmp(key, "keyboard.layouts")) return kbd_layouts(out, n);
    if (!strcmp(key, "time.zones")) return tz_list(out, n);
    if (!strcmp(key, "display.current")) return ksnprintf(out, n, "%ux%u", bootinfo.fb_w, bootinfo.fb_h);
    if (!strcmp(key, "lock.password")) return ksnprintf(out, n, "%s", settings_get(key)[0] ? "set" : "");
    return ksnprintf(out, n, "%s", settings_get(key));
}

int settings_set(const char *key, const char *val) {
    if (!strcmp(key, "lock.password")) {                         /* the value is the new password: store its hash */
        if (!val[0]) store(key, "");
        else {
            uint8_t salt[16], h[32]; char enc[100];
            random_bytes(salt, 16); hash_pw(val, salt, h);
            hex(salt, 16, enc); enc[32] = '$'; hex(h, 32, enc + 33);
            store(key, enc);
        }
    } else {
        bool known = false;
        for (unsigned i = 0; i < sizeof defaults / sizeof defaults[0]; i++) if (!strcmp(defaults[i][0], key)) known = true;
        if (!known) return -1;
        store(key, val);
        apply(key);
    }
    save();
    return 0;
}
