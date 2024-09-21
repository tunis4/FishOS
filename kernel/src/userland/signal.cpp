#include <userland/signal.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <sched/context.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstdio.hpp>
#include <errno.h>

namespace userland {
    void dispatch_pending_signal(sched::Thread *thread) {
        thread->entering_signal = false;
        u64 accepted_signals = thread->pending_signals & ~thread->signal_mask;
        if (thread->signal_entry == 0 || accepted_signals == 0)
            return;

        int signal = -1;
        for (int i = 0; i < 64; i++) {
            if ((accepted_signals >> i) & 1) {
                signal = i;
                break;
            }
        }
        ASSERT(signal != -1);
        thread->pending_signals &= ~(1 << signal);

        auto *action = &thread->process->signal_actions[signal];
        auto *state = &thread->gpr_state;

        u64 old_rsp = state->rsp;
        state->rsp -= 128; // respect red zone

        state->rsp -= sizeof(ucontext_t);
        ucontext_t *ucontext = (ucontext_t*)state->rsp;
        sched::to_ucontext(state, ucontext);
        ucontext->uc_mcontext.gregs[REG_RSP] = old_rsp;

        state->rsp = klib::align_down<uptr, 16>(state->rsp) - 8; // stack alignment

        uptr handler = (uptr)action->handler;
        if (action->flags & SA_SIGINFO)
            handler |= (uptr)1 << 63;

        state->rip = thread->signal_entry;
        state->rdi = signal;
        state->rsi = 0;
        state->rdx = handler;
        state->rcx = thread->signal_mask;
        state->r8 = (uptr)ucontext;

        thread->signal_mask |= action->mask;
        if (!(action->flags & SA_NODEFER))
            thread->signal_mask |= 1 << signal;
    }

    void return_from_signal(sched::Thread *thread) {
        thread->exiting_signal = false;
        sched::from_ucontext(&thread->gpr_state, thread->signal_ucontext);
        thread->signal_mask = thread->saved_signal_mask;
        thread->signal_ucontext = nullptr;
        thread->saved_signal_mask = 0;
    }

    isize syscall_sigentry(uptr entry) {
#if SYSCALL_TRACE
        klib::printf("sigentry(%#lX)\n", entry);
#endif
        cpu::get_current_thread()->signal_entry = entry;
        return 0;
    }

    isize syscall_sigreturn(ucontext_t *ucontext, u64 saved_mask) {
#if SYSCALL_TRACE
        klib::printf("sigreturn(%#lX, %#lX)\n", (uptr)ucontext, saved_mask);
#endif
        auto *thread = cpu::get_current_thread();
        cpu::toggle_interrupts(false); // will remain disabled until sysret
        ASSERT(!thread->entering_signal);
        ASSERT(!thread->exiting_signal);
        thread->exiting_signal = true;
        thread->signal_ucontext = ucontext;
        thread->saved_signal_mask = saved_mask;
        sched::reschedule_self();
        return 0;
    }

    isize syscall_sigmask(int how, const u64 *set, u64 *retrieve) {
#if SYSCALL_TRACE
        klib::printf("sigmask(%d, %#lX, %#lX)\n", how, (uptr)set, (uptr)retrieve);
#endif
        u64 *mask = &cpu::get_current_thread()->signal_mask;
        if (retrieve)
            *retrieve = *mask;
        if (set) {
            switch (how) {
            case SIG_BLOCK: *mask |= *set; break;
            case SIG_UNBLOCK: *mask &= ~*set; break;
            case SIG_SETMASK: *mask = *set; break;
            default: return -EINVAL;
            }
        }
        return 0;
    }

    isize syscall_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
#if SYSCALL_TRACE
        klib::printf("sigaction(%d, %#lX, %#lX)\n", signum, (uptr)act, (uptr)oldact);
#endif
        if (signum < 0 || signum > 34 || signum == SIGKILL || signum == SIGSTOP)
            return -EINVAL;
        auto *process = cpu::get_current_thread()->process;
        if (oldact)
            process->signal_actions[signum].to_sigaction(oldact);
        // if (act && (act->sa_flags & ~(SA_SIGINFO)))
        //     klib::printf("sigaction: unimplemented flags %#lX\n", act->sa_flags);
        if (act)
            process->signal_actions[signum].from_sigaction(act);
        return 0;
    }

    isize syscall_kill(pid_t pid, int signal) {
#if SYSCALL_TRACE
        klib::printf("kill(%d, %d)\n", pid, signal);
#endif
        if (signal < 0 || signal > 63)
            return -EINVAL;
        auto *thread = sched::Thread::get_from_tid(pid);
        if (!thread)
            return -ESRCH;
        thread->send_signal(signal);
        return 0;
    }
}
