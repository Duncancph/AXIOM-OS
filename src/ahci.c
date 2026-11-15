/* SATA disks through an AHCI controller (PCI class 01:06:01). Polled, one command at a time per port.
 * Each disk becomes a block device for the disk layer (disk.c). */
#include "kernel.h"
#include "blockdev.h"
#include "disk.h"

#define BUF_BYTES 65536

/* HBA registers */
#define CAP   0x00
#define GHC   0x04
#define PI    0x0C
#define CAP2  0x24
#define BOHC  0x28
/* port registers (0x100 + port * 0x80) */
#define PxCLB 0x00
#define PxFB  0x08
#define PxIS  0x10
#define PxIE  0x14
#define PxCMD 0x18
#define PxTFD 0x20
#define PxSIG 0x24
#define PxSSTS 0x28
#define PxSCTL 0x2C
#define PxSERR 0x30
#define PxCI  0x38

struct ahci_port {
    volatile uint8_t *r;            /* port registers */
    uint8_t *clb, *fb, *ctba;       /* command list, received FIS, command table */
    uint8_t *buf;                   /* 64 KiB bounce buffer for transfers */
    struct blockdev bd;
    bool lba48;
    int num;
    volatile int busy;              /* a command is in flight (other tasks wait their turn) */
    int slow_reports;
};

static inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

static bool wait_clear(volatile uint8_t *r, uint32_t off, uint32_t mask, int ms) {
    for (int i = 0; i < ms * 10; i++) { if (!(rd(r, off) & mask)) return true; udelay(100); }
    return false;
}

static void port_stop(volatile uint8_t *r) {
    wr(r, PxCMD, rd(r, PxCMD) & ~1u);                    /* ST */
    wait_clear(r, PxCMD, 1u << 15, 500);                 /* CR */
    wr(r, PxCMD, rd(r, PxCMD) & ~(1u << 4));             /* FRE */
    wait_clear(r, PxCMD, 1u << 14, 500);                 /* FR */
}

static void port_start(volatile uint8_t *r) {
    wait_clear(r, PxCMD, 1u << 15, 500);
    wr(r, PxCMD, rd(r, PxCMD) | (1u << 4));
    wr(r, PxCMD, rd(r, PxCMD) | 1u);
}

/* Run one ATA command. dir: 0 none, 1 read into buf, 2 write from buf. Returns 0 ok. */
static int ata_cmd(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, int dir) {
    volatile uint8_t *r = p->r;
    while (p->busy) kyield();
    p->busy = 1;
    if (!wait_clear(r, PxTFD, 0x88, 2000)) { p->busy = 0; return -1; }    /* BSY | DRQ */
    uint8_t *h = p->clb;                                 /* command header 0 */
    memset(h, 0, 32);
    h[0] = 5 | (dir == 2 ? 0x40 : 0);                    /* FIS length 5 dwords, W bit */
    *(uint16_t *)(h + 2) = bytes ? 1 : 0;                /* PRDT entries */
    *(uint64_t *)(h + 8) = (uint64_t)(uintptr_t)p->ctba;
    uint8_t *t = p->ctba;
    memset(t, 0, 0x80 + 16);
    t[0] = 0x27; t[1] = 0x80; t[2] = cmd;                 /* register host-to-device FIS, command */
    t[4] = lba; t[5] = lba >> 8; t[6] = lba >> 16; t[7] = 0x40;    /* LBA mode */
    t[8] = lba >> 24; t[9] = lba >> 32; t[10] = lba >> 40;
    t[12] = count; t[13] = count >> 8;
    if (bytes) {
        *(uint64_t *)(t + 0x80) = (uint64_t)(uintptr_t)p->buf;
        *(uint32_t *)(t + 0x80 + 12) = bytes - 1;        /* byte count - 1 */
    }
    wr(r, PxIS, 0xFFFFFFFF);
    wr(r, PxCI, 1);
    /* wait for it, letting the rest of the system run meanwhile (a slow or stuck disk mustn't freeze everything) */
    uint64_t t0 = uptime_ms();
    for (;;) {
        if (!(rd(r, PxCI) & 1)) break;
        if (rd(r, PxIS) & (1u << 30)) break;             /* task file error */
        if (uptime_ms() - t0 > 10000) break;              /* give up after 10 s */
        if (uptime_ms() - t0 < 2) udelay(20); else kyield();
    }
    uint64_t took = uptime_ms() - t0;
    if (took > 1000 && p->slow_reports++ < 10) LOG_WARN("SATA disk %d: command %02x took %lu ms\n", p->num, cmd, took);
    uint32_t tfd = rd(r, PxTFD);
    if ((rd(r, PxCI) & 1) || (rd(r, PxIS) & (1u << 30)) || (tfd & 1)) {
        LOG_WARN("SATA disk %d: command %02x failed (status %02x error %02x)\n", p->num, cmd, tfd & 0xFF, (tfd >> 8) & 0xFF);
        port_stop(r); wr(r, PxSERR, 0xFFFFFFFF); wr(r, PxIS, 0xFFFFFFFF); port_start(r);   /* recover */
        p->busy = 0;
        return -1;
    }
    p->busy = 0;
    return 0;
}

static int ahci_rw(struct ahci_port *p, uint64_t lba, uint32_t n, void *buf, bool write) {
    uint32_t max = BUF_BYTES / p->bd.bsize;
    while (n) {
        uint32_t c = n < max ? n : max, bytes = c * p->bd.bsize;
        if (write) memcpy(p->buf, buf, bytes);
        uint8_t cmd = p->lba48 ? (write ? 0x35 : 0x25) : (write ? 0xCA : 0xC8);    /* READ/WRITE DMA (EXT) */
        int r = -1;
        for (int tries = 0; tries < 3 && r; tries++) r = ata_cmd(p, cmd, lba, c, bytes, write ? 2 : 1);
        if (r) return -1;
        if (!write) memcpy(buf, p->buf, bytes);
        lba += c; n -= c; buf = (uint8_t *)buf + bytes;
    }
    return 0;
}
static int bd_read(void *ctx, uint64_t lba, uint32_t n, void *buf) { return ahci_rw(ctx, lba, n, buf, false); }
static int bd_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) { return ahci_rw(ctx, lba, n, (void *)buf, true); }
static int bd_flush(void *ctx) { struct ahci_port *p = ctx; return ata_cmd(p, p->lba48 ? 0xEA : 0xE7, 0, 0, 0, 0); }

static void ata_string(char *out, const uint16_t *w, int words) {
    int k = 0;
    for (int i = 0; i < words; i++) { out[k++] = w[i] >> 8; out[k++] = w[i] & 0xFF; }
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
    char *s = out; while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

static void port_init(volatile uint8_t *hba, int num) {
    volatile uint8_t *r = hba + 0x100 + num * 0x80;
    uint32_t ssts = rd(r, PxSSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return;      /* no device / not active */
    uint32_t sig = rd(r, PxSIG);
    if (sig == 0xEB140101) { LOG_INFO("SATA port %d: optical drive (not supported)\n", num); return; }
    if (sig != 0x00000101) return;
    struct ahci_port *p = kmalloc(sizeof *p);
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->r = r; p->num = num;
    uint8_t *mem = page_alloc(2);                       /* command list 1 KiB, FIS 256 B, command table */
    p->buf = page_alloc(BUF_BYTES / 4096);
    if (!mem || !p->buf) return;
    p->clb = mem; p->fb = mem + 1024; p->ctba = mem + 4096;
    port_stop(r);
    /* no link power saving: some disks and controllers stop answering after dropping into partial/slumber */
    wr(r, PxSCTL, (rd(r, PxSCTL) & ~0xF00u) | 0x300u);
    wr(r, PxCMD, rd(r, PxCMD) & ~((1u << 26) | (1u << 27)));     /* ALPE, ASP */
    *(volatile uint64_t *)(r + PxCLB) = (uint64_t)(uintptr_t)p->clb;
    *(volatile uint64_t *)(r + PxFB) = (uint64_t)(uintptr_t)p->fb;
    wr(r, PxSERR, 0xFFFFFFFF); wr(r, PxIS, 0xFFFFFFFF); wr(r, PxIE, 0);
    port_start(r);
    if (ata_cmd(p, 0xEC, 0, 0, 512, 1)) { LOG_WARN("SATA port %d: IDENTIFY failed\n", num); return; }   /* IDENTIFY DEVICE */
    uint16_t *id = (uint16_t *)p->buf;
    p->lba48 = (id[83] >> 10) & 1;
    uint64_t sectors = p->lba48 ? *(uint64_t *)(id + 100) : *(uint32_t *)(id + 60);
    uint32_t bsize = 512;
    if ((id[106] & 0xD000) == 0x5000 && (id[106] & (1 << 12))) bsize = 2 * (*(uint32_t *)(id + 117));   /* long logical sectors */
    if (bsize != 512 && bsize != 4096) { LOG_WARN("SATA port %d: %u-byte sectors not supported\n", num, bsize); return; }
    char model[48]; ata_string(model, id + 27, 20);
    p->bd.ctx = p; p->bd.bsize = bsize; p->bd.blocks = sectors;
    p->bd.read = bd_read; p->bd.write = bd_write; p->bd.flush = bd_flush;
    strncpy0(p->bd.name, model[0] ? model : "SATA disk", sizeof p->bd.name);
    LOG_OK("SATA disk: %s, %lu MB\n", p->bd.name, sectors * bsize >> 20);
    disk_add(&p->bd, DISK_SATA);
}

static void ahci_init(struct pci_dev *pd) {
    uint64_t sz, phys = pci_bar(pd, 5, &sz);
    if (!phys) { dev_claim(pd, "ahci", DEV_FAILED, 0, "Its registers aren't set up (no memory window), so it can't be used."); return; }
    uint16_t cmd = pci_read16(pd->bus, pd->dev, pd->fn, 4);
    pci_write16(pd->bus, pd->dev, pd->fn, 4, cmd | 6);    /* memory space + bus master */
    volatile uint8_t *hba = map_mmio(phys, sz ? sz : 0x1100);
    if (rd(hba, CAP2) & 1) {                             /* BIOS/OS handoff */
        wr(hba, BOHC, rd(hba, BOHC) | 2);
        for (int i = 0; i < 250 && (rd(hba, BOHC) & 1); i++) sleep_ms(10);
    }
    wr(hba, GHC, rd(hba, GHC) | (1u << 31));             /* AHCI mode */
    wr(hba, GHC, rd(hba, GHC) & ~2u);                    /* no interrupts: polled */
    uint32_t pi = rd(hba, PI);
    int np = 0;
    for (int i = 0; i < 32; i++) if (pi & (1u << i)) { port_init(hba, i); np++; }
    char note[80]; ksnprintf(note, sizeof note, "SATA controller (AHCI), %d port%s", np, np == 1 ? "" : "s");
    dev_claim(pd, "ahci", DEV_OK, 0, note);
}

void ahci_init_all(void) {
    for (int i = 0; i < pci_count; i++)
        if (pci_devs[i].class_code == 0x01 && pci_devs[i].subclass == 0x06 && pci_devs[i].prog_if == 0x01) ahci_init(&pci_devs[i]);
}
