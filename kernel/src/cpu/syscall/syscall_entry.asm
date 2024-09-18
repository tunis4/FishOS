section .text

SYSCALL_TABLE_SIZE equ 46
SYSCALL_FORK equ 13
SYSCALL_EXECVE equ 14
ENOSYS equ 1051

extern __syscall_table
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

    mov rcx, r10 ; to retrieve function arguments properly
    mov rax, [rsp + 16 * 8] ; retrieve the original value of rax
    cmp rax, SYSCALL_TABLE_SIZE ; check if rax is a valid syscall table index
    jae .out_of_bounds
    
    cmp rax, SYSCALL_FORK ; check if the syscall is fork
    je .is_fork
    cmp rax, SYSCALL_EXECVE ; check if the syscall is not execve
    jne .is_not_execve
.is_fork:
    mov rcx, rdx ; |
    mov rdx, rsi ; | shift arguments to the right for execve
    mov rsi, rdi ; |
    mov rdi, rsp ; fork and execve need to know the program state
.is_not_execve:
    sti
    call [__syscall_table + rax * 8]
    cli
    jmp .end

.out_of_bounds:
    mov rax, -ENOSYS

.end:
    mov [rsp + 16 * 8], rax ; set the new value of rax

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
