# EmberOS

A small 64-bit operating system written from scratch in C and assembly.
It boots on real PCs (both modern UEFI and older BIOS machines) from a USB stick,
and includes its own USB 3 driver and a first-stage Bluetooth stack.

## What works (v0.1)

| Area | Status |
|---|---|
| Boot | GRUB → Multiboot2 → 64-bit long mode. UEFI and legacy BIOS, from USB stick or CD |
| Display | Firmware framebuffer text console (any resolution; 2x font on 4K), VGA text fallback |
| Keyboard | PS/2 (laptop built-in keyboards) and USB keyboards |
| USB | xHCI (USB 3.x) host controller driver: USB 1.1 / 2.0 / 3.x devices on root ports |
| Bluetooth | USB Bluetooth adapters: adapter info, scan for nearby Classic + Low Energy devices with names and signal strength |
| Hardware | PCI device listing, CPU/memory info |
| Power | ACPI power-off and reboot |

Shell commands: `help`, `sysinfo`, `pci`, `usb`, `bt`, `bt scan [seconds]`, `bt list`, `echo`, `clear`, `uptime`, `reboot`, `poweroff`.

## Put it on a PC

1. Write `emberos.iso` to a USB stick with [balenaEtcher](https://etcher.balena.io/) or Rufus (choose "DD image" mode),
   or on Linux/macOS: `sudo dd if=emberos.iso of=/dev/sdX bs=4M` (double-check the device!).
2. In the PC's firmware settings, **disable Secure Boot** (EmberOS isn't signed).
3. Plug in the stick, power on and open the boot menu (usually F12, F11, F8, Esc or F10), pick the USB stick.
4. Nothing on the computer's disk is touched. Remove the stick and reboot to get back to your normal OS.

## Known limitations

- **Intel Bluetooth** (most laptops since ~2019: AX200/AX201/AX210/AX211) needs a firmware file uploaded
  at start-up before it works. Not implemented yet; Realtek, Broadcom, Qualcomm/CSR USB dongles generally
  answer without it. MediaTek also needs firmware.
- **USB hubs** aren't supported yet, so devices behind a hub (including some desktop front ports and
  hub-based docks) won't show up.
- No pairing/connecting to Bluetooth devices yet, no disk/file system, no networking, no mouse.
- The OS polls hardware rather than using interrupts (simpler and robust, but uses a CPU core at 100%).
- Tested in QEMU (BIOS and UEFI, USB 2/3 devices). Not yet tested on physical hardware.

## Build

Needs `gcc`, `nasm`, `ld`, `grub-mkrescue` (grub-pc-bin + grub-efi-amd64-bin), `xorriso`, `mtools`.

```
make            # builds emberos.iso
make run        # boot in QEMU (BIOS)
make run-uefi   # boot in QEMU (UEFI, needs ovmf)
```

`tests/test_bt.c` runs the Bluetooth driver on the host against a simulated adapter:
`gcc -std=gnu17 -w -o test_bt tests/test_bt.c src/bt.c && ./test_bt`

## Layout

```
src/boot.asm     Multiboot2 header, 32→64-bit switch, initial page tables
src/main.c       boot info parsing, start-up sequence
src/console.c    framebuffer text console     src/font.h  8x16 font (from DejaVu Sans Mono)
src/kbd.c        PS/2 keyboard + input queue  src/usbkbd.c USB HID keyboard
src/xhci.c       USB 3 host controller + device enumeration
src/bt.c         Bluetooth HCI over USB: info, inquiry, LE scanning, remote names
src/pci.c acpi.c mem.c timer.c idt.c serial.c lib.c shell.c
```

## Roadmap

1. USB hub support (reach every port on desktops/docks)
2. Intel/MediaTek Bluetooth firmware loading (read firmware from the boot USB stick)
3. Bluetooth connections: L2CAP, pairing (SMP / Secure Simple Pairing)
4. Bluetooth keyboards and mice (HID over GATT / Classic HID)
5. Interrupt-driven I/O, mouse, a simple windowed desktop
6. FAT32 file system on the USB stick, NVMe/SATA disks
