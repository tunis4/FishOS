#include <sched/context.hpp>
#include <klib/cstring.hpp>

namespace sched {
    void from_ucontext(cpu::InterruptState *gpr_state, void *extended_state, const KernelUContext *ucontext) {
        const sigcontext *mcontext = &ucontext->uc_mcontext;
        gpr_state->r15 = mcontext->r15;
        gpr_state->r14 = mcontext->r14;
        gpr_state->r13 = mcontext->r13;
        gpr_state->r12 = mcontext->r12;
        gpr_state->r11 = mcontext->r11;
        gpr_state->r10 = mcontext->r10;
        gpr_state->r9  = mcontext->r9;
        gpr_state->r8  = mcontext->r8;
        gpr_state->rbp = mcontext->rbp;
        gpr_state->rdi = mcontext->rdi;
        gpr_state->rsi = mcontext->rsi;
        gpr_state->rdx = mcontext->rdx;
        gpr_state->rcx = mcontext->rcx;
        gpr_state->rbx = mcontext->rbx;
        gpr_state->rax = mcontext->rax;
        gpr_state->rip = mcontext->rip;
        gpr_state->rsp = mcontext->rsp;
        gpr_state->rflags = mcontext->eflags;
        memcpy(extended_state, mcontext->fpstate, cpu::extended_state_size);
    }

    void to_ucontext(const cpu::InterruptState *gpr_state, void *extended_state, KernelUContext *ucontext) {
        sigcontext *mcontext = &ucontext->uc_mcontext;
        mcontext->r15 = gpr_state->r15;
        mcontext->r14 = gpr_state->r14;
        mcontext->r13 = gpr_state->r13;
        mcontext->r12 = gpr_state->r12;
        mcontext->r11 = gpr_state->r11;
        mcontext->r10 = gpr_state->r10;
        mcontext->r9 = gpr_state->r9;
        mcontext->r8 = gpr_state->r8;
        mcontext->rbp = gpr_state->rbp;
        mcontext->rdi = gpr_state->rdi;
        mcontext->rsi = gpr_state->rsi;
        mcontext->rdx = gpr_state->rdx;
        mcontext->rcx = gpr_state->rcx;
        mcontext->rbx = gpr_state->rbx;
        mcontext->rax = gpr_state->rax;
        mcontext->rip = gpr_state->rip;
        mcontext->rsp = gpr_state->rsp;
        mcontext->eflags = gpr_state->rflags;
        memcpy(mcontext->fpstate, extended_state, cpu::extended_state_size);
    }
}
