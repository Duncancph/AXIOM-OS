/* Minimal ACPI: enough to power off (via the \_S5 sleep package), reset, and see the power button pressed. */
#include "kernel.h"

struct sdt { char sig[4]; uint32_t len; uint8_t rev, sum; char oem[6], tbl[8]; uint32_t oemrev, cid, crev; } __attribute__((packed));
static uint32_t pm1a_cnt, pm1b_cnt, pm1a_evt, pm1b_evt, smi_cmd;
static uint8_t acpi_enable_val, reset_reg_space, reset_val;
static uint64_t reset_reg_addr;
static uint16_t slp_typa, slp_typb;
static bool have_s5;

void *acpi_table(const char *sig);
static struct sdt *find_table_n(const char *sig, int which) {
    uint8_t *r = bootinfo.rsdp_copy;
    if (!bootinfo.rsdp) return 0;
    bool xsdt = r[15] >= 2 && *(uint64_t *)(r + 24);
    struct sdt *root = (struct sdt *)(uintptr_t)(xsdt ? *(uint64_t *)(r + 24) : *(uint32_t *)(r + 16));
    int n = (root->len - sizeof(struct sdt)) / (xsdt ? 8 : 4);
    for (int i = 0; i < n; i++) {
        uint8_t *p = (uint8_t *)root + sizeof(struct sdt);
        struct sdt *t = (struct sdt *)(uintptr_t)(xsdt ? ((uint64_t *)p)[i] : ((uint32_t *)p)[i]);
        if ((uint64_t)t < 0x100000000ULL && !memcmp(t->sig, sig, 4) && which-- == 0) return t;
    }
    return 0;
}
static struct sdt *find_table(const char *sig) { return find_table_n(sig, 0); }

static int pkg_int(uint8_t **pp) {
    uint8_t *p = *pp; int v;
    if (*p == 0x0A) { v = p[1]; p += 2; }        /* BytePrefix */
    else if (*p == 0x0B) { v = p[1] | p[2] << 8; p += 3; }
    else if (*p == 0x00) { v = 0; p++; }          /* Zero */
    else if (*p == 0x01) { v = 1; p++; }          /* One */
    else { v = *p; p++; }
    *pp = p; return v;
}

static void scan_s5(struct sdt *d);
static void acpi_mode(void);
void acpi_init(void) {
    struct sdt *fadt = find_table("FACP");
    if (!fadt) return;
    uint8_t *f = (uint8_t *)fadt;
    uint64_t dsdt = *(uint32_t *)(f + 40);
    if (fadt->len >= 148 && *(uint64_t *)(f + 140)) dsdt = *(uint64_t *)(f + 140);
    smi_cmd = *(uint32_t *)(f + 48);
    acpi_enable_val = f[52];
    pm1a_cnt = *(uint32_t *)(f + 64);
    pm1b_cnt = *(uint32_t *)(f + 68);
    if (fadt->len >= 129 && (*(uint32_t *)(f + 112) & (1 << 10))) {   /* RESET_REG_SUP */
        reset_reg_space = f[116];
        reset_reg_addr = *(uint64_t *)(f + 120);
        reset_val = f[128];
    }
    pm1a_evt = *(uint32_t *)(f + 56);
    pm1b_evt = *(uint32_t *)(f + 60);
    uint8_t evt_len = fadt->len > 88 ? f[88] : 4;
    acpi_mode();
    acpi_power_button();
    /* PWRBTN_EN in the enable half of each PM1 event block: some chipsets only note the press when it's set. Its
     * interrupt (the SCI) isn't routed anywhere, so nothing else changes; the desktop looks at the status bit. */
    if (pm1a_evt) outw(pm1a_evt + evt_len / 2, inw(pm1a_evt + evt_len / 2) | (1 << 8));
    if (pm1b_evt) outw(pm1b_evt + evt_len / 2, inw(pm1b_evt + evt_len / 2) | (1 << 8));                                  /* forget a press from before we started */
    if (dsdt && dsdt < 0x100000000ULL) scan_s5((struct sdt *)(uintptr_t)dsdt);
    for (int i = 0; !have_s5 && i < 16; i++) { struct sdt *t = find_table_n("SSDT", i); if (!t) break; scan_s5(t); }   /* some firmware keeps it there */
}

static void scan_s5(struct sdt *d) {
    uint8_t *p = (uint8_t *)d + sizeof(struct sdt), *end = (uint8_t *)d + d->len;
    for (; p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4)) continue;
        if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;   /* NameOp */
        p += 4;
        if (*p != 0x12) break;   /* PackageOp */
        p++;
        p += ((*p & 0xC0) >> 6) + 1;  /* PkgLength */
        p++;                          /* NumElements */
        slp_typa = pkg_int(&p);
        slp_typb = pkg_int(&p);
        have_s5 = true;
        break;
    }
}

/* ACPI mode: the firmware hands the power button (and sleep) to the OS */
static void acpi_mode(void) {
    if (!pm1a_cnt || (inw(pm1a_cnt) & 1)) return;
    if (smi_cmd && acpi_enable_val) {
        outb(smi_cmd, acpi_enable_val);
        for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++) sleep_ms(10);
    }
}

/* was the power button pressed since we last looked? (PWRBTN_STS in the PM1 status register; writing 1 clears it) */
bool acpi_power_button(void) {
    bool hit = false;
    if (pm1a_evt && (inw(pm1a_evt) & (1 << 8))) { outw(pm1a_evt, 1 << 8); hit = true; }
    if (pm1b_evt && (inw(pm1b_evt) & (1 << 8))) { outw(pm1b_evt, 1 << 8); hit = true; }
    return hit;
}

void disk_flush_all(void);

void acpi_poweroff(void) {
    disk_flush_all();
    if (!have_s5 || !pm1a_cnt) { kprintf("ACPI power-off is not available on this machine.\n"); return; }
    acpi_mode();
    outw(pm1a_cnt, (slp_typa << 10) | (1 << 13));
    if (pm1b_cnt) outw(pm1b_cnt, (slp_typb << 10) | (1 << 13));
    sleep_ms(1000);
    kprintf("Power-off failed.\n");
}

void acpi_reboot(void) {
    disk_flush_all();
    if (reset_reg_addr && reset_reg_space == 1) outb((uint16_t)reset_reg_addr, reset_val);
    sleep_ms(50);
    outb(0xCF9, 0x02); io_wait(); outb(0xCF9, 0x06);      /* chipset reset */
    sleep_ms(50);
    if (ps2_present) outb(0x64, 0xFE);                    /* keyboard controller reset */
    sleep_ms(50);
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) z = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(z));           /* triple fault */
}

void *acpi_table(const char *sig) { return find_table(sig); }
