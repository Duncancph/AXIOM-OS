; Start-up code for the other cores, copied to 0x8000. A core starts here in 16-bit real mode after
; the start-up IPI; it goes through 32-bit protected mode into 64-bit long mode, using the kernel's page
; tables, then jumps into the kernel (ap_main) on the stack the first core prepared for it.
bits 16
org 0x8000
start:
    jmp short real16
align 8
t_cr3:   dq 0           ; +8  page tables
t_stack: dq 0           ; +16 stack top
t_entry: dq 0           ; +24 C entry
t_efer:  dq 0           ; +32 EFER (long mode, NX...)
t_cr4:   dq 0           ; +40
t_cr0:   dq 0           ; +48
t_mark:  dq 0           ; +56 how far a starting core got (for diagnosis)
real16:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov byte [t_mark], 1
    o32 lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:prot32
bits 32
prot32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov byte [t_mark], 2
    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax
    mov eax, [t_cr3]
    mov cr3, eax
    mov ecx, 0xC0000080
    mov eax, [t_efer]
    mov edx, [t_efer + 4]
    wrmsr
    mov eax, cr0
    or eax, 0x80000001
    and eax, ~0x60000000                ; caches on
    mov cr0, eax
    jmp 0x18:long64
bits 64
long64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov byte [t_mark], 3
    mov rax, [t_cr4]
    mov cr4, rax
    mov rax, [t_cr0]
    mov cr0, rax
    mov rsp, [t_stack]
    mov rax, [t_entry]
    call rax
.h: cli
    hlt
    jmp .h
align 16
gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10 data
    dq 0x00AF9A000000FFFF               ; 0x18 64-bit code
gdt_ptr:
    dw gdt_ptr - gdt - 1
    dd gdt
