#!/usr/bin/env python3
"""Builds AXIOM packages for GTK 3 apps from the libraries and data of this (Ubuntu 24.04, x86-64) machine.
Both unpack into AXIOM's Linux folder (root=yes): lib/..., usr/share/..., as on Linux.

  gtk3      the shared runtime: GTK 3 and its libraries, gdk-pixbuf loaders, fonts (DejaVu), fontconfig, XKB
            keyboard data, the MIME database (gdk-pixbuf needs it to tell PNG from JPEG), GSettings schemas,
            the Adwaita icons, the
            en_GB.UTF-8 locale, and GTK's demo apps (its dock entry)
  and the apps in APPS below (each requires gtk3): Mousepad, AbiWord, Gnumeric, Aisleriot, Mines, Evince, Ristretto,
  Galculator, Thunar, Geany, Iagno
  qt5       Qt 5 for Qt apps (requires gtk3): its libraries and the Wayland, image and icon plugins
  and the Qt apps in APPS (each requires qt5): FeatherPad, SpeedCrunch

  python3 ports/gtk/build-packages.py <out-dir> [app...]   -> <out-dir>/<name>.tar.gz and the staging folders
Then sign each with tools/make-package.py (root=yes, requires=gtk3 for the apps) and publish. Build them all
together: each carries the machine's compiled GSettings schemas, which must include every app's.
Needs: libgtk-3-bin, librsvg2-common (the SVG loader), locales, and the apps installed."""
import gzip, os, re, shutil, subprocess, sys, tarfile

OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "/tmp/axiom-gtk")
LIB = "lib/x86_64-linux-gnu"
PIXBUF = "usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0"

def ldd(path):
    out = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
    return [m.group(1) for m in re.finditer(r"=> (/\S+)", out)]

def closure(files):
    seen = set()
    todo = list(files)
    while todo:
        for l in ldd(todo.pop()):
            if l not in seen: seen.add(l); todo.append(l)
    return seen

def put(stage, src, dst):                                  # copy, following links (FAT has none)
    d = os.path.join(stage, dst)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if os.path.isdir(src): shutil.copytree(src, d, symlinks=False, dirs_exist_ok=True)
    else: shutil.copy2(os.path.realpath(src), d)

def libs(stage, files, exclude=()):
    for l in sorted(closure(files)):
        name = os.path.basename(l)
        if name not in exclude: put(stage, l, os.path.join(LIB, name))

def tar(stage, name):
    path = os.path.join(OUT, name + ".tar")
    with tarfile.open(path, "w", format=tarfile.GNU_FORMAT) as t:
        for root, dirs, files in os.walk(stage):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(root, f)
                ti = t.gettarinfo(full, os.path.relpath(full, stage)); ti.uid = ti.gid = 0; ti.uname = ti.gname = ""
                with open(full, "rb") as fh: t.addfile(ti, fh)
    gzpath = path + ".gz"                       # independent gzip members of 1 MiB each: AXIOM expands one at a time
    with open(path, "rb") as f, open(gzpath, "wb") as g:
        for block in iter(lambda: f.read(1 << 20), b""): g.write(gzip.compress(block, 9, mtime=0))
    os.remove(path); path = gzpath
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(stage) for f in fs)
    print("%s: %.1f MB archive, %.1f MB installed" % (path, os.path.getsize(path) / 1e6, size / 1e6))

# libraries most apps need, kept in gtk3 so each app doesn't bring its own copy (ICU alone is 36 MB); librsvg also
# lets GTK draw the SVG icons itself
SHARED = ["libxml2.so.2", "libicuuc.so.74", "libicui18n.so.74", "libicudata.so.74", "librsvg-2.so.2",
          "libstdc++.so.6", "libgcc_s.so.1", "libxslt.so.1", "libunistring.so.5"]

def gtk3():
    stage = os.path.join(OUT, "gtk3"); shutil.rmtree(stage, ignore_errors=True)
    apps = ["/usr/bin/gtk3-demo", "/usr/bin/gtk3-widget-factory"]
    loaders = [os.path.join("/" + PIXBUF, "loaders", f) for f in sorted(os.listdir("/" + PIXBUF + "/loaders"))]
    for a in apps: put(stage, a, a[1:])
    libs(stage, apps + loaders + [os.path.realpath("/lib/x86_64-linux-gnu/" + l) for l in SHARED])
    put(stage, "/lib64/ld-linux-x86-64.so.2", "lib64/ld-linux-x86-64.so.2")
    for l in loaders: put(stage, l, os.path.join(PIXBUF, "loaders", os.path.basename(l)))
    put(stage, "/" + PIXBUF + "/loaders.cache", PIXBUF + "/loaders.cache")
    put(stage, "/usr/lib/x86_64-linux-gnu/gconv", "usr/lib/x86_64-linux-gnu/gconv")      # iconv's character sets (AbiWord
                                                                                          # crashes without them)
    put(stage, "/usr/share/fonts/truetype/dejavu", "usr/share/fonts/truetype/dejavu")
    put(stage, "/etc/fonts", "etc/fonts")
    put(stage, "/usr/share/X11/xkb", "usr/share/X11/xkb")
    for f in ("mime.cache", "magic", "globs2", "types", "aliases", "subclasses"):
        if os.path.exists("/usr/share/mime/" + f): put(stage, "/usr/share/mime/" + f, "usr/share/mime/" + f)
    put(stage, "/usr/share/glib-2.0/schemas/gschemas.compiled", "usr/share/glib-2.0/schemas/gschemas.compiled")
    os.makedirs(os.path.join(stage, "usr/lib/locale"), exist_ok=True)
    subprocess.run(["localedef", "-i", "en_GB", "-f", "UTF-8", os.path.join(stage, "usr/lib/locale/en_GB.UTF-8")], check=True)
    for d in ("scalable", "symbolic", "symbolic-up-to-32", "16x16", "index.theme"):        # (no cursors: AXIOM draws its own)
        put(stage, "/usr/share/icons/Adwaita/" + d, "usr/share/icons/Adwaita/" + d)
    put(stage, "/usr/share/icons/hicolor/index.theme", "usr/share/icons/hicolor/index.theme")
    tar(stage, "gtk3")
    return {os.path.basename(p) for p in os.listdir(os.path.join(stage, LIB))}

# Qt 5 (requires gtk3, whose fonts, fontconfig, XKB data, ICU and libstdc++ it uses): Qt's libraries and the
# plugins a Qt Widgets app needs on AXIOM: Wayland (wl_shm, xdg-shell, its own window decorations), image formats
# and SVG icons. Qt finds them in its built-in plugin folder, which the Linux folder keeps as on Linux.
QT_PLUGINS = "usr/lib/x86_64-linux-gnu/qt5/plugins"
QT_PLUGIN_FILES = ["platforms/libqwayland-generic.so", "wayland-shell-integration/libxdg-shell.so",
                   "wayland-shell-integration/libwl-shell.so", "wayland-decoration-client/libbradient.so",
                   "iconengines/libqsvgicon.so"] + ["imageformats/" + f for f in
                   sorted(os.listdir("/" + QT_PLUGINS + "/imageformats")) if f.endswith(".so")]
QT_LIBS = ["libQt5Core.so.5", "libQt5Gui.so.5", "libQt5Widgets.so.5", "libQt5DBus.so.5", "libQt5Svg.so.5",
           "libQt5Network.so.5", "libQt5PrintSupport.so.5", "libQt5WaylandClient.so.5"]

def qt5(base):
    stage = os.path.join(OUT, "qt5"); shutil.rmtree(stage, ignore_errors=True)
    plugins = [os.path.join("/" + QT_PLUGINS, f) for f in QT_PLUGIN_FILES]
    for pl in plugins: put(stage, pl, os.path.join(QT_PLUGINS, os.path.relpath(pl, "/" + QT_PLUGINS)))
    qtlibs = [os.path.realpath("/lib/x86_64-linux-gnu/" + l) for l in QT_LIBS]
    for l, real in zip(QT_LIBS, qtlibs): put(stage, real, os.path.join(LIB, l))     # (the libraries themselves, then
    libs(stage, plugins + qtlibs, exclude=base)                                       # what they and the plugins need)
    tar(stage, "qt5")
    return {os.path.basename(p) for p in os.listdir(os.path.join(stage, LIB))}

# The apps: the Debian packages whose files they bring (without docs, man pages, translations and help), the
# programs (whose libraries come along, unless gtk3 has them), and what Settings shows.
APPS = {
    "mousepad": dict(debs=["mousepad", "libmousepad0", "libgtksourceview-4-0", "libgtksourceview-4-common",
                           "libgspell-1-2", "libgspell-1-common"], bins=["/usr/bin/mousepad"]),
    "abiword":  dict(debs=["abiword", "abiword-common", "libabiword-3.0", "libgoffice-0.10-10t64",
                           "libgoffice-0.10-10-common", "libgsf-1-114", "libgsf-1-common"], bins=["/usr/bin/abiword"]),
    "gnumeric": dict(debs=["gnumeric", "gnumeric-common", "libgoffice-0.10-10t64", "libgoffice-0.10-10-common",
                           "libgsf-1-114", "libgsf-1-common"], bins=["/usr/bin/gnumeric"]),
    "aisleriot": dict(debs=["aisleriot", "guile-3.0-libs", "libcanberra0t64", "libcanberra-gtk3-0t64"],
                      bins=["/usr/games/sol"]),
    "gnome-mines": dict(debs=["gnome-mines", "libgnome-games-support-1-3", "libgnome-games-support-common",
                              "libhandy-1-0"], bins=["/usr/games/gnome-mines"]),
    "evince": dict(debs=["evince", "evince-common", "libevdocument3-4t64", "libevview3-3t64"], bins=["/usr/bin/evince"]),
    "ristretto": dict(debs=["ristretto", "libxfce4ui-2-0", "libxfce4ui-common", "libxfce4util7", "libxfce4util-common",
                            "libxfconf-0-3", "libmagic-mgc"], bins=["/usr/bin/ristretto"],
                      skip=("/usr/lib/file/", "/usr/share/file/")),     # (libmagic reads /usr/share/misc/magic.mgc)
    "galculator": dict(debs=["galculator"], bins=["/usr/bin/galculator"]),
    "thunar": dict(debs=["thunar", "thunar-data", "libthunarx-3-0", "libexo-2-0", "libexo-common", "libxfce4ui-2-0",
                         "libxfce4ui-common", "libxfce4util7", "libxfce4util-common", "libxfconf-0-3"],
                   bins=["/usr/bin/thunar"]),
    "geany": dict(debs=["geany", "geany-common"], bins=["/usr/bin/geany"]),
    "iagno": dict(debs=["iagno", "libgsound0t64"], bins=["/usr/games/iagno"]),
    # Qt 5 apps (require qt5)
    "featherpad": dict(debs=["featherpad"], bins=["/usr/bin/featherpad"], qt=True),
    "speedcrunch": dict(debs=["speedcrunch"], bins=["/usr/bin/speedcrunch"], qt=True),
}
SKIP = ("/usr/share/doc/", "/usr/share/man/", "/usr/share/locale/", "/usr/share/help/", "/usr/share/lintian/",
        "/usr/share/bug/", "/usr/share/metainfo/", "/usr/share/applications/", "/usr/share/info/",
        "/usr/share/gnome-shell/", "/usr/share/dbus-1/", "/usr/share/glib-2.0/schemas/")

def is_elf(path):
    try:
        with open(path, "rb") as f: return f.read(4) == b"\x7fELF"
    except OSError: return False

def app(name, base):
    spec = APPS[name]
    stage = os.path.join(OUT, name); shutil.rmtree(stage, ignore_errors=True)
    elves = list(spec["bins"])
    for deb in spec["debs"]:
        files = subprocess.run(["dpkg", "-L", deb], capture_output=True, text=True, check=True).stdout.split("\n")
        # FAT has no symlinks: a library comes as its link's name (libfoo.so.1), not also as the file it points to
        linked = {os.path.realpath(f) for f in files if f and os.path.islink(f) and not f.endswith(".so")}
        for f in files:
            if not f or not os.path.isfile(f) or f.startswith(SKIP + spec.get("skip", ())) or f.startswith("/etc/"): continue
            if os.path.islink(f) and f.endswith(".so"): continue                 # (for building against it)
            if not os.path.islink(f) and f in linked: continue
            put(stage, f, f[1:])
            if is_elf(f): elves.append(f)
    for b in spec["bins"]: put(stage, b, b[1:])
    have = {os.path.basename(f) for r, _, fs in os.walk(stage) for f in fs}
    libs(stage, elves, exclude=base | have)
    # (the compiled schemas include every app's: this replaces gtk3's copy with a superset)
    put(stage, "/usr/share/glib-2.0/schemas/gschemas.compiled", "usr/share/glib-2.0/schemas/gschemas.compiled")
    tar(stage, name)

os.makedirs(OUT, exist_ok=True)
base = gtk3()
qtbase = base | qt5(base)
for name in (sys.argv[2:] or APPS): app(name, qtbase if APPS[name].get("qt") else base)
