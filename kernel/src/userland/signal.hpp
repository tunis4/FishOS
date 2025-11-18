#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <signal.h>

namespace userland {
    enum class SignalDefault {
        IGNORE, TERMINATE, CORE, CONTINUE, STOP
    };
    SignalDefault signal_default_action(int signal);

    struct KernelSigaction {
        void (*handler)(int);
        u64 flags;
        void (*restorer)();
        u64 mask;
    };

    void dispatch_pending_signal(sched::Thread *thread);
    void return_from_signal(sched::Thread *thread);

    inline u64 get_signal_bit(int signal) { return (u64)1 << (signal - 1); }

    isize syscall_rt_sigreturn();
    isize syscall_rt_sigprocmask(int how, const u64 *set, u64 *retrieve);
    isize syscall_rt_sigaction(int signum, const KernelSigaction *act, KernelSigaction *oldact);
    isize syscall_sigaltstack(const stack_t *new_signal_stack, stack_t *old_signal_stack);
    isize syscall_kill(pid_t pid, int signal);
    isize syscall_tgkill(pid_t pid, pid_t tid, int signal);
}
