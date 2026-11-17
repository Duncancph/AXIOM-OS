#!/usr/bin/env python3
"""Builds AXIOM packages for GTK 3 apps from the libraries and data of this (Ubuntu 24.04, x86-64) machine.
Both unpack into AXIOM's Linux folder (root=yes): lib/..., usr/share/..., as on Linux.

  gtk3      the shared runtime: GTK 3 and its libraries, gdk-pixbuf loaders, fonts (DejaVu), fontconfig, XKB
            keyboard data, the MIME database (gdk-pixbuf needs it to tell PNG from JPEG), GSettings schemas,
            the Adwaita icons (pre-rendered to PNG: no SVG loader, which would bring librsvg and ICU), the
            en_GB.UTF-8 locale, and GTK's demo apps (its dock entry)
  mousepad  Xfce's text editor: its own libraries (gtksourceview, libxml2, ICU) and data; requires gtk3

  python3 ports/gtk/build-packages.py <out-dir>      -> <out-dir>/{gtk3,mousepad}.tar.gz and the staging folders
Then sign each with tools/make-package.py (root=yes, requires=gtk3 for mousepad) and publish.
Needs: libgtk-3-bin (gtk3-demo, gtk3-widget-factory), mousepad, librsvg2-bin (rsvg-convert), locales."""
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

def icons(stage):
    """Adwaita for GTK 3, as PNGs: the symbolic icons rendered at 16, 24 and 32 px, plus its own 16x16 PNGs."""
    src = "/usr/share/icons/Adwaita"
    base = os.path.join(stage, "usr/share/icons/Adwaita")
    dirs = []
    for size in (16, 24, 32):
        for ctx in sorted(os.listdir(os.path.join(src, "symbolic"))):
            sd = os.path.join(src, "symbolic", ctx)
            if not os.path.isdir(sd): continue
            rel = "symbolic-%d/%s" % (size, ctx)
            os.makedirs(os.path.join(base, rel), exist_ok=True)
            for f in sorted(os.listdir(sd)):
                if f.endswith(".svg"):
                    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o",
                                    os.path.join(base, rel, f[:-4] + ".png"), os.path.join(sd, f)], check=True)
            dirs.append((rel, size, ctx))
    for ctx in sorted(os.listdir(os.path.join(src, "16x16"))):
        put(stage, os.path.join(src, "16x16", ctx), "usr/share/icons/Adwaita/16x16/" + ctx)
        dirs.append(("16x16/" + ctx, 16, ctx))
    with open(os.path.join(base, "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=Adwaita\nComment=Adwaita, as PNGs for AXIOM\nInherits=hicolor\n")
        f.write("Directories=%s\n\n" % ",".join(d for d, _, _ in dirs))
        for d, size, ctx in dirs:
            f.write("[%s]\nContext=%s\nSize=%d\nMinSize=%d\nMaxSize=%d\nType=Scalable\n\n" % (d, ctx.capitalize(), size, size, size))
    put(stage, "/usr/share/icons/hicolor/index.theme", "usr/share/icons/hicolor/index.theme")

def gtk3():
    stage = os.path.join(OUT, "gtk3"); shutil.rmtree(stage, ignore_errors=True)
    apps = ["/usr/bin/gtk3-demo", "/usr/bin/gtk3-widget-factory"]
    loaders = [os.path.join("/" + PIXBUF, "loaders", f) for f in sorted(os.listdir("/" + PIXBUF + "/loaders")) if "svg" not in f]
    for a in apps: put(stage, a, a[1:])
    libs(stage, apps + loaders)
    put(stage, "/lib64/ld-linux-x86-64.so.2", "lib64/ld-linux-x86-64.so.2")
    for l in loaders: put(stage, l, os.path.join(PIXBUF, "loaders", os.path.basename(l)))
    cache = open("/" + PIXBUF + "/loaders.cache").read().split("\n\n")
    with open(os.path.join(stage, PIXBUF, "loaders.cache"), "w") as f:
        f.write("\n\n".join(b for b in cache if "svg" not in b.split("\n", 1)[0] and "svg" not in b[:200]))
    put(stage, "/usr/share/fonts/truetype/dejavu", "usr/share/fonts/truetype/dejavu")
    put(stage, "/etc/fonts", "etc/fonts")
    put(stage, "/usr/share/X11/xkb", "usr/share/X11/xkb")
    for f in ("mime.cache", "magic", "globs2", "types", "aliases", "subclasses"):
        if os.path.exists("/usr/share/mime/" + f): put(stage, "/usr/share/mime/" + f, "usr/share/mime/" + f)
    put(stage, "/usr/share/glib-2.0/schemas/gschemas.compiled", "usr/share/glib-2.0/schemas/gschemas.compiled")
    os.makedirs(os.path.join(stage, "usr/lib/locale"), exist_ok=True)
    subprocess.run(["localedef", "-i", "en_GB", "-f", "UTF-8", os.path.join(stage, "usr/lib/locale/en_GB.UTF-8")], check=True)
    icons(stage)
    tar(stage, "gtk3")
    return {os.path.basename(p) for p in os.listdir(os.path.join(stage, LIB))}

def mousepad(base):
    stage = os.path.join(OUT, "mousepad"); shutil.rmtree(stage, ignore_errors=True)
    plugins = "/usr/lib/x86_64-linux-gnu/mousepad/plugins"
    put(stage, "/usr/bin/mousepad", "usr/bin/mousepad")
    put(stage, plugins, plugins[1:])
    libs(stage, ["/usr/bin/mousepad"] + [os.path.join(plugins, f) for f in os.listdir(plugins)], exclude=base)
    put(stage, "/usr/share/gtksourceview-4", "usr/share/gtksourceview-4")
    # (the compiled schemas include Mousepad's: this replaces gtk3's copy with a superset)
    put(stage, "/usr/share/glib-2.0/schemas/gschemas.compiled", "usr/share/glib-2.0/schemas/gschemas.compiled")
    tar(stage, "mousepad")

os.makedirs(OUT, exist_ok=True)
mousepad(gtk3())
