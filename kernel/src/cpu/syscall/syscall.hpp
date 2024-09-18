#pragma once

#include <klib/common.hpp>

#define SYSCALL_TRACE 0

namespace cpu::syscall {
    struct [[gnu::packed]] SyscallState {
        u64 ds, es;
        u64 r15, r14, r13, r12, r11, r10, r9, r8;
        u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    };

    void init_syscall_table();
}
