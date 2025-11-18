#include <userland/pid.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/cstdio.hpp>
#include <sched/sched.hpp>

namespace userland {
    isize syscall_gettid() {
        log_syscall("gettid()\n");
        return cpu::get_current_thread()->tid;
    }

    isize syscall_getpid() {
        log_syscall("getpid()\n");
        return cpu::get_current_thread()->process->pid;
    }

    isize syscall_getppid() {
        log_syscall("getppid()\n");
        return cpu::get_current_thread()->process->parent->pid;
    }

    isize syscall_getpgid(int pid) {
        log_syscall("getpgid(%d)\n", pid);
        sched::Process *process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread)
                return -ESRCH;
            process = thread->process;
        } else {
            process = cpu::get_current_thread()->process;
        }
        return process->group->leader_process->pid;
    }

    isize syscall_getpgrp() {
        log_syscall("getpgrp()\n");
        return cpu::get_current_thread()->process->group->leader_process->pid;
    }

    isize syscall_setpgid(int pid, int pgid) {
        log_syscall("setpgid(%d, %d)\n", pid, pgid);
        sched::Process *current_process = cpu::get_current_thread()->process;
        sched::Process *target_process = current_process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread) return -ESRCH;
            target_process = thread->process;
            if (target_process != current_process && target_process->parent != current_process) return -ESRCH;
            if (target_process->parent == current_process && target_process->has_performed_execve) return -EACCES;
        }
        if (target_process->session_leader() == target_process)
            return -EPERM;

        sched::ProcessGroup *group;
        if (pgid && pgid != target_process->pid) {
            if (pgid < 0) return -EINVAL;

            auto *thread = sched::Thread::get_from_tid(pgid);
            if (!thread) return -EPERM;

            group = thread->process->group;
            if (group->session != target_process->group->session) return -EPERM;
        } else {
            if (target_process->group->leader_process == target_process) return 0;
            group = new sched::ProcessGroup(target_process->group->session, target_process);
        }

        klib::InterruptLock interrupt_guard;
        group->add_process(target_process);
        return 0;
    }

    isize syscall_getsid(int pid) {
        log_syscall("getsid(%d)\n", pid);
        sched::Process *process;
        if (pid) {
            auto *thread = sched::Thread::get_from_tid(pid);
            if (!thread) return -ESRCH;
            process = thread->process;
        } else {
            process = cpu::get_current_thread()->process;
        }
        return process->session_leader()->pid;
    }

    isize syscall_setsid() {
        log_syscall("setsid()\n");
        auto *process = cpu::get_current_thread()->process;
        if (process->group->leader_process == process)
            return -EPERM;

        klib::InterruptLock interrupt_guard;
        auto *session = new sched::Session();
        auto *group = new sched::ProcessGroup(session, process);
        group->add_process(process);
        session->leader_group = process->group;
        return process->pid;
    }
}
