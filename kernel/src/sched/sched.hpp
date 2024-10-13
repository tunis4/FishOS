#pragma once

#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/common.hpp>
#include <klib/vector.hpp>
#include <klib/list.hpp>
#include <fs/vfs.hpp>
#include <sched/event.hpp>
#include <userland/signal.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sys/utsname.h>

namespace sched {
    struct Process;

    struct Thread {
        int tid;
        Process *process;
        klib::ListHead thread_link;

        cpu::InterruptState gpr_state;
        u64 gs_base, fs_base;
        uptr stack;
        uptr kernel_stack;
        uptr saved_user_stack;
        uptr saved_kernel_stack;
        usize running_on;

        klib::ListHead sched_link;
        klib::Spinlock yield_await; // not an actual lock

        klib::Vector<Event::Listener*> listeners;
        usize which_event;

        uptr signal_entry;
        u64 signal_mask;
        u64 pending_signals;
        int enqueued_by_signal = -1;
        bool entering_signal, exiting_signal;
        ucontext_t *signal_ucontext; // only valid when exiting signal
        u64 saved_signal_mask; // only valid when exiting signal

        // used for SA_RESTART
        usize last_syscall_num;
        usize last_syscall_rip;
        isize last_syscall_ret;

        enum State {
            READY,
            RUNNING,
            BLOCKED,
            STOPPED,
            ZOMBIE
        };

        State state;

        Thread(Process *process, int tid);
        ~Thread();

        Thread(const Thread &other) = delete;
        Thread(Thread &&other) = delete;

        static Thread* get_from_tid(int tid);

        void init_user(uptr entry, uptr new_stack);
        void send_signal(int signal);
        bool has_pending_signals();
    };

    struct Process {
        int pid;
        klib::ListHead thread_list;

        vmm::Pagemap *pagemap;

        klib::Vector<vfs::FileDescriptor> file_descriptors;
        usize num_file_descriptors; // the actual number
        int first_free_fdnum; // used for allocating file descriptor numbers

        vfs::Entry *cwd; // current working directory

        uptr mmap_anon_base; // used for mmap bump allocator

        Process *parent;
        klib::ListHead children_list, sibling_link;

        Event zombie_event, stopped_event, continued_event;
        int status;

        userland::SignalAction signal_actions[64];

        Process *session_leader;
        vfs::VNode *controlling_terminal; // only valid for session leader

        Process *process_group_leader;
        klib::ListHead process_group_list;

        Process();
        ~Process();

        Process(const Process &other) = delete;
        Process(Process &&other) = delete;

        Thread* get_main_thread();
        int allocate_fdnum(int min_fdnum = 0);
        void send_process_group_signal(int signal);
    };

    void init();
    void start();

    Thread* new_kernel_thread(void (*func)(), bool enqueue);
    Process* new_user_process(vfs::VNode *elf_file, bool enqueue, int argc, char **argv);

    void dequeue_thread(Thread *thread, int stop_signal = -1);
    void enqueue_thread(Thread *thread, int signal = -1);
    void terminate_thread(Thread *thread, int terminate_signal = -1);
    [[noreturn]] void terminate_self();
    void yield();

    void reschedule_self();
    usize scheduler_isr(void *priv, cpu::InterruptState *gpr_state);

    [[noreturn]] void syscall_exit(int status);
    isize syscall_fork(cpu::syscall::SyscallState *state);
    isize syscall_execve(cpu::syscall::SyscallState *state, const char *path, const char **argv, const char **envp);
    void syscall_set_fs_base(uptr value);
    isize syscall_waitpid(int pid, int *status, int flags);
    isize syscall_uname(struct utsname *buf);
    isize syscall_thread_spawn(void *entry, void *stack);
    [[noreturn]] void syscall_thread_exit();
}
