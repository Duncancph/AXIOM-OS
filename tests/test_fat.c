/* Host-side test of the FAT32 driver against an image file. Afterwards the image is
 * checked with fsck.fat and read back with mtools (independent implementations). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/kernel.h"
#include "../src/vfs.h"
#include "../src/blockdev.h"

struct task *current;
void *kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void *p) { free(p); }
void rtc_read(struct rtc_time *t) { t->year = 2026; t->mon = 11; t->day = 14; t->hour = 9; t->min = 41; t->sec = 30; }
void rtc_read_local(struct rtc_time *t) { rtc_read(t); t->utc_offset_min = 0; }
void civil_from_days(int64_t z, int *y, int *m, int *d) {        /* (as src/tz.c) */
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *m = mp < 10 ? mp + 3 : mp - 9; *y = (int)(yy + (*m <= 2));
}
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {   /* (as src/tz.c) */
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void random_bytes(void *b, size_t n) { memset(b, 0x5A, n); }
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }

static FILE *img; static long reads, writes;
static int rd(void *c, uint64_t lba, uint32_t n, void *buf) { reads++; fseek(img, lba * 512, SEEK_SET); return fread(buf, 512, n, img) == n ? 0 : -1; }
static int wrt(void *c, uint64_t lba, uint32_t n, const void *buf) { writes++; fseek(img, lba * 512, SEEK_SET); return fwrite(buf, 512, n, img) == n ? 0 : -1; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv) {
    img = fopen(argv[1], "r+b");
    fseek(img, 0, SEEK_END);
    struct blockdev bd = { 0, 512, ftell(img) / 512, rd, wrt, 0, 0, "test" };
    if (argc > 2 && !strcmp(argv[2], "--format")) {          /* make a new file system with DuncOS's formatter first */
        int r = fat_format(&bd, 0, bd.blocks, "DUNCFMT");
        printf("fat_format: %s\n", r ? "FAILED" : "ok");
        if (r) return 1;
    }
    char label[16] = "", why[96] = "";
    void *v = fat_mount(&bd, label, why, sizeof why);
    if (!v) { printf("mount failed: %s\n", why); return 1; }
    printf("mounted, label '%s'\n", label);
    const struct fs_ops *o = &fat_ops;
    struct vfs_dirent d[64]; struct dunc_stat st;

    int n = o->list(v, "/", d, 64);
    printf("root has %d entries:", n); for (int i = 0; i < n; i++) printf(" %s%s", d[i].name, d[i].is_dir ? "/" : ""); printf("\n");

    /* read a file written by mtools */
    char buf[70000];
    long r = o->read(v, "/Hello From Linux.txt", buf, 0, sizeof buf);
    bool formatted = argc > 2;
    CHECK(formatted || (r == 27 && !memcmp(buf, "written by mtools on Linux\n", 27)), "read mtools file (%ld)", r);

    /* create files with short + long names, sizes around cluster boundaries */
    CHECK(o->write(v, "/NOTES.TXT", "short name\n", 11) == 11, "write short");
    CHECK(o->write(v, "/readme.md", "lower case 8.3\n", 15) == 15, "write lower 8.3");
    static char big[300000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = 'A' + i % 26;
    CHECK(o->write(v, "/A Long File Name With Spaces.data", big, sizeof big) == (long)sizeof big, "write big long name");
    CHECK(o->write(v, "/empty file.txt", "", 0) == 0, "write empty");
    CHECK(o->mkdir(v, "/My Folder") == 0, "mkdir");
    CHECK(o->mkdir(v, "/My Folder") == FS_EEXIST, "mkdir exists");
    CHECK(o->mkdir(v, "/My Folder/Sub dir") == 0, "mkdir nested");
    {   /* long names (84 and 31 characters): a folder and a file in it, as packages unpack them */
        const char *ld = "/My Folder/a-folder-with-a-rather-long-name-to-need-gnu-long-name-records-in-the-tar-archive";
        int r = o->mkdir(v, ld);
        CHECK(r == 0, "mkdir long name: %d", r);
        char lf[300]; snprintf(lf, sizeof lf, "%s/readme-with-a-long-name-too.txt", ld);
        long w = o->write(v, lf, "long\n", 5);
        CHECK(w == 5, "write in long-named folder: %ld", w);
    }
    {   /* a folder of 231 long names, many with the same start (GTK's status icons: network-*, battery-*...),
         * as the gtk3 package unpacks them */
        static const char *icons[] = {
        "airplane-mode-symbolic.png",
        "alarm-symbolic.png",
        "appointment-missed-symbolic.png",
        "appointment-soon-symbolic.png",
        "audio-volume-high-rtl-symbolic.png",
        "audio-volume-high-symbolic-rtl.png",
        "audio-volume-high-symbolic.png",
        "audio-volume-low-rtl-symbolic.png",
        "audio-volume-low-symbolic-rtl.png",
        "audio-volume-low-symbolic.png",
        "audio-volume-medium-rtl-symbolic.png",
        "audio-volume-medium-symbolic-rtl.png",
        "audio-volume-medium-symbolic.png",
        "audio-volume-muted-rtl-symbolic.png",
        "audio-volume-muted-symbolic-rtl.png",
        "audio-volume-muted-symbolic.png",
        "audio-volume-overamplified-rtl-symbolic.png",
        "audio-volume-overamplified-symbolic-rtl.png",
        "audio-volume-overamplified-symbolic.png",
        "auth-sim-locked-symbolic.png",
        "auth-sim-missing-symbolic.png",
        "avatar-default-symbolic.png",
        "battery-action-symbolic.png",
        "battery-caution-symbolic.png",
        "battery-level-0-charging-symbolic.png",
        "battery-level-0-symbolic.png",
        "battery-level-10-charging-symbolic.png",
        "battery-level-10-symbolic.png",
        "battery-level-100-charged-symbolic.png",
        "battery-level-100-symbolic.png",
        "battery-level-20-charging-symbolic.png",
        "battery-level-20-symbolic.png",
        "battery-level-30-charging-symbolic.png",
        "battery-level-30-symbolic.png",
        "battery-level-40-charging-symbolic.png",
        "battery-level-40-symbolic.png",
        "battery-level-50-charging-symbolic.png",
        "battery-level-50-symbolic.png",
        "battery-level-60-charging-symbolic.png",
        "battery-level-60-symbolic.png",
        "battery-level-70-charging-symbolic.png",
        "battery-level-70-symbolic.png",
        "battery-level-80-charging-symbolic.png",
        "battery-level-80-symbolic.png",
        "battery-level-90-charging-symbolic.png",
        "battery-level-90-symbolic.png",
        "battery-low-symbolic.png",
        "battery-missing-symbolic.png",
        "bluetooth-acquiring-symbolic.png",
        "bluetooth-active-symbolic.png",
        "bluetooth-disabled-symbolic.png",
        "bluetooth-disconnected-symbolic.png",
        "bluetooth-hardware-disabled-symbolic.png",
        "call-incoming-symbolic.png",
        "call-missed-symbolic.png",
        "call-outgoing-symbolic.png",
        "camera-disabled-symbolic.png",
        "camera-hardware-disabled-symbolic.png",
        "changes-allow-symbolic.png",
        "changes-prevent-symbolic.png",
        "channel-insecure-symbolic.png",
        "channel-secure-symbolic.png",
        "computer-fail-symbolic.png",
        "content-loading-symbolic.png",
        "daytime-sunrise-symbolic.png",
        "daytime-sunset-symbolic.png",
        "dialog-error-symbolic.png",
        "dialog-information-symbolic.png",
        "dialog-password-symbolic.png",
        "dialog-question-symbolic.png",
        "dialog-warning-symbolic.png",
        "display-brightness-symbolic.png",
        "folder-drag-accept-symbolic.png",
        "folder-open-symbolic.png",
        "folder-visiting-symbolic.png",
        "image-loading-symbolic.png",
        "image-missing-symbolic.png",
        "keyboard-brightness-symbolic.png",
        "location-services-active-symbolic.png",
        "location-services-disabled-symbolic.png",
        "mail-attachment-symbolic.png",
        "mail-read-symbolic.png",
        "mail-replied-rtl-symbolic.png",
        "mail-replied-symbolic-rtl.png",
        "mail-replied-symbolic.png",
        "mail-unread-symbolic.png",
        "media-playlist-consecutive-symbolic.png",
        "media-playlist-repeat-song-symbolic.png",
        "media-playlist-repeat-symbolic.png",
        "media-playlist-shuffle-symbolic.png",
        "microphone-disabled-symbolic.png",
        "microphone-hardware-disabled-symbolic.png",
        "microphone-sensitivity-high-symbolic.png",
        "microphone-sensitivity-low-symbolic.png",
        "microphone-sensitivity-medium-symbolic.png",
        "microphone-sensitivity-muted-symbolic.png",
        "network-cellular-2g-symbolic.png",
        "network-cellular-3g-symbolic.png",
        "network-cellular-4g-symbolic.png",
        "network-cellular-5g-symbolic.png",
        "network-cellular-acquiring-rtl-symbolic.png",
        "network-cellular-acquiring-symbolic-rtl.png",
        "network-cellular-acquiring-symbolic.png",
        "network-cellular-connected-symbolic.png",
        "network-cellular-disabled-rtl-symbolic.png",
        "network-cellular-disabled-symbolic-rtl.png",
        "network-cellular-disabled-symbolic.png",
        "network-cellular-edge-symbolic.png",
        "network-cellular-gprs-symbolic.png",
        "network-cellular-hardware-disabled-rtl-symbolic.png",
        "network-cellular-hardware-disabled-symbolic-rtl.png",
        "network-cellular-hardware-disabled-symbolic.png",
        "network-cellular-hspa-symbolic.png",
        "network-cellular-no-route-rtl-symbolic.png",
        "network-cellular-no-route-symbolic-rtl.png",
        "network-cellular-no-route-symbolic.png",
        "network-cellular-offline-rtl-symbolic.png",
        "network-cellular-offline-symbolic-rtl.png",
        "network-cellular-offline-symbolic.png",
        "network-cellular-signal-excellent-rtl-symbolic.png",
        "network-cellular-signal-excellent-symbolic-rtl.png",
        "network-cellular-signal-excellent-symbolic.png",
        "network-cellular-signal-good-rtl-symbolic.png",
        "network-cellular-signal-good-symbolic-rtl.png",
        "network-cellular-signal-good-symbolic.png",
        "network-cellular-signal-none-rtl-symbolic.png",
        "network-cellular-signal-none-symbolic-rtl.png",
        "network-cellular-signal-none-symbolic.png",
        "network-cellular-signal-ok-rtl-symbolic.png",
        "network-cellular-signal-ok-symbolic-rtl.png",
        "network-cellular-signal-ok-symbolic.png",
        "network-cellular-signal-weak-rtl-symbolic.png",
        "network-cellular-signal-weak-symbolic-rtl.png",
        "network-cellular-signal-weak-symbolic.png",
        "network-error-symbolic.png",
        "network-idle-symbolic.png",
        "network-no-route-symbolic.png",
        "network-offline-symbolic.png",
        "network-receive-symbolic-rtl.png",
        "network-receive-symbolic.png",
        "network-transmit-receive-symbolic.png",
        "network-transmit-symbolic-rtl.png",
        "network-transmit-symbolic.png",
        "network-vpn-acquiring-symbolic.png",
        "network-vpn-disabled-symbolic.png",
        "network-vpn-disconnected-symbolic.png",
        "network-vpn-no-route-symbolic.png",
        "network-vpn-symbolic.png",
        "network-wired-acquiring-symbolic.png",
        "network-wired-disconnected-symbolic.png",
        "network-wired-no-route-symbolic.png",
        "network-wireless-acquiring-symbolic.png",
        "network-wireless-connected-symbolic.png",
        "network-wireless-disabled-symbolic.png",
        "network-wireless-encrypted-symbolic.png",
        "network-wireless-hardware-disabled-symbolic.png",
        "network-wireless-hotspot-symbolic.png",
        "network-wireless-no-route-symbolic.png",
        "network-wireless-offline-symbolic.png",
        "network-wireless-signal-excellent-symbolic.png",
        "network-wireless-signal-good-symbolic.png",
        "network-wireless-signal-none-symbolic.png",
        "network-wireless-signal-ok-symbolic.png",
        "network-wireless-signal-weak-symbolic.png",
        "night-light-disabled-symbolic.png",
        "night-light-symbolic.png",
        "non-starred-symbolic.png",
        "notifications-disabled-symbolic.png",
        "orientation-landscape-inverse-symbolic.png",
        "orientation-landscape-symbolic.png",
        "orientation-portrait-left-symbolic-rtl.png",
        "orientation-portrait-left-symbolic.png",
        "orientation-portrait-right-symbolic-rtl.png",
        "orientation-portrait-right-symbolic.png",
        "power-profile-balanced-rtl-symbolic.png",
        "power-profile-balanced-symbolic-rtl.png",
        "power-profile-balanced-symbolic.png",
        "power-profile-performance-rtl-symbolic.png",
        "power-profile-performance-symbolic-rtl.png",
        "power-profile-performance-symbolic.png",
        "power-profile-power-saver-rtl-symbolic.png",
        "power-profile-power-saver-symbolic-rtl.png",
        "power-profile-power-saver-symbolic.png",
        "printer-error-symbolic.png",
        "printer-printing-symbolic.png",
        "printer-warning-symbolic.png",
        "rotation-allowed-symbolic.png",
        "rotation-locked-symbolic.png",
        "screen-shared-symbolic.png",
        "security-high-symbolic.png",
        "security-low-symbolic.png",
        "security-medium-rtl-symbolic.png",
        "security-medium-symbolic-rtl.png",
        "security-medium-symbolic.png",
        "semi-starred-rtl-symbolic.png",
        "semi-starred-symbolic-rtl.png",
        "semi-starred-symbolic.png",
        "software-update-available-symbolic.png",
        "software-update-urgent-symbolic.png",
        "starred-symbolic.png",
        "system-lock-screen-symbolic.png",
        "task-due-symbolic.png",
        "task-past-due-symbolic.png",
        "thunderbolt-acquiring-symbolic.png",
        "touch-disabled-symbolic.png",
        "touchpad-disabled-symbolic.png",
        "user-available-symbolic.png",
        "user-away-symbolic.png",
        "user-busy-symbolic.png",
        "user-idle-symbolic.png",
        "user-invisible-symbolic.png",
        "user-not-tracked-symbolic.png",
        "user-offline-symbolic.png",
        "user-status-pending-symbolic.png",
        "user-trash-full-symbolic.png",
        "view-wrapped-rtl-symbolic.png",
        "view-wrapped-symbolic-rtl.png",
        "view-wrapped-symbolic.png",
        "weather-clear-night-symbolic.png",
        "weather-clear-symbolic.png",
        "weather-few-clouds-night-symbolic.png",
        "weather-few-clouds-symbolic.png",
        "weather-fog-symbolic.png",
        "weather-overcast-symbolic.png",
        "weather-severe-alert-symbolic.png",
        "weather-showers-scattered-symbolic.png",
        "weather-showers-symbolic.png",
        "weather-snow-symbolic.png",
        "weather-storm-symbolic.png",
        "weather-tornado-symbolic.png",
        "weather-windy-symbolic.png" };
        int ni = sizeof icons / sizeof icons[0], ok = 0;
        CHECK(o->mkdir(v, "/icons") == 0, "mkdir icons");
        for (int i = 0; i < ni; i++) {
            char p[200]; snprintf(p, sizeof p, "/icons/%s", icons[i]);
            long w = o->write(v, p, "png", 3);
            if (w == 3) ok++; else { CHECK(0, "write icon %d %s: %ld", i, icons[i], w); break; }
        }
        struct vfs_dirent *all = calloc(400, sizeof *all);
        int nl = o->list(v, "/icons", all, 400);
        CHECK(ok == ni && nl == ni, "icons: %d written, %d listed, of %d", ok, nl, ni);
        free(all);
    }
    for (int i = 0; i < 40; i++) {   /* force the folder to grow beyond one cluster */
        char p[96], t[32]; snprintf(p, sizeof p, "/My Folder/Sub dir/document number %02d.txt", i); snprintf(t, sizeof t, "file %d\n", i);
        CHECK(o->write(v, p, t, strlen(t)) == (long)strlen(t), "write many %d", i);
    }
    {   /* dates: a file written now has today's date (the test's clock: 2026-11-14 09:41:30, UTC) */
        int64_t t = o->mtime(v, "/My Folder/Sub dir/document number 00.txt");
        CHECK(t >= 1794649290 - 2 && t <= 1794649290, "mtime %lld", (long long)t);
        CHECK(o->set_mtime(v, "/My Folder/Sub dir/document number 00.txt", 1700000000) == 0, "set_mtime");
        t = o->mtime(v, "/My Folder/Sub dir/document number 00.txt");
        CHECK(t == 1700000000, "mtime after set_mtime %lld", (long long)t);
    }
    n = o->list(v, "/My Folder/Sub dir", d, 64);
    CHECK(n == 40, "list many (%d)", n);
    r = o->read(v, "/A Long File Name With Spaces.data", buf, 4090, 10);
    CHECK(r == 10 && !memcmp(buf, big + 4090, 10), "read at offset across cluster");
    /* overwrite (shrink) and case-insensitive lookup */
    CHECK(o->write(v, "/a long file name with spaces.data", "tiny\n", 5) == 5, "overwrite");
    CHECK(o->stat(v, "/A Long File Name With Spaces.data", &st) == 0 && st.size == 5, "size after overwrite");
    /* rename within a folder and move between folders */
    CHECK(o->rename(v, "/NOTES.TXT", "/Notes renamed.txt") == 0, "rename");
    CHECK(o->rename(v, "/readme.md", "/My Folder/readme.md") == 0, "move file");
    CHECK(o->mkdir(v, "/moveme") == 0 && o->write(v, "/moveme/inside.txt", "in\n", 3) == 3, "make moveme");
    CHECK(o->rename(v, "/moveme", "/My Folder/moved folder") == 0, "move folder");
    CHECK(o->read(v, "/My Folder/moved folder/inside.txt", buf, 0, 10) == 3, "read in moved folder");
    /* delete */
    CHECK(o->remove(v, "/My Folder") == FS_ENOTEMPTY, "remove non-empty");
    CHECK(o->write(v, "/delete me.txt", "bye\n", 4) == 4 && o->remove(v, "/delete me.txt") == 0, "delete file");
    CHECK(o->stat(v, "/delete me.txt", &st) == FS_ENOENT, "deleted gone");
    for (int i = 0; i < 40; i += 2) { char p[96]; snprintf(p, sizeof p, "/My Folder/Sub dir/document number %02d.txt", i); CHECK(o->remove(v, p) == 0, "remove %d", i); }
    /* append: across cluster boundaries, to an empty file, to a file with exact cluster size */
    CHECK(o->write(v, "/log.txt", "start\n", 6) == 6, "append base");
    for (int i = 0; i < 300; i++) { char t[32]; int n = snprintf(t, sizeof t, "line %03d\n", i); CHECK(o->append(v, "/log.txt", t, n) == n, "append %d", i); }
    CHECK(o->stat(v, "/log.txt", &st) == 0 && st.size == 6 + 300 * 9, "append size %lu", st.size);
    r = o->read(v, "/log.txt", buf, 6 + 299 * 9, 9);
    CHECK(r == 9 && !memcmp(buf, "line 299\n", 9), "append content");
    CHECK(o->append(v, "/empty file.txt", big, 1000) == 1000 && o->read(v, "/empty file.txt", buf, 0, 2000) == 1000 && !memcmp(buf, big, 1000), "append to empty");
    CHECK(o->write(v, "/exact.bin", big, 512) == 512 && o->append(v, "/exact.bin", big + 512, 700) == 700, "append after exact cluster");
    CHECK(o->read(v, "/exact.bin", buf, 0, 2000) == 1212 && !memcmp(buf, big, 1212), "exact content");
    uint64_t tot, fr; o->space(v, &tot, &fr);
    printf("space: %lu MB total, %lu KB free; %ld sector reads, %ld writes\n", tot >> 20, fr >> 10, reads, writes);
    fat_unmount(v);
    fclose(img);
    printf(fails ? "%d CHECKS FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
