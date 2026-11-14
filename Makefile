CC      := gcc
LD      := ld
NASM    := nasm
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -m64 -nostdlib
LDFLAGS := -n -T src/linker.ld -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -z noexecstack

C_SRCS  := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,build/%.o,$(C_SRCS)) build/boot.o build/isr.o

.PHONY: all iso run run-uefi clean
all: emberos.iso

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: src/%.asm | build
	$(NASM) -f elf64 $< -o $@
build:
	mkdir -p build

build/ember.elf: $(OBJS) src/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

emberos.iso: build/ember.elf iso/boot/grub/grub.cfg
	cp build/ember.elf iso/boot/ember.elf
	grub-mkrescue -o $@ iso 2>/dev/null

QEMU := qemu-system-x86_64 -m 512M -device qemu-xhci -device usb-kbd -serial stdio
run: emberos.iso
	$(QEMU) -cdrom $<
run-uefi: emberos.iso
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $<

clean:
	rm -rf build iso/boot/ember.elf emberos.iso
