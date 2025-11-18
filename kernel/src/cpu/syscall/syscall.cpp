#include <cpu/cpu.hpp>
#include <klib/cstdio.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <mem/vmm.hpp>
#include <fs/vfs.hpp>
#include <userland/socket/socket.hpp>
#include <userland/pipe.hpp>
#include <userland/futex.hpp>
#include <userland/signal.hpp>
#include <userland/pid.hpp>
#include <userland/info.hpp>
#include <userland/epoll.hpp>
#include <userland/inotify.hpp>
#include <userland/eventfd.hpp>
#include <sys/syscall.h>

namespace cpu::syscall {
    static constexpr usize syscall_table_size = 453;
    static void *syscall_table[syscall_table_size] = {};
    static const char *syscall_name_table[syscall_table_size] = {};

#if SYSCALL_TRACE
    static constexpr int min_tid = 150;
#endif

    static bool should_trace_syscall(usize num) {
#if SYSCALL_TRACE
        auto *thread = cpu::get_current_thread();
        if (thread->tid < min_tid)
            return false;
        // if (sched::get_clock(CLOCK_BOOTTIME).seconds < 300)
        //     return false;
        // if (klib::strstr(thread->process->name, "java") == nullptr)
        //     return false;
        return num != SYS_clock_gettime && num != SYS_clock_nanosleep && num != SYS_gettid && num != SYS_futex;
        // return num == SYS_mmap || num == SYS_mprotect || num == SYS_munmap;
#else
        return false;
#endif
    }

    extern "C" void __syscall_handler(SyscallState *state) {
        auto *thread = cpu::get_current_thread();
        thread->syscall_state = state;
        defer { thread->syscall_state = nullptr; };

        usize syscall_num = state->rax;
        thread->last_syscall_num = syscall_num;
        thread->last_syscall_rip = state->rcx;

        cpu::toggle_interrupts(true);

        if (syscall_num >= syscall_table_size || syscall_table[syscall_num] == nullptr) {
            state->rax = -ENOSYS;
        } else {
            auto *syscall = (isize (*)(usize, usize, usize, usize, usize, usize))syscall_table[syscall_num];
            cpu::toggle_interrupts(false);
            state->rax = syscall(state->rdi, state->rsi, state->rdx, state->r10, state->r8, state->r9);
        }

#if SYSCALL_TRACE || UNIMPLEMENTED_SYSCALL_TRACE
        if ((int)state->rax == -ENOSYS && thread->tid >= min_tid) {
            if (syscall_num >= syscall_table_size || syscall_name_table[syscall_num] == nullptr)
                klib::printf("[%d %s] !! unimplemented syscall: %lu !!\n", thread->tid, thread->name, syscall_num);
            else
                klib::printf("[%d %s] !! unimplemented syscall: %s !!\n", thread->tid, thread->name, syscall_name_table[syscall_num]);
        }
#endif

        cpu::toggle_interrupts(false);

        if (thread->has_poll_saved_signal_mask) {
            thread->signal_mask = thread->poll_saved_signal_mask;
            thread->has_poll_saved_signal_mask = false;
        }

        thread->last_syscall_ret = (isize)state->rax;
        if (!thread->exiting_signal && !thread->entering_signal && thread->has_pending_signals()) {
            thread->entering_signal = true;
            sched::reschedule_self(); // interrupts will be reenabled after sysret and the self irq will be immediately handled
        }

#if SYSCALL_TRACE
        if (should_trace_syscall(thread->last_syscall_num)) {
            if ((isize)state->rax >= 0)
                klib::printf("[%d %s] return: %#lX\n", thread->tid, thread->name, state->rax);
            else
                klib::printf("[%d %s] return errno: %ld\n", thread->tid, thread->name, -(isize)state->rax);
        }
#endif
    }

    int log_syscall_impl(const char *format, ...) {
        sched::Thread *thread = cpu::get_current_thread();
        if (!should_trace_syscall(thread->last_syscall_num))
            return 0;

        klib::PrintGuard print_guard;
        klib::printf_unlocked("[%d %s] ", thread->tid, thread->name);

        va_list list;
        va_start(list, format);
        int i = klib::vprintf_unlocked(format, list);
        va_end(list);
        return i;
    }

    void init_syscall_table() {
#define SYSCALL(name_space, name) \
        ASSERT(!syscall_name_table[SYS_ ##name]); \
        syscall_table[SYS_ ##name] = (void*)CONCAT2(name_space:,CONCAT2(:syscall_, name)); \
        syscall_name_table[SYS_ ##name] = #name
#define UNIMPLEMENTED_SYSCALL(name) \
        ASSERT(!syscall_name_table[SYS_ ##name]); \
        syscall_name_table[SYS_ ##name] = #name
#define STUB_SYSCALL(name) \
        ASSERT(!syscall_name_table[SYS_ ##name]); \
        syscall_table[SYS_ ##name] = (void*)+[] () { return 0; }; \
        syscall_name_table[SYS_ ##name] = #name
#include "syscalls.inc"
#undef UNIMPLEMENTED_SYSCALL
#undef SYSCALL
    }
}
