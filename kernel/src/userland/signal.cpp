#include <userland/signal.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <sched/context.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstdio.hpp>
#include <errno.h>

namespace userland {
    struct StackFrame {
        StackFrame *next;
        uptr ip;
    };

    static void print_ip_with_file_offset(mem::Pagemap *pagemap, uptr ip) {
        int written = klib::printf_unlocked("%#lX", ip);
        if (auto *range = pagemap->addr_to_range(ip)) {
            if (range->type == mem ::MappedRange::Type::FILE) {
                for (; written < 20; written++)
                    klib::putchar(' ');
                range->file->entry->print_path(klib::putchar);
                klib::printf_unlocked(" + %#lX", ip - range->base + range->file_offset);
            }
        }
        klib::putchar('\n');
    }

    static void print_crash_info(sched::Thread *thread, cpu::InterruptState *state, int signal) {
        klib::PrintGuard print_guard;
        mem::Pagemap *pagemap = thread->process->pagemap;

        klib::printf_unlocked("\nThread crashed from signal %d (tid: %d, process name: \"%s\")\n", signal, thread->tid, thread->process->name);
        if (state->err) klib::printf_unlocked("Error code: %#04lX\n", state->err);
        if (signal == SIGSEGV) klib::printf_unlocked("CR2=%016lX\n", cpu::read_cr2());
        if (signal == SIGFPE) klib::printf_unlocked("MXCSR=%08X\n", cpu::read_mxcsr());
        klib::printf_unlocked("RIP=%016lX\n", state->rip);
        // klib::printf_unlocked("FS=%016lX GS=%016lX KERNEL_GS=%016lX\n", cpu::read_fs_base(), cpu::read_gs_base(), cpu::read_kernel_gs_base());

        klib::printf_unlocked("\nStacktrace:\n");
        print_ip_with_file_offset(pagemap, state->rip);
        StackFrame *frame = (StackFrame*)state->rbp;
        while (frame) {
            print_ip_with_file_offset(pagemap, frame->ip);

            if (!pagemap->addr_to_range((uptr)frame->next))
                break;
            frame = frame->next;
        }

        klib::printf_unlocked("\nPagemap:\n");
        pagemap->print(klib::putchar);
    }

    void dispatch_pending_signal(sched::Thread *thread) {
        thread->entering_signal = false;
        thread->enqueued_by_signal = -1;
        u64 accepted_signals = thread->pending_signals & ~thread->signal_mask;
        if (thread->process->signal_entry == 0 || accepted_signals == 0)
            return;

        int signal = -1;
        for (int i = 0; i < 64; i++) {
            if ((accepted_signals >> i) & 1) {
                signal = i + 1;
                break;
            }
        }
        ASSERT(signal != -1);
        thread->pending_signals &= ~(1 << (signal - 1));

        klib::printf("[%d] dispatching signal %d\n", thread->tid, signal);

        auto *state = &thread->gpr_state;
        auto *action = &thread->process->signal_actions[signal];

        if ((action->flags & SA_RESTART) && thread->last_syscall_ret == -EINTR && state->rip == thread->last_syscall_rip) {
            // klib::printf("[%d] restarting syscall\n", thread->tid);
            state->rip -= 2; // length of syscall instruction
            state->rax = thread->last_syscall_num;
        }

        if (signal == SIGSEGV || signal == SIGBUS)
            print_crash_info(thread, state, signal);

        if (action->handler == SIG_IGN)
            return;

        if (action->handler == SIG_DFL) {
            switch (signal_default_action(signal)) {
            case SignalDefault::IGNORE:
                return;
            case SignalDefault::TERMINATE:
                sched::terminate_process(thread->process, signal);
                return;
            case SignalDefault::CORE:
                print_crash_info(thread, state, signal);
                sched::terminate_process(thread->process, signal);
                return;
            case SignalDefault::CONTINUE:
                return; // already handled in scheduler isr
            case SignalDefault::STOP:
                sched::dequeue_thread(thread, signal);
                return;
            }
        }

        ASSERT(action->handler != SIG_IGN && action->handler != SIG_DFL);

        u64 old_rsp = state->rsp;
        if ((action->flags & SA_ONSTACK) && !(thread->signal_alt_stack.ss_flags & SS_DISABLE)) {
            state->rsp = (uptr)thread->signal_alt_stack.ss_sp;
            thread->signal_alt_stack.ss_flags |= SS_ONSTACK;
        } else {
            state->rsp -= 128; // respect red zone
        }

        state->rsp -= cpu::extended_state_size;
        _fpstate *fpstate = (_fpstate*)state->rsp;
        state->rsp = klib::align_down(state->rsp, 16) - 8;

        state->rsp -= sizeof(ucontext_t);
        ucontext_t *ucontext = (ucontext_t*)state->rsp;
        ucontext->uc_mcontext.fpregs = fpstate;
        sched::to_ucontext(state, thread->extended_state, ucontext);
        ucontext->uc_mcontext.gregs[REG_RSP] = old_rsp;
        state->rsp = klib::align_down(state->rsp, 16) - 8;

        state->rsp -= sizeof(siginfo_t);
        siginfo_t *siginfo = (siginfo_t*)state->rsp;
        memset(siginfo, 0, sizeof(siginfo_t));

        siginfo->si_signo = signal;
        if (signal == SIGSEGV) { // extremely janky
            siginfo->si_errno = EFAULT;
            if (state->err & 1)
                siginfo->si_code = SEGV_ACCERR;
            else
                siginfo->si_code = SEGV_MAPERR;
            siginfo->si_addr = (void*)cpu::read_cr2();
        }

        state->rsp = klib::align_down(state->rsp, 16) - 8;

        uptr handler = (uptr)action->handler;
        if (action->flags & SA_SIGINFO)
            handler |= (uptr)1 << 63;

        state->rip = thread->process->signal_entry;
        state->rdi = signal;
        state->rsi = (uptr)siginfo;
        state->rdx = handler;
        state->rcx = thread->signal_mask;
        state->r8 = (uptr)ucontext;

        thread->signal_mask |= action->mask;
        if (!(action->flags & SA_NODEFER))
            thread->signal_mask |= 1 << (signal - 1);

        thread->executing_signal = signal;
    }

    void return_from_signal(sched::Thread *thread) {
        thread->exiting_signal = false;
        if (thread->signal_alt_stack.ss_flags & SS_ONSTACK)
            thread->signal_alt_stack.ss_flags &= ~SS_ONSTACK;
        sched::from_ucontext(&thread->gpr_state, thread->extended_state, thread->signal_ucontext);
        thread->signal_mask = thread->saved_signal_mask;
        thread->signal_ucontext = nullptr;
        thread->saved_signal_mask = 0;
        if (thread->has_poll_saved_signal_mask) {
            thread->signal_mask = thread->poll_saved_signal_mask;
            thread->has_poll_saved_signal_mask = false;
        }
    }

    isize syscall_sigentry(uptr entry) {
        log_syscall("sigentry(%#lX)\n", entry);
        cpu::get_current_thread()->process->signal_entry = entry;
        return 0;
    }

    isize syscall_sigreturn(ucontext_t *ucontext, u64 saved_mask) {
        log_syscall("sigreturn(%#lX, %#lX)\n", (uptr)ucontext, saved_mask);
        auto *thread = cpu::get_current_thread();
        cpu::toggle_interrupts(false); // will remain disabled until sysret
        ASSERT(!thread->entering_signal);
        ASSERT(!thread->exiting_signal);
        ASSERT(thread->executing_signal != -1);
        thread->executing_signal = -1;
        thread->exiting_signal = true;
        thread->signal_ucontext = ucontext;
        thread->saved_signal_mask = saved_mask;
        sched::reschedule_self();
        return 0;
    }

    isize syscall_sigmask(int how, const u64 *set, u64 *retrieve) {
        log_syscall("sigmask(%d, %#lX, %#lX)\n", how, (uptr)set, (uptr)retrieve);
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
        log_syscall("sigaction(%d, %#lX, %#lX)\n", signum, (uptr)act, (uptr)oldact);
        if (signum < 0 || signum >= 64 || signum == SIGKILL || signum == SIGSTOP)
            return -EINVAL;
        auto *process = cpu::get_current_thread()->process;
        if (oldact)
            process->signal_actions[signum].to_sigaction(oldact);
        if (act && (act->sa_flags & ~(SA_SIGINFO | SA_RESTART | SA_ONSTACK)))
            klib::printf("sigaction: unimplemented flags %#lX\n", act->sa_flags);
        if (act)
            process->signal_actions[signum].from_sigaction(act);
        return 0;
    }

    isize syscall_kill(pid_t pid, int signal) {
        log_syscall("kill(%d, %d)\n", pid, signal);
        if (signal < 0 || signal >= 64)
            return -EINVAL;

        bool to_process_group = false;
        if (pid < 0) {
            to_process_group = true;
            pid = -pid;
        } else if (pid == 0) {
            to_process_group = true;
            pid = cpu::get_current_thread()->process->group->leader_process->pid;
        }

        auto *thread = sched::Thread::get_from_tid(pid);
        if (!thread || thread->state == sched::Thread::ZOMBIE)
            return -ESRCH;
        if (to_process_group && thread->process->group->leader_process != thread->process)
            return -ESRCH;

        if (signal == 0)
            return 0;

        if (to_process_group)
            thread->process->group->send_signal(signal);
        else
            thread->send_signal(signal);
        return 0;
    }

    isize syscall_sigaltstack(const stack_t *new_signal_stack, stack_t *old_signal_stack) {
        log_syscall("sigaltstack(%#lX, %#lX)\n", (uptr)new_signal_stack, (uptr)old_signal_stack);
        auto *thread = cpu::get_current_thread();

        if (old_signal_stack) {
            *old_signal_stack = thread->signal_alt_stack;
        }

        if (new_signal_stack) {
            if (new_signal_stack->ss_flags & (1 << 31)) {
                klib::printf("sigaltstack: SS_AUTODISARM not supported\n");
                return -EINVAL;
            }

            if (new_signal_stack->ss_flags == SS_DISABLE) {
                thread->signal_alt_stack.ss_sp = nullptr;
                thread->signal_alt_stack.ss_flags = SS_DISABLE;
                thread->signal_alt_stack.ss_size = 0;
            } else if (new_signal_stack->ss_flags == 0) {
                thread->signal_alt_stack = *new_signal_stack;
            } else {
                return -EINVAL;
            }
        }
        return -ENOSYS;
    }

    SignalDefault signal_default_action(int signal) {
        switch (signal) {
        case   SIGABRT: return SignalDefault::CORE;
        case   SIGALRM: return SignalDefault::TERMINATE;
        case    SIGBUS: return SignalDefault::CORE;
        case   SIGCHLD: return SignalDefault::IGNORE;
        case   SIGCONT: return SignalDefault::CONTINUE;
        case    SIGFPE: return SignalDefault::CORE;
        case    SIGHUP: return SignalDefault::TERMINATE;
        case    SIGILL: return SignalDefault::CORE;
        case    SIGINT: return SignalDefault::TERMINATE;
        case     SIGIO: return SignalDefault::TERMINATE;
        case   SIGKILL: return SignalDefault::TERMINATE;
        case   SIGPIPE: return SignalDefault::TERMINATE;
        case   SIGPROF: return SignalDefault::TERMINATE;
        case    SIGPWR: return SignalDefault::TERMINATE;
        case   SIGQUIT: return SignalDefault::CORE;
        case   SIGSEGV: return SignalDefault::CORE;
        case SIGSTKFLT: return SignalDefault::TERMINATE;
        case   SIGSTOP: return SignalDefault::STOP;
        case   SIGTSTP: return SignalDefault::STOP;
        case    SIGSYS: return SignalDefault::CORE;
        case   SIGTERM: return SignalDefault::TERMINATE;
        case   SIGTRAP: return SignalDefault::CORE;
        case   SIGTTIN: return SignalDefault::STOP;
        case   SIGTTOU: return SignalDefault::STOP;
        case    SIGURG: return SignalDefault::IGNORE;
        case   SIGUSR1: return SignalDefault::TERMINATE;
        case   SIGUSR2: return SignalDefault::TERMINATE;
        case SIGVTALRM: return SignalDefault::TERMINATE;
        case   SIGXCPU: return SignalDefault::CORE;
        case   SIGXFSZ: return SignalDefault::CORE;
        case  SIGWINCH: return SignalDefault::IGNORE;
        default: return SignalDefault::CORE;
        }
    }
}
