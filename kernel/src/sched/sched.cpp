#include <sched/sched.hpp>
#include <sched/context.hpp>
#include <sched/timer/apic_timer.hpp>
#include <sched/timer/hpet.hpp>
#include <mem/pmm.hpp>
#include <mem/vmm.hpp>
#include <cpu/cpu.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdio.hpp>
#include <klib/algorithm.hpp>
#include <userland/elf.hpp>
#include <gfx/framebuffer.hpp>
#include <dev/tty/console.hpp>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace sched {
    constexpr usize sched_freq = 200; // Hz
    constexpr usize kernel_stack_size = 0x10000;
    constexpr usize user_stack_size = 0x100000;
    static klib::ListHead sched_list_head;
    static Process *kernel_process;
    static Thread *idle_thread;

    static usize num_threads = 0, first_free_tid = 0;

    static klib::Vector<Thread*>& get_thread_table() {
        static klib::Vector<Thread*> thread_table;
        return thread_table;
    }

    static int allocate_tid() {
        auto &thread_table = get_thread_table();
        num_threads++;
        for (usize i = first_free_tid; i < thread_table.size(); i++) {
            if (thread_table[i] == nullptr) {
                first_free_tid = i;
                return i;
            }
        }
        first_free_tid = thread_table.size();
        thread_table.push_back(nullptr);
        return thread_table.size() - 1;
    }

    Thread* Process::get_main_thread() {
        Thread *thread = LIST_HEAD(&thread_list, Thread, thread_link);
        ASSERT(thread->tid == pid);
        return thread;
    }

    int Process::allocate_fdnum(int min_fdnum) {
        num_file_descriptors++;
        int begin = min_fdnum;
        if (min_fdnum < first_free_fdnum)
            begin = first_free_fdnum;
        for (usize i = begin; i < file_descriptors.size(); i++) {
            if (file_descriptors[i].get_description() == nullptr) {
                if (min_fdnum < first_free_fdnum)
                    first_free_fdnum = i + 1;
                return i;
            }
        }
        if (min_fdnum < first_free_fdnum)
            first_free_fdnum = file_descriptors.size();
        file_descriptors.push_back(vfs::FileDescriptor());
        return file_descriptors.size() - 1;
    }

    Thread::Thread(Process *process, int tid) : tid(tid), process(process) {
        process->thread_list.add_before(&thread_link);
        get_thread_table()[tid] = this;
        state = BLOCKED;
    }

    Thread::~Thread() {
        get_thread_table()[tid] = nullptr;
        thread_link.remove();
    }

    Process::Process() {
        pid = allocate_tid();
        num_file_descriptors = 0;
        first_free_fdnum = 0;
        thread_list.init();
        children_list.init();

        // these should be correctly initiliazed after construction
        parent = this;
        process_group_leader = this;
        process_group_list.init();
        session_leader = this;
    }

    Process::~Process() {
        for (usize i = 0; i < file_descriptors.size(); i++)
            if (file_descriptors[i].get_description() != nullptr)
                file_descriptors[i].close(this, i);

        delete pagemap;
        // klib::printf("free pages after process cleanup: %lu\n", pmm::stats.total_free_pages);
    }

    Thread* Thread::get_from_tid(int tid) {
        auto &thread_table = get_thread_table();
        if (tid <= 0 || tid >= (int)thread_table.size())
            return nullptr;
        return thread_table[tid];
    }

    void Thread::send_signal(int signal) {
        pending_signals |= 1 << signal;
        enqueue_thread(this, signal);
    }

    bool Thread::has_pending_signals() {
        return pending_signals & ~signal_mask;
    }

    void Process::send_process_group_signal(int signal) {
        sched::Process *target;
        LIST_FOR_EACH(target, &process_group_leader->process_group_list, process_group_list)
            target->get_main_thread()->send_signal(signal);
        process_group_leader->get_main_thread()->send_signal(signal);
    }

    Thread* new_kernel_thread(void (*func)(), bool enqueue) {
        Thread *thread = new Thread(kernel_process, allocate_tid());

        void *kernel_stack = klib::malloc(kernel_stack_size);
        memset(kernel_stack, 0, kernel_stack_size);
        thread->user_stack = (uptr)kernel_stack + kernel_stack_size;
        thread->saved_user_stack = thread->user_stack;

        thread->running_on = 0;
        thread->gpr_state.cs = u64(cpu::GDTSegment::KERNEL_CODE_64);
        thread->gpr_state.ds = u64(cpu::GDTSegment::KERNEL_DATA_64);
        thread->gpr_state.es = u64(cpu::GDTSegment::KERNEL_DATA_64);
        thread->gpr_state.ss = u64(cpu::GDTSegment::KERNEL_DATA_64);
        thread->gpr_state.rflags = 0x202; // only set the interrupt flag 
        thread->gpr_state.rip = (uptr)func;
        thread->gpr_state.rsp = thread->user_stack;

        if (enqueue) {
            thread->state = Thread::READY;
            sched_list_head.add_before(&thread->sched_link);
        }

        return thread;
    }

    void Thread::init_user(uptr entry, uptr new_stack) {
        if (!kernel_stack) {
            void *new_kernel_stack = klib::malloc(kernel_stack_size);
            memset(new_kernel_stack, 0, kernel_stack_size);
            kernel_stack = (uptr)new_kernel_stack + kernel_stack_size;
            saved_kernel_stack = kernel_stack;
        }

        running_on = 0;
        gpr_state = cpu::InterruptState();
        gpr_state.cs = u64(cpu::GDTSegment::USER_CODE_64) | 3;
        gpr_state.ds = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        gpr_state.es = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        gpr_state.ss = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        gpr_state.rflags = 0x202;
        gpr_state.rip = entry;
        gs_base = (uptr)cpu::get_current_cpu();
        fs_base = 0;

        user_stack = new_stack;
        gpr_state.rsp = user_stack;
        saved_user_stack = user_stack;
    }

    static isize user_exec(Process *process, vfs::VNode *elf_file, char **argv, char **envp, int argv_len, int envp_len) {
        asm volatile("cli");

        Thread *thread = process->get_main_thread();

        isize err = 0;
        auto *old_pagemap = process->pagemap;
        auto old_mmap_anon_base = process->mmap_anon_base;
        defer {
            if (err < 0) {
                process->pagemap = old_pagemap;
                process->mmap_anon_base = old_mmap_anon_base;
                old_pagemap->activate();
            }
        };

        process->pagemap = new vmm::Pagemap();
        process->pagemap->pml4 = (u64*)(pmm::alloc_pages(1) + vmm::hhdm);
        memset(process->pagemap->pml4, 0, 0x1000);
        process->pagemap->map_kernel();
        process->pagemap->range_list.init();
        process->mmap_anon_base = 0;

        process->pagemap->activate();

        elf::Auxval auxv {}, ld_auxv {};
        char *ld_path = nullptr;
        defer { if (ld_path) delete[] ld_path; };
        if (err = elf::load(process->pagemap, elf_file, 0x400000, &ld_path, &auxv, &process->mmap_anon_base); err < 0)
            return err;

        if (ld_path) {
            vfs::VNode *ld_file;
            {
                vfs::Entry *starting_point = ld_path[0] != '/' ? process->cwd : nullptr;
                auto *entry = vfs::path_to_entry(ld_path, starting_point);
                if (entry->vnode == nullptr)
                    return err = -ENOENT;
                if (entry->vnode->type == vfs::NodeType::DIRECTORY)
                    return err = -EISDIR;
                ld_file = entry->vnode;
            }

            if (isize err = elf::load(process->pagemap, ld_file, 0x40000000, nullptr, &ld_auxv, &process->mmap_anon_base); err < 0) {
                if (err == -ENOEXEC)
                    err = -ELIBBAD;
                return err;
            }
        }

        process->mmap_anon_base = 0x1000000000; // idk anymore

        process->pagemap->map_range(process->mmap_anon_base, user_stack_size, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NO_EXECUTE, vmm::MappedRange::Type::ANONYMOUS);
        process->mmap_anon_base += user_stack_size;
        thread->user_stack = process->mmap_anon_base;

        uptr entry = ld_path ? ld_auxv.at_entry : auxv.at_entry;
        thread->init_user(entry, thread->user_stack);

        cpu::write_fs_base(thread->fs_base);

        u16 default_fcw = 0b1100111111;
        asm volatile("fldcw %0" :: "m" (default_fcw) : "memory");
        u32 default_mxcsr = 0b1111110000000;
        asm volatile("ldmxcsr %0" :: "m" (default_mxcsr) : "memory");
        
        uptr *stack = (uptr*)thread->user_stack;

        for (int i = 0; i < envp_len; i++) {
            usize length = klib::strlen(envp[i]);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, envp[i], length);
        }

        for (int i = 0; i < argv_len; i++) {
            usize length = klib::strlen(argv[i]);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, argv[i], length);
        }
        
        stack = (uptr*)klib::align_down<uptr, 16>((uptr)stack);
        if (((argv_len + envp_len + 1) & 1) != 0) {
            stack--;
        }

        *(--stack) = 0, *(--stack) = 0;
        stack -= 2; stack[0] = AT_SECURE, stack[1] = 0;
        stack -= 2; stack[0] = AT_ENTRY,  stack[1] = auxv.at_entry;
        stack -= 2; stack[0] = AT_PHDR,   stack[1] = auxv.at_phdr;
        stack -= 2; stack[0] = AT_PHENT,  stack[1] = auxv.at_phent;
        stack -= 2; stack[0] = AT_PHNUM,  stack[1] = auxv.at_phnum;

        uptr old_stack = thread->user_stack;

        *(--stack) = 0;
        stack -= envp_len;
        for (int i = 0; i < envp_len; i++) {
            old_stack -= klib::strlen(envp[i]) + 1;
            stack[i] = old_stack;
        }

        *(--stack) = 0;
        stack -= argv_len;
        for (int i = 0; i < argv_len; i++) {
            old_stack -= klib::strlen(argv[i]) + 1;
            stack[i] = old_stack;
        }

        *(--stack) = argv_len;

        thread->user_stack = (uptr)stack;
        thread->gpr_state.rsp = thread->user_stack;
        thread->saved_user_stack = thread->user_stack;
        cpu::get_current_cpu()->user_stack = thread->user_stack;

        for (int i = 0; i < (int)process->file_descriptors.size(); i++) {
            auto &file_descriptor = process->file_descriptors[i];
            if (file_descriptor.get_flags() & FD_CLOEXEC)
                file_descriptor.close(process, i);
        }

        memset(process->signal_actions, 0, sizeof(Process::signal_actions));

        delete old_pagemap;

        return 0;
    }

    Process* new_user_process(vfs::VNode *elf_file, bool enqueue, int argc, char **argv) {
        klib::InterruptLock guard;

        Process *process = new Process();
        Thread *thread = new Thread(process, process->pid);

        if (!thread->kernel_stack) {
            void *kernel_stack = klib::malloc(kernel_stack_size);
            memset(kernel_stack, 0, kernel_stack_size);
            thread->kernel_stack = (uptr)kernel_stack + kernel_stack_size;
            thread->saved_kernel_stack = thread->kernel_stack;
        }

        auto *console_entry = vfs::path_to_entry("/dev/console");
        auto *console = (dev::tty::ConsoleDevNode*)console_entry->vnode;
        ASSERT(console != nullptr);
        process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_RDONLY), 0)); // stdin
        process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_WRONLY), 0)); // stdout
        process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_WRONLY), 0)); // stderr
        process->num_file_descriptors = 3;
        process->first_free_fdnum = 3;
        process->cwd = vfs::get_root_entry();

        console->set_controlling_terminal(process, console);

        char *envp[] = { nullptr };
        auto ret = user_exec(process, elf_file, argv, envp, argc, 0);
        ASSERT(ret >= 0);

        if (enqueue) {
            thread->state = Thread::READY;
            sched_list_head.add_before(&thread->sched_link);
        }

        return process;
    }

    void init() {
        sched_list_head.init();
        kernel_process = new Process();
        kernel_process->pagemap = &vmm::kernel_pagemap;

        idle_thread = new_kernel_thread([] {
            while (true)
                asm volatile("hlt");
        }, false);
        idle_thread->state = Thread::READY;
    }

    void start() {
        sched::timer::apic_timer::oneshot(1000000 / sched_freq);
    }

    void dequeue_thread(Thread *thread, int stop_signal) {
        klib::InterruptLock guard;
        ASSERT(thread->state == Thread::READY || thread->state == Thread::RUNNING);
        thread->sched_link.remove();
        if (stop_signal != -1) {
            thread->state = Thread::STOPPED;
            thread->process->status = W_EXITCODE(stop_signal, 0x7f);
            thread->process->stopped_event.trigger();
            // thread->process->parent->get_main_thread()->send_signal(SIGCHLD);
        } else {
            thread->state = Thread::BLOCKED;
        }
    }

    void enqueue_thread(Thread *thread, int signal) {
        klib::InterruptLock guard;
        if (thread->state != Thread::BLOCKED && thread->state != Thread::STOPPED)
            return;
        if (thread->state == Thread::STOPPED && signal == -1)
            return;
        thread->enqueued_by_signal = signal;
        sched_list_head.add_before(&thread->sched_link);
        if (thread->state == Thread::BLOCKED)
            thread->state = Thread::READY;
    }

    void terminate_thread(Thread *thread, int terminate_signal) {
        klib::InterruptLock guard;
        ASSERT(thread->state == Thread::READY || thread->state == Thread::RUNNING);
        thread->sched_link.remove();
        thread->state = Thread::ZOMBIE;
        if (terminate_signal != -1)
            thread->process->status = W_EXITCODE(0, terminate_signal);
        thread->process->zombie_event.trigger();
        // thread->process->parent->get_main_thread()->send_signal(SIGCHLD);
    }

    [[noreturn]] void terminate_self() {
        klib::InterruptLock guard;
        terminate_thread(cpu::get_current_thread());
        yield();
        klib::unreachable();
    }

    void yield() {
        klib::InterruptLock guard;

        Thread *thread = cpu::get_current_thread();
        thread->yield_await.lock();

        reschedule_self();

        cpu::toggle_interrupts(true);
        thread->yield_await.lock();
        thread->yield_await.unlock();
    }

    void reschedule_self() {
        timer::apic_timer::self_interrupt();
    }

    usize scheduler_isr(void *priv, cpu::InterruptState *gpr_state) {
        cpu::CPU *cpu = cpu::get_current_cpu();
        Thread *current_thread = cpu->running_thread;
    retry:
        if (current_thread) {
            current_thread->yield_await.unlock();

            // copy the saved registers into the current thread
            memcpy(&current_thread->gpr_state, gpr_state, sizeof(cpu::InterruptState));
            current_thread->gs_base = cpu::read_kernel_gs_base(); // this was the regular gs base before the swapgs of the interrupt (if it was a kernel thread then the kernel gs base is the same anyway)
            current_thread->fs_base = cpu::read_fs_base();
            current_thread->saved_user_stack = cpu->user_stack;
            current_thread->saved_kernel_stack = cpu->kernel_stack;
            if (current_thread->state == Thread::RUNNING)
                current_thread->state = Thread::READY;
        }

        // switch to the next thread
        if (current_thread && current_thread->sched_link.next && current_thread->sched_link.next != &sched_list_head)
            current_thread = LIST_ENTRY(current_thread->sched_link.next, Thread, sched_link);
        else if (!sched_list_head.is_empty())
            current_thread = LIST_ENTRY(sched_list_head.next, Thread, sched_link);
        else
            current_thread = idle_thread;

        ASSERT(current_thread->state != Thread::BLOCKED && current_thread->state != Thread::ZOMBIE);

        cpu->user_stack = current_thread->saved_user_stack;
        cpu->kernel_stack = current_thread->saved_kernel_stack;

        if ((current_thread->gpr_state.cs & 3) == 3) // user thread
            cpu::write_kernel_gs_base(current_thread->gs_base); // will be swapped to be the regular gs base
        else
            cpu::write_kernel_gs_base((u64)cpu);

        cpu->running_thread = current_thread;
        cpu::write_fs_base(current_thread->fs_base);

        current_thread->process->pagemap->activate();

        if (current_thread->enqueued_by_signal == SIGCONT)
            if (current_thread->state == Thread::STOPPED)
                current_thread->state = Thread::READY;

        if ((current_thread->gpr_state.cs & 3) == 3) {
            if (current_thread->entering_signal)
                userland::dispatch_pending_signal(current_thread);
            else if (current_thread->exiting_signal)
                userland::return_from_signal(current_thread);
        }

        if (current_thread->state == Thread::ZOMBIE || current_thread->state == Thread::STOPPED)
            goto retry;
        current_thread->state = Thread::RUNNING;

        // load the new thread's registers
        memcpy(gpr_state, &current_thread->gpr_state, sizeof(cpu::InterruptState));

        return 1000000 / sched_freq;
    }

    [[noreturn]] void syscall_exit(int status) {
#if SYSCALL_TRACE
        klib::printf("exit(%d)\n", status);
#endif
        cpu::get_current_thread()->process->status = W_EXITCODE(status, 0);
        terminate_self();
    }
    
    isize syscall_fork(cpu::syscall::SyscallState *state) {
#if SYSCALL_TRACE
        klib::printf("fork()\n");
#endif
        // klib::printf("free pages before fork: %lu\n", pmm::stats.total_free_pages);

        auto *cpu = cpu::get_current_cpu();
        Thread *old_thread = cpu->running_thread;
        Process *old_process = old_thread->process;

        Process *new_process = new Process();
        Thread *new_thread = new Thread(new_process, new_process->pid);
        new_thread->running_on = 0;

        new_process->pagemap = old_process->pagemap->fork();
        new_process->mmap_anon_base = old_process->mmap_anon_base;

        memcpy(&new_thread->gpr_state, state, sizeof(cpu::syscall::SyscallState)); // the top part of the syscall state and the interrupt state are the same
        new_thread->gpr_state.cs = u64(cpu::GDTSegment::USER_CODE_64) | 3;
        new_thread->gpr_state.ds = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.es = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.ss = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.rflags = state->r11;
        new_thread->gpr_state.rip = state->rcx;
        new_thread->gpr_state.rsp = cpu->user_stack;
        new_thread->gpr_state.rax = 0; // return value of the syscall for the new thread
        new_thread->gs_base = cpu::read_kernel_gs_base(); // is actually the thread's gs base
        new_thread->fs_base = cpu::read_fs_base();

        new_thread->user_stack = old_thread->user_stack;
        new_thread->saved_user_stack = cpu->user_stack;

        void *kernel_stack = klib::malloc(kernel_stack_size);
        memset(kernel_stack, 0, kernel_stack_size);
        new_thread->kernel_stack = (uptr)kernel_stack + kernel_stack_size;
        new_thread->saved_kernel_stack = new_thread->kernel_stack;

        new_process->cwd = old_process->cwd;
        for (auto &old_file_descriptor : old_process->file_descriptors) {
            if (old_file_descriptor.get_description())
                new_process->file_descriptors.push_back(old_file_descriptor.duplicate());
            else
                new_process->file_descriptors.emplace_back();
        }
        new_process->num_file_descriptors = old_process->num_file_descriptors;
        new_process->first_free_fdnum = old_process->first_free_fdnum;

        memcpy(new_process->signal_actions, old_process->signal_actions, sizeof(Process::signal_actions));

        new_process->parent = old_process;
        old_process->children_list.add_before(&new_process->sibling_link);
        new_process->process_group_leader = old_process->process_group_leader;
        old_process->process_group_list.add_before(&new_process->process_group_list);
        new_process->session_leader = old_process->session_leader;

        new_thread->state = Thread::READY;
        sched_list_head.add_before(&new_thread->sched_link);

        return new_thread->tid;
    }

    isize syscall_execve(cpu::syscall::SyscallState *state, const char *path, const char **argv, const char **envp) {
#if SYSCALL_TRACE
        klib::printf("execve(\"%s\")\n", path);
#endif
        Thread *thread = cpu::get_current_thread();
        Process *process = thread->process;

        vfs::VNode *elf_file;
        {
            vfs::Entry *starting_point = path[0] != '/' ? process->cwd : nullptr;
            auto *entry = vfs::path_to_entry(path, starting_point);
            if (entry->vnode == nullptr)
                return -ENOENT;
            if (entry->vnode->type != vfs::NodeType::REGULAR)
                return -EACCES;
            elf_file = entry->vnode;
        }

        int envp_len;
        for (envp_len = 0; envp[envp_len] != NULL; envp_len++) {}
        char **envp_copy = new char*[envp_len + 1];
        envp_copy[envp_len] = nullptr;
        for (int i = 0; i < envp_len; i++) {
            usize length = klib::strlen(envp[i]);
            envp_copy[i] = new char[length + 1];
            memcpy(envp_copy[i], envp[i], length + 1);
        }

        int argv_len;
        for (argv_len = 0; argv[argv_len] != NULL; argv_len++) {}
        char **argv_copy = new char*[argv_len + 1];
        argv_copy[argv_len] = nullptr;
        for (int i = 0; i < argv_len; i++) {
            usize length = klib::strlen(argv[i]);
            argv_copy[i] = new char[length + 1];
            memcpy(argv_copy[i], argv[i], length + 1);
        }

        defer {
            for (int i = 0; i < argv_len; i++)
                delete[] argv_copy[i];
            delete[] argv_copy;
            for (int i = 0; i < envp_len; i++)
                delete[] envp_copy[i];
            delete[] envp_copy;
        };

        auto ret = user_exec(process, elf_file, argv_copy, envp_copy, argv_len, envp_len);
        if (ret < 0)
            return ret;

        *state = cpu::syscall::SyscallState();
        state->rcx = thread->gpr_state.rip;
        state->r11 = thread->gpr_state.rflags;

        return 0;
    }

    void syscall_set_fs_base(uptr value) {
#if SYSCALL_TRACE
        klib::printf("set_fs_base(%#lX)\n", value);
#endif
        cpu::write_fs_base(value);
    }

    isize syscall_waitpid(int pid, int *status, int options) {
#if SYSCALL_TRACE
        klib::printf("waitpid(%d, %#lX, %d)\n", pid, (uptr)status, options);
#endif
        if (options & ~(WNOHANG | WUNTRACED | WCONTINUED))
            klib::printf("waitpid: ignoring options %#X\n", options);

        Process *current_process = cpu::get_current_thread()->process;
        Process *target_child = nullptr;

        klib::Vector<Process*> target_children;
        klib::Vector<Event*> events;

        auto add_child = [&] (Process *child) {
            target_children.push_back(child);
            events.push_back(&child->zombie_event);
            if (options & WUNTRACED) {
                target_children.push_back(child);
                events.push_back(&child->stopped_event);
            }
            if (options & WCONTINUED) {
                target_children.push_back(child);
                events.push_back(&child->continued_event);
            }
        };

        if (pid == -1) {
            if (current_process->children_list.is_empty())
                return -ECHILD;

            Process *child;
            LIST_FOR_EACH(child, &current_process->children_list, sibling_link)
                add_child(child);
        } else if (pid > 0) {
            auto &thread_table = get_thread_table();
            if (pid >= (int)thread_table.size())
                return -ECHILD;

            Process *child = thread_table[pid]->process;
            if (child->parent != current_process)
                return -ECHILD;

            add_child(child);
        } else {
            klib::printf("waitpid: unhandled pid %d\n", pid);
            return 0;
        }

        isize which = Event::await(events, options & WNOHANG);
        if (which < 0)
            return which;

        target_child = target_children[which];
        int child_pid = target_child->pid;

        if (status)
            *status = target_child->status;

        if (!WIFSTOPPED(target_child->status) && !WIFCONTINUED(target_child->status)) {
            // klib::printf("deleting child %d\n", target_child->pid);
            target_child->sibling_link.remove();
            delete target_child;
        }

        return child_pid;
    }

    isize syscall_uname(struct utsname *buf) {
#if SYSCALL_TRACE
        klib::printf("uname(%#lX)\n", (uptr)buf);
#endif
        klib::strncpy(buf->sysname, "FishOS", sizeof(buf->sysname));
        klib::strncpy(buf->nodename, "fishpc", sizeof(buf->nodename));
        klib::strncpy(buf->release, "0.0.1", sizeof(buf->release));
        klib::strncpy(buf->version, __DATE__ " " __TIME__, sizeof(buf->version));
        klib::strncpy(buf->machine, "x86_64", sizeof(buf->machine));
        return 0;
    }

    isize syscall_thread_spawn(void *entry, void *stack) {
#if SYSCALL_TRACE
        klib::printf("thread_spawn(%#lX, %#lX)\n", (uptr)entry, (uptr)stack);
#endif
        Process *process = cpu::get_current_thread()->process;
        Thread *new_thread = new Thread(process, allocate_tid());
        new_thread->init_user((uptr)entry, (uptr)stack);
        enqueue_thread(new_thread);
        return new_thread->tid;
    }

    [[noreturn]] void syscall_thread_exit() {
#if SYSCALL_TRACE
        klib::printf("thread_exit()\n");
#endif
        terminate_self();
    }
}
