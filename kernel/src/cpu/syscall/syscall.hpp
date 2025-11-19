#pragma once

#include <klib/common.hpp>

#define SYSCALL_TRACE 0
#define UNIMPLEMENTED_SYSCALL_TRACE 0

#if SYSCALL_TRACE
#include <klib/cstdio.hpp>
#define log_syscall(format, ...) cpu::syscall::log_syscall_impl(format __VA_OPT__(,) __VA_ARGS__)
#else
#define log_syscall(format, ...)
#endif

namespace cpu::syscall {
    struct [[gnu::packed]] SyscallState {
        u64 ds, es;
        u64 r15, r14, r13, r12, r11, r10, r9, r8;
        u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    };

    void init_syscall_table();
    [[gnu::format(printf, 1, 2)]] int log_syscall_impl(const char *format, ...);
}
