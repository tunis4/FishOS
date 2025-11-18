#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <signal.h>

struct KernelUContext {
    u64 uc_flags;
    KernelUContext *uc_link;
    stack_t uc_stack;
    sigcontext uc_mcontext;
    u64 uc_sigmask;
};

struct SignalFrame {
    void *restorer;
    KernelUContext ucontext;
    siginfo_t siginfo;
    // fp context
};

namespace sched {
    void from_ucontext(cpu::InterruptState *gpr_state, void *extended_state, const KernelUContext *ucontext);
    void to_ucontext(const cpu::InterruptState *gpr_state, void *extended_state, KernelUContext *ucontext);
}
