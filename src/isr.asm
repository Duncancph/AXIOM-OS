; Interrupt/exception entry stubs (all 256 vectors), context switching and task entry trampolines.
bits 64
section .text
extern isr_dispatch
extern task_exit

%macro STUB 1
isr%1:
%if (%1 == 8) || (%1 >= 10 && %1 <= 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
    push %1
%else
    push 0
    push %1
%endif
    jmp isr_common
%endmacro

%assign i 0
%rep 256
STUB i
%assign i i+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call isr_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; void switch_to(uint64_t *save_rsp, uint64_t new_rsp)
global switch_to
switch_to:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; First run of a kernel thread: rbx = function, r12 = argument
global thread_entry
thread_entry:
    mov rdi, r12
    call rbx
    xor edi, edi
    call task_exit
.h: hlt
    jmp .h

; First run of a user process: the stack holds an iretq frame into ring 3
global user_entry
user_entry:
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    iretq

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr %+ i
%assign i i+1
%endrep
