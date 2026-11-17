# The AXIOM Wayland compositor (axwl): a Linux program that runs on AXIOM and shows Wayland apps (GTK, Qt...)
# in AXIOM windows. Built on Linux (needs wayland-scanner, libffi.a and a static libwayland-server, see
# build-libwayland.sh). 'make' builds axwl (for AXIOM) and axwl-test (the same with a test backend, to run here).
WL ?= /tmp/claude-0/axwl
CFLAGS = -O2 -g -Wall -Wno-unused-parameter -D_GNU_SOURCE -I. -Igen -I$(WL) -I../../common
PROTOS = xdg-shell xdg-decoration-unstable-v1 server-decoration
GEN = $(foreach p,$(PROTOS),gen/$(p)-server.h gen/$(p).c)
all: axwl axwl-test
gen/%-server.h: protocol/%.xml
	@mkdir -p gen; wayland-scanner server-header $< $@
gen/%.c: protocol/%.xml
	@mkdir -p gen; wayland-scanner private-code $< $@
gen/keymap.h: keymap_us.xkb
	@mkdir -p gen; python3 -c "import sys; d=open('$<','rb').read(); print('static const char keymap_us[] = {' + ','.join(str(b) for b in d) + ',0};')" > $@
SRC = axwl.c $(foreach p,$(PROTOS),gen/$(p).c)
axwl: $(SRC) backend_axiom.c $(GEN) gen/keymap.h
	$(CC) $(CFLAGS) -static-pie -o $@ $(SRC) backend_axiom.c $(WL)/libwayland-server.a /usr/lib/x86_64-linux-gnu/libffi.a
axwl-test: $(SRC) backend_test.c $(GEN) gen/keymap.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(SRC) backend_test.c $(WL)/libwayland-server.a /usr/lib/x86_64-linux-gnu/libffi.a
clean:
	rm -rf gen axwl axwl-test
