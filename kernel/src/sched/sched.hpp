#pragma once

#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/common.hpp>
#include <klib/vector.hpp>
#include <klib/list.hpp>
#include <fs/vfs.hpp>
#include <sched/event.hpp>
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

        enum State {
            READY,
            RUNNING,
            BLOCKED,
            ZOMBIE
        };

        State state;

        Thread(Process *process, int tid);
        ~Thread();

        Thread(const Thread &other) = delete;
        Thread(Thread &&other) = delete;
    };

    struct Process {
        int pid;
        klib::ListHead thread_list;

        mem::vmm::Pagemap *pagemap;

        klib::Vector<vfs::FileDescriptor> file_descriptors;
        usize num_file_descriptors; // the actual number
        int first_free_fdnum; // used for allocating file descriptor numbers

        vfs::Entry *cwd; // current working directory

        uptr mmap_anon_base; // used for mmap bump allocator

        Process *parent;
        klib::ListHead children_list, sibling_link;
        Event event;

        bool is_zombie = false;
        int exit_status;

        Process();
        ~Process();

        Process(const Process &other) = delete;
        Process(Process &&other) = delete;

        Thread* get_main_thread();
        int allocate_fdnum(int min_fdnum = 0);
    };

    void init();
    void start();

    Thread* new_kernel_thread(void (*func)(), bool enqueue);
    Process* new_user_process(vfs::VNode *elf_file, bool enqueue);

    void dequeue_thread(Thread *thread);
    void enqueue_thread(Thread *thread);
    [[noreturn]] void dequeue_and_die();
    void yield();
    
    usize scheduler_isr(u64 vec, cpu::InterruptState *gpr_state);
    
    [[noreturn]] void syscall_exit(int status);
    isize syscall_fork(cpu::syscall::SyscallState *state);
    isize syscall_execve(cpu::syscall::SyscallState *state, const char *path, const char **argv, const char **envp);
    void syscall_set_fs_base(uptr value);
    isize syscall_waitpid(int pid, int *status, int flags);
    isize syscall_uname(struct utsname *buf);
    isize syscall_thread_spawn(void *entry, void *stack);
    [[noreturn]] void syscall_thread_exit();
}
