#pragma once

#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/common.hpp>
#include <klib/vector.hpp>
#include <klib/list.hpp>
#include <fs/vfs.hpp>
#include <sched/event.hpp>
#include <sched/context.hpp>
#include <sched/time.hpp>
#include <userland/signal.hpp>
#include <userland/cred.hpp>
#include <cpu/syscall/syscall.hpp>
#include <linux/sched.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sched.h>

namespace sched {
    struct Process;

    struct Thread {
        int tid;
        Process *process;
        klib::ListHead thread_link;

        cpu::InterruptState gpr_state;
        void *extended_state = nullptr;
        u64 gs_base, fs_base;
        uptr user_stack;
        uptr kernel_stack;
        uptr saved_user_stack;
        uptr saved_kernel_stack;
        usize running_on;
        cpu::syscall::SyscallState *syscall_state = nullptr; // only valid while inside a syscall

        klib::ListHead sched_link;
        volatile bool yield_await = false;

        klib::Vector<Event::Listener> listeners;
        usize which_event;

        userland::Cred cred;

        stack_t signal_alt_stack;
        u64 signal_mask = 0;
        u64 pending_signals = 0;
        int enqueued_by_signal = -1;
        bool entering_signal = false, exiting_signal = false;
        SignalFrame *signal_frame = nullptr; // only valid when exiting signal
        u64 poll_saved_signal_mask = 0; // only valid when poll is called with a signal mask
        bool has_poll_saved_signal_mask = false;

        // used for SA_RESTART
        usize last_syscall_num;
        usize last_syscall_rip;
        isize last_syscall_ret;
 
        // userspace ptrs, see set_tid_address(2)
        uptr set_child_tid = 0, clear_child_tid = 0;

        enum State {
            READY,
            RUNNING,
            BLOCKED,
            STOPPED,
            ZOMBIE
        };

        State state;
        char name[64] = {};

        vfs::Entry *procfs_dir = nullptr;

        Thread(Process *process, int tid);
        ~Thread();

        Thread(const Thread &other) = delete;
        Thread(Thread &&other) = delete;

        static Thread* get_from_tid(int tid);

        void init_user(uptr entry, uptr new_stack);
        void send_signal(int signal);
        bool has_pending_signals();

        void clear_listeners();
    };

    struct ProcessGroup;

    struct Session {
        ProcessGroup *leader_group;
        vfs::VNode *controlling_terminal;
        klib::ListHead process_group_list;

        Session() {
            process_group_list.init();
        }
    };

    struct ProcessGroup {
        Session *session;
        Process *leader_process;
        klib::ListHead process_list;
        klib::ListHead session_link;

        ProcessGroup(Session *session, Process *leader_process);

        void add_process(Process *process);
        void send_signal(int signal);
    };

    struct Process {
        int pid;
        klib::ListHead thread_list;
        int num_living_threads = 0;

        mem::Pagemap *pagemap = nullptr;

        klib::Vector<vfs::FileDescriptor> file_descriptors;
        usize num_file_descriptors = 0; // the actual number
        int first_free_fdnum = 0; // used for allocating file descriptor numbers

        vfs::Entry *cwd = nullptr; // current working directory
        vfs::Entry *exe = nullptr; // executable file

        mode_t umask = S_IWGRP | S_IWOTH;
        bool dumpable = true;

        uptr mmap_anon_base = 0; // used for mmap bump allocator

        Process *parent = nullptr;
        klib::ListHead children_list, sibling_link;

        Event zombie_event, stopped_event, continued_event;
        // FIXME: these should be per-thread
        int wait_status = 0, wait_code = 0; // siginfo return of waitid
        bool is_zombie = false;
        bool has_performed_execve = false;

        userland::KernelSigaction signal_actions[64];

        Timer itimer_real;

        ProcessGroup *group = nullptr;
        klib::ListHead group_link;

        vfs::Entry *procfs_dir = nullptr, *procfs_task_dir = nullptr;

        Process();
        ~Process();

        Process(const Process &other) = delete;
        Process(Process &&other) = delete;

        Thread* get_main_thread();
        Process* session_leader() { return group->session->leader_group->leader_process; }

        int allocate_fdnum(int min_fdnum = 0);
        void set_parent(Process *new_parent);
        void zombify(int terminate_signal);
        void send_signal(int signal);

        void print_file_descriptors();
    };

    void init();
    void start();

    Thread* new_kernel_thread(void (*func)(), bool enqueue, const char *name);
    Process* create_init_process(const char *path, int argc, char **argv);

    void dequeue_thread(Thread *thread, int stop_signal = -1);
    void enqueue_thread(Thread *thread, int signal = -1);
    void terminate_thread(Thread *thread, int terminate_signal = -1);
    void terminate_process(Process *process, int terminate_signal = -1);
    [[noreturn]] void terminate_self(bool whole_process);
    void yield();

    void reschedule_self();
    usize scheduler_isr(void *priv, cpu::InterruptState *gpr_state);

    void debug_print_threads();

    [[noreturn]] void syscall_exit(int status);
    [[noreturn]] void syscall_exit_group(int status);
    isize syscall_fork();
    isize syscall_vfork();
    isize syscall_clone(u64 flags, uptr stack, int *parent_tid, int *child_tid, u64 tls);
    isize syscall_clone3(clone_args *clone_args, usize size);
    isize syscall_execve(const char *path, const char **argv, const char **envp);
    isize syscall_arch_prctl(int op, uptr addr);
    isize syscall_wait4(int pid, int *status, int flags, struct rusage *rusage);
    isize syscall_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options, struct rusage *rusage);
    isize syscall_set_tid_address(int *tidptr);
    mode_t syscall_umask(mode_t mode);
    void syscall_sched_yield();
    isize syscall_sched_getaffinity(int pid, usize cpusetsize, cpu_set_t *mask);
    isize syscall_getcpu(uint *cpu, uint *node);
    isize syscall_prlimit64(int pid, uint resource, const rlimit64 *new_limit, rlimit64 *old_limit);
    isize syscall_prctl(int op, usize arg1, usize arg2, usize arg3, usize arg4);
}
