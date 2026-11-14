CC      := gcc
LD      := ld
NASM    := nasm
COMMON  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only -m64 -nostdlib -fno-tree-loop-distribute-patterns
KCFLAGS := $(COMMON) -mno-red-zone -mcmodel=small -Icommon
UCFLAGS := $(COMMON) -mcmodel=large
LDFLAGS := -n -T src/linker.ld -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -z noexecstack
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

APPS    := about terminal monitor bluetooth calc sandbox
K_SRCS  := $(filter-out src/desktop_highland.c,$(wildcard src/*.c)) common/gfx.c common/str.c
K_OBJS  := $(patsubst %.c,build/k/%.o,$(K_SRCS)) build/k/boot.o build/k/isr.o build/k/apps.o
U_LIB   := build/u/crt0.o build/u/libdunc.o build/u/gfx.o build/u/str.o
U_ELFS  := $(patsubst %,build/user/%.elf,$(APPS))
HDRS    := $(wildcard src/*.h common/*.h user/lib/*.h)

.PHONY: all run run-uefi clean
all: duncos.iso

build/k/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@
build/k/boot.o: src/boot.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
build/k/isr.o: src/isr.asm
	$(NASM) -f elf64 $< -o $@
build/k/apps.o: src/apps.asm $(U_ELFS)
	$(NASM) -f elf64 $< -o $@

build/u/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
build/u/libdunc.o: user/lib/libdunc.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/%.o: common/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/u/app_%.o: user/apps/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
build/user/%.elf: build/u/app_%.o $(U_LIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments -o $@ $(U_LIB) $< $(LIBGCC)

build/thane.elf: $(K_OBJS) src/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(K_OBJS)

duncos.iso: build/thane.elf iso/boot/grub/grub.cfg
	cp build/thane.elf iso/boot/thane.elf
	grub-mkrescue -o $@ iso 2>/dev/null

QEMU := qemu-system-x86_64 -m 1G -device qemu-xhci -device usb-kbd -device usb-mouse -serial stdio
run: duncos.iso
	$(QEMU) -cdrom $<
run-uefi: duncos.iso
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $<

clean:
	rm -rf build iso/boot/thane.elf duncos.iso
