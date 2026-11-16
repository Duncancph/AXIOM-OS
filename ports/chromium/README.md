# Chromium on AXIOM (Vertex)

Chromium runs on AXIOM as a Linux program, through the Linux compatibility layer (`src/linux.c`).
This directory holds the AXIOM-specific parts of the Chromium tree. The browser's own interface (later)
is called **Vertex**.

## What's here

- `ui/ozone/platform/axiom/`: an Ozone platform, `ozone_platform=axiom`. Each Chromium window is an AXIOM
  window opened through `/dev/axiom/window` (see `common/axiom_window.h`; `axiom_window_abi.h` is a copy of it).
  - Software frames: Skia draws into its own surface, and each frame's damaged rows are copied into the
    window's mmap'd pixels, then `AXWIN_FLUSH`.
  - Input: the window fd is watched on the UI thread. Mouse, wheel and keys become Chromium events
    (AXIOM sends no key releases yet, so a release follows each press; US layout).
  - Resizing (maximize, drag): `AXWIN_EV_RESIZE`, then unmap, `AXWIN_RESIZE`, map and `UpdateBounds`.
  - AXIOM draws the frame (`ShouldUseNativeFrame()` is true); the default title is "Vertex".
  - It builds on the headless platform (screen, input method, surface factory base).
- `chromium.patch`: registers the platform (`build/config/ozone.gni`, `ui/ozone/BUILD.gn`).

## Building (on Linux)

Tested with Chromium main 160.0.8165.0 (a162341e6a01, 2026-11-15).

    cp -r ui/ozone/platform/axiom ~/chromium/src/ui/ozone/platform/
    cd ~/chromium/src && git apply /path/to/axiom-os/ports/chromium/chromium.patch
    gn gen out/axiom --args='is_debug=false is_component_build=false symbol_level=1 blink_symbol_level=0
        v8_symbol_level=0 ozone_platform="axiom" ozone_platform_axiom=true'
    ninja -C out/axiom content_shell

Add `ozone_platform_headless=true` to also build `headless_shell` (the same as Chrome's headless mode).
`use_dbus=false` breaks the gn graph; D-Bus stays in and just fails to connect on AXIOM.

## Running on AXIOM

Copy to the AXIOM disk: `content_shell` (`llvm-objcopy --strip-debug` takes it from 1.4 GB to 520 MB and
keeps function names), `*.pak`, `icudtl.dat`, `snapshot_blob.bin`, `v8_context_snapshot.bin`; the
shared libraries `ldd` lists go in `/linux/lib/x86_64-linux-gnu`, fonts in `/linux/usr/share/fonts`.
Then, in the Terminal:

    run cs/content_shell --no-sandbox --single-process --no-zygote --disable-gpu --enable-logging=stderr
        --disable-in-process-stack-traces cs/t.html

- `--disable-in-process-stack-traces`: AXIOM delivers no signals, so a failed CHECK ends the program at
  once; the kernel then logs the return addresses on its stack (file + offset, for `llvm-symbolizer`).
  Without the flag, Chromium first symbolizes its own stack by reading the binary 4 KB at a time,
  which takes minutes.
- Debugging a hang: Ctrl+\ (0x1C) on the serial line makes AXIOM list each Linux thread, the system call it's
  in and its stack.

State (2026-11-16, QEMU): pages render; clicks, typing, Page Down scrolling, maximize and the
close button work; closing exits with code 0.
