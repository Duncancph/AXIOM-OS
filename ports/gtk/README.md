# GTK apps on AXIOM

GTK 3 apps run on AXIOM's Linux layer and show their windows through AXIOM's Wayland compositor,
`axwl` (see `ports/wayland`). They're installed from Settings > Updates > More apps as signed packages
that unpack into the Linux folder (`root=yes`). On the disk that folder is `/linux`; Linux programs see
it as `/`.

| Package    | What it brings                                                               | Download | Installed |
|------------|------------------------------------------------------------------------------|---------:|----------:|
| `gtk3`     | GTK 3.24 and its libraries, pixbuf loaders, DejaVu fonts, fontconfig, XKB data, the MIME database, GSettings schemas, Adwaita icons as PNGs, the en_GB.UTF-8 locale, and GTK's demo apps | 20 MB | 51 MB |
| `mousepad` | Xfce's text editor, with gtksourceview, libxml2 and ICU. Needs `gtk3` (`requires=gtk3`): Settings installs that first if it isn't there | 15 MB | 39 MB |

## Building

On Ubuntu 24.04 (x86-64), with `libgtk-3-bin`, `mousepad`, `librsvg2-bin` and `locales` installed:

    python3 ports/gtk/build-packages.py /tmp/axiom-gtk

This writes `gtk3.tar.gz` and `mousepad.tar.gz`, plus their staging folders. Each archive is a series of
gzip members holding at most 1 MiB of tar each. That's still an ordinary `.tar.gz`, and it lets AXIOM's
installer expand one member at a time with the inflater it already has.

Sign each package with the update key. The archive can live anywhere on the web.

    python3 tools/make-package.py key.pem out id=gtk3 name="GTK 3" version=3.24.41 icon=gear \
        "description=Runs GTK 3 apps on AXIOM. Opens GTK's demo." min-build=<build> root=yes \
        url=https://.../gtk3.tar.gz run=usr/bin/gtk3-demo archive=/tmp/axiom-gtk/gtk3.tar.gz
    python3 tools/make-package.py key.pem out id=mousepad name=Mousepad version=0.6.1 icon=note \
        "description=A simple text editor with syntax highlighting." min-build=<build> root=yes \
        requires=gtk3 url=https://.../mousepad.tar.gz run=usr/bin/mousepad archive=/tmp/axiom-gtk/mousepad.tar.gz

## Choices

- **Icons as PNGs.** GTK loads SVG icons through librsvg, which brings libxml2 and ICU, about 55 MB more.
  Instead the Adwaita symbolic icons are rendered to PNG at 16, 24 and 32 px when the package is built.
  They're gray rather than recoloured to match the theme.
- **One locale.** AXIOM gives Linux programs `LANG=en_GB.UTF-8`, so the package includes that locale and
  GTK stops falling back to the C locale.
- **No cursor theme.** AXIOM draws its own pointer.
- **Licences.** These are Ubuntu's binaries of free software (LGPL and GPL). Their sources are Ubuntu's
  source packages of the same versions.
