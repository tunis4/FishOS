#include <sched/context.hpp>

namespace sched {
    void from_ucontext(cpu::InterruptState *gpr_state, const ucontext_t *ucontext) {
        const mcontext_t *mcontext = &ucontext->uc_mcontext;
        gpr_state->r15 = mcontext->gregs[REG_R15];
        gpr_state->r14 = mcontext->gregs[REG_R14];
        gpr_state->r13 = mcontext->gregs[REG_R13];
        gpr_state->r12 = mcontext->gregs[REG_R12];
        gpr_state->r11 = mcontext->gregs[REG_R11];
        gpr_state->r10 = mcontext->gregs[REG_R10];
        gpr_state->r9  = mcontext->gregs[REG_R9];
        gpr_state->r8  = mcontext->gregs[REG_R8];
        gpr_state->rbp = mcontext->gregs[REG_RBP];
        gpr_state->rdi = mcontext->gregs[REG_RDI];
        gpr_state->rsi = mcontext->gregs[REG_RSI];
        gpr_state->rdx = mcontext->gregs[REG_RDX];
        gpr_state->rcx = mcontext->gregs[REG_RCX];
        gpr_state->rbx = mcontext->gregs[REG_RBX];
        gpr_state->rax = mcontext->gregs[REG_RAX];
        gpr_state->rip = mcontext->gregs[REG_RIP];
        gpr_state->rsp = mcontext->gregs[REG_RSP];
        gpr_state->rflags = mcontext->gregs[REG_EFL];
    }

    void to_ucontext(const cpu::InterruptState *gpr_state, ucontext_t *ucontext) {
        mcontext_t *mcontext = &ucontext->uc_mcontext;
        mcontext->gregs[REG_R15] = gpr_state->r15;
        mcontext->gregs[REG_R14] = gpr_state->r14;
        mcontext->gregs[REG_R13] = gpr_state->r13;
        mcontext->gregs[REG_R12] = gpr_state->r12;
        mcontext->gregs[REG_R11] = gpr_state->r11;
        mcontext->gregs[REG_R10] = gpr_state->r10;
        mcontext->gregs[REG_R9] = gpr_state->r9;
        mcontext->gregs[REG_R8] = gpr_state->r8;
        mcontext->gregs[REG_RBP] = gpr_state->rbp;
        mcontext->gregs[REG_RDI] = gpr_state->rdi;
        mcontext->gregs[REG_RSI] = gpr_state->rsi;
        mcontext->gregs[REG_RDX] = gpr_state->rdx;
        mcontext->gregs[REG_RCX] = gpr_state->rcx;
        mcontext->gregs[REG_RBX] = gpr_state->rbx;
        mcontext->gregs[REG_RAX] = gpr_state->rax;
        mcontext->gregs[REG_RIP] = gpr_state->rip;
        mcontext->gregs[REG_RSP] = gpr_state->rsp;
        mcontext->gregs[REG_EFL] = gpr_state->rflags;
    }
}
