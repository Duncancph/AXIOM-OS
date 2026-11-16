/* Disks (SATA, NVMe, USB): partitions, the AXIOM system volume, and the installer. */
#pragma once
#include "kernel.h"
#include "blockdev.h"

/* Register a disk. Returns true if it holds the AXIOM volume and was mounted as the system
 * volume (/home, /apps, /system/config); the caller must then not mount it elsewhere. */
bool disk_add(struct blockdev *bd, int kind);
void disk_remove(struct blockdev *bd);
int  disk_list(struct dunc_disk *out, int max);
int  disk_install(int id, int mode, uint64_t size_mb);   /* starts the installer (size_mb: AXIOM's partition when erasing; 0 = all of it); 0 or an FS_ error */
void disk_install_status(struct dunc_install *out);
int  disk_system_id(void);                         /* the disk AXIOM runs from (installed), or -1 */
/* install new system files from memory (an update) instead of the running ones; the buffers are kept */
int  disk_update_from(int id, const uint8_t *const data[4], const uint64_t len[4]);   /* kernel, initrd, bootx64, biosboot (may be 0) */
int  disk_set_boot_option(const char *key, const char *value);   /* e.g. ("gfxmode", "1920x1080x32") */
void disk_flush_all(void);                         /* before power-off */
void disk_boot_done(void);
void ahci_init_all(void);
void nvme_init_all(void);
