; EmberOS boot entry. GRUB (multiboot2) drops us here in 32-bit protected mode,
; on both legacy BIOS and UEFI machines. We set up paging, enter 64-bit long mode
; and call kmain(magic, multiboot_info).

MB2_MAGIC equ 0xE85250D6

section .multiboot2
align 8
mb2_start:
    dd MB2_MAGIC
    dd 0                                ; architecture: i386
    dd mb2_end - mb2_start
    dd -(MB2_MAGIC + (mb2_end - mb2_start))
align 8
    ; Ask for a linear graphics framebuffer (needed on UEFI, no VGA text there).
    dw 5, 1                             ; type=framebuffer, flags=optional
    dd 20
    dd 1024, 768, 32
align 8
    dw 0, 0                             ; end tag
    dd 8
mb2_end:

section .data
global mb_magic, mb_info
mb_magic: dd 0
mb_info:  dd 0

section .rodata
align 16
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF               ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF               ; 0x10: data
gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1
    dq gdt64

section .bss
align 4096
global boot_pml4
boot_pml4:  resb 4096
boot_pdpt:  resb 4096
boot_pd:    resb 4096 * 4               ; identity-map first 4 GiB with 2 MiB pages
stack_bottom: resb 65536
stack_top:

section .text
bits 32
global _start
extern kmain
_start:
    cli
    cld
    mov esp, stack_top
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; Require a 64-bit CPU.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; PML4[0] -> PDPT
    mov eax, boot_pdpt
    or eax, 3
    mov [boot_pml4], eax
    ; PDPT[0..3] -> 4 page directories
    xor ecx, ecx
.pdpt_loop:
    mov eax, ecx
    shl eax, 12
    add eax, boot_pd
    or eax, 3
    mov [boot_pdpt + ecx*8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt_loop
    ; 2048 x 2 MiB pages
    xor ecx, ecx
.pd_loop:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                        ; present | writable | huge
    mov [boot_pd + ecx*8], eax
    mov dword [boot_pd + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jne .pd_loop

    mov eax, boot_pml4
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; long mode enable
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | 1               ; paging + protected mode
    and eax, ~((1 << 30) | (1 << 29))   ; make sure caches are enabled
    mov cr0, eax
    lgdt [gdt64_ptr]
    jmp 0x08:long_mode

.no_long_mode:
    ; Legacy VGA text (only visible on BIOS machines)
    mov dword [0xB8000], 0x4F6E4F4E     ; "No"
    mov dword [0xB8004], 0x4F344F20     ; " 4"
    mov dword [0xB8008], 0x4F2D4F36     ; "6-"
    mov dword [0xB800C], 0x4F694F62     ; "bi"
    mov dword [0xB8010], 0x4F204F74     ; "t "
.hang32:
    hlt
    jmp .hang32

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    mov edi, [mb_magic]
    mov esi, [mb_info]
    call kmain
.hang:
    cli
    hlt
    jmp .hang
