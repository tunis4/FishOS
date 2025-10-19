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
#include <userland/inotify.hpp>
#include <userland/eventfd.hpp>
#include <fishos/syscall.h>

namespace cpu::syscall {
    static void *syscall_table[75];

    static bool should_trace_syscall(usize num) {
#if SYSCALL_TRACE
        return true;
        // return num == SYS_poll || num == SYS_sigmask || num == SYS_sigreturn || num == SYS_sigaction || num == SYS_kill || num == SYS_pipe;
        // return num == SYS_mmap || num == SYS_mprotect || num == SYS_munmap;
        // return num == SYS_setsid || num == SYS_setpgid || num == SYS_execve || num == SYS_fork || num == SYS_thread_spawn || num == SYS_exit;
        // return num != SYS_clock_gettime && num != SYS_futex; // && num != SYS_read && num != SYS_write;
#else
        return false;
#endif
    }

    extern "C" void __syscall_handler(SyscallState *state) {
        auto *thread = cpu::get_current_thread();
        thread->syscall_state = state;
        defer { thread->syscall_state = nullptr; };

        thread->last_syscall_num = state->rax;
        thread->last_syscall_rip = state->rcx;

#if SYSCALL_TRACE
        if (should_trace_syscall(thread->last_syscall_num))
            klib::printf("[%d] ", thread->tid);
#endif
        cpu::toggle_interrupts(true);

        if (state->rax >= sizeof(syscall_table) / sizeof(void*)) {
            klib::printf("invalid syscall %lu\n", state->rax);
            state->rax = -ENOSYS;
        } else {
            if (state->rax == SYS_fork || state->rax == SYS_execve) {
                auto *syscall = (isize (*)(SyscallState *, usize, usize, usize, usize, usize, usize))syscall_table[state->rax];
                state->rax = syscall(state, state->rdi, state->rsi, state->rdx, state->r10, state->r8, state->r9);
            } else {
                auto *syscall = (isize (*)(usize, usize, usize, usize, usize, usize))syscall_table[state->rax];
                state->rax = syscall(state->rdi, state->rsi, state->rdx, state->r10, state->r8, state->r9);
            }
        }

        cpu::toggle_interrupts(false);
        thread->last_syscall_ret = (isize)state->rax;
        if (!thread->exiting_signal && !thread->entering_signal && thread->has_pending_signals()) {
            thread->entering_signal = true;
            sched::reschedule_self(); // interrupts will be reenabled after sysret and the self irq will be immediately handled
        } else {
            if (thread->has_poll_saved_signal_mask) {
                thread->signal_mask = thread->poll_saved_signal_mask;
                thread->has_poll_saved_signal_mask = false;
            }
        }

#if SYSCALL_TRACE
        if (should_trace_syscall(thread->last_syscall_num)) {
            if ((isize)state->rax >= 0)
                klib::printf("[%d] return: %#lX\n", thread->tid, state->rax);
            else
                klib::printf("[%d] return errno: %ld\n", thread->tid, -(isize)state->rax);
        }
#endif
    }

    int log_syscall_impl(const char *format, ...) {
        if (!should_trace_syscall(cpu::get_current_thread()->last_syscall_num))
            return 0;

        va_list list;
        va_start(list, format);
        int i = klib::vprintf(format, list);
        va_end(list);
        return i;
    }

    void init_syscall_table() {
        syscall_table[             SYS_exit] = (void*)&sched::syscall_exit;
        syscall_table[             SYS_fork] = (void*)&sched::syscall_fork;
        syscall_table[           SYS_execve] = (void*)&sched::syscall_execve;
        syscall_table[          SYS_waitpid] = (void*)&sched::syscall_waitpid;
        syscall_table[            SYS_sleep] = (void*)&sched::syscall_sleep;
        syscall_table[    SYS_clock_gettime] = (void*)&sched::syscall_clock_gettime;
        syscall_table[     SYS_clock_getres] = (void*)&sched::syscall_clock_getres;
        syscall_table[      SYS_set_fs_base] = (void*)&sched::syscall_set_fs_base;
        syscall_table[            SYS_umask] = (void*)&sched::syscall_umask;
        syscall_table[     SYS_thread_spawn] = (void*)&sched::syscall_thread_spawn;
        syscall_table[      SYS_thread_exit] = (void*)&sched::syscall_thread_exit;
        syscall_table[   SYS_thread_getname] = (void*)&sched::syscall_thread_getname;
        syscall_table[   SYS_thread_setname] = (void*)&sched::syscall_thread_setname;
        syscall_table[      SYS_sched_yield] = (void*)&sched::syscall_sched_yield;

        syscall_table[             SYS_mmap] = (void*)&mem::syscall_mmap;
        syscall_table[           SYS_munmap] = (void*)&mem::syscall_munmap;
        syscall_table[         SYS_mprotect] = (void*)&mem::syscall_mprotect;

        syscall_table[            SYS_uname] = (void*)&userland::syscall_uname;
        syscall_table[             SYS_pipe] = (void*)&userland::syscall_pipe;

        syscall_table[             SYS_open] = (void*)&vfs::syscall_open;
        syscall_table[            SYS_mkdir] = (void*)&vfs::syscall_mkdir;
        syscall_table[            SYS_close] = (void*)&vfs::syscall_close;
        syscall_table[             SYS_read] = (void*)&vfs::syscall_read;
        syscall_table[            SYS_pread] = (void*)&vfs::syscall_pread;
        syscall_table[            SYS_write] = (void*)&vfs::syscall_write;
        syscall_table[           SYS_pwrite] = (void*)&vfs::syscall_pwrite;
        syscall_table[            SYS_readv] = (void*)&vfs::syscall_readv;
        syscall_table[           SYS_writev] = (void*)&vfs::syscall_writev;
        syscall_table[             SYS_seek] = (void*)&vfs::syscall_seek;
        syscall_table[           SYS_getcwd] = (void*)&vfs::syscall_getcwd;
        syscall_table[            SYS_chdir] = (void*)&vfs::syscall_chdir;
        syscall_table[          SYS_readdir] = (void*)&vfs::syscall_readdir;
        syscall_table[           SYS_unlink] = (void*)&vfs::syscall_unlink;
        syscall_table[            SYS_fcntl] = (void*)&vfs::syscall_fcntl;
        syscall_table[              SYS_dup] = (void*)&vfs::syscall_dup;
        syscall_table[             SYS_stat] = (void*)&vfs::syscall_stat;
        syscall_table[           SYS_rename] = (void*)&vfs::syscall_rename;
        syscall_table[             SYS_poll] = (void*)&vfs::syscall_poll;
        syscall_table[         SYS_readlink] = (void*)&vfs::syscall_readlink;
        syscall_table[            SYS_ioctl] = (void*)&vfs::syscall_ioctl;
        syscall_table[             SYS_link] = (void*)&vfs::syscall_link;
        syscall_table[          SYS_symlink] = (void*)&vfs::syscall_symlink;
        syscall_table[           SYS_fchdir] = (void*)&vfs::syscall_fchdir;

        syscall_table[            SYS_futex] = (void*)&userland::syscall_futex;
        syscall_table[      SYS_sigaltstack] = (void*)&userland::syscall_sigaltstack;
        syscall_table[         SYS_sigentry] = (void*)&userland::syscall_sigentry;
        syscall_table[        SYS_sigreturn] = (void*)&userland::syscall_sigreturn;
        syscall_table[          SYS_sigmask] = (void*)&userland::syscall_sigmask;
        syscall_table[        SYS_sigaction] = (void*)&userland::syscall_sigaction;
        syscall_table[             SYS_kill] = (void*)&userland::syscall_kill;
        syscall_table[           SYS_gettid] = (void*)&userland::syscall_gettid;
        syscall_table[           SYS_getpid] = (void*)&userland::syscall_getpid;
        syscall_table[          SYS_getppid] = (void*)&userland::syscall_getppid;
        syscall_table[          SYS_getpgid] = (void*)&userland::syscall_getpgid;
        syscall_table[          SYS_setpgid] = (void*)&userland::syscall_setpgid;
        syscall_table[           SYS_getsid] = (void*)&userland::syscall_getsid;
        syscall_table[           SYS_setsid] = (void*)&userland::syscall_setsid;
        syscall_table[          SYS_sysinfo] = (void*)&userland::syscall_sysinfo;
        syscall_table[   SYS_inotify_create] = (void*)&userland::syscall_inotify_create;
        syscall_table[SYS_inotify_add_watch] = (void*)&userland::syscall_inotify_add_watch;
        syscall_table[ SYS_inotify_rm_watch] = (void*)&userland::syscall_inotify_rm_watch;
        syscall_table[   SYS_eventfd_create] = (void*)&userland::syscall_eventfd_create;

        syscall_table[           SYS_socket] = (void*)&socket::syscall_socket;
        syscall_table[       SYS_socketpair] = (void*)&socket::syscall_socketpair;
        syscall_table[             SYS_bind] = (void*)&socket::syscall_bind;
        syscall_table[          SYS_connect] = (void*)&socket::syscall_connect;
        syscall_table[           SYS_listen] = (void*)&socket::syscall_listen;
        syscall_table[           SYS_accept] = (void*)&socket::syscall_accept;
        syscall_table[          SYS_recvmsg] = (void*)&socket::syscall_recvmsg;
        syscall_table[          SYS_sendmsg] = (void*)&socket::syscall_sendmsg;
        syscall_table[         SYS_shutdown] = (void*)&socket::syscall_shutdown;
        syscall_table[       SYS_getsockopt] = (void*)&socket::syscall_getsockopt;
        syscall_table[       SYS_setsockopt] = (void*)&socket::syscall_setsockopt;
        syscall_table[      SYS_getsockname] = (void*)&socket::syscall_getsockname;
        syscall_table[      SYS_getpeername] = (void*)&socket::syscall_getpeername;
    }
}
