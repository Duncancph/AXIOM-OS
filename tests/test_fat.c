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
    for (int i = 0; i < 40; i++) {   /* force the folder to grow beyond one cluster */
        char p[96], t[32]; snprintf(p, sizeof p, "/My Folder/Sub dir/document number %02d.txt", i); snprintf(t, sizeof t, "file %d\n", i);
        CHECK(o->write(v, p, t, strlen(t)) == (long)strlen(t), "write many %d", i);
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
