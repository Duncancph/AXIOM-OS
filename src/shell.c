#include "kernel.h"

#define HIST 16
static char history[HIST][128]; static int hist_n;

static void read_line(char *buf, int max) {
    int len = 0, pos = 0, h = hist_n;
    buf[0] = 0;
    console_cursor(true);
    for (;;) {
        int c = input_getc();
        if (c == '\n') break;
        if (c == '\b') {
            if (pos == 0) continue;
            memmove(buf + pos - 1, buf + pos, len - pos);
            pos--; len--;
            console_putc('\b');
            for (int i = pos; i < len; i++) console_putc(buf[i]);
            console_putc(' ');
            for (int i = pos; i <= len; i++) console_putc('\b');
        } else if (c == KEY_LEFT) { if (pos > 0) { pos--; console_putc('\b'); } }
        else if (c == KEY_RIGHT) { if (pos < len) console_putc(buf[pos++]); }
        else if (c == KEY_UP || c == KEY_DOWN) {
            int nh = c == KEY_UP ? h - 1 : h + 1;
            if (nh < 0 || nh > hist_n || nh < hist_n - HIST) continue;
            h = nh;
            while (pos < len) console_putc(buf[pos++]);
            while (len) { console_write("\b \b"); len--; }
            const char *src = h == hist_n ? "" : history[h % HIST];
            strncpy0(buf, src, max);
            len = pos = strlen(buf);
            console_write(buf);
        } else if (c >= 32 && c < 127 && len < max - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = c; len++;
            for (int i = pos; i < len; i++) console_putc(buf[i]);
            pos++;
            for (int i = pos; i < len; i++) console_putc('\b');
        }
    }
    buf[len] = 0;
    console_cursor(false);
    console_putc('\n');
    if (len && (hist_n == 0 || strcmp(history[(hist_n - 1) % HIST], buf))) strncpy0(history[hist_n++ % HIST], buf, 128);
}

static void cmd_help(void) {
    static const char *cmds[][2] = {
        {"help", "show this list"},
        {"sysinfo", "CPU, memory, display and boot information"},
        {"pci", "list hardware on the PCI bus"},
        {"usb", "list USB devices"},
        {"bt", "Bluetooth adapter information"},
        {"bt scan [secs]", "find nearby Bluetooth devices (default 10 s)"},
        {"bt list", "show devices from the last scan"},
        {"echo <text>", "print text"},
        {"clear", "clear the screen"},
        {"uptime", "time since boot"},
        {"log", "kernel messages"},
        {"reboot", "restart the computer"},
        {"poweroff", "turn the computer off"},
    };
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        console_set_color(C_LCYAN); kprintf("  %-16s", cmds[i][0]);
        console_set_color(C_LGRAY); kprintf("%s\n", cmds[i][1]);
    }
}

static void cmd_sysinfo(void) {
    uint32_t r[12], a, b, c, d;
    char brand[49] = {0}, vendor[13] = {0};
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (int i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
        memcpy(brand, r, 48);
    }
    char *bp = brand; while (*bp == ' ') bp++;
    char disp[96]; console_info(disp, sizeof disp);
    kprintf("  OS        %s %s (%s kernel)\n", OS_NAME, OS_VERSION, KERNEL_NAME);
    kprintf("  CPU       %s (%s), %lu MHz\n", bp[0] ? bp : "unknown", vendor, tsc_hz() / 1000000);
    kprintf("  Memory    %lu MiB usable, %lu KiB used by kernel allocations\n", mem_total() >> 20, mem_used() >> 10);
    kprintf("  Display   %s\n", disp);
    kprintf("  Firmware  %s, loader: %s\n", bootinfo.uefi ? "UEFI" : "BIOS", bootinfo.bootloader);
    kprintf("  Keyboard  %s\n", ps2_present ? "PS/2 + USB" : "USB");
}

static void cmd_pci(void) {
    for (int i = 0; i < pci_count; i++) {
        struct pci_dev *p = &pci_devs[i];
        kprintf("  %02x:%02x.%d  %04x:%04x  %s\n", p->bus, p->dev, p->fn, p->vendor, p->device,
                pci_class_name(p->class_code, p->subclass, p->prog_if));
    }
}

static void cmd_uptime(void) {
    uint64_t s = uptime_ms() / 1000;
    kprintf("up %lu h %lu min %lu s\n", s / 3600, (s / 60) % 60, s % 60);
}

void shell_execute(char *line) {
    char *argv[8]; int argc = 0;
    for (char *p = line; *p && argc < 8;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (!argc) return;
    const char *c = argv[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) cmd_help();
    else if (!strcmp(c, "clear") || !strcmp(c, "cls")) console_clear();
    else if (!strcmp(c, "sysinfo") || !strcmp(c, "about")) cmd_sysinfo();
    else if (!strcmp(c, "pci") || !strcmp(c, "lspci")) cmd_pci();
    else if (!strcmp(c, "usb") || !strcmp(c, "lsusb")) usb_list();
    else if (!strcmp(c, "bt") || !strcmp(c, "bluetooth")) {
        if (argc == 1 || !strcmp(argv[1], "info")) bt_info();
        else if (!strcmp(argv[1], "scan")) bt_scan(argc > 2 ? (int)katoi(argv[2]) : 10);
        else if (!strcmp(argv[1], "list")) bt_list();
        else kprintf("usage: bt [info|scan [seconds]|list]\n");
    }
    else if (!strcmp(c, "echo")) { for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : ""); kprintf("\n"); }
    else if (!strcmp(c, "uptime")) cmd_uptime();
    else if (!strcmp(c, "log") || !strcmp(c, "dmesg")) {
        char *b = page_alloc(8);
        if (b) { console_log_read(b, 8 * 4096); console_write(b); for (int i = 0; i < 8; i++) page_free(b + i * 4096); }
    }
    else if (!strcmp(c, "reboot") || !strcmp(c, "restart")) { kprintf("Restarting...\n"); acpi_reboot(); }
    else if (!strcmp(c, "poweroff") || !strcmp(c, "shutdown")) { kprintf("Powering off...\n"); acpi_poweroff(); }
    else kprintf("Unknown command '%s'. Type 'help'.\n", c);
}

void shell_run(void) {
    kprintf("\nType ");
    console_set_color(C_LCYAN); kprintf("help"); console_set_color(C_LGRAY);
    kprintf(" to see what DuncOS can do.\n\n");
    char line[128];
    for (;;) {
        console_set_color(C_LGREEN); kprintf("dunc");
        console_set_color(C_WHITE); kprintf("> ");
        console_set_color(C_LGRAY);
        read_line(line, sizeof line);
        shell_execute(line);
    }
}
