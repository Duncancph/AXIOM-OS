/* Disks: the list of SATA, NVMe and USB disks, their partitions, the AXIOM system volume, and the installer.
 *
 * Internal disks are only ever touched in two ways: their partitions are read to describe them, and a
 * partition named AXIOM (GPT) or labelled AXIOM (FAT32; AXIOM/DUNCOS from before the rename are recognised too) is mounted as the system volume. Other
 * partitions (Windows, Linux, recovery) are never mounted or written, except when the user asks the
 * installer to erase that disk.
 *
 * The system volume is a FAT32 EFI system partition:
 *   /EFI/BOOT/BOOTX64.EFI   GRUB (standalone image, finds /duncos/thane.elf on any disk)
 *   /duncos/                kernel, initial files, grub.cfg            -> /system/boot
 *   /home/                  the user's files                            -> /home
 *   /apps/                  apps installed with clan                    -> /apps
 *   /config/                settings                                    -> /system/config */
#include "disk.h"
#include "vfs.h"

#define MAXD 8
struct part { uint64_t first, count; char fs[12], label[24]; bool duncos; };
struct disk { bool used; int id, kind, nparts; struct blockdev *bd; struct part p[8]; void *sysvol; };
static struct disk disks[MAXD];
static int next_id = 1;
static struct disk *sysdisk;
static bool booted;                                 /* notifications only after start-up */
static struct dunc_install inst;
static uint8_t *secbuf;                             /* 8 KiB scratch for scanning */

static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc32(const void *data, size_t n, uint32_t crc) {
    const uint8_t *p = data; crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1)); }
    return ~crc;
}

static const uint8_t ESP_GUID[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };

/* what is in a partition: file system and label */
static void probe_fs(struct blockdev *bd, struct part *pt) {
    strncpy0(pt->fs, "unknown", sizeof pt->fs);
    uint32_t need = 2048 / bd->bsize ? 2048 / bd->bsize : 1;
    if (bd->read(bd->ctx, pt->first, need, secbuf)) { strncpy0(pt->fs, "unreadable", sizeof pt->fs); return; }
    const uint8_t *b = secbuf;
    if ((b[0] == 0xEB || b[0] == 0xE9) && !memcmp(b + 82, "FAT32   ", 8)) {
        strncpy0(pt->fs, "FAT32", sizeof pt->fs);
        char l[12]; memcpy(l, b + 71, 11); l[11] = 0;
        for (int k = 10; k >= 0 && l[k] == ' '; k--) l[k] = 0;
        if (strcmp(l, "NO NAME")) strncpy0(pt->label, l, sizeof pt->label);
        if (!strcmp(l, "AXIOM") || !strcmp(l, "DUNCOS")) pt->duncos = true;   /* DUNCOS: installed under the old name */
    }
    else if (!memcmp(b + 3, "NTFS    ", 8)) strncpy0(pt->fs, "NTFS", sizeof pt->fs);
    else if (!memcmp(b + 3, "EXFAT   ", 8)) strncpy0(pt->fs, "exFAT", sizeof pt->fs);
    else if (!memcmp(b + 54, "FAT16   ", 8) || !memcmp(b + 54, "FAT12   ", 8)) strncpy0(pt->fs, "FAT16", sizeof pt->fs);
    else if (b[1024 + 56] == 0x53 && b[1024 + 57] == 0xEF) strncpy0(pt->fs, "ext4", sizeof pt->fs);
    else if (!memcmp(b + 3, "-FVE-FS-", 8)) strncpy0(pt->fs, "BitLocker", sizeof pt->fs);
}

static void scan(struct disk *d) {
    struct blockdev *bd = d->bd;
    d->nparts = 0;
    if (bd->read(bd->ctx, 0, 1, secbuf)) return;
    uint8_t mbr[512]; memcpy(mbr, secbuf, 512);
    if ((mbr[0] == 0xEB || mbr[0] == 0xE9) && !memcmp(mbr + 82, "FAT32   ", 8)) {     /* no partition table */
        d->p[0].first = 0; d->p[0].count = bd->blocks; probe_fs(bd, &d->p[0]); d->nparts = 1; return;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return;
    if (mbr[446 + 4] == 0xEE) {                                                        /* GPT */
        if (bd->read(bd->ctx, 1, 1, secbuf) || memcmp(secbuf, "EFI PART", 8)) return;
        uint64_t elba = r64(secbuf + 72); uint32_t cnt = r32(secbuf + 80), esz = r32(secbuf + 84);
        if (esz < 128 || esz > 512) return;
        uint8_t ent[512];
        for (uint32_t i = 0; i < cnt && i < 128 && d->nparts < 8; i++) {
            uint64_t off = (uint64_t)i * esz;
            if (bd->read(bd->ctx, elba + off / bd->bsize, 1, secbuf)) break;
            memcpy(ent, secbuf + off % bd->bsize, esz < 128 ? esz : 128);
            bool used = false; for (int k = 0; k < 16; k++) if (ent[k]) used = true;
            if (!used) continue;
            struct part *pt = &d->p[d->nparts++];
            memset(pt, 0, sizeof *pt);
            pt->first = r64(ent + 32); pt->count = r64(ent + 40) - pt->first + 1;
            char name[37]; for (int k = 0; k < 36; k++) name[k] = ent[56 + 2 * k] < 128 ? ent[56 + 2 * k] : '?'; name[36] = 0;
            probe_fs(bd, pt);
            if (!pt->label[0]) strncpy0(pt->label, name, sizeof pt->label);
            if ((!strcmp(name, "AXIOM") || !strcmp(name, "AXIOM")) && !strcmp(pt->fs, "FAT32")) pt->duncos = true;
        }
    } else {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            if (!e[4] || !r32(e + 12)) continue;
            struct part *pt = &d->p[d->nparts++];
            memset(pt, 0, sizeof *pt);
            pt->first = r32(e + 8); pt->count = r32(e + 12);
            probe_fs(bd, pt);
        }
    }
}

static const char *kind_name(int k) { return k == DISK_SATA ? "SATA" : k == DISK_NVME ? "NVMe" : "USB"; }

static bool mount_system(struct disk *d, struct part *pt) {
    char label[16], why[96];
    void *vol = fat_open(d->bd, pt->first, label, why, sizeof why);
    if (!vol) { LOG_WARN("AXIOM volume on %s: %s\n", d->bd->name, why); return false; }
    static const char *dirs[] = { "/home", "/apps", "/config", "/duncos" };
    for (int i = 0; i < 4; i++) fat_ops.mkdir(vol, dirs[i]);
    char kind[16]; ksnprintf(kind, sizeof kind, "%s disk", kind_name(d->kind));
    vfs_mount_at("/home", "/home", "AXIOM", kind, &fat_ops, vol);
    vfs_mount_at("/apps", "/apps", "AXIOM", kind, &fat_ops, vol);
    vfs_mount_at("/system/config", "/config", "AXIOM", kind, &fat_ops, vol);
    vfs_mount_at("/system/boot", "/duncos", "AXIOM", kind, &fat_ops, vol);
    static const char *homedirs[] = { "/home/Documents", "/home/Downloads", "/home/Pictures", "/home/Trash" };
    for (int i = 0; i < 4; i++) vfs_mkdir(homedirs[i]);
    d->sysvol = vol; sysdisk = d;
    apps_refresh();
    settings_load();
    LOG_OK("Your files are saved on %s (%s)\n", d->bd->name, kind_name(d->kind));
    if (booted) desktop_notify("AXIOM drive connected", "Your files and settings are saved on it again.");
    return true;
}

bool disk_add(struct blockdev *bd, int kind) {
    if (!secbuf) secbuf = kmalloc(8192);
    struct disk *d = 0;
    for (int i = 0; i < MAXD; i++) if (!disks[i].used) { d = &disks[i]; break; }
    if (!d || !secbuf) return false;
    memset(d, 0, sizeof *d);
    d->used = true; d->id = next_id++; d->kind = kind; d->bd = bd;
    scan(d);
    for (int i = 0; i < d->nparts; i++)
        LOG_INFO("  %s partition %d: %lu MB %s %s%s\n", bd->name, i + 1, d->p[i].count * bd->bsize >> 20, d->p[i].fs, d->p[i].label, d->p[i].duncos ? " (AXIOM)" : "");
    if (!sysdisk)
        for (int i = 0; i < d->nparts; i++) if (d->p[i].duncos) return mount_system(d, &d->p[i]);
    return false;
}

void disk_remove(struct blockdev *bd) {
    for (int i = 0; i < MAXD; i++) {
        struct disk *d = &disks[i];
        if (!d->used || d->bd != bd) continue;
        if (d->sysvol) {
            vfs_unmount(d->sysvol); fat_unmount(d->sysvol); d->sysvol = 0; sysdisk = 0;
            apps_refresh();
            desktop_notify("AXIOM drive removed", "Your files are on that drive. Until it's back, new files are kept in memory only.");
        }
        d->used = false;
    }
}

int disk_list(struct dunc_disk *out, int max) {
    int n = 0;
    for (int i = 0; i < MAXD && n < max; i++) {
        struct disk *d = &disks[i];
        if (!d->used) continue;
        struct dunc_disk *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = d->id; o->kind = d->kind; o->bsize = d->bd->bsize; o->bytes = d->bd->blocks * d->bd->bsize;
        strncpy0(o->model, d->bd->name, sizeof o->model);
        o->system = d == sysdisk;
        o->nparts = d->nparts;
        for (int k = 0; k < d->nparts; k++) {
            o->part[k].bytes = d->p[k].count * d->bd->bsize;
            strncpy0(o->part[k].fs, d->p[k].fs, sizeof o->part[k].fs);
            strncpy0(o->part[k].label, d->p[k].label, sizeof o->part[k].label);
            if (d->p[k].duncos) o->duncos = 1;
        }
    }
    return n;
}

void disk_flush_all(void) {
    for (int i = 0; i < MAXD; i++) if (disks[i].used && disks[i].bd->flush) disks[i].bd->flush(disks[i].bd->ctx);
}

/* ---------------- installer ---------------- */
static int inst_id, inst_mode;

static void progress(int pct, const char *msg) {
    inst.percent = pct; strncpy0(inst.msg, msg, sizeof inst.msg);
    kprintf("[install] %d%% %s\n", pct, msg);
}

/* a fresh GPT with one partition (the AXIOM EFI system partition) covering the disk */
static int write_gpt(struct blockdev *bd, uint64_t *first, uint64_t *count) {
    uint32_t bs = bd->bsize;
    uint64_t last = bd->blocks - 1;
    uint32_t ent_blocks = (128 * 128 + bs - 1) / bs;
    uint64_t first_usable = 2 + ent_blocks, last_usable = last - 1 - ent_blocks;
    uint64_t align = (1024 * 1024) / bs;                         /* 1 MiB alignment */
    uint64_t pfirst = (first_usable + align - 1) / align * align;
    uint64_t plast = last_usable;
    if (plast - pfirst + 1 > 0xFFFFFFFFull) plast = pfirst + 0xFFFFFFFEull;    /* FAT32 limit */
    if (plast <= pfirst + 131072) return FS_ENOSPC;
    uint8_t *ents = kmalloc(ent_blocks * bs), *b = kmalloc(bs);
    if (!ents || !b) { kfree(ents); kfree(b); return FS_ENOSPC; }
    memset(ents, 0, ent_blocks * bs);
    memcpy(ents, ESP_GUID, 16);
    random_bytes(ents + 16, 16); ents[16 + 7] = (ents[16 + 7] & 0x0F) | 0x40; ents[16 + 8] = (ents[16 + 8] & 0x3F) | 0x80;
    w64(ents + 32, pfirst); w64(ents + 40, plast);
    const char *nm = "AXIOM"; for (int k = 0; nm[k]; k++) ents[56 + 2 * k] = nm[k];
    uint32_t ecrc = crc32(ents, 128 * 128, 0);
    uint8_t guid[16]; random_bytes(guid, 16); guid[7] = (guid[7] & 0x0F) | 0x40; guid[8] = (guid[8] & 0x3F) | 0x80;
    int r = FS_OK;
    for (int copy = 0; copy < 2 && r == FS_OK; copy++) {
        memset(b, 0, bs);
        memcpy(b, "EFI PART", 8); w32(b + 8, 0x00010000); w32(b + 12, 92);
        uint64_t my = copy ? last : 1, alt = copy ? 1 : last, elba = copy ? last - ent_blocks : 2;
        w64(b + 24, my); w64(b + 32, alt); w64(b + 40, first_usable); w64(b + 48, last_usable);
        memcpy(b + 56, guid, 16); w64(b + 72, elba); w32(b + 80, 128); w32(b + 84, 128); w32(b + 88, ecrc);
        w32(b + 16, crc32(b, 92, 0));
        if (bd->write(bd->ctx, elba, ent_blocks, ents) || bd->write(bd->ctx, my, 1, b)) r = FS_EIO;
    }
    /* protective MBR */
    memset(b, 0, bs);
    uint8_t *e = b + 446;
    e[1] = 0; e[2] = 2; e[4] = 0xEE; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
    w32(e + 8, 1); w32(e + 12, last > 0xFFFFFFFFull ? 0xFFFFFFFF : (uint32_t)last);
    b[510] = 0x55; b[511] = 0xAA;
    if (r == FS_OK && bd->write(bd->ctx, 0, 1, b)) r = FS_EIO;
    kfree(ents); kfree(b);
    *first = pfirst; *count = plast - pfirst + 1;
    return r;
}

static const char *GRUBCFG_FMT =
    "# AXIOM boot menu. Settings > Display changes the screen size here.\n"
    "set timeout=2\nset default=0\ninsmod all_video\ninsmod gfxterm\n"
    "menuentry \"AXIOM\" {\n"
    "    set gfxpayload=%s1920x1080x32,1920x1200x32,1680x1050x32,1600x900x32,1440x900x32,1366x768x32,1280x800x32,1280x720x32,1024x768x32,auto\n"
    "    multiboot2 /duncos/thane.elf\n    module2 /duncos/initrd.tar initrd\n"
    "    module2 /duncos/thane.elf kernel\n    module2 /EFI/BOOT/BOOTX64.EFI bootx64\n%s    boot\n}\n"
    "menuentry \"AXIOM (safe graphics 1024x768)\" {\n"
    "    set gfxpayload=1024x768x32,auto\n"
    "    multiboot2 /duncos/thane.elf\n    module2 /duncos/initrd.tar initrd\n"
    "    module2 /duncos/thane.elf kernel\n    module2 /EFI/BOOT/BOOTX64.EFI bootx64\n%s    boot\n}\n";

static char gfx_pref[32];

static int write_grubcfg(const char *dir) {
    char cfg[2400], first[40] = "", path[64];
    if (gfx_pref[0]) ksnprintf(first, sizeof first, "%s,", gfx_pref);
    struct dunc_stat st; char bp[64]; ksnprintf(bp, sizeof bp, "%s/biosboot.img", dir);
    const char *bios = vfs_stat(bp, &st) == FS_OK ? "    module2 /duncos/biosboot.img biosboot\n" : "";   /* only if it's there */
    int n = ksnprintf(cfg, sizeof cfg, GRUBCFG_FMT, first, bios, bios);
    if (n < (int)sizeof cfg - 300)                         /* what the graphics card offers (Settings > Display can only pick from these) */
        n += ksnprintf(cfg + n, sizeof cfg - n, "menuentry \"Show the screen sizes this computer offers\" {\n    videoinfo\n"
                       "    echo\n    echo \"AXIOM can use the sizes listed above. Press a key to go back.\"\n    sleep --interruptible 300\n}\n");
    char pk[64]; ksnprintf(pk, sizeof pk, "%s/thane-prev.elf", dir);
    if (vfs_stat(pk, &st) == FS_OK && n < (int)sizeof cfg - 400)       /* the version before the last update */
        n += ksnprintf(cfg + n, sizeof cfg - n, "menuentry \"AXIOM (previous version)\" {\n    set gfxpayload=auto\n"
                       "    multiboot2 /duncos/thane-prev.elf\n    module2 /duncos/initrd-prev.tar initrd\n"
                       "    module2 /duncos/thane-prev.elf kernel\n    module2 /EFI/BOOT/BOOTX64.EFI bootx64\n    boot\n}\n");
    ksnprintf(path, sizeof path, "%s/grub.cfg", dir);
    static char old[2400]; long on = vfs_read(path, old, 0, sizeof old);
    if (on == n && !memcmp(old, cfg, n)) return FS_OK;     /* unchanged: don't write */
    long r = vfs_write(path, cfg, n);
    return r < 0 ? (int)r : FS_OK;
}

static const uint8_t *src_data[4]; static uint64_t src_len[4];     /* an update's files, else the running ones */
/* Make the disk start in legacy BIOS mode too: GRUB's boot code goes in the MBR (the partition table is kept)
 * and its core image right after the GPT entries (LBA 34), before the first partition. */
static int write_biosboot(struct blockdev *bd, uint64_t first_part) {
    uint64_t len; const uint8_t *img = bootmod("biosboot", &len);
    if (!img || len < 1024 || bd->bsize != 512) return FS_ENOENT;
    uint64_t core_lba = 34, core_sect = (len - 512 + 511) / 512;
    if (core_lba + core_sect > first_part) { LOG_WARN("installer: no room for the BIOS boot loader\n"); return FS_ENOSPC; }
    uint8_t *b = kmalloc(core_sect * 512 > 512 ? core_sect * 512 : 512);
    if (!b) return FS_ENOSPC;
    int r = FS_OK;
    if (bd->read(bd->ctx, 0, 1, b)) r = FS_EIO;
    else {
        memcpy(b, img, 440);                                 /* boot code only: keep the disk signature and partitions */
        w64(b + 0x5C, core_lba);                             /* where the core image starts */
        b[0x64] = 0xFF;                                      /* boot from the drive the BIOS started */
        if (bd->write(bd->ctx, 0, 1, b)) r = FS_EIO;
    }
    if (r == FS_OK) {
        memset(b, 0, core_sect * 512);
        memcpy(b, img + 512, len - 512);
        w64(b + 512 - 12, core_lba + 1);                     /* the core image loads its remaining sectors from here */
        b[512 - 4] = (uint8_t)(core_sect - 1); b[512 - 3] = (uint8_t)((core_sect - 1) >> 8);
        if (bd->write(bd->ctx, core_lba, (uint32_t)core_sect, b)) r = FS_EIO;
    }
    kfree(b);
    return r;
}

static int put(const char *path, const char *mod) {
    uint64_t len; const uint8_t *data;
    int which = !strcmp(mod, "kernel") ? 0 : !strcmp(mod, "initrd") ? 1 : !strcmp(mod, "bootx64") ? 2 : !strcmp(mod, "biosboot") ? 3 : -1;
    if (which >= 0 && src_data[which]) { data = src_data[which]; len = src_len[which]; }
    else data = bootmod(mod, &len);
    if (!data) return FS_ENOENT;
    long r = vfs_write(path, data, len);
    return r < 0 ? (int)r : FS_OK;
}

static void installer(void *arg) {
    struct disk *d = 0;
    for (int i = 0; i < MAXD; i++) if (disks[i].used && disks[i].id == inst_id) d = &disks[i];
    int r = FS_OK;
    void *vol = 0;
    char why[96] = "", label[16];
    if (!d) { r = FS_ENOENT; strncpy0(why, "that disk is gone", sizeof why); goto done; }
    struct blockdev *bd = d->bd;
    if (inst_mode == INSTALL_ERASE) {
        if (d == sysdisk) { r = FS_EINVAL; strncpy0(why, "AXIOM is using that disk right now; use Update instead", sizeof why); goto done; }
        if (bd->release) bd->release(bd->ctx);              /* e.g. a USB stick that is open in Files */
        progress(2, "Making a new partition table");
        uint64_t first, count;
        r = write_gpt(bd, &first, &count);
        if (r) { ksnprintf(why, sizeof why, "the partition table couldn't be written (%s)", fs_error(r)); goto done; }
        progress(6, "Formatting the AXIOM partition (FAT32)");
        r = fat_format(bd, first, count, "AXIOM");
        if (r) { strncpy0(why, "formatting failed", sizeof why); goto done; }
        kyield();
        vol = fat_open(bd, first, label, why, sizeof why);
        if (!vol) { r = FS_EIO; goto done; }
    } else {
        int pi = -1;
        for (int i = 0; i < d->nparts; i++) if (d->p[i].duncos) pi = i;
        if (pi < 0) { r = FS_ENOENT; strncpy0(why, "there's no AXIOM on that disk to update", sizeof why); goto done; }
        vol = d->sysvol ? d->sysvol : fat_open(bd, d->p[pi].first, label, why, sizeof why);
        if (!vol) { r = FS_EIO; goto done; }
    }
    vfs_mount_at("/install", "/", "Installer", "disk", &fat_ops, vol);
    static const char *dirs[] = { "/install/EFI", "/install/EFI/BOOT", "/install/duncos", "/install/home", "/install/apps", "/install/config" };
    for (int i = 0; i < 6; i++) vfs_mkdir(dirs[i]);
    progress(12, "Copying the boot loader");
    if ((r = put("/install/EFI/BOOT/BOOTX64.EFI", "bootx64"))) { strncpy0(why, "the boot loader isn't available (start AXIOM from its USB stick or disk)", sizeof why); goto unmount; }
    kyield();
    if (inst_mode == INSTALL_UPDATE) {                    /* keep the version that works now, for the boot menu */
        struct dunc_stat st;
        if (vfs_stat("/install/duncos/thane.elf", &st) == FS_OK && vfs_stat("/install/duncos/initrd.tar", &st) == FS_OK) {
            vfs_delete("/install/duncos/thane-prev.elf"); vfs_delete("/install/duncos/initrd-prev.tar");
            vfs_rename("/install/duncos/thane.elf", "/install/duncos/thane-prev.elf");
            vfs_rename("/install/duncos/initrd.tar", "/install/duncos/initrd-prev.tar");
        }
    }
    progress(25, "Copying the Thane kernel");
    if ((r = put("/install/duncos/thane.elf", "kernel"))) { strncpy0(why, "the kernel image isn't available", sizeof why); goto unmount; }
    kyield();
    progress(40, "Copying the system files");
    if ((r = put("/install/duncos/initrd.tar", "initrd"))) { strncpy0(why, "the system files aren't available", sizeof why); goto unmount; }
    if (put("/install/duncos/biosboot.img", "biosboot") == FS_OK) {      /* legacy BIOS start-up as well as UEFI */
        uint64_t pf = 0;
        for (int i = 0; i < d->nparts; i++) if (d->p[i].duncos) pf = d->p[i].first;
        if (inst_mode == INSTALL_ERASE) pf = 2048 * 512 / bd->bsize;
        if (pf && write_biosboot(bd, pf) == FS_OK) LOG_INFO("installer: the disk can also start in legacy BIOS mode\n");
    }
    if ((r = write_grubcfg("/install/duncos"))) { strncpy0(why, "the boot menu couldn't be written", sizeof why); goto unmount; }
    if (inst_mode == INSTALL_ERASE) {
        static const char *from[] = { "/home", "/apps", "/system/config" }, *to[] = { "/install/home", "/install/apps", "/install/config" };
        struct dunc_dirent *e = kmalloc(sizeof *e * 128);
        for (int k = 0; k < 3 && e && r == FS_OK; k++) {
            progress(k == 0 ? 60 : 85, k == 0 ? "Copying your files" : "Copying apps and settings");
            int n = vfs_list(from[k], e, 128);
            for (int i = 0; i < n && r == FS_OK; i++) {
                char a[160], b[160];
                ksnprintf(a, sizeof a, "%s/%s", from[k], e[i].name); ksnprintf(b, sizeof b, "%s/%s", to[k], e[i].name);
                int c = vfs_copy(a, b);
                if (c && c != FS_EEXIST) r = c;
                kyield();
            }
        }
        kfree(e);
        if (r) { strncpy0(why, "your files couldn't be copied", sizeof why); goto unmount; }
    }
unmount:
    vfs_unmount_at("/install");
    if (bd->flush) bd->flush(bd->ctx);
    if (vol != d->sysvol) fat_unmount(vol);
    if (inst_mode == INSTALL_ERASE && r == FS_OK) {
        scan(d);
        if (!sysdisk) for (int i = 0; i < d->nparts; i++) if (d->p[i].duncos) { mount_system(d, &d->p[i]); break; }
    }
done:
    if (r) {
        char m[120]; ksnprintf(m, sizeof m, "Install failed: %s.", why[0] ? why : fs_error(r));
        progress(inst.percent, m); inst.state = 3;
    } else {
        progress(100, inst_mode == INSTALL_ERASE ? "AXIOM is installed. Your files are now saved on this disk." :
                      src_data[0] ? "The update is installed. Restart to start using it." : "AXIOM is updated.");
        inst.state = 2;
    }
    task_exit(0);
}

int disk_install(int id, int mode) {
    if (inst.state == 1) return FS_EINVAL;
    if (mode != INSTALL_ERASE && mode != INSTALL_UPDATE) return FS_EINVAL;
    memset(&inst, 0, sizeof inst);
    inst.state = 1; inst_id = id; inst_mode = mode;
    progress(0, "Starting");
    task_create_kernel("Installer", installer, 0);
    return FS_OK;
}

void disk_install_status(struct dunc_install *out) { *out = inst; }

int disk_system_id(void) { return sysdisk ? sysdisk->id : -1; }

int disk_update_from(int id, const uint8_t *const data[4], const uint64_t len[4]) {
    if (inst.state == 1) return FS_EINVAL;
    for (int i = 0; i < 4; i++) { src_data[i] = data[i]; src_len[i] = len[i]; }
    return disk_install(id, INSTALL_UPDATE);
}

int disk_set_boot_option(const char *key, const char *value) {
    if (strcmp(key, "gfxmode")) return FS_EINVAL;
    strncpy0(gfx_pref, value, sizeof gfx_pref);
    if (!sysdisk) return FS_ENOENT;                 /* takes effect when AXIOM is installed */
    return write_grubcfg("/system/boot");
}

void disk_boot_done(void) { booted = true; }
