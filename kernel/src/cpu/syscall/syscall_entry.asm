section .text

extern __syscall_handler
global __syscall_entry
__syscall_entry:
    swapgs

    mov gs:[24], rsp ; save user stack
    mov rsp, gs:[16] ; switch to kernel stack

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

    mov eax, es
    push rax
    mov eax, ds
    push rax

    cld
    mov eax, 0x10 ; kernel data segment
    mov ds, eax
    mov es, eax

    xor rbp, rbp
    mov rdi, rsp

    call __syscall_handler

    pop rax
    mov ds, eax
    pop rax
    mov es, eax

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

    mov rsp, gs:[24] ; restore user stack

    swapgs
    o64 sysret
