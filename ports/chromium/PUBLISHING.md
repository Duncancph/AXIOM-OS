# Publishing Vertex (or any big Linux app) as an AXIOM package

AXIOM installs big apps from **Settings > Updates > More apps**: it downloads the archive straight to disk
(resuming if the connection drops), checks its SHA-256 against a description signed with the AXIOM update
key, unpacks it into the Linux folder and adds the app to the dock and menu.

## 1. Linux PC: make the archive and upload it

Put everything the app needs in one folder, laid out as it should appear in the Linux folder (`/linux` on
the AXIOM disk). For Vertex, with its libraries and sound configuration:

    staging/
      opt/vertex/              the Chromium build (vertex binary, .pak files, locales/, icudtl.dat, ...)
      lib/x86_64-linux-gnu/    the shared libraries it needs (as on the Linux PC's AXIOM disk today)
      lib64/ld-linux-x86-64.so.2
      usr/share/alsa/          from libasound2-data (sound)
      etc/asound.conf          tests/linux/asound.conf (sound)
      usr/share/fonts/, etc/fonts/   if it uses fontconfig

Then:

    python3 tools/pack-app.py staging vertex-1.0.tar      # prints size, sha256, installed-size
    gh release create vertex-1.0 vertex-1.0.tar --repo Duncancph/AXIOM-OS --title "Vertex 1.0" --notes "Vertex browser for AXIOM"

and send the cloud session: the release URL of the asset, the three numbers pack-app printed, and the
command line to start it (relative to the Linux folder, e.g. `opt/vertex/vertex --no-sandbox`).

Names: FAT allows up to 255 characters per file name (AXIOM's file layer does too). Folder listings
that a Linux program reads show names up to 63 characters; opening a longer name directly works.

## 2. Cloud session: sign and publish the description

    python3 tools/make-package.py <update key> <updates checkout> id=vertex name=Vertex version=1.0 icon=arc \
        "description=The full web browser (Chromium)" min-build=151 root=yes \
        url=<asset URL> size=<..> sha256=<..> installed-size=<..> "run=opt/vertex/vertex --no-sandbox"

This writes `packages/vertex.txt` and `packages/index.txt` next to `latest.txt` in the updates branch.
The id `vertex` takes over the dock's browser once installed (the built-in one stays in the menu).
