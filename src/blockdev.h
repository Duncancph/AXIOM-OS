#pragma once
#include <stdint.h>
/* A disk that reads/writes fixed-size blocks (e.g. a USB flash drive). */
struct blockdev {
    void *ctx;
    uint32_t bsize;          /* bytes per block */
    uint64_t blocks;
    int (*read)(void *ctx, uint64_t lba, uint32_t n, void *buf);         /* 0 = ok */
    int (*write)(void *ctx, uint64_t lba, uint32_t n, const void *buf);
    char name[48];
};
/* fat.c */
void *fat_mount(struct blockdev *bd, char *label_out, char *why_not, int why_len);
void fat_unmount(void *vol);
extern const struct fs_ops fat_ops;
