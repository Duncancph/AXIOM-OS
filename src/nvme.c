/* NVMe SSDs (PCI class 01:08:02). One admin queue and one I/O queue pair, polled.
 * Namespace 1 of each controller becomes a block device for the disk layer (disk.c). */
#include "kernel.h"
#include "blockdev.h"
#include "disk.h"

#define BUF_BYTES 65536
#define QD 16                                   /* entries per queue */

struct nvq { uint8_t *sq, *cq; uint16_t sq_tail, cq_head; uint8_t phase; volatile uint32_t *sq_db, *cq_db; uint16_t cid; };

struct nvme {
    volatile uint8_t *r;
    uint32_t dstrd;
    struct nvq admin, io;
    uint8_t *buf;                               /* 64 KiB transfer buffer (physically contiguous) */
    uint64_t *prp;                              /* PRP list page for transfers over 8 KiB */
    uint32_t nsid, lbads;
    struct blockdev bd;
    int64_t timeout_ms;
};

static inline uint32_t rd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline uint64_t rd64(volatile uint8_t *b, uint32_t o) { return *(volatile uint64_t *)(b + o); }
static inline void wr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void wr64(volatile uint8_t *b, uint32_t o, uint64_t v) { *(volatile uint64_t *)(b + o) = v; }

static bool q_init(struct nvme *n, struct nvq *q, int qid) {
    q->sq = page_alloc(1); q->cq = page_alloc(1);
    if (!q->sq || !q->cq) return false;
    q->sq_tail = q->cq_head = 0; q->phase = 1; q->cid = 0;
    q->sq_db = (volatile uint32_t *)(n->r + 0x1000 + (2 * qid) * (4u << n->dstrd));
    q->cq_db = (volatile uint32_t *)(n->r + 0x1000 + (2 * qid + 1) * (4u << n->dstrd));
    return true;
}

/* submit a 64-byte command and wait for its completion; returns the status field (0 = success) or -1 on timeout */
static int submit(struct nvme *n, struct nvq *q, uint32_t *cmd) {
    uint16_t cid = ++q->cid;
    cmd[0] = (cmd[0] & 0xFFFF) | (uint32_t)cid << 16;
    memcpy(q->sq + q->sq_tail * 64, cmd, 64);
    q->sq_tail = (q->sq_tail + 1) % QD;
    __asm__ volatile("mfence" ::: "memory");
    *q->sq_db = q->sq_tail;
    for (int64_t i = 0; i < n->timeout_ms * 10; i++) {
        volatile uint8_t *e = q->cq + q->cq_head * 16;
        uint16_t st = *(volatile uint16_t *)(e + 14);
        if ((st & 1) == q->phase) {
            uint16_t got = *(volatile uint16_t *)(e + 12);
            q->cq_head = (q->cq_head + 1) % QD;
            if (!q->cq_head) q->phase ^= 1;
            *q->cq_db = q->cq_head;
            if (got != cid) continue;                 /* a stale completion: keep waiting for ours */
            return (st >> 1) & 0x7FFF;
        }
        udelay(100);
    }
    return -1;
}

/* data pointers for a transfer from the bounce buffer */
static void set_prp(struct nvme *n, uint32_t *cmd, uint32_t bytes) {
    uint64_t b = (uint64_t)(uintptr_t)n->buf;
    *(uint64_t *)(cmd + 6) = b;
    uint32_t pages = (bytes + 4095) / 4096;
    if (pages == 2) *(uint64_t *)(cmd + 8) = b + 4096;
    else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++) n->prp[i - 1] = b + i * 4096ull;
        *(uint64_t *)(cmd + 8) = (uint64_t)(uintptr_t)n->prp;
    }
}

static int nvme_rw(struct nvme *n, uint64_t lba, uint32_t cnt, void *buf, bool write) {
    uint32_t max = BUF_BYTES / n->bd.bsize;
    while (cnt) {
        uint32_t c = cnt < max ? cnt : max, bytes = c * n->bd.bsize;
        if (write) memcpy(n->buf, buf, bytes);
        uint32_t cmd[16]; memset(cmd, 0, sizeof cmd);
        cmd[0] = write ? 0x01 : 0x02; cmd[1] = n->nsid;
        set_prp(n, cmd, bytes);
        cmd[10] = (uint32_t)lba; cmd[11] = (uint32_t)(lba >> 32); cmd[12] = c - 1;
        int st = -1;
        for (int tries = 0; tries < 3 && st; tries++) st = submit(n, &n->io, cmd);
        if (st) { LOG_WARN("NVMe: %s failed at block %lu (status %x)\n", write ? "write" : "read", lba, st); return -1; }
        if (!write) memcpy(buf, n->buf, bytes);
        lba += c; cnt -= c; buf = (uint8_t *)buf + bytes;
    }
    return 0;
}
static int bd_read(void *ctx, uint64_t lba, uint32_t n, void *buf) { return nvme_rw(ctx, lba, n, buf, false); }
static int bd_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) { return nvme_rw(ctx, lba, n, (void *)buf, true); }
static int bd_flush(void *ctx) {
    struct nvme *n = ctx;
    uint32_t cmd[16]; memset(cmd, 0, sizeof cmd); cmd[0] = 0x00; cmd[1] = n->nsid;
    return submit(n, &n->io, cmd) ? -1 : 0;
}

static void nvme_init(struct pci_dev *pd) {
    uint64_t sz, phys = pci_bar(pd, 0, &sz);
    dev_claim(pd, "nvme", DEV_FAILED, 0, "The SSD didn't start (see the boot log).");
    if (!phys) return;
    uint16_t pcmd = pci_read16(pd->bus, pd->dev, pd->fn, 4);
    pci_write16(pd->bus, pd->dev, pd->fn, 4, (pcmd | 6) | (1 << 10));   /* memory + bus master, INTx off */
    struct nvme *n = kmalloc(sizeof *n);
    if (!n) return;
    memset(n, 0, sizeof *n);
    n->r = map_mmio(phys, sz ? sz : 0x4000);
    uint64_t cap = rd64(n->r, 0x00);
    n->dstrd = (cap >> 32) & 0xF;
    n->timeout_ms = ((cap >> 24) & 0xFF) * 500 + 1000;
    if (!((cap >> 37) & 1)) { LOG_WARN("NVMe: controller without the NVM command set\n"); return; }
    if (((cap >> 48) & 0xF) > 0) { LOG_WARN("NVMe: 4 KiB pages not supported by controller\n"); return; }
    /* reset: disable, wait for not-ready */
    wr32(n->r, 0x14, rd32(n->r, 0x14) & ~1u);
    for (int64_t i = 0; i < n->timeout_ms && (rd32(n->r, 0x1C) & 1); i++) sleep_ms(1);
    if (!q_init(n, &n->admin, 0) || !q_init(n, &n->io, 1)) return;
    n->buf = page_alloc(BUF_BYTES / 4096); n->prp = page_alloc(1);
    if (!n->buf || !n->prp) return;
    wr32(n->r, 0x24, (QD - 1) << 16 | (QD - 1));         /* AQA */
    wr64(n->r, 0x28, (uint64_t)(uintptr_t)n->admin.sq);  /* ASQ */
    wr64(n->r, 0x30, (uint64_t)(uintptr_t)n->admin.cq);  /* ACQ */
    wr32(n->r, 0x14, (4u << 20) | (6u << 16) | 1);       /* CC: IOCQES 16 B, IOSQES 64 B, 4 KiB pages, NVM, enable */
    int64_t t;
    for (t = 0; t < n->timeout_ms && !(rd32(n->r, 0x1C) & 1); t++) sleep_ms(1);
    if (!(rd32(n->r, 0x1C) & 1) || (rd32(n->r, 0x1C) & 2)) { LOG_WARN("NVMe: controller didn't start\n"); return; }

    uint32_t cmd[16];
    /* identify controller: model name */
    memset(cmd, 0, sizeof cmd); cmd[0] = 0x06; set_prp(n, cmd, 4096); cmd[10] = 1;
    if (submit(n, &n->admin, cmd)) { LOG_WARN("NVMe: identify failed\n"); return; }
    char model[48]; memcpy(model, n->buf + 24, 40); model[40] = 0;
    for (int k = 39; k >= 0 && model[k] == ' '; k--) model[k] = 0;
    /* identify namespace 1 */
    n->nsid = 1;
    memset(cmd, 0, sizeof cmd); cmd[0] = 0x06; cmd[1] = n->nsid; set_prp(n, cmd, 4096); cmd[10] = 0;
    if (submit(n, &n->admin, cmd)) { LOG_WARN("NVMe %s: no namespace 1\n", model); return; }
    uint64_t nsze = *(uint64_t *)n->buf;
    uint8_t flbas = n->buf[26] & 0xF;
    uint32_t lbaf = *(uint32_t *)(n->buf + 128 + flbas * 4);
    n->lbads = (lbaf >> 16) & 0xFF;
    if (!nsze || (n->lbads != 9 && n->lbads != 12)) { LOG_WARN("NVMe %s: unsupported format\n", model); return; }
    /* I/O completion queue 1, then submission queue 1 (no interrupts) */
    memset(cmd, 0, sizeof cmd); cmd[0] = 0x05; *(uint64_t *)(cmd + 6) = (uint64_t)(uintptr_t)n->io.cq;
    cmd[10] = (QD - 1) << 16 | 1; cmd[11] = 1;            /* physically contiguous */
    if (submit(n, &n->admin, cmd)) { LOG_WARN("NVMe: can't create completion queue\n"); return; }
    memset(cmd, 0, sizeof cmd); cmd[0] = 0x01; *(uint64_t *)(cmd + 6) = (uint64_t)(uintptr_t)n->io.sq;
    cmd[10] = (QD - 1) << 16 | 1; cmd[11] = 1 << 16 | 1;  /* completion queue 1, contiguous */
    if (submit(n, &n->admin, cmd)) { LOG_WARN("NVMe: can't create submission queue\n"); return; }

    n->bd.ctx = n; n->bd.bsize = 1u << n->lbads; n->bd.blocks = nsze;
    n->bd.read = bd_read; n->bd.write = bd_write; n->bd.flush = bd_flush;
    strncpy0(n->bd.name, model[0] ? model : "NVMe SSD", sizeof n->bd.name);
    LOG_OK("NVMe SSD: %s, %lu MB\n", n->bd.name, nsze * n->bd.bsize >> 20);
    disk_add(&n->bd, DISK_NVME);
    char note[96]; ksnprintf(note, sizeof note, "NVMe SSD controller, %lu GB", nsze * n->bd.bsize >> 30);
    dev_claim(pd, "nvme", DEV_OK, n->bd.name, note);
}

void nvme_init_all(void) {
    for (int i = 0; i < pci_count; i++)
        if (pci_devs[i].class_code == 0x01 && pci_devs[i].subclass == 0x08 && pci_devs[i].prog_if == 0x02) nvme_init(&pci_devs[i]);
}
