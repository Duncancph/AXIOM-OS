/* USB Mass Storage (Bulk-Only Transport + SCSI): USB flash drives and card readers. */
#include "usb.h"
#include "vfs.h"
#include "blockdev.h"
#include "disk.h"

#define BUF_BYTES 65536

struct msc {
    struct usb_dev *d;
    int iface, in_dci, out_dci; uint8_t in_ep, out_ep;
    uint32_t tag;
    uint8_t *buf;            /* 64 KiB DMA data buffer */
    uint8_t *cmd;            /* CBW/CSW buffer */
    struct blockdev bd;
    void *vol;
    char label[32];
};

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rbe32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void reset_recovery(struct msc *m) {
    usb_control(m->d, 0x21, 0xFF, 0, m->iface, 0, 0);        /* Bulk-Only Mass Storage Reset */
    usb_clear_halt(m->d, m->in_dci, m->in_ep);
    usb_clear_halt(m->d, m->out_dci, m->out_ep);
}

/* One SCSI command. Returns 0 ok, 1 command failed (check sense), -1 transport error. */
static int scsi(struct msc *m, const uint8_t *cb, int cblen, bool in, uint32_t len) {
    uint8_t *cbw = m->cmd;
    memset(cbw, 0, 31);
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    uint32_t tag = ++m->tag;
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = cblen;
    memcpy(cbw + 15, cb, cblen);
    long r = usb_bulk(m->d, m->out_dci, cbw, 31, 3000);
    if (r == -2) { usb_clear_halt(m->d, m->out_dci, m->out_ep); return -1; }
    if (r != 31) { reset_recovery(m); return -1; }
    if (len) {
        r = usb_bulk(m->d, in ? m->in_dci : m->out_dci, m->buf, len, 10000);
        if (r == -2) usb_clear_halt(m->d, in ? m->in_dci : m->out_dci, in ? m->in_ep : m->out_ep);
        else if (r < 0) { reset_recovery(m); return -1; }
    }
    uint8_t *csw = m->cmd + 64;
    r = usb_bulk(m->d, m->in_dci, csw, 13, 3000);
    if (r == -2) { usb_clear_halt(m->d, m->in_dci, m->in_ep); r = usb_bulk(m->d, m->in_dci, csw, 13, 3000); }
    if (r != 13 || memcmp(csw, "USBS", 4) || memcmp(csw + 4, &tag, 4)) { reset_recovery(m); return -1; }
    if (csw[12] == 2) { reset_recovery(m); return -1; }   /* phase error */
    return csw[12] ? 1 : 0;
}

static int msc_rw(struct msc *m, uint64_t lba, uint32_t n, void *buf, bool write) {
    uint32_t max = BUF_BYTES / m->bd.bsize;
    usb_lock(m->d);
    int rc = 0;
    while (n && rc == 0) {
        uint32_t c = n < max ? n : max, bytes = c * m->bd.bsize;
        uint8_t cb[10] = { write ? 0x2A : 0x28, 0 };
        be32(cb + 2, (uint32_t)lba); cb[7] = c >> 8; cb[8] = c;
        if (write) memcpy(m->buf, buf, bytes);
        int r = -1;
        for (int tries = 0; tries < 3 && r != 0; tries++) {
            if (!m->d->used) break;
            r = scsi(m, cb, 10, !write, bytes);
        }
        if (r != 0) { rc = -1; break; }
        if (!write) memcpy(buf, m->buf, bytes);
        lba += c; n -= c; buf = (uint8_t *)buf + bytes;
    }
    usb_unlock(m->d);
    return rc;
}
static int bd_read(void *ctx, uint64_t lba, uint32_t n, void *buf) { return msc_rw(ctx, lba, n, buf, false); }
static int bd_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) { return msc_rw(ctx, lba, n, (void *)buf, true); }

static void msc_release(void *ctx) {
    struct msc *m = ctx;
    if (m->vol) { vfs_unmount(m->vol); fat_unmount(m->vol); m->vol = 0; }
}

static void msc_detach(struct usb_dev *d) {
    struct msc *m = d->drv;
    if (!m) return;
    disk_remove(&m->bd);
    if (m->vol) {
        vfs_unmount(m->vol);
        fat_unmount(m->vol);
        char t[96]; ksnprintf(t, sizeof t, "%s was removed.", m->label);
        desktop_notify("USB drive removed", t);
    }
    d->drv = 0;
}

static void trim(char *s) { int l = strlen(s); while (l && s[l - 1] == ' ') s[--l] = 0; }

void msc_attach(struct usb_dev *d, int iface) {
    const uint8_t *ssi, *sso;
    const uint8_t *epi = usb_find_endpoint(d, iface, 2, true, &ssi), *epo = usb_find_endpoint(d, iface, 2, false, &sso);
    if (!epi || !epo) return;
    struct msc *m = kmalloc(sizeof *m);
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->d = d; m->iface = iface;
    m->in_ep = epi[2]; m->out_ep = epo[2];
    m->in_dci = usb_configure_ep(d, epi, ssi);
    m->out_dci = usb_configure_ep(d, epo, sso);
    m->buf = page_alloc(BUF_BYTES / 4096);
    m->cmd = page_alloc(1);
    if (m->in_dci < 0 || m->out_dci < 0 || !m->buf || !m->cmd) { LOG_WARN("USB drive: setup failed\n"); return; }
    d->driver = "storage"; d->drv = m; d->detach = msc_detach;

    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    char vendor[9] = "", product[17] = "";
    if (scsi(m, inq, 6, true, 36) == 0) {
        memcpy(vendor, m->buf + 8, 8); vendor[8] = 0; trim(vendor);
        memcpy(product, m->buf + 16, 16); product[16] = 0; trim(product);
    }
    /* wait for the medium to be ready (card readers / slow sticks) */
    bool ready = false;
    for (int i = 0; i < 25 && !ready; i++) {
        uint8_t tur[6] = { 0 };
        int r = scsi(m, tur, 6, false, 0);
        if (r == 0) ready = true;
        else { uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 }; scsi(m, rs, 6, true, 18); sleep_ms(200); }
    }
    uint8_t rc[10] = { 0x25 };
    if (!ready || scsi(m, rc, 10, true, 8) != 0) { LOG_WARN("USB drive %s %s: no medium\n", vendor, product); return; }
    m->bd.blocks = (uint64_t)rbe32(m->buf) + 1;
    m->bd.bsize = rbe32(m->buf + 4);
    if (m->bd.bsize != 512 && m->bd.bsize != 4096) { LOG_WARN("USB drive: unsupported block size %u\n", m->bd.bsize); return; }
    m->bd.ctx = m; m->bd.read = bd_read; m->bd.write = bd_write;
    ksnprintf(m->bd.name, sizeof m->bd.name, "%s %s", vendor, product);
    uint64_t mb = m->bd.blocks * m->bd.bsize >> 20;
    LOG_OK("USB drive: %s %s, %lu MB\n", vendor, product, mb);

    m->bd.release = msc_release;
    if (disk_add(&m->bd, DISK_USB)) return;              /* a DuncOS drive: it holds /home now */
    char label[16] = "", why[96] = "";
    m->vol = fat_mount(&m->bd, label, why, sizeof why);
    if (!m->vol) {
        char t[160]; ksnprintf(t, sizeof t, "%s %s: %s", vendor, product, why);
        desktop_notify("USB drive not readable", t);
        return;
    }
    char path[16];
    strncpy0(m->label, label[0] ? label : "USB drive", sizeof m->label);
    vfs_mount(m->label, "FAT32", &fat_ops, m->vol, path);
    char t[128]; ksnprintf(t, sizeof t, "%s (%lu MB) is ready in Files and at %s.", m->label, mb, path);
    desktop_notify("USB drive connected", t);
}
