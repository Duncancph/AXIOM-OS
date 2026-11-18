# GTK and Qt apps on AXIOM

GTK 3 and Qt 5 apps run on AXIOM's Linux layer and show their windows through AXIOM's Wayland compositor,
`axwl` (see `ports/wayland`). They're installed from Settings > Apps as signed packages
that unpack into the Linux folder (`root=yes`). On the disk that folder is `/linux`; Linux programs see
it as `/`.

| Package       | What it brings | Download | Installed |
|---------------|----------------|---------:|----------:|
| `gtk3`        | GTK 3.24 and its libraries, pixbuf loaders (SVG too), DejaVu fonts, fontconfig, XKB data, the MIME database, GSettings schemas, the Adwaita icons, the en_GB.UTF-8 locale, iconv's character sets, GTK's demo apps, and libraries most apps share (ICU, libxml2, librsvg) | 39 MB | 101 MB |
| `abiword`     | AbiWord, a word processor (Word, OpenDocument, RTF) | 25 MB | 68 MB |
| `gnumeric`    | Gnumeric, a spreadsheet (Excel, OpenDocument, CSV) | 8 MB | 20 MB |
| `mousepad`    | Mousepad, Xfce's text editor | 0.8 MB | 4 MB |
| `aisleriot`   | AisleRiot ("Solitaire"): Klondike, FreeCell, Spider and 80 more card games, with Guile, the Scheme interpreter its games are written in | 14 MB | 64 MB |
| `gnome-mines` | GNOME Mines | 0.6 MB | 2 MB |
| `evince`      | Evince, a document viewer (PDF, PostScript, DjVu, TIFF, XPS, comics) | 18 MB | 61 MB |
| `ristretto`   | Ristretto, Xfce's image viewer | 1 MB | 10 MB |
| `galculator`  | Galculator, a scientific calculator | 0.3 MB | 1 MB |
| `thunar`      | Thunar, Xfce's file manager | 1.5 MB | 4 MB |
| `geany`       | Geany, a programmer's editor | 2.3 MB | 8 MB |
| `iagno`       | Reversi (Iagno) | 0.6 MB | 12 MB |
| `qt5`         | Qt 5.15 for Qt apps: its libraries, its Wayland, image and icon plugins, and a gallery of Qt's widgets (`qt5-demo.cpp`) | 13 MB | 33 MB |
| `featherpad`  | FeatherPad, a text editor (Qt) | 1.9 MB | 5 MB |
| `speedcrunch` | SpeedCrunch, a calculator (Qt) | 1.5 MB | 4 MB |

Every GTK app needs `gtk3` (`requires=gtk3`), and Settings installs it first if it isn't there. Qt apps need
`qt5`, which needs `gtk3` (for fonts, fontconfig, XKB data and ICU); Settings installs both in turn. The
packages are listed in Settings > Apps.

## Building

On Ubuntu 24.04 (x86-64), with `libgtk-3-bin`, `librsvg2-common`, `locales` and the apps installed (and for Qt:
`qtwayland5`, `qt5-image-formats-plugins`, `libqt5svg5`, `qtbase5-dev` and `g++`):

    python3 ports/gtk/build-packages.py /tmp/axiom-gtk

This writes `<name>.tar.gz` for gtk3 and each app in its `APPS` table, plus their staging folders. An app is the
Debian packages whose files it brings (without docs, man pages, translations and help), plus the libraries its
programs need that gtk3 doesn't have. Build them all together: each package carries the machine's compiled
GSettings schemas, which must include every app's.

Each archive is a series of
gzip members holding at most 1 MiB of tar each. That's still an ordinary `.tar.gz`, and it lets AXIOM's
installer expand one member at a time with the inflater it already has.

Sign each package with the update key (`tools/make-package.py`, with `root=yes`, and `requires=gtk3` for the
apps). The archives can live anywhere on the web. AXIOM's own are on the `updates` branch, under `packages/`.

## Choices

- **Shared libraries in gtk3.** ICU (36 MB), libxml2 and librsvg are needed by most apps, so gtk3 has them
  once instead of each app bringing its own copy. librsvg also lets GTK draw the Adwaita SVG icons itself.
- **No symlinks.** FAT has none, so a library comes under the name its link has (the one the loader asks
  for), not as a second copy under the file's own name.
- **Test from the packages alone.** A chroot made of just the unpacked packages (no `/proc`, nothing else from
  the build machine) shows what an app misses on AXIOM. AbiWord crashed there and on AXIOM without iconv's
  gconv modules or its own window icon, but ran on the full build machine.
- **One locale.** AXIOM gives Linux programs `LANG=en_GB.UTF-8`, so the package includes that locale and
  GTK stops falling back to the C locale.
- **No cursor theme.** AXIOM draws its own pointer.
- **Qt draws into shared memory.** Qt uses its Wayland plugin with `wl_shm` (no OpenGL), and AXIOM's own frame
  through the KDE server-decoration protocol, as GTK apps do. Qt 5's semaphores need `FUTEX_WAKE_OP`; without
  it a Qt app stops before its first window.
- **Not packaged: GTK 4 and OpenGL apps.** GNOME's newer apps (Calculator, Sudoku, Chess, Mahjongg 3.40) are
  GTK 4, and Quadrapassel draws with Clutter (OpenGL), which AXIOM doesn't have.
- **Licences.** These are Ubuntu's binaries of free software (LGPL and GPL). Their sources are Ubuntu's
  source packages of the same versions.
