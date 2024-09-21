#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <signal.h>

namespace sched {
    void from_ucontext(cpu::InterruptState *gpr_state, const ucontext_t *ucontext);
    void to_ucontext(const cpu::InterruptState *gpr_state, ucontext_t *ucontext);
}
