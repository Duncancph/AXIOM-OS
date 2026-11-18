# Vertex: Chromium on AXIOM

Chromium runs on AXIOM as a Linux program, through the Linux compatibility layer (`src/linux.c`). Built with
the files here it's **Vertex**: the full browser (tabs, address bar, bookmarks, settings, downloads) drawn in
AXIOM windows, under its own name and logo.

## What's here

- `ui/ozone/platform/axiom/`: an Ozone platform, `ozone_platform=axiom`. Each Chromium window is an AXIOM
  window opened through `/dev/axiom/window` (`axiom_window_abi.h` is a copy of `common/axiom_window.h`).
  - Software frames: Skia draws into its own surface; each frame's damaged rows are copied into the
    window's mmap'd pixels, then `AXWIN_FLUSH`. The browser owns the windows and reads their events; the
    compositor, which may run in a separate GPU process, draws them. So the browser sends each window's
    file to the GPU service (`AxiomGpuHost` → `mojom/axiom_window_pixels.mojom` → `AxiomPixels`), and the
    canvas maps the pixels itself and resizes them to the size it draws at (`AXWIN_RESIZE_TO`: AXIOM won't
    resize mapped pixels, and only the drawing side knows when they're free). The same path is used when
    the GPU service runs in the browser (`--single-process`, `--in-process-gpu`).
  - Windows: AXIOM draws the frame (`ShouldUseNativeFrame()`), places them and sizes them (maximize, drag,
    snapping); `AXWIN_EV_MOVE` keeps Chromium told where they are.
  - Menus, dropdowns, the address bar's suggestions, tooltips and bubbles are AXIOM popups (`AXWIN_POPUP`):
    no frame, placed in screen coordinates, never focused. AXIOM has no pointer grabs, so capture is kept in
    the platform across Vertex's windows; when Vertex loses the focus (a click elsewhere) capture is lost,
    which closes menus as a grab would. Popups are opaque (no alpha), so `IsWindowCompositingSupported()` is
    false and views draws square menus without shadows.
  - Pointer shapes: Chromium's cursor types map to AXIOM's (`AXWIN_CURSOR`: arrow, text, hand, wait,
    crosshair, move, resize arrows, not-allowed, none).
  - Keys: AXIOM applies the keyboard layout chosen in its Settings (US, UK, DE, FR, ES, with AltGr and dead
    keys) and sends Windows-1252 characters plus the physical key (USB usage), which becomes the DOM code
    (`KeyboardEvent.code`). AXIOM sends no key releases yet, so a release follows each press.
  - Clipboard (`axiom_clipboard.*`): see below.
- `chrome/browser/ui/views/axiom/`: file open/save dialogs drawn by views (there's no GTK, Qt or desktop
  portal on AXIOM): the folder (editable) with Up, its contents, the file name for saving. Installed as the
  `SelectFileDialog` factory when the Ozone platform is `axiom`.
- `chromium.patch`: registers the platform (`build/config/ozone.gni`, `ui/ozone/BUILD.gn`), the file dialog
  (`chrome/browser/ui/views/BUILD.gn`, `chrome_browser_main_extra_parts_views_linux.cc`), the product name
  (`chrome/app/theme/chromium/BRANDING`) and the "Vertex" brand in User-Agent Client Hints
  (`components/embedder_support/user_agent_utils.cc`). On AXIOM (when `/dev/axiom/window` exists) it also:
  picks the `axiom` platform (`ui/linux/display_server_utils.cc`), skips Crashpad, whose handler needs
  `fork()` (`chrome/app/chrome_main_delegate.cc`), lets Vertex be the only instance without the Unix-socket
  and symlink lock (`chrome/browser/process_singleton_posix.cc`), and doesn't show the "unsupported flag" bar
  for `--no-sandbox` and `--single-process` (`chrome/browser/ui/startup/bad_flags_prompt.cc`).
- `rebrand.py`: renames Chromium to Vertex in the product strings (`chromium_strings.grd` and its parts,
  `components_chromium_strings.grd`) and keeps all 81 translations: they're keyed by a fingerprint of the
  English text, so grit works out each message's id before and after and the translations move with them.
- `logo.py`: draws the Vertex logo (a dark disc, a cream V meeting at a bright vertex) at every size Chromium
  uses, plus its SVG, monochrome, name and X11 versions.

### The user agent

Unchanged from Chromium's (`Mozilla/5.0 (X11; Linux x86_64) ... Chrome/160.0.0.0 Safari/537.36`): Vertex is
Chromium's Linux build running on AXIOM, and that's what sites need to know to serve it. It doesn't claim to
be Google Chrome; User-Agent Client Hints list the brand "Vertex" next to "Chromium", as other
Chromium-based browsers list theirs.

### The clipboard

Vertex uses AXIOM's shared clipboard through ioctls on a `/dev/axiom/window` fd that it opens for this alone,
with no window (`AXWIN_CLIP_SET` / `AXWIN_CLIP_GET` and `struct axwin_clip` in `common/axiom_window.h`,
copied here as `axiom_window_abi.h`):

- `AXWIN_CLIP_SET`: copy `len` bytes of UTF-8 text from `data` (at most 1 MiB); `seq` comes back;
- `AXWIN_CLIP_GET`: fill the buffer at `data`/`len`; returns the full length and `seq` (`data = 0`: only those);
- `seq` goes up whenever any app copies.

Vertex keeps every format it copies (text, HTML, images) itself and gives AXIOM the text; it pastes its own
data while `seq` is still the one its copy returned, else the text from AXIOM. It checks `seq` once a second
to tell web pages the clipboard changed. Without AXIOM, copy and paste work within Vertex.

## Building (on Linux)

Tested with Chromium main 160.0.8165.0 (a162341e6a01, 2026-11-15).

    cp -r ui chrome ~/chromium/src/                    # (the platform and the dialog)
    cd ~/chromium/src && git apply /path/to/axiom-os/ports/chromium/chromium.patch
    python3 /path/to/axiom-os/ports/chromium/rebrand.py ~/chromium/src
    python3 /path/to/axiom-os/ports/chromium/logo.py ~/chromium/src/chrome/app/theme

Two builds: `out/vertex` to ship, `out/axiom` with symbols for debugging.

    # out/vertex/args.gn
    is_official_build = true
    is_debug = false
    is_component_build = false
    symbol_level = 0
    blink_symbol_level = 0
    v8_symbol_level = 0
    dcheck_always_on = false
    chrome_pgo_phase = 0      # (no PGO profiles in a checkout without the Google hooks)
    is_cfi = false
    ozone_platform = "axiom"
    ozone_platform_axiom = true
    ozone_platform_headless = true

    # out/axiom/args.gn
    is_debug = false
    is_component_build = false
    symbol_level = 1
    blink_symbol_level = 0
    v8_symbol_level = 0
    ozone_platform = "axiom"
    ozone_platform_axiom = true
    ozone_platform_headless = true
    concurrent_links = 1

    ninja -C out/vertex chrome

`content_shell` and `headless_shell` build the same way. `use_dbus=false` breaks the gn graph; D-Bus stays in
and just fails to connect on AXIOM.

## Running on AXIOM

Copy to the AXIOM disk, in one folder: `chrome` (`out/vertex`'s, or `out/axiom`'s with
`llvm-objcopy --strip-debug`, which keeps function names), `chrome_100_percent.pak`, `chrome_200_percent.pak`, `resources.pak`, `locales/` (at least
`en-US.pak`), `icudtl.dat`, `v8_context_snapshot.bin`. The shared libraries `ldd` lists go in
`/linux/lib/x86_64-linux-gnu` (with NSS's `libsoftokn3.so`, `libfreeblpriv3.so` and `.chk` files, which NSS
loads itself), fonts in `/linux/usr/share/fonts`. Then, in the Terminal:

    run vx/chrome --no-sandbox --single-process --no-zygote --disable-gpu --no-first-run
        --user-data-dir=/home/vx/profile --disable-in-process-stack-traces https://www.google.com

(Installed as a package, 1.0 runs `opt/vertex/vertex --no-sandbox --single-process --no-zygote --disable-gpu`.)

Multi-process (each site's pages in their own renderer process, the GPU service in its own process), from
Vertex 1.1 and a kernel with `/proc` folders and `AXWIN_RESIZE_TO`:

    opt/vertex/vertex --no-sandbox --no-zygote --disable-gpu --no-default-browser-check

- `--no-zygote`: renderers are started with `fork` + `exec` like the other child processes.
- `--no-default-browser-check`: the check runs `xdg-settings`, which isn't there (the child exits with 127).
- `--single-process` still works, and uses the least memory: with a test page, a downloaded file and
  YouTube playing, 639 MiB in use on AXIOM build 174, against 972 MiB multi-process with
  `--in-process-gpu` and 1048 MiB with the GPU process separate (programs' read-only pages are shared
  between processes since build 174; before that, multi-process took 2279 MiB).
- `--disable-in-process-stack-traces`: AXIOM delivers no signals, so a failed CHECK ends the program at
  once; the kernel then logs the return addresses on its stack (file + offset, for `llvm-symbolizer`).
- Debugging a hang: Ctrl+\ (0x1C) on the serial line makes AXIOM list how busy each core has been and each
  Linux thread: the system call it's in, its stack, its core and its CPU time.

## Widevine (Netflix)

Not built in: `enable_widevine` is off in these builds. Protected video needs Google's Widevine CDM
(`libwidevinecdm.so`), which is proprietary: Google licenses it to browser makers, and only licensed browsers
may ship it or download it with the component updater. Building with `enable_widevine=true` would let Vertex
load a CDM the user installs, but streaming services also check the browser and platform they serve, so this
is a licensing question before it's a technical one.
