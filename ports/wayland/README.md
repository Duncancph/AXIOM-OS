# axwl: Wayland on AXIOM

`axwl` is AXIOM's Wayland compositor. It's a Linux program that runs on AXIOM's Linux layer. Linux desktop
apps (GTK, Qt, SDL, anything that speaks Wayland) connect to it, and each app window becomes a normal
AXIOM window:

- **Windows:** each `xdg_toplevel` is an AXIOM window (`/dev/axiom/window`), and AXIOM draws its frame. Apps
  are told to leave decorations to the server (`xdg-decoration`, plus KDE's `server-decoration`, which GTK 3
  uses).
- **Menus:** popups and subsurfaces are drawn into their toplevel's window.
- **Input:** mouse and keys become `wl_pointer` and `wl_keyboard` events (evdev key codes, US keymap).
- **Clipboard:** copy and paste go through the AXIOM clipboard in both directions.
- **Buffers:** `wl_shm` (shared memory) only, with no GPU.

AXIOM starts axwl by itself. Linux programs get `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/tmp`,
`GDK_BACKEND=wayland` and `QT_QPA_PLATFORM=wayland`. The first time an app connects to `/tmp/wayland-0`,
the kernel starts `/system/bin/axwl` and waits for it to listen.

## Building (on Linux)

Building needs `wayland-scanner`, `libffi.a` (from libffi-dev) and a static `libwayland-server.a`. Distros
only ship the shared library, so `build-libwayland.sh` builds the static one from the wayland 1.22 source:

    apt source wayland                                        # wayland-1.22.0/
    ./build-libwayland.sh /tmp/axwl wayland-1.22.0
    make WL=/tmp/axwl

Copy `axwl` (stripped) to `rootfs/system/bin/axwl`.

## Testing on Linux

`axwl-test` is the same compositor with a test backend in place of AXIOM windows. Windows are kept in
memory and driven through a control FIFO:

    export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-9 AXWL_CONTROL=/tmp/axwl.ctl
    ./axwl-test &  GDK_BACKEND=wayland gtk3-widget-factory &
    echo "click 180 160" > $AXWL_CONTROL; echo "type hello" > $AXWL_CONTROL
    echo "snap 0 /tmp/w.ppm" > $AXWL_CONTROL

See the comment at the top of `backend_test.c` for the full list of commands.

## Not there yet

- Drag and drop
- Touch
- Fractional or HiDPI scaling
- `linux-dmabuf` (GPU buffers)
- Moving or resizing windows from the app's own title bar (AXIOM's frame does both)
- Primary selection
- Keyboard layouts other than US
