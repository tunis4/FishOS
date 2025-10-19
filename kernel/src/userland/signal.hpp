#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <signal.h>

namespace userland {
    enum class SignalDefault {
        IGNORE, TERMINATE, CORE, CONTINUE, STOP
    };
    SignalDefault signal_default_action(int signal);

    struct SignalAction {
        void *handler;
        usize flags;
        u64 mask;

        void from_sigaction(const struct sigaction *act) {
            handler = (void*)act->sa_handler;
            flags = act->sa_flags;
            mask = *(u64*)&act->sa_mask;
        }

        void to_sigaction(struct sigaction *act) {
            act->sa_handler = (void(*)(int))handler;
            act->sa_flags = flags;
            *(u64*)&act->sa_mask = mask;
        }
    };

    void dispatch_pending_signal(sched::Thread *thread);
    void return_from_signal(sched::Thread *thread);

    isize syscall_sigentry(uptr entry);
    isize syscall_sigreturn(ucontext_t *ucontext, u64 saved_mask);
    isize syscall_sigmask(int how, const u64 *set, u64 *retrieve);
    isize syscall_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
    isize syscall_kill(pid_t pid, int signal);
    isize syscall_sigaltstack(const stack_t *new_signal_stack, stack_t *old_signal_stack);
}
