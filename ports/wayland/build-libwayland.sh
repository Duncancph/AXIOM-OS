#!/bin/sh
# Build a static libwayland-server.a (and its headers) into $1 (default /tmp/axwl) from the wayland 1.22
# source in $2 (default: the current directory, e.g. after `apt source wayland`). Distros ship only the
# shared library; axwl links statically so it runs on AXIOM with no libraries of its own.
set -e
OUT=$(realpath -m "${1:-/tmp/axwl}"); SRC=$(realpath "${2:-.}")
mkdir -p "$OUT/obj"; cd "$OUT/obj"
cat > config.h <<'H'
#define HAVE_ACCEPT4 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_PRCTL 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_MREMAP 1
#define HAVE_STRNDUP 1
#define HAVE_SYS_PRCTL_H 1
#define PACKAGE "wayland"
#define PACKAGE_VERSION "1.22.0"
H
sed -e 's/@WAYLAND_VERSION_MAJOR@/1/; s/@WAYLAND_VERSION_MINOR@/22/; s/@WAYLAND_VERSION_MICRO@/0/; s/@WAYLAND_VERSION@/1.22.0/' \
    "$SRC/src/wayland-version.h.in" > wayland-version.h
wayland-scanner server-header "$SRC/protocol/wayland.xml" wayland-server-protocol.h
wayland-scanner public-code "$SRC/protocol/wayland.xml" wayland-protocol.c
for f in connection wayland-server wayland-shm event-loop wayland-os wayland-util; do
    gcc -O2 -fPIC -I. -I"$SRC/src" -c "$SRC/src/$f.c" -o $f.o
done
gcc -O2 -fPIC -I. -I"$SRC/src" -c wayland-protocol.c -o wayland-protocol.o
rm -f "$OUT/libwayland-server.a"
ar rcs "$OUT/libwayland-server.a" connection.o wayland-server.o wayland-shm.o event-loop.o wayland-os.o wayland-util.o wayland-protocol.o
cp wayland-server-protocol.h wayland-version.h "$SRC"/src/wayland-server.h "$SRC"/src/wayland-server-core.h "$SRC"/src/wayland-util.h "$OUT/"
echo "built $OUT/libwayland-server.a"
