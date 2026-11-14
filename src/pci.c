#include "kernel.h"

struct pci_dev pci_devs[256];
int pci_count;

static uint32_t addr(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    return 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC);
}
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { outl(0xCF8, addr(b, d, f, off)); return inl(0xCFC); }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) { outl(0xCF8, addr(b, d, f, off)); outl(0xCFC, v); }
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { return pci_read32(b, d, f, off) >> ((off & 2) * 8); }
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v) {
    uint32_t x = pci_read32(b, d, f, off), sh = (off & 2) * 8;
    x = (x & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(b, d, f, off, x);
}

uint64_t pci_bar(struct pci_dev *p, int bar, uint64_t *size) {
    uint8_t off = 0x10 + bar * 4;
    uint32_t lo = pci_read32(p->bus, p->dev, p->fn, off);
    if (lo & 1) { if (size) *size = 0; return lo & ~3u; }  /* I/O BAR */
    uint64_t a = lo & ~0xFu;
    bool is64 = ((lo >> 1) & 3) == 2;
    if (is64) a |= (uint64_t)pci_read32(p->bus, p->dev, p->fn, off + 4) << 32;
    if (size) {
        uint16_t cmd = pci_read16(p->bus, p->dev, p->fn, 4);
        pci_write16(p->bus, p->dev, p->fn, 4, cmd & ~3);   /* disable decode while sizing */
        pci_write32(p->bus, p->dev, p->fn, off, 0xFFFFFFFF);
        uint32_t s = pci_read32(p->bus, p->dev, p->fn, off) & ~0xFu;
        pci_write32(p->bus, p->dev, p->fn, off, lo);
        pci_write16(p->bus, p->dev, p->fn, 4, cmd);
        *size = s ? (uint64_t)(~s + 1) : 0;
    }
    return a;
}

static void add(uint8_t b, uint8_t d, uint8_t f) {
    if (pci_count >= 256) return;
    uint32_t id = pci_read32(b, d, f, 0), cls = pci_read32(b, d, f, 8);
    pci_devs[pci_count++] = (struct pci_dev){ b, d, f, id & 0xFFFF, id >> 16, cls >> 24, (cls >> 16) & 0xFF, (cls >> 8) & 0xFF, cls & 0xFF };
}

void pci_scan(void) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++) {
            if ((pci_read32(b, d, 0, 0) & 0xFFFF) == 0xFFFF) continue;
            add(b, d, 0);
            if (pci_read32(b, d, 0, 0x0C) & 0x00800000)    /* multi-function */
                for (int f = 1; f < 8; f++)
                    if ((pci_read32(b, d, f, 0) & 0xFFFF) != 0xFFFF) add(b, d, f);
        }
}

const char *pci_class_name(uint8_t c, uint8_t s, uint8_t pif) {
    switch (c) {
    case 0x01: return s == 0x06 ? "SATA controller" : s == 0x08 ? "NVMe storage" : "Storage controller";
    case 0x02: return s == 0x80 ? "Network controller (Wi-Fi?)" : "Ethernet controller";
    case 0x03: return "Display controller";
    case 0x04: return s == 0x03 ? "Audio device" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06: return s == 0 ? "Host bridge" : s == 1 ? "ISA bridge" : s == 4 ? "PCI bridge" : "Bridge";
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input controller";
    case 0x0C:
        if (s == 0x03) return pif == 0x30 ? "USB 3 (xHCI) controller" : pif == 0x20 ? "USB 2 (EHCI) controller" :
                              pif == 0x40 ? "USB4 controller" : "USB 1 controller";
        if (s == 0x05) return "SMBus controller";
        if (s == 0x80) return "Serial bus controller";
        return "Serial bus controller";
    case 0x0D: return s == 0x11 ? "Bluetooth controller" : "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    case 0x13: return "Instrumentation";
    }
    return "Device";
}
