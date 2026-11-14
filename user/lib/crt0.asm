; AXIOM program entry: call main(), then exit with its return value.
bits 64
section .text
global _start
extern main
_start:
    and rsp, -16
    call main
    mov rdi, rax
    xor eax, eax            ; SYS_EXIT
    int 0x80
.h: jmp .h
