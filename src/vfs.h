#pragma once
#include "kernel.h"

/* A mounted file system (e.g. a FAT32 USB drive at /usb). Paths given to the ops are
 * relative to the mount point and start with '/'. */
struct fs_ops {
    int  (*list)(void *vol, const char *rel, struct dunc_dirent *out, int max);
    long (*read)(void *vol, const char *rel, void *buf, uint64_t off, uint64_t len);
    long (*write)(void *vol, const char *rel, const void *buf, uint64_t len);
    int  (*mkdir)(void *vol, const char *rel);
    int  (*remove)(void *vol, const char *rel);         /* file or empty directory */
    int  (*rename)(void *vol, const char *from, const char *to);
    int  (*stat)(void *vol, const char *rel, struct dunc_stat *st);
    void (*space)(void *vol, uint64_t *total, uint64_t *free);
};

int  vfs_mount(const char *label, const char *kind, const struct fs_ops *ops, void *vol, char *path_out);
void vfs_unmount(void *vol);
int  vfs_list(const char *path, struct dunc_dirent *out, int max);
long vfs_read(const char *path, void *buf, uint64_t off, uint64_t len);
long vfs_write(const char *path, const void *buf, uint64_t len);
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
