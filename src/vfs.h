#pragma once
#include "kernel.h"

/* A mounted file system (e.g. a FAT32 USB drive at /usb). Paths given to the ops are
 * relative to the mount point and start with '/'. */
/* a folder entry inside the kernel: names up to 255 characters (apps' struct dunc_dirent has 63) */
struct vfs_dirent { char name[256]; int is_dir; uint64_t size; };
struct fs_ops {
    int  (*list)(void *vol, const char *rel, struct vfs_dirent *out, int max);
    long (*read)(void *vol, const char *rel, void *buf, uint64_t off, uint64_t len);
    long (*write)(void *vol, const char *rel, const void *buf, uint64_t len);
    int  (*mkdir)(void *vol, const char *rel);
    int  (*remove)(void *vol, const char *rel);         /* file or empty directory */
    int  (*rename)(void *vol, const char *from, const char *to);
    int  (*stat)(void *vol, const char *rel, struct dunc_stat *st);
    void (*space)(void *vol, uint64_t *total, uint64_t *free);
    long (*append)(void *vol, const char *rel, const void *buf, uint64_t len);   /* file must exist */
};

int  vfs_mount(const char *label, const char *kind, const struct fs_ops *ops, void *vol, char *path_out);
/* mount a folder of a volume at a path, e.g. ("/home", "/home") or ("/system/config", "/config") */
int  vfs_mount_at(const char *at, const char *sub, const char *label, const char *kind, const struct fs_ops *ops, void *vol);
void vfs_unmount(void *vol);                             /* removes every mount of that volume */
void vfs_unmount_at(const char *at);
bool vfs_home_persistent(void);                          /* /home is on a disk (else in memory) */
int  vfs_list(const char *path, struct dunc_dirent *out, int max);       /* for apps: names cut to 63 */
int  vfs_list_full(const char *path, struct vfs_dirent *out, int max);   /* full names (Linux programs' getdents) */
long vfs_read(const char *path, void *buf, uint64_t off, uint64_t len);
long vfs_write(const char *path, const void *buf, uint64_t len);
long vfs_append(const char *path, const void *buf, uint64_t len);   /* creates the file if missing */
int  vfs_mkdir(const char *path);
int  vfs_delete(const char *path);                       /* recursive */
int  vfs_rename(const char *from, const char *to);       /* works across volumes */
int  vfs_copy(const char *from, const char *to);         /* recursive */
int  vfs_stat(const char *path, struct dunc_stat *st);
int  vfs_volumes(struct dunc_volume *out, int max);
void *vfs_read_all(const char *path, uint64_t *len);     /* kmalloc'd */
void vfs_init(void);
void initrd_load(const uint8_t *start, const uint8_t *end);
const char *fs_error(int e);
