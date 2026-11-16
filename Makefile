CC      := gcc
LD      := ld
NASM    := nasm
COMMON  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-type-limits -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only -m64 -nostdlib -fno-tree-loop-distribute-patterns
KCFLAGS := $(COMMON) -fno-omit-frame-pointer -mno-red-zone -mcmodel=small -Icommon
UCFLAGS := $(COMMON) -mcmodel=large
LDFLAGS := -n -T src/linker.ld -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -z noexecstack
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

APPS    := files about terminal monitor bluetooth network calc sandbox settings viewer web notes calendar music video
REPO_APPS := editor clock
K_SRCS  := $(filter-out src/desktop_highland.c,$(wildcard src/*.c)) common/gfx.c common/str.c common/inet.c user/lib/crypto/sha2.c user/lib/crypto/aead.c user/lib/crypto/wpa.c common/wifi_sta.c user/lib/crypto/bn.c user/lib/crypto/ecc.c user/lib/img/pngenc.c user/lib/img/inflate.c
K_OBJS  := $(patsubst %.c,build/k/%.o,$(K_SRCS)) build/k/boot.o build/k/isr.o build/k/apps.o build/k/ap_blob.o
U_LIB   := build/u/crt0.o build/u/libdunc.o build/u/malloc.o build/u/gfx.o build/u/str.o build/u/inet.o build/u/json.o
CRYPTO  := sha2 aead x25519 bn rsa ecc x509 tls
NETLIB  := build/u/libnet.a
IMGLIB  := build/u/libimg.a
IMGCODECS := img png jpeg gif bmp inflate pngenc
WEB_SRCS := $(wildcard user/apps/web/*.c)
WEB_OBJS := $(patsubst user/apps/web/%.c,build/u/web_%.o,$(WEB_SRCS))
JS_SRCS  := $(wildcard user/apps/web/js/*.c)
JS_OBJS  := $(patsubst user/apps/web/js/%.c,build/u/js_%.o,$(JS_SRCS))
WEB_ENGINE := user/apps/web/html.c user/apps/web/css.c user/apps/web/layout.c
U_ELFS  := $(patsubst %,build/user/%.elf,$(APPS))
HDRS    := $(wildcard src/*.h common/*.h user/lib/*.h)

.PHONY: all run run-uefi clean test
all: duncos.iso

# build number = number of commits; updates must have a higher one than the running system
BUILD_NUM := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
$(shell mkdir -p build; [ "`cat build/build_num 2>/dev/null`" = "$(BUILD_NUM)" ] || echo $(BUILD_NUM) > build/build_num)
K_BUILD_OBJS := build/k/src/main.o build/k/src/update.o build/k/src/syscall.o build/k/src/shell.o build/k/src/sched.o build/k/src/idt.o
$(K_BUILD_OBJS): build/build_num
$(K_BUILD_OBJS): KCFLAGS += -DOS_BUILD=$(BUILD_NUM)


build/k/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@
build/k/boot.o: src/boot.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
build/k/isr.o: src/isr.asm
	$(NASM) -f elf64 $< -o $@
build/k/ap_tramp.bin: src/ap_tramp.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@
build/k/ap_blob.o: src/ap_blob.asm build/k/ap_tramp.bin
	$(NASM) -f elf64 $< -o $@
build/k/apps.o: src/apps.asm $(U_ELFS)
	$(NASM) -f elf64 $< -o $@

build/u/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
build/u/libdunc.o: user/lib/libdunc.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/json.o: user/lib/json.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/malloc.o: user/lib/malloc.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/http.o: user/lib/http.c $(HDRS) $(wildcard user/lib/crypto/*.h) user/lib/img/img.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/c_%.o: user/lib/crypto/%.c $(wildcard user/lib/crypto/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(NETLIB): build/u/http.o $(patsubst %,build/u/c_%.o,$(CRYPTO))
	rm -f $@ && ar rcs $@ $^
build/u/i_%.o: user/lib/img/%.c $(wildcard user/lib/img/*.h) common/str.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(IMGLIB): $(patsubst %,build/u/i_%.o,$(IMGCODECS))
	rm -f $@ && ar rcs $@ $^
build/u/web_%.o: user/apps/web/%.c user/apps/web/web.h common/webfont_data.h $(HDRS) user/lib/img/img.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/%.o: common/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
# code that uses floating point (the JavaScript engine); the kernel saves each app's SSE registers
UCFLAGS_FP := $(filter-out -mgeneral-regs-only,$(UCFLAGS)) -msse2 -mfpmath=sse
build/u/app_%.o: user/apps/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/user/%.elf: build/u/app_%.o $(U_LIB) $(NETLIB) $(IMGLIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -o $@ $(U_LIB) $< $(NETLIB) $(IMGLIB) $(LIBGCC)
# Web is built from several files (user/apps/web.c and user/apps/web/*.c)
# the JavaScript engine and its bridge to the page use floating point
build/u/js_%.o: user/apps/web/js/%.c $(wildcard user/apps/web/js/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
# the MP3 decoder (music) uses floating point too
AUDIO_OBJS := $(patsubst user/lib/audio/%.c,build/u/aud_%.o,$(wildcard user/lib/audio/*.c))
build/u/aud_%.o: user/lib/audio/%.c $(wildcard user/lib/audio/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
build/user/music.elf: build/u/app_music.o $(AUDIO_OBJS) $(U_LIB) $(NETLIB) $(IMGLIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -o $@ $(U_LIB) $< $(AUDIO_OBJS) $(NETLIB) $(IMGLIB) $(LIBGCC)
# video: the H.264 decoder, the MP4 reader and the AAC decoder (floating point too)
VIDEO_OBJS := $(patsubst user/lib/video/%.c,build/u/vid_%.o,$(wildcard user/lib/video/*.c)) $(patsubst user/lib/media/%.c,build/u/med_%.o,$(wildcard user/lib/media/*.c))
build/u/vid_%.o: user/lib/video/%.c $(wildcard user/lib/video/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
build/u/med_%.o: user/lib/media/%.c $(wildcard user/lib/media/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
build/u/app_video.o: user/apps/video.c $(wildcard user/lib/video/*.h user/lib/media/*.h) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
build/user/video.elf: build/u/app_video.o $(VIDEO_OBJS) $(U_LIB) $(NETLIB) $(IMGLIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -o $@ $(U_LIB) $< $(VIDEO_OBJS) $(NETLIB) $(IMGLIB) $(LIBGCC)
build/gen/dom_prelude.h: user/apps/web/dom_prelude.js tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ dom_prelude
build/u/web_dom.o: user/apps/web/dom.c build/gen/dom_prelude.h user/apps/web/web.h user/apps/web/js/js.h $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -Ibuild/gen -c $< -o $@
# <canvas> drawing too
build/u/web_canvas.o: user/apps/web/canvas.c user/apps/web/web.h $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
# SVG drawing uses floating point
build/u/web_svg.o: user/apps/web/svg.c user/apps/web/web.h $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
# <video>/<audio>: the media player part of Web uses the decoders (floating point)
build/u/web_media.o: user/apps/web/media.c user/apps/web/web.h $(wildcard user/lib/video/*.h user/lib/media/*.h user/lib/audio/*.h) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_FP) -c $< -o $@
build/user/web.elf: build/u/app_web.o $(WEB_OBJS) $(JS_OBJS) $(VIDEO_OBJS) $(AUDIO_OBJS) $(U_LIB) $(NETLIB) $(IMGLIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -o $@ $(U_LIB) $< $(WEB_OBJS) $(JS_OBJS) $(VIDEO_OBJS) $(AUDIO_OBJS) $(NETLIB) $(IMGLIB) $(LIBGCC)
build/u/app_web.o: user/apps/web/web.h

build/thane.elf: $(K_OBJS) src/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(K_OBJS)

# Initial files: home folder, the OS source in ~/Projects/thane, and the clan package repository
REPO_PKGS := $(patsubst %,build/user/%.elf,$(REPO_APPS))
CA_BUNDLE := /etc/ssl/certs/ca-certificates.crt
build/initrd.tar: $(shell find rootfs -type f) $(REPO_PKGS) repo/index.txt $(K_SRCS) README.md
	rm -rf build/rootfs && mkdir -p build/rootfs && cp -r rootfs/. build/rootfs/
	mkdir -p build/rootfs/home/Projects/thane/kernel build/rootfs/home/Projects/thane/drivers \
	         build/rootfs/home/Projects/thane/boot build/rootfs/home/Projects/thane/fonts build/rootfs/home/Projects/thane/docs \
	         build/rootfs/system/repo
	cp src/sched.c src/vm.c src/syscall.c src/idt.c src/cpu.c src/isr.asm src/mem.c src/kmalloc.c src/vfs.c src/apps.c \
	   src/desktop.c src/highland.c src/desktop_highland.c src/gfx.h build/rootfs/home/Projects/thane/kernel/
	cp $(wildcard src/xhci.c src/usb*.c src/bt.c src/kbd.c src/pci.c src/acpi.c src/rtc.c src/fat.c) build/rootfs/home/Projects/thane/drivers/
	cp src/boot.asm src/fb_boot_logo.c src/linker.ld iso/boot/grub/grub.cfg build/rootfs/home/Projects/thane/boot/
	cp src/font8x16.h tools/genunifont.py common/theme_highland.h build/rootfs/home/Projects/thane/fonts/
	cp README.md build/rootfs/home/Projects/thane/docs/
	cp repo/index.txt $(REPO_PKGS) build/rootfs/system/repo/ 2>/dev/null || true
	mkdir -p build/rootfs/system/certs/user && cp $(CA_BUNDLE) build/rootfs/system/certs/ca-bundle.crt
	tar --format=ustar --owner=0 --group=0 -cf $@ -C build/rootfs .

# The boot loader the installer puts on disks: GRUB with its modules built in, finds /duncos on any disk
GRUB_MODS := part_gpt part_msdos fat multiboot2 normal search search_fs_file configfile all_video gfxterm echo sleep test videoinfo
build/bootx64.efi: tools/grub-embed.cfg Makefile
	@mkdir -p build
	grub-mkstandalone -O x86_64-efi -o $@ --modules="$(GRUB_MODS)" --install-modules="$(GRUB_MODS)" \
	    --locales= --fonts= --themes= "boot/grub/grub.cfg=tools/grub-embed.cfg"

# ... and for PCs started in legacy BIOS mode: GRUB's MBR code (boot.img) followed by its core image, which
# the installer writes into the gap between the partition table and the first partition
build/biosboot.img: tools/grub-embed.cfg Makefile
	@mkdir -p build
	grub-mkimage -O i386-pc -o build/core.img -p /duncos -c tools/grub-embed.cfg biosdisk $(GRUB_MODS)
	cat /usr/lib/grub/i386-pc/boot.img build/core.img > $@

duncos.iso: build/thane.elf build/initrd.tar build/bootx64.efi build/biosboot.img iso/boot/grub/grub.cfg
	cp build/biosboot.img iso/boot/biosboot.img
	cp build/thane.elf iso/boot/thane.elf
	cp build/initrd.tar iso/boot/initrd.tar
	cp build/bootx64.efi iso/boot/bootx64.efi
	grub-mkrescue -o $@ iso 2>/dev/null

QEMU := qemu-system-x86_64 -m 1G -device qemu-xhci -device usb-kbd -device usb-mouse -serial stdio
run: duncos.iso
	$(QEMU) -cdrom $<
run-uefi: duncos.iso
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $<

clean:
	rm -rf build iso/boot/thane.elf iso/boot/initrd.tar iso/boot/bootx64.efi iso/boot/biosboot.img duncos.iso

test:
	@mkdir -p build/test
	gcc -std=gnu17 -O1 -w -fno-builtin -DBT_TEST -DHOST_TEST -o build/test/test_bt tests/test_bt.c src/bt.c src/sbc.c common/str.c -lm && build/test/test_bt | tail -8
	python3 tests/test_bt_audio.py
	gcc -O2 -o build/test/test_sbc tests/test_sbc.c src/sbc.c -lm && python3 tests/test_sbc.py build/test/test_sbc
	@if command -v ffmpeg >/dev/null && python3 -c 'import numpy' 2>/dev/null; then python3 tests/mp3/run.py --quick > build/test/mp3.log 2>&1; r=$$?; tail -3 build/test/mp3.log; [ $$r = 0 ] || exit 1; else echo 'mp3: skipped (needs ffmpeg and numpy)'; fi
	@if command -v ffmpeg >/dev/null && python3 -c 'import numpy' 2>/dev/null; then python3 tests/h264/run.py --quick > build/test/h264.log 2>&1; r=$$?; tail -3 build/test/h264.log; [ $$r = 0 ] || exit 1; else echo 'h264: skipped (needs ffmpeg and numpy)'; fi
	@if command -v ffmpeg >/dev/null && python3 -c 'import numpy' 2>/dev/null; then python3 tests/media/run.py --quick > build/test/media.log 2>&1; r=$$?; tail -3 build/test/media.log; [ $$r = 0 ] || exit 1; else echo 'mp4/aac: skipped (needs ffmpeg and numpy)'; fi
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Isrc -o build/test/test_fat tests/test_fat.c src/fat.c common/str.c
	rm -f build/test/fat.img && truncate -s 64M build/test/fat.img && mkfs.fat -F 32 -n DUNCTEST build/test/fat.img >/dev/null
	printf 'written by mtools on Linux\n' > build/test/hello.txt && MTOOLS_SKIP_CHECK=1 mcopy -i build/test/fat.img build/test/hello.txt "::/Hello From Linux.txt"
	build/test/test_fat build/test/fat.img
	fsck.fat -n build/test/fat.img && echo "fsck.fat: clean"
	rm -f build/test/fmt.img && truncate -s 100M build/test/fmt.img && build/test/test_fat build/test/fmt.img --format
	fsck.fat -n build/test/fmt.img && echo "fsck.fat after fat_format: clean"
	gcc -std=gnu17 -O1 -w -fno-builtin -o build/test/test_inet tests/test_inet.c common/inet.c common/str.c
	python3 tests/test_inet.py build/test/test_inet
	gcc -O1 -g -fsanitize=address,undefined -DJS_HOST_TEST -o build/test/jsrun tests/js/jsrun.c user/apps/web/js/*.c && python3 tests/js/run.py --no-perf | tail -1
	gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -DJSON_TEST -o build/test/test_json tests/test_json.c user/lib/json.c && python3 tests/test_json.py build/test/test_json
	gcc -std=gnu17 -O1 -DTZ_TEST -o build/test/test_tz tests/test_tz.c src/tz.c && python3 tests/test_tz.py build/test/test_tz
	gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -DMALLOC_TEST -o build/test/test_malloc tests/test_malloc.c user/lib/malloc.c && build/test/test_malloc
	gcc -std=gnu17 -O2 -w -fno-builtin -o build/test/test_crypto tests/test_crypto.c $(addprefix user/lib/crypto/,sha2.c aead.c x25519.c bn.c rsa.c ecc.c) common/str.c
	python3 tests/test_crypto.py build/test/test_crypto
	gcc -std=gnu17 -O1 -g -w -fno-builtin -fsanitize=address,undefined -o build/test/test_wpa tests/test_wpa.c $(addprefix user/lib/crypto/,wpa.c sha2.c aead.c) common/str.c
	python3 tests/test_wpa.py build/test/test_wpa | tail -1
	gcc -std=gnu17 -O1 -g -w -fno-builtin -fsanitize=address,undefined -o build/test/test_wifi_sta tests/test_wifi_sta.c common/wifi_sta.c $(addprefix user/lib/crypto/,wpa.c sha2.c aead.c) common/str.c
	python3 tests/test_wifi_sta.py build/test/test_wifi_sta | tail -1
	gcc -std=gnu17 -O2 -w -DX509_TESTING -fno-builtin -o build/test/test_roots tests/test_roots.c $(addprefix user/lib/crypto/,x509.c sha2.c bn.c rsa.c ecc.c) common/str.c common/inet.c
	build/test/test_roots $(CA_BUNDLE) | tail -3
	gcc -std=gnu17 -O2 -w -D_GNU_SOURCE -fno-builtin -o build/test/test_tls tests/test_tls.c $(addprefix user/lib/crypto/,tls.c x509.c sha2.c aead.c x25519.c bn.c rsa.c ecc.c) common/str.c common/inet.c
	tests/tls/matrix.sh build/test/test_tls build/test/tls
	gcc -std=gnu17 -O1 -g -w -fno-builtin -fsanitize=address,undefined -o build/test/test_web tests/test_web.c $(WEB_ENGINE) common/str.c
	build/test/test_web tests/web/*.html | tail -4
	gcc -std=gnu17 -O2 -w -fno-builtin -mgeneral-regs-only -o build/test/test_web_stack tests/test_web.c $(WEB_ENGINE) common/str.c
	sh -c 'ulimit -s 160 && build/test/test_web_stack tests/web/*.html' | tail -1
