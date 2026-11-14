#include "kernel.h"
#include "vfs.h"

struct bootinfo bootinfo;

static void parse_multiboot(uint64_t addr) {
    uint8_t *p = (uint8_t *)addr + 8, *end = (uint8_t *)addr + *(uint32_t *)addr;
    while (p < end) {
        uint32_t type = *(uint32_t *)p, size = *(uint32_t *)(p + 4);
        if (type == 0) break;
        switch (type) {
        case 1: strncpy0(bootinfo.cmdline, (char *)p + 8, sizeof bootinfo.cmdline); break;
        case 2: strncpy0(bootinfo.bootloader, (char *)p + 8, sizeof bootinfo.bootloader); break;
        case 6: {
            uint32_t esz = *(uint32_t *)(p + 8);
            for (uint8_t *e = p + 16; e + esz <= p + size && bootinfo.mmap_count < 64; e += esz) {
                struct mmap_ent *m = &bootinfo.mmap[bootinfo.mmap_count++];
                m->base = *(uint64_t *)e; m->len = *(uint64_t *)(e + 8); m->type = *(uint32_t *)(e + 16);
            }
            break;
        }
        case 8:
            bootinfo.fb_addr = *(uint64_t *)(p + 8);
            bootinfo.fb_pitch = *(uint32_t *)(p + 16);
            bootinfo.fb_w = *(uint32_t *)(p + 20);
            bootinfo.fb_h = *(uint32_t *)(p + 24);
            bootinfo.fb_bpp = p[28]; bootinfo.fb_type = p[29];
            bootinfo.r_pos = p[32]; bootinfo.r_size = p[33];
            bootinfo.g_pos = p[34]; bootinfo.g_size = p[35];
            bootinfo.b_pos = p[36]; bootinfo.b_size = p[37];
            break;
        case 3:
            if (!bootinfo.mod_start) { bootinfo.mod_start = *(uint32_t *)(p + 8); bootinfo.mod_end = *(uint32_t *)(p + 12); }
            break;
        case 11: case 12: bootinfo.uefi = true; break;
        case 14: case 15: {
            uint32_t n = size - 8 < sizeof bootinfo.rsdp_copy ? size - 8 : sizeof bootinfo.rsdp_copy;
            if (type == 15 || !bootinfo.rsdp) { memcpy(bootinfo.rsdp_copy, p + 8, n); bootinfo.rsdp = 1; }
            break;
        }
        }
        p += (size + 7) & ~7u;
    }
}

/* Watches for USB devices being plugged in or removed. */
static void usb_thread(void *arg) {
    for (;;) { xhci_service(); task_sleep(100); }
}

void kmain(uint32_t magic, uint64_t mbi) {
    serial_init();
    if (magic != 0x36D76289) {
        kprintf("DuncOS must be started by a Multiboot2 loader (GRUB).\n");
        return;
    }
    parse_multiboot(mbi);
    mem_init();
    vm_init();
    gdt_init();
    idt_init();
    console_init();
    bool gui = desktop_available();
    if (gui) console_gui_mode();
    boot_progress(5);

    console_set_color(C_YELLOW);
    kprintf("%s %s", OS_NAME, OS_VERSION);
    console_set_color(C_LGRAY);
    kprintf("  -  %s kernel, booted %s via %s\n\n", KERNEL_NAME, bootinfo.uefi ? "UEFI" : "BIOS",
            bootinfo.bootloader[0] ? bootinfo.bootloader : "multiboot2");

    timer_init();
    LOG_OK("CPU clock %lu MHz\n", tsc_hz() / 1000000);
    LOG_OK("Memory %lu MiB usable\n", mem_total() >> 20);
    char buf[96]; console_info(buf, sizeof buf);
    LOG_OK("Display %s\n", buf);
    boot_progress(15);
    ps2_init();
    if (ps2_present) LOG_OK("PS/2 controller%s\n", mouse_present ? " (keyboard + mouse)" : "");
    acpi_init();
    boot_progress(25);
    vfs_init();
    if (bootinfo.mod_start) initrd_load((const uint8_t *)bootinfo.mod_start, (const uint8_t *)bootinfo.mod_end);
    apps_refresh();
    pci_scan();
    LOG_OK("PCI: %d devices\n", pci_count);
    boot_progress(35);
    xhci_init_all();
    if (!bt_present()) LOG_INFO("No USB Bluetooth adapter detected\n");
    boot_progress(80);

    if (!gui) {
        LOG_WARN("No 32-bit graphics mode: starting the text shell instead of the desktop\n");
        shell_run();
    }
    desktop_init();
    boot_progress(92);
    sched_init();
    task_create_kernel("Desktop", desktop_thread, 0);
    task_create_kernel("USB", usb_thread, 0);
    task_spawn("about");
    boot_progress(100);
    sleep_ms(400);
    LOG_OK("Starting desktop\n");
    sched_start();
}
