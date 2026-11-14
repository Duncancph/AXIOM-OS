# DuncOS

A 64-bit operating system built from scratch, running on its own kernel, **Thane**.
Created by Bryan Duncan.

It boots on real PCs (modern UEFI and older BIOS) from a USB stick into a modern desktop,
with true multitasking and memory protection between apps.

## Features (v0.1)

**Thane kernel**
- 64-bit x86 kernel in C and assembly, booted by GRUB (Multiboot2)
- **Preemptive multitasking**: the local APIC timer switches between programs 250 times a second,
  so a frozen or busy app can't lock up the computer
- **Memory protection**: every app runs in user mode (ring 3) in its own address space.
  The kernel is mapped supervisor-only, and apps can't see each other's memory or touch hardware ports.
  An app that misbehaves is stopped, the rest of the system carries on, and a notification explains what happened
- System call interface (`int 0x80`); every pointer from an app is checked against its own page tables
- ELF program loader, per-process page tables, CPU-time accounting
- Drivers: framebuffer, PS/2 keyboard + mouse/touchpad, USB 3 (xHCI) with hubs and hot-plug,
  USB keyboards, mice and flash drives, USB Bluetooth (HCI), PCI, ACPI power-off/reboot, real-time clock
- File system: an in-memory system disk (`/home`, `/apps`, `/system`) plus FAT32 USB drives at `/usb`
  (read/write, long file names, MBR/GPT/unpartitioned), mounted and removed as you plug them in
- Apps can be installed from files: the `clan` package manager installs them into `/apps`
- **Networking**: wired Ethernet with its own TCP/IP stack: Ethernet, ARP, IPv4, ICMP (ping), UDP,
  DHCP (automatic address), DNS, and TCP (retransmission, flow control, orderly close).
  Network cards: Intel e1000 / e1000e / I217-I219 (tested: 82540EM, 82574L) and Realtek RTL8111/8168
  (written from the datasheet, not yet tested on hardware). Cable unplug/replug is detected.
- **HTTPS / TLS 1.3**, written from scratch (no OpenSSL): AES-128-GCM and ChaCha20-Poly1305,
  X25519 and P-256 key exchange, RSA (PKCS#1 v1.5 and PSS) and ECDSA (P-256, P-384) signatures,
  SHA-256/384/512. Every site's certificate chain is checked against the Mozilla root list
  (`/system/certs/ca-bundle.crt`): signatures, host name (including wildcards and IP addresses),
  expiry dates, CA rules, and that the certificate is meant for web servers.
  If a check fails, the connection is refused and you're told why. `trust <file.pem>` in castle adds
  your own authority (for example a home server's), saved in `/system/certs/user`
- Kernel random number generator for encryption keys: the CPU's RDSEED/RDRAND where available,
  mixed with timing jitter and keyboard, mouse and network events, feeding a ChaCha20 generator

**Desktop: the "Highland" theme** (design by Bryan Duncan: `src/desktop_highland.c`, `src/gfx.h`,
`common/theme_highland.h`, used unchanged, wired in by `src/highland.c`)
- Mountain-ridge wallpaper with moon, translucent top bar (crown menu, active app, Bluetooth, clock)
- Floating dock with pixel-art app icons, running dots and tooltips
- Compositing window manager: rounded windows with shadows and red/gold/green window buttons
  (close, minimise, maximise), drag to move, resize from the right/bottom edge or corner,
  double-click the title bar to maximise
- Crown menu (click DuncOS in the top bar, or press the Windows/Super key) with apps, Restart and Shut down
- Notifications, a "not responding" detector with force-close
- Change the colours and sizes of the whole desktop and the apps in `common/theme_highland.h`
- GNU Unifont for the desktop chrome and terminal; Inter for app content
- Graphical boot splash with progress bar (`src/fb_boot_logo.c`)

**Apps** (each a separate, protected program)
| App | What it does |
|---|---|
| Files | Your Highland mockup made real: places sidebar, USB drives, folder grid; open, new folder, rename, copy/paste (also to and from USB drives), delete to Trash, empty Trash |
| Terminal (castle shell) | `ls`, `cd`, `cat`, `mkdir`, `rm`, `mv`, `cp`, `touch`, `echo ... > file`, `edit`, `df`, `ip`, `ping`, `host`, `wget`, `curl` (http and https), `dns`, `trust`, `fetch`, `apps`, `open <app> [file]`, `ps`, `kill`, `date`, `clan`, plus `sysinfo`, `pci`, `usb`, `bt`, `bt scan`, `log`, `reboot`, `poweroff` |
| System Monitor | Live CPU and memory graphs, every process with CPU % and memory, End task |
| Bluetooth | Adapter details, scan for nearby Classic + Low Energy devices with names and signal strength |
| Network | Connection status, address, gateway, DNS, live traffic graph, Test connection, Renew address, choose a DNS server |
| Calculator | Mouse or keyboard, 6 decimal places |
| Protection Test | Deliberately crashes or freezes itself to show the kernel containing it |
| About DuncOS | Welcome screen with system details |

**clan packages** (`clan list`, `clan install <name>`, `clan remove <name>`)
| Package | What it does |
|---|---|
| editor 0.3 | Text editor: line numbers, save / save as, unsaved-changes prompt (`edit <file>` in castle, or double-click a file in Files) |
| clock 1.0 | Analog and digital clock |
| web 0.2 | Web browser for `https://` and `http://` sites: headings, text, bold, lists, links, back, address bar. A padlock and the status bar show the encryption and who the certificate belongs to; sites that fail the security checks are blocked with an explanation. The start page links to text-only sites that suit it, such as text.npr.org and lite.duckduckgo.com |

## Put it on a PC

1. Write `duncos.iso` to a USB stick with [balenaEtcher](https://etcher.balena.io/) or Rufus ("DD image" mode).
2. In the PC's firmware settings, **disable Secure Boot** (DuncOS isn't signed).
3. Boot from the stick using the boot menu key (usually F12, F11, F8, Esc or F10).
   If the screen looks wrong, pick **DuncOS (safe graphics 1024x768)** in the boot menu.
4. Nothing on the computer's disk is touched. Remove the stick and restart to get your normal OS back.

## Known limitations

- **Files in `~` (the system disk) live in memory** and are reset when the computer restarts.
  Copy anything you want to keep to a USB drive. (A persistent system disk needs SATA/NVMe drivers.)
- **USB drives must be FAT32.** exFAT (common on drives of 64 GB or more), NTFS and FAT16 are recognised
  but not readable yet. Changes are written immediately, so a drive is safe to unplug once a copy finishes.
- **Intel Bluetooth** (most laptops since ~2019) and MediaTek need firmware uploaded at start-up. Not
  implemented yet; Realtek, Broadcom and CSR USB dongles generally work without it. Bluetooth can scan
  but not pair or connect yet.
- **Touchpads** work only if they appear as a PS/2 device; I2C touchpads (common on newer laptops) don't yet.
- **HTTPS is TLS 1.3 only.** Most sites support it, but some older servers offer only TLS 1.2 and can't be
  reached (you get a clear message). Not supported yet: P-521 and Ed25519 certificates (rare), checking
  whether a certificate has been revoked (OCSP/CRL), and certificates signed with SHA-1 (deliberately).
- The encryption code is careful but new and has not had an independent security review. AES uses lookup
  tables, which can in theory leak timing information, so ChaCha20-Poly1305 is preferred when the server
  agrees. Don't rely on DuncOS for anything sensitive yet.
- The clock comes from the PC's real-time clock, which is assumed to be UTC. If it's very wrong,
  certificates can look expired or not yet valid.
- **No Wi-Fi.** Wired Ethernet only. Wi-Fi chips need vendor firmware plus an 802.11/WPA2 stack.
- Networking is IPv4 only (no IPv6), without IP fragments; the web browser shows text and links only.
- No sound yet. One CPU core is used. Kernel threads are cooperative; apps are preemptive.
- Needs a 32-bit graphics mode (any UEFI PC; most BIOS PCs). Otherwise it falls back to the text shell.
- Tested in QEMU (UEFI and BIOS, USB 2/3 devices, a USB hub, USB drive hot-plug, Intel e1000/e1000e
  networking against local web and DNS servers, HTTPS against OpenSSL test servers). The sandbox it was
  built in has no internet access, so HTTPS has not yet been tried against real public websites.
  Not yet tested on physical hardware.

## Build

Needs `gcc`, `nasm`, `ld`, `grub-mkrescue` (grub-pc-bin + grub-efi-amd64-bin), `xorriso`, `mtools`.

```
make            # builds duncos.iso
make run        # QEMU, BIOS
make run-uefi   # QEMU, UEFI (needs the ovmf package)
```

Fonts are generated by `tools/genunifont.py` (GNU Unifont, GPLv2+ with font embedding exception)
and `tools/genuifont.py` (Inter, SIL Open Font License).
`make test` runs the host-side tests: the Bluetooth driver against a simulated adapter, and the FAT32
driver against a disk image made by `mkfs.fat`, which is then checked with `fsck.fat` and read back
with `mtools` (needs dosfstools and mtools). It also checks the crypto against Python's `cryptography`
(OpenSSL) on random inputs, parses every Mozilla root certificate, and runs the TLS client against
`openssl s_server` with 21 good and deliberately broken setups (needs python3-cryptography and openssl).

The Mozilla root certificates are copied from the build machine's `/etc/ssl/certs/ca-certificates.crt`
(Mozilla Public License 2.0).

## Layout

```
src/             Thane kernel
  boot.asm         Multiboot2 entry, switch to 64-bit mode
  cpu.c            GDT/TSS (privilege levels), local APIC timer, write-combining video memory
  isr.asm idt.c    interrupt entry, context switch, exception handling (stops faulting apps)
  sched.c          tasks, preemptive scheduler, process spawn/exit/kill
  vm.c             per-process page tables, ELF loader
  vfs.c kmalloc.c  file system layer (memory disk + mounted drives), kernel heap
  apps.c           app registry and the clan package manager
  msc.c fat.c      USB flash drives (Bulk-Only/SCSI) and the FAT32 file system
  random.c         random number generator (RDSEED/RDRAND, jitter, events -> ChaCha20)
  net.c            TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP
  nic_e1000.c nic_rtl8168.c   network card drivers
  syscall.c        system calls
  desktop.c        window manager + compositor, crown menu, notifications, input
  desktop_highland.c gfx.h   Highland design: wallpaper, top bar, window frames, dock
  highland.c       connects the Highland design to the window manager; DuncOS app icons
  fb_boot_logo.c   boot splash
  xhci.c usbkbd.c  USB 3 host controller, hubs, hot-plug; USB keyboard/mouse
  bt.c             Bluetooth HCI
  kbd.c pci.c acpi.c rtc.c mem.c timer.c console.c shell.c ...
common/          shared by kernel and apps: system call ABI, Highland theme tokens, 2D graphics, fonts, strings
user/lib         libdunc: app runtime (system calls, windows, UI widgets, HTTP/HTTPS client)
  crypto/          SHA-2, HMAC/HKDF, AES-GCM, ChaCha20-Poly1305, X25519, big numbers, RSA, ECDSA/ECDH,
                   X.509 certificates, TLS 1.3 client
tests/           host-side tests (make test); tests/tls has the TLS test matrix
user/apps        the apps (editor.c, clock.c and web.c are clan packages, listed in repo/index.txt)
rootfs/          files copied onto the system disk at start-up
```

## Roadmap

1. Persistent system disk (SATA/AHCI and NVMe drivers), exFAT for big USB drives
2. Intel/MediaTek Bluetooth firmware loading; Bluetooth pairing and connections
3. Wi-Fi; I2C-HID touchpads (needs an ACPI interpreter)
4. TLS 1.2 fallback, revocation checks; images and CSS in Web
5. Multi-core scheduling, sound, IPv6
