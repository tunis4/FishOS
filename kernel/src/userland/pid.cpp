#include <userland/pid.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/cstdio.hpp>
#include <sched/sched.hpp>

namespace userland {
    isize syscall_gettid() {
#if SYSCALL_TRACE
        klib::printf("gettid()\n");
#endif
        return cpu::get_current_thread()->tid;
    }

    isize syscall_getpid() {
#if SYSCALL_TRACE
        klib::printf("getpid()\n");
#endif
        return cpu::get_current_thread()->process->pid;
    }

    isize syscall_getppid() {
#if SYSCALL_TRACE
        klib::printf("getppid()\n");
#endif
        return cpu::get_current_thread()->process->parent->pid;
    }

    isize syscall_getpgid(int pid) {
#if SYSCALL_TRACE
        klib::printf("getpgid(%d)\n", pid);
#endif
        sched::Process *process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread)
                return -ESRCH;
            process = thread->process;
        } else {
            process = cpu::get_current_thread()->process;
        }
        return process->process_group_leader->pid;
    }

    isize syscall_setpgid(int pid, int pgid) {
#if SYSCALL_TRACE
        klib::printf("setpgid(%d, %d)\n", pid, pgid);
#endif
        sched::Process *process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread)
                return -ESRCH;
            process = thread->process;
        } else {
            process = cpu::get_current_thread()->process;
        }
        sched::Process *process_group;
        if (pgid) {
            auto *thread = sched::Thread::get_from_tid(pgid);
            if (!thread)
                return -EPERM;
            process_group = thread->process;
        } else {
            process_group = process;
        }
        process->process_group_leader = process_group;
        if (!process->process_group_list.is_empty())
            process->process_group_list.remove();
        if (process_group->process_group_list.is_invalid())
            process_group->process_group_list.init();
        if (process != process_group)
            process_group->process_group_list.add_before(&process->process_group_list);
        return 0;
    }

    isize syscall_getsid(int pid) {
#if SYSCALL_TRACE
        klib::printf("getsid(%d)\n", pid);
#endif
        sched::Process *process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread)
                return -ESRCH;
            process = thread->process;
        } else {
            process = cpu::get_current_thread()->process;
        }
        return process->session_leader->pid;
    }

    isize syscall_setsid() {
#if SYSCALL_TRACE
        klib::printf("setsid()\n");
#endif
        auto *process = cpu::get_current_thread()->process;
        process->session_leader = process;
        return 0;
    }
}
