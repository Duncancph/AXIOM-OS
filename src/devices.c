/* The Device Manager: every device AXIOM found and which driver looks after it.
 *
 * Drivers record the PCI devices they take with dev_claim() (working, or failed and why). The list for
 * Settings > Devices and the CMD 'devices' command is built when it's asked for, from the PCI devices
 * (claimed or not), the USB devices (which already note their driver), the drives, the processor, the
 * screen and the input devices. One line per device:
 *     kind \t name \t driver \t status \t where \t ids \t details
 * status: ok, failed, nodriver (nothing in AXIOM drives it), system (needs no driver), off. */
#include "kernel.h"
#include "usb.h"

struct claim { uint8_t bus, dev, fn, status; char driver[24], name[64], note[112]; };
static struct claim claims[64]; static int nclaims;

void dev_claim(const struct pci_dev *pd, const char *driver, int status, const char *name, const char *note) {
    struct claim *c = 0;
    for (int i = 0; i < nclaims; i++) if (claims[i].bus == pd->bus && claims[i].dev == pd->dev && claims[i].fn == pd->fn) c = &claims[i];
    if (!c) { if (nclaims >= 64) return; c = &claims[nclaims++]; c->bus = pd->bus; c->dev = pd->dev; c->fn = pd->fn; }
    c->status = (uint8_t)status;
    strncpy0(c->driver, driver ? driver : "", sizeof c->driver);
    strncpy0(c->name, name ? name : "", sizeof c->name);
    strncpy0(c->note, note ? note : "", sizeof c->note);
}

/* ---- building the list ---- */
static char *o; static int on, ocap;
static void put(const char *s) {
    for (; s && *s && on < ocap - 2; s++) o[on++] = (*s == '\t' || *s == '\n') ? ' ' : *s;
}
void dev_line(const char *kind, const char *name, const char *driver, const char *status, const char *where, const char *ids, const char *detail) {
    if (on > ocap - 300) return;
    put(kind); o[on++] = '\t'; put(name); o[on++] = '\t'; put(driver); o[on++] = '\t'; put(status); o[on++] = '\t';
    put(where); o[on++] = '\t'; put(ids); o[on++] = '\t'; put(detail); o[on++] = '\n'; o[on] = 0;
}

static const char *vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: case 0x8087: return "Intel"; case 0x10EC: case 0x0BDA: return "Realtek"; case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros"; case 0x17CB: return "Qualcomm"; case 0x1022: return "AMD"; case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA"; case 0x1B4B: return "Marvell"; case 0x144D: return "Samsung"; case 0x15B7: return "Western Digital";
    case 0x1987: return "Phison"; case 0x126F: return "Silicon Motion"; case 0x1B21: return "ASMedia"; case 0x1912: return "Renesas";
    case 0x1234: return "QEMU"; case 0x1AF4: return "Red Hat (virtual)"; case 0x15AD: return "VMware"; case 0x80EE: return "VirtualBox";
    case 0x14C3: return "MediaTek"; case 0x1969: return "Qualcomm Atheros"; case 0x11AB: return "Marvell";
    }
    return 0;
}
static const char *pci_kind(const struct pci_dev *p) {
    switch (p->class_code) {
    case 0x01: return "Storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Sound";
    case 0x0C: return p->subclass == 0x03 ? "USB" : "System";
    case 0x0D: return p->subclass == 0x11 ? "Bluetooth" : "Network";
    case 0x09: return "Input";
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x10: case 0x11: case 0x13: return "System";
    }
    return "Other";
}
/* parts of the PC that work without a driver of their own (bridges, memory controller, management) */
static bool needs_no_driver(const struct pci_dev *p) {
    return p->class_code == 0x05 || p->class_code == 0x06 || p->class_code == 0x08 || p->class_code == 0x11 ||
           p->class_code == 0x13 || p->class_code == 0x10 || p->class_code == 0x07 || (p->class_code == 0x0C && p->subclass != 0x03);
}
static const char *st_name(int s) { return s == DEV_OK ? "ok" : s == DEV_FAILED ? "failed" : s == DEV_OFF ? "off" : "ok"; }

static void devices_collect(void);
/* the list, grouped by kind in a fixed order */
int devices_report(char *out, int cap) {
    static char tmp[8192];
    o = tmp; on = 0; ocap = sizeof tmp; tmp[0] = 0;
    devices_collect();
    static const char *order[] = { "Processor", "Display", "Sound", "Network", "Storage", "Drives", "USB", "Input", "Bluetooth", "System", 0 };
    int n = 0; if (cap > 0) out[0] = 0;
    for (int pass = 0; pass <= 10; pass++) {
        for (char *l = tmp; *l; ) {
            char *e = l; while (*e && *e != '\n') e++;
            int kl = 0; while (l + kl < e && l[kl] != '\t') kl++;
            bool known = false, match = false;
            for (int k = 0; order[k]; k++) if ((int)strlen(order[k]) == kl && !memcmp(order[k], l, kl)) { known = true; match = k == pass; }
            if (pass == 10) match = !known;
            int len = (int)(e - l) + (*e ? 1 : 0);
            if (match && n + len < cap) { memcpy(out + n, l, len); n += len; out[n] = 0; }
            l = *e ? e + 1 : e;
        }
    }
    return n;
}
static void devices_collect(void) {
    char name[128], where[48], ids[24], det[160];
    /* the processor */
    {
        uint32_t r[12] = { 0 }, a, b, c, d; char brand[49] = { 0 };
        cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x80000004) { for (int i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]); memcpy(brand, r, 48); }
        const char *bp = brand; while (*bp == ' ') bp++;
        ksnprintf(det, sizeof det, "%d core%s found, %d in use (Settings > About can turn on the rest)", ncpus_found, ncpus_found == 1 ? "" : "s", ncpus_on);
        if (ncpus_on == ncpus_found) ksnprintf(det, sizeof det, "%d core%s, all in use", ncpus_on, ncpus_on == 1 ? "" : "s");
        dev_line("Processor", bp[0] ? bp : "Processor", "built in", "ok", "", "", det);
        ksnprintf(det, sizeof det, "%lu MB", mem_total() >> 20);
        dev_line("Processor", "Memory", "built in", "ok", "", "", det);
    }
    /* PCI devices */
    for (int i = 0; i < pci_count; i++) {
        struct pci_dev *p = &pci_devs[i];
        struct claim *c = 0;
        for (int k = 0; k < nclaims; k++) if (claims[k].bus == p->bus && claims[k].dev == p->dev && claims[k].fn == p->fn) c = &claims[k];
        const char *vn = vendor_name(p->vendor), *cn = pci_class_name(p->class_code, p->subclass, p->prog_if);
        if (c && c->name[0]) strncpy0(name, c->name, sizeof name);
        else if (vn) ksnprintf(name, sizeof name, "%s %s", vn, cn);
        else strncpy0(name, cn, sizeof name);
        ksnprintf(where, sizeof where, "PCI %02x:%02x.%x", p->bus, p->dev, p->fn);
        ksnprintf(ids, sizeof ids, "%04x:%04x", p->vendor, p->device);
        if (c) dev_line(pci_kind(p), name, c->driver, st_name(c->status), where, ids, c->note);
        else if (needs_no_driver(p)) dev_line(pci_kind(p), name, "", "system", where, ids, "Part of the computer's chipset; it works without a driver of its own.");
        else {
            const char *why = "AXIOM doesn't have a driver for this device yet.";
            if (p->class_code == 0x02 && p->subclass == 0x80) why = "Wi-Fi: AXIOM doesn't have a Wi-Fi driver yet. Use the wired network, a USB network adapter or phone tethering.";
            else if (p->class_code == 0x0C && p->subclass == 0x03 && p->prog_if != 0x30) why = "An older USB controller (USB 1/2). AXIOM uses the USB 3 (xHCI) controller, which serves the same ports.";
            else if (p->class_code == 0x03) why = "AXIOM uses the screen as the firmware set it up (no graphics driver for this card yet).";
            dev_line(pci_kind(p), name, "", "nodriver", where, ids, why);
        }
    }
    /* USB devices, drives, input, the screen */
    usb_devices();
    disk_devices();
    if (ps2_present) dev_line("Input", "PS/2 keyboard", "kbd", "ok", "PS/2", "", "The built-in keyboard port");
    if (mouse_present) dev_line("Input", "PS/2 mouse", "kbd", "ok", "PS/2", "", "The built-in mouse port");
    {
        struct bt_paired bp[8]; int n = bt_paired(bp, 8);
        for (int i = 0; i < n; i++) {
            ksnprintf(where, sizeof where, "%02X:%02X:%02X:%02X:%02X:%02X", bp[i].addr[5], bp[i].addr[4], bp[i].addr[3], bp[i].addr[2], bp[i].addr[1], bp[i].addr[0]);
            dev_line("Bluetooth", bp[i].name[0] ? bp[i].name : "Bluetooth device", "bt", "ok", where, "", "Paired (keyboards and mice work; sound devices don't yet)");
        }
    }
    ksnprintf(det, sizeof det, "%ux%u, %s", bootinfo.fb_w, bootinfo.fb_h, intelgfx_monitor()[0] ? intelgfx_monitor() : "the monitor didn't say what it is");
    dev_line("Display", "Screen", "framebuffer", "ok", "", "", det);
}
