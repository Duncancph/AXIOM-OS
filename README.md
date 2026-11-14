# AXIOM

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
  USB keyboards, mice and flash drives, USB network adapters, USB Bluetooth (HCI), **SATA (AHCI) and
  NVMe disks**, **Intel HD Audio sound**, PCI, ACPI power-off/reboot, real-time clock
- **Install to a disk**: Settings > Storage installs AXIOM on an internal SATA or NVMe disk, or on a USB
  stick (it erases that disk: a GPT disk with one FAT32 EFI system partition holding the boot loader,
  the system and your files). Started from there, your files (`/home`), installed apps (`/apps`) and
  settings (`/system/config`) are kept across restarts. "Update AXIOM" puts a newer version on the disk
  and keeps your files. Other partitions on internal disks (Windows, Linux) are listed but never touched
- File system: FAT32 on the AXIOM disk and on USB drives at `/usb` (read/write, long file names,
  MBR/GPT/unpartitioned), mounted and removed as you plug them in; started without a AXIOM disk,
  the system disk lives in memory
- Apps can be installed from files: the `clan` package manager installs them into `/apps`
- Apps get a real memory allocator (malloc/free) that grows as they need, up to 1 GiB each
- **Text in Western European languages**: the fonts and keyboards use Windows-1252 (accents, ß, £, €);
  UTF-8 files and web pages are converted. Keyboard layouts: English (US), English (UK), German, French
  and Spanish, with AltGr and accent (dead) keys. Shared clipboard (Ctrl+C / Ctrl+X / Ctrl+V)
- **Time zones** with daylight saving (EU, US/Canada, Australia, New Zealand), 12- or 24-hour clock,
  and an option for PCs whose clock keeps local time (as Windows sets it)
- **Lock screen** with a password (Windows key + L, at start-up and after an idle time you choose),
  and the screen turns off after a set idle time. Print Screen saves a screenshot in `~/Pictures`
- **Networking**: wired Ethernet with its own TCP/IP stack: Ethernet, ARP, IPv4, ICMP (ping), UDP,
  DHCP (automatic address), DNS, and TCP (retransmission, flow control, orderly close).
  Network cards: Intel e1000 / e1000e / I217-I219 (tested: 82540EM, 82574L) and Realtek RTL8111/8168
  (written from the datasheet, not yet tested on hardware). Cable unplug/replug is detected.
  **USB networking**: USB Ethernet adapters (CDC-ECM and CDC-NCM, which covers e.g. Realtek RTL8152/8153
  adapters in their standard mode) and **Android phones with USB tethering** (RNDIS), plugged in at
  any time. With several adapters, AXIOM uses the one that has a working connection
- **IPv6**, alongside IPv4 or on its own (IPv6-only networks work too): Neighbour Discovery, automatic
  addresses from the router's advertisements (SLAAC) with duplicate address detection, DNS servers from
  router advertisements (RDNSS) or stateless DHCPv6, link MTU, ICMPv6 ping, MLDv2 so switches deliver
  its traffic, and "packet too big" / "unreachable" handling for TCP. The global address uses a random
  interface ID that is new each start-up, so it doesn't reveal the network card's hardware address.
  Names that have both kinds of address are tried over IPv6 first; if that doesn't connect within a
  few seconds, AXIOM falls back to IPv4. `http://[2001:db8::1]:8080/` style addresses work in Web and castle
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
- Floating dock with smooth, modern app icons (gradient tiles with white symbols, drawn by
  `tools/genicons.py`), running dots and tooltips
- Compositing window manager: rounded windows with shadows and red/gold/green window buttons
  (close, minimise, maximise), drag to move, resize from the right/bottom edge or corner,
  double-click the title bar to maximise
- Crown menu (click AXIOM in the top bar, or press the Windows/Super key) with apps, Restart and Shut down
- Notifications, a "not responding" detector with force-close
- Change the colours and sizes of the whole desktop and the apps in `common/theme_highland.h`
- GNU Unifont for the desktop chrome and terminal; Inter for app content
- Graphical boot splash with progress bar (`src/fb_boot_logo.c`)

**Apps** (each a separate, protected program)
| App | What it does |
|---|---|
| Files | Your Highland mockup made real: places sidebar, USB drives, folder grid; open (pictures in Viewer, web pages in Web, text in Editor, installing the app if needed), new folder, rename, copy/paste (also to and from USB drives), delete to Trash, empty Trash |
| Terminal (castle shell) | `ls`, `cd`, `cat`, `mkdir`, `rm`, `mv`, `cp`, `touch`, `echo ... > file`, `edit`, `df`, `disks`, `install`, `settings`, `set`, `ip`, `ping` (`-4`/`-6`, `ping6`), `host`, `wget`, `curl` (http and https), `dns`, `trust`, `fetch`, `apps`, `open <app or file>`, `ps`, `kill`, `date`, `clan`, plus `sysinfo`, `pci`, `usb`, `bt`, `bt scan`, `log`, `reboot`, `poweroff`. Scroll back with the mouse wheel or PgUp/PgDn; select with the mouse to copy (Ctrl+C or Ctrl+Shift+C), paste with Ctrl+V or a right click |
| Settings | Keyboard layout, mouse (pointer speed, natural scrolling, left-handed), date & time (time zone, 24-hour clock), display size, sound volume, lock screen password, screen-off time, restart / shut down, updates over the internet (signed; see below), storage (install or update AXIOM on a disk), about |
| Web 0.4 | Web browser for `https://`, `http://` and `file://` pages: style sheets (CSS selectors, cascade, `@media`, the box model, colours, borders, lists, tables, simple flex rows), pictures (PNG, JPEG, GIF, BMP, loaded after the text), forms (GET and POST), cookies that survive restarts, gzip, UTF-8 pages, back and forward, a resizable window. A padlock and the status bar show the encryption and who the certificate belongs to; sites that fail the security checks are blocked with an explanation. **JavaScript** (AXIOM's own engine, `user/apps/web/js`: modern JavaScript with classes, async/await, Promises, generators, regular expressions, JSON, Map/Set and more) with the page objects scripts expect: `document`, elements, events, timers, `fetch`, `XMLHttpRequest`, `localStorage`, `URL`. Script errors are listed at `about:console`. Big web apps (Gmail, YouTube...) need more than Web has |
| Notes | Quick notes that save themselves as you type (in `/home/Notes`); the first line is the title |
| Calendar | Month view with today marked; add things to any day (saved in `/home/.calendar.txt`) |
| Viewer | Pictures (PNG, JPEG, GIF, BMP): fit to window, zoom, drag, previous/next in the folder |
| System Monitor | Live CPU and memory graphs, every process with CPU % and memory, End task |
| Bluetooth | Adapter details, scan for nearby Classic + Low Energy devices with names and signal strength, pair with Classic devices (number check), paired devices kept and forgettable |
| Network | Connection status (IPv4, IPv6 or both), addresses, gateway and IPv6 router, DNS, MTU, live traffic graph, Test connection, Renew address, choose a DNS server (IPv4 or IPv6) |
| Calculator | Mouse or keyboard, 6 decimal places |
| Protection Test | Deliberately crashes or freezes itself to show the kernel containing it |
| About AXIOM | Welcome screen with system details |

**clan packages** (`clan list`, `clan install <name>`, `clan remove <name>`)
| Package | What it does |
|---|---|
| editor 0.4 | Text editor: line numbers, selection (Shift+arrows, mouse), copy/cut/paste, select all, save / save as, UTF-8 files, unsaved-changes prompt (`edit <file>` in castle, or double-click a file in Files) |
| clock 1.0 | Analog and digital clock |

## Updates over the internet

Settings > Updates checks for a newer AXIOM and installs it on the disk AXIOM is installed on
(your files, apps and settings are kept; restart to use it). Every update is signed:

- `latest.txt` (the manifest) lists the version, a build number and the kernel, system files and
  boot loader with their sizes and SHA-256 hashes, and is signed with ECDSA P-256.
- The **kernel** checks the signature against the public key built into it (`src/update_key.h`),
  that the build is newer than the running one, and every downloaded file's hash, before anything
  is written. A changed or unsigned update is refused.
- Build numbers are the number of git commits (`build/build_num`).

To publish an update: commit, `make`, then
`python3 tools/make-release.py <signing key .pem> "what's new"`, and upload the `release/` folder
(`latest.txt` and `b<build>/`) to where `UPDATE_URL` in `src/kernel.h` points: the `updates` branch of
the AXIOM-OS repository on GitHub, which must be public (or the `update.url`
setting: `set update.url <url>` in castle). `tools/update-keygen.py` makes a new key pair; the
private key must stay private, because whoever has it can make updates that AXIOM installs.

## Put it on a PC

1. Write `duncos.iso` to a USB stick with [balenaEtcher](https://etcher.balena.io/) or Rufus ("DD image" mode).
2. In the PC's firmware settings, **disable Secure Boot** (AXIOM isn't signed).
3. Boot from the stick using the boot menu key (usually F12, F11, F8, Esc or F10).
   If the screen looks wrong or stretched, pick a size under **Screen size...** in the boot menu
   (or **AXIOM (safe graphics 1024x768)**).
4. Nothing on the computer's disk is touched. Remove the stick and restart to get your normal OS back.
5. To keep AXIOM and your files: Settings > Storage > **Install here...** on a disk or another USB stick.
   This erases that whole disk. The installed system starts on UEFI PCs (pick the disk in the boot menu,
   or make it the first boot option). Display size for installed systems: Settings > Display.

## Known limitations

- **Started from the ISO, files in `~` live in memory** and are reset when the computer restarts; install
  AXIOM on a disk or USB stick (Settings > Storage) to keep them.
- **Installing erases the whole target disk**: AXIOM can't yet share a disk with Windows or Linux (no
  partition resizing). Installed systems start on UEFI only (old BIOS-only PCs can keep using the ISO).
- **USB drives must be FAT32.** exFAT (common on drives of 64 GB or more), NTFS and FAT16 are recognised
  but not readable yet. Changes are written immediately, so a drive is safe to unplug once a copy finishes.
- **Intel Bluetooth** (most laptops since ~2019) and MediaTek need firmware uploaded at start-up. Not
  implemented yet; Realtek, Broadcom and CSR USB dongles generally work without it. Bluetooth can scan
  and pair with Classic devices. Classic keyboards and mice work once paired (boot protocol: standard
  keys and buttons, wheel); they reconnect by themselves when switched on. Headsets and Low Energy
  devices (most newer mice) don't work yet. Bluetooth is tested only against a simulated adapter and
  keyboard (`make test`), as there's no Bluetooth hardware in the test setup.
- **Touchpads** work only if they appear as a PS/2 device; I2C touchpads (common on newer laptops) don't yet.
- **HTTPS is TLS 1.3 only.** Most sites support it, but some older servers offer only TLS 1.2 and can't be
  reached (you get a clear message). Not supported yet: P-521 and Ed25519 certificates (rare), checking
  whether a certificate has been revoked (OCSP/CRL), and certificates signed with SHA-1 (deliberately).
- The encryption code is careful but new and has not had an independent security review. AES uses lookup
  tables, which can in theory leak timing information, so ChaCha20-Poly1305 is preferred when the server
  agrees. Don't rely on AXIOM for anything sensitive yet.
- The clock comes from the PC's real-time clock (UTC, or local time if set in Settings); there's no
  network time yet. If it's very wrong, certificates can look expired or not yet valid.
- **No Wi-Fi yet.** Every Wi-Fi chip needs its own driver plus vendor firmware, and an 802.11 + WPA2/WPA3
  stack, and none of it can be tested in the emulator used to build AXIOM. Until then: a network cable,
  a USB Ethernet adapter, or USB tethering from an Android phone. CDC-NCM (some newer adapters and phones)
  is implemented but untested; CDC-ECM and RNDIS are tested.
- **No battery level or sleep**: laptops report these through ACPI programs (AML) that AXIOM can't run yet.
- Sound: Intel HD Audio only (nearly all PCs; USB audio isn't supported), tested in QEMU, not on real
  codecs. Only system sounds for now (test chime, notifications).
- Keyboard layouts and text cover Western European languages only (Windows-1252).
- IP fragments aren't supported (rarely needed with TCP). Web runs no JavaScript and has no positioned boxes
  (they are laid out in the normal flow), `rowspan`, web fonts, SVG or video; animated GIFs show their first frame.
- IPv6 gets its address by SLAAC. Networks that hand out IPv6 addresses only by DHCPv6 (the router's
  "M" flag with no autoconfiguration prefix, found on some business networks) give AXIOM no global
  IPv6 address; it then uses IPv4. Stateless DHCPv6 (for DNS) is supported.
- IPv6 path MTU discovery is implemented for TCP but hasn't been tested yet.
- One CPU core is used. Kernel threads are cooperative; apps are preemptive.
- Needs a 32-bit graphics mode (any UEFI PC; most BIOS PCs). Otherwise it falls back to the text shell.
- Tested in QEMU (UEFI and BIOS, USB 2/3 devices, a USB hub, USB drive hot-plug, Intel e1000/e1000e
  networking against local web and DNS servers, HTTPS against OpenSSL test servers, IPv6 against
  QEMU's network and against `tests/ipv6/v6net.py`, a scripted IPv6-only router, DNS, DHCPv6 and web
  relay that can also fake an address conflict, NVMe/SATA/USB installs booted again from each disk,
  USB network adapters (ECM and RNDIS, hot-plug), Intel HD Audio recorded to a file). The sandbox it was
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
`openssl s_server` with 21 good and deliberately broken setups (needs python3-cryptography and openssl),
and checks IPv4/IPv6 address parsing and formatting against Python's `ipaddress` on 6,000 addresses,
time zones against Python's `zoneinfo` (every zone, hourly for 7 years), the app memory allocator under
AddressSanitizer, FAT32 formatting with `fsck.fat`, Bluetooth pairing against a simulated phone, and the
image codecs against Pillow (`python3 tests/test_img.py`).
`tests/ipv6/v6net.py` (needs scapy) is an IPv6 test network to run AXIOM against in QEMU; see the
top of the file for how to start it.

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
  msc.c fat.c      USB flash drives (Bulk-Only/SCSI) and the FAT32 file system (+ formatting)
  ahci.c nvme.c    SATA and NVMe disks
  disk.c           disk list, partitions, the AXIOM system volume, the installer
  usbnet.c         USB network adapters (CDC-ECM, CDC-NCM, RNDIS)
  hda.c            Intel HD Audio
  keymap.c keymaps.h  keyboard layouts (generated by tools/genkeymaps.py)
  settings.c tz.c  saved settings, time zones, lock screen password
  random.c         random number generator (RDSEED/RDRAND, jitter, events -> ChaCha20)
  net.c            TCP/IP stack: Ethernet, ARP, IPv4, ICMP, DHCP, IPv6, ICMPv6, NDP, SLAAC, DHCPv6, UDP, DNS, TCP
  nic_e1000.c nic_rtl8168.c   network card drivers
  syscall.c        system calls
  desktop.c        window manager + compositor, crown menu, notifications, input
  desktop_highland.c gfx.h   Highland design: wallpaper, top bar, window frames, dock
  highland.c       connects the Highland design to the window manager; AXIOM app icons
  fb_boot_logo.c   boot splash
  xhci.c usbkbd.c  USB 3 host controller, hubs, hot-plug; USB keyboard/mouse
  bt.c             Bluetooth HCI
  kbd.c pci.c acpi.c rtc.c mem.c timer.c console.c shell.c ...
common/          shared by kernel and apps: system call ABI, Highland theme tokens, 2D graphics, fonts, strings, IP addresses
user/lib         libdunc: app runtime (system calls, windows, UI widgets, HTTP/HTTPS client)
  crypto/          SHA-2, HMAC/HKDF, AES-GCM, ChaCha20-Poly1305, X25519, big numbers, RSA, ECDSA/ECDH,
                   X.509 certificates, TLS 1.3 client
tests/           host-side tests (make test); tests/tls has the TLS test matrix
user/apps        the apps (editor.c, clock.c and web.c are clan packages, listed in repo/index.txt);
                 web/ holds the browser's HTML parser, CSS engine and layout (tests/test_web.c, tests/web/)
rootfs/          files copied onto the system disk at start-up
```

## Roadmap

1. Test on real PCs; exFAT for big USB drives; installing next to Windows (partition resizing)
2. Bluetooth profiles (keyboards and mice first), Intel/MediaTek Bluetooth firmware loading
3. Wi-Fi; an ACPI interpreter (battery, sleep, I2C-HID touchpads)
4. TLS 1.2 fallback, revocation checks; JavaScript in Web
5. Multi-core scheduling, USB audio, network time
