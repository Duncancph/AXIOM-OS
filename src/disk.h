/* Disks (SATA, NVMe, USB): partitions, the DuncOS system volume, and the installer. */
#pragma once
#include "kernel.h"
#include "blockdev.h"

/* Register a disk. Returns true if it holds the DuncOS volume and was mounted as the system
 * volume (/home, /apps, /system/config); the caller must then not mount it elsewhere. */
bool disk_add(struct blockdev *bd, int kind);
void disk_remove(struct blockdev *bd);
int  disk_list(struct dunc_disk *out, int max);
int  disk_install(int id, int mode);               /* starts the installer; 0 or an FS_ error */
void disk_install_status(struct dunc_install *out);
int  disk_set_boot_option(const char *key, const char *value);   /* e.g. ("gfxmode", "1920x1080x32") */
void disk_flush_all(void);                         /* before power-off */
void disk_boot_done(void);
void ahci_init_all(void);
void nvme_init_all(void);
