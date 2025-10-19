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
    constexpr usize kernel_stack_size = 64 * 1024;
    constexpr usize user_stack_size = 8 * 1024 * 1024;
    static klib::ListHead sched_list_head;
    static Process *kernel_process;
    static Thread *idle_thread;
    static Process *init_process;

    static usize num_threads = 0, first_free_tid = 2;

    static klib::Vector<Thread*>& get_thread_table() {
        static klib::Vector<Thread*> thread_table;
        return thread_table;
    }

    static int allocate_tid() {
        auto &thread_table = get_thread_table();
        if (thread_table.size() == 0) {
            thread_table.push_back(nullptr);
            thread_table.push_back(nullptr);
        }
        num_threads++;
        for (usize i = first_free_tid; i < thread_table.size(); i++) {
            if (thread_table[i] == nullptr) {
                first_free_tid = i + 1;
                return i;
            }
        }
        first_free_tid = thread_table.size() + 1;
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
        process->num_living_threads++;
        get_thread_table()[tid] = this;
        state = BLOCKED;
    }

    Thread::~Thread() {
        ASSERT(state == ZOMBIE);
        get_thread_table()[tid] = nullptr;
        thread_link.remove();
        clear_listeners();
        if (extended_state)
            klib::free(extended_state);
    }

    Process::Process() {
        pid = allocate_tid();
        thread_list.init();
        children_list.init();
    }

    Process::~Process() {
        ASSERT(is_zombie);
        sibling_link.remove();
        group_link.remove();

        Thread *thread;
        LIST_FOR_EACH_SAFE(thread, &this->thread_list, thread_link) {
            if (thread->state != Thread::ZOMBIE)
                terminate_thread(thread);
            delete thread;
        }

        delete pagemap;
    }

    Thread* Thread::get_from_tid(int tid) {
        auto &thread_table = get_thread_table();
        if (tid <= 0 || tid >= (int)thread_table.size())
            return nullptr;
        return thread_table[tid];
    }

    void Thread::send_signal(int signal) {
        pending_signals |= 1 << (signal - 1);
        enqueue_thread(this, signal);
    }

    bool Thread::has_pending_signals() {
        return pending_signals & ~signal_mask;
    }

    void Thread::clear_listeners() {
        for (auto *listener : listeners) {
            if (!listener->listener_link.is_invalid()) {
                listener->listener_link.remove();
                listener->event->num_listeners--;
            }
            delete listener;
        }
        listeners.clear();
    }

    ProcessGroup::ProcessGroup(Session *session, Process *leader_process) : session(session), leader_process(leader_process) {
        process_list.init();
        session->process_group_list.add_before(&session_link);
    }

    void ProcessGroup::add_process(Process *process) {
        if (process->group)
            process->group_link.remove();
        this->process_list.add_before(&process->group_link);
        process->group = this;
    }

    void ProcessGroup::send_signal(int signal) {
        Process *target;
        LIST_FOR_EACH(target, &process_list, group_link) {
            ASSERT(target->group == this);
            target->get_main_thread()->send_signal(signal);
        }
    }

    void Process::set_parent(Process *new_parent) {
        this->parent = new_parent;
        if (!this->sibling_link.is_invalid()) this->sibling_link.remove();
        new_parent->children_list.add_before(&this->sibling_link);

        new_parent->group->add_process(this);
    }

    void Process::zombify(int terminate_signal) {
        klib::InterruptLock interrupt_guard;

        is_zombie = true;
        if (terminate_signal != -1)
            status = W_EXITCODE(0, terminate_signal);

        Process *child;
        LIST_FOR_EACH_SAFE(child, &children_list, sibling_link) {
            child->set_parent(init_process);
        }

        for (usize i = 0; i < file_descriptors.size(); i++)
            if (file_descriptors[i].get_description() != nullptr)
                file_descriptors[i].close(this, i);

        zombie_event.trigger();
        parent->get_main_thread()->send_signal(SIGCHLD);
    }

    void Process::print_file_descriptors() {
        klib::PrintGuard print_guard;
        klib::printf_unlocked("Printing process %d file descriptors:\n", this->pid);
        for (usize i = 0; i < file_descriptors.size(); i++) {
            if (auto *description = file_descriptors[i].get_description()) {
                klib::printf_unlocked("FD %lu: ", i);
                if (description->entry)
                    description->entry->print_path(klib::putchar);
                else
                    klib::printf_unlocked("(null entry)");
                klib::putchar('\n');
            }
        }
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

        extended_state = klib::aligned_alloc(cpu::extended_state_size, 64);
        memset(extended_state, 0, cpu::extended_state_size);
        {
            klib::InterruptLock interrupt_guard;
            Thread *active_thread = cpu::get_current_thread();

            if (active_thread->extended_state)
                cpu::save_extended_state(active_thread->extended_state);
            cpu::restore_extended_state(extended_state);

            u16 default_fcw = 0b1100111111;
            asm volatile("fldcw %0" : : "m" (default_fcw) : "memory");
            u32 default_mxcsr = 0b1111110000000;
            asm volatile("ldmxcsr %0" : : "m" (default_mxcsr) : "memory");

            cpu::save_extended_state(extended_state);
            if (active_thread->extended_state)
                cpu::restore_extended_state(active_thread->extended_state);
        }

        user_stack = new_stack;
        gpr_state.rsp = user_stack;
        saved_user_stack = user_stack;
    }

    static isize user_exec(Process *process, const char *path, char **argv, char **envp, int argv_len, int envp_len) {
        vfs::VNode *elf_file;
        {
            vfs::Entry *starting_point = path[0] != '/' ? process->cwd : nullptr;
            auto *entry = vfs::path_to_entry(path, starting_point);
            if (entry->vnode == nullptr)
                return -ENOENT;
            if (entry->vnode->node_type != vfs::NodeType::REGULAR)
                return -EACCES;
            elf_file = entry->vnode;
        }

        decltype(process->name) old_name;
        klib::strncpy(old_name, process->name, sizeof(process->name));
        klib::strncpy(process->name, path, sizeof(process->name));

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
                klib::strncpy(process->name, old_name, sizeof(process->name));
            }
        };

        process->pagemap = new mem::Pagemap();
        process->pagemap->pml4 = (u64*)(pmm::alloc_pages(1) + mem::hhdm);
        memset(process->pagemap->pml4, 0, 0x1000);
        process->pagemap->map_kernel();
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
                if (entry->vnode->node_type == vfs::NodeType::DIRECTORY)
                    return err = -EISDIR;
                ld_file = entry->vnode;
            }

            if (err = elf::load(process->pagemap, ld_file, 0x40000000, nullptr, &ld_auxv, &process->mmap_anon_base); err < 0) {
                if (err == -ENOEXEC)
                    err = -ELIBBAD;
                return err;
            }
        }

        process->mmap_anon_base = 0x1000000000; // idk anymore

        process->pagemap->map_anonymous(process->mmap_anon_base, user_stack_size, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NO_EXECUTE);
        process->mmap_anon_base += user_stack_size;
        thread->user_stack = process->mmap_anon_base;
        process->mmap_anon_base += 0x10000; // guard

        uptr entry = ld_path ? ld_auxv.at_entry : auxv.at_entry;
        thread->init_user(entry, thread->user_stack);

        cpu::write_fs_base(thread->fs_base);

        uptr *stack = (uptr*)thread->user_stack;

        usize path_length = klib::strlen(path);
        stack = (uptr*)((uptr)stack - path_length - 1);
        uptr *execfn = stack;
        memcpy(execfn, path, path_length);

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
        
        stack = (uptr*)klib::align_down((uptr)stack, 16);
        if (((argv_len + envp_len + 1) & 1) != 0) {
            stack--;
        }

        *(--stack) = 0, *(--stack) = 0;
        stack -= 2; stack[0] = AT_SECURE, stack[1] = 0;
        stack -= 2; stack[0] = AT_ENTRY,  stack[1] = auxv.at_entry;
        stack -= 2; stack[0] = AT_PHDR,   stack[1] = auxv.at_phdr;
        stack -= 2; stack[0] = AT_PHENT,  stack[1] = auxv.at_phent;
        stack -= 2; stack[0] = AT_PHNUM,  stack[1] = auxv.at_phnum;
        stack -= 2; stack[0] = AT_PAGESZ, stack[1] = 0x1000;
        stack -= 2; stack[0] = AT_EXECFN, stack[1] = (uptr)execfn;

        uptr old_stack = thread->user_stack;
        old_stack -= path_length + 1;

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
        thread->signal_alt_stack.ss_sp = nullptr;
        thread->signal_alt_stack.ss_flags = SS_DISABLE;
        thread->signal_alt_stack.ss_size = 0;

        klib::strncpy(thread->name, process->name, sizeof(thread->name));

        process->has_performed_execve = true;

        delete old_pagemap;

        return 0;
    }

    Process* create_init_process(const char *path, int argc, char **argv) {
        klib::InterruptLock guard;

        init_process = new Process();
        init_process->pid = 1;

        auto *session = new Session();
        init_process->group = new ProcessGroup(session, init_process);
        session->leader_group = init_process->group;

        Thread *thread = new Thread(init_process, init_process->pid);

        if (!thread->kernel_stack) {
            void *kernel_stack = klib::malloc(kernel_stack_size);
            memset(kernel_stack, 0, kernel_stack_size);
            thread->kernel_stack = (uptr)kernel_stack + kernel_stack_size;
            thread->saved_kernel_stack = thread->kernel_stack;
        }

        auto *console_entry = vfs::path_to_entry("/dev/console");
        auto *console = (dev::tty::ConsoleDevNode*)console_entry->vnode;
        ASSERT(console != nullptr);
        init_process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_RDONLY), 0)); // stdin
        init_process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_WRONLY), 0)); // stdout
        init_process->file_descriptors.push_back(vfs::FileDescriptor(new vfs::FileDescription(console_entry, O_WRONLY), 0)); // stderr
        init_process->num_file_descriptors = 3;
        init_process->first_free_fdnum = 3;
        init_process->cwd = vfs::get_root_entry();

        console->set_controlling_terminal(init_process, console);

        char *envp[] = { nullptr };
        auto ret = user_exec(init_process, path, argv, envp, argc, 0);
        ASSERT(ret >= 0);

        thread->state = Thread::READY;
        sched_list_head.add_before(&thread->sched_link);
        return init_process;
    }

    void init() {
        sched_list_head.init();
        kernel_process = new Process();
        kernel_process->pagemap = &mem::vmm->kernel_pagemap;

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
            thread->process->parent->get_main_thread()->send_signal(SIGCHLD);
        } else {
            thread->state = Thread::BLOCKED;
        }
    }

    void enqueue_thread(Thread *thread, int signal) {
        klib::InterruptLock guard;
        if (thread->state != Thread::BLOCKED && thread->state != Thread::STOPPED)
            return;
        if (thread->state == Thread::STOPPED && signal != SIGCONT)
            return;
        thread->enqueued_by_signal = signal;
        sched_list_head.add_before(&thread->sched_link);
        if (thread->state == Thread::BLOCKED)
            thread->state = Thread::READY;
    }

    void terminate_thread(Thread *thread, int terminate_signal) {
        klib::InterruptLock guard;
        if (thread->state == Thread::ZOMBIE)
            return;
        if (thread->state == Thread::READY || thread->state == Thread::RUNNING)
            thread->sched_link.remove();
        thread->state = Thread::ZOMBIE;

        thread->process->num_living_threads--;
        if (thread->process->num_living_threads == 0)
            thread->process->zombify(terminate_signal);
    }

    void terminate_process(Process *process, int terminate_signal) {
        klib::InterruptLock guard;
        Thread *thread;
        LIST_FOR_EACH_SAFE(thread, &process->thread_list, thread_link) {
            terminate_thread(thread, terminate_signal);
        }
    }

    [[noreturn]] void terminate_self(bool whole_process) {
        klib::InterruptLock guard;
        if (whole_process)
            terminate_process(cpu::get_current_thread()->process);
        else
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
            if (current_thread->extended_state)
                cpu::save_extended_state(current_thread->extended_state);
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

        auto *pagemap = current_thread->process->pagemap;
        if (pagemap != &mem::vmm->kernel_pagemap) // kernel pages are global so no need to switch
            pagemap->activate();

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
        if (current_thread->extended_state)
            cpu::restore_extended_state(current_thread->extended_state);

        return 1000000 / sched_freq;
    }

    [[noreturn]] void syscall_exit(int status) {
        log_syscall("exit(%d)\n", status);
        cpu::get_current_thread()->process->status = W_EXITCODE(status, 0);
        terminate_self(true);
    }
    
    isize syscall_fork(cpu::syscall::SyscallState *state) {
        log_syscall("fork()\n");
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
        new_thread->extended_state = klib::aligned_alloc(cpu::extended_state_size, 64);
        memcpy(new_thread->extended_state, old_thread->extended_state, cpu::extended_state_size);

        new_thread->user_stack = old_thread->user_stack;
        new_thread->saved_user_stack = cpu->user_stack;

        void *kernel_stack = klib::malloc(kernel_stack_size);
        memset(kernel_stack, 0, kernel_stack_size);
        new_thread->kernel_stack = (uptr)kernel_stack + kernel_stack_size;
        new_thread->saved_kernel_stack = new_thread->kernel_stack;

        new_thread->signal_mask = old_thread->signal_mask;
        new_thread->signal_alt_stack = old_thread->signal_alt_stack;

        new_process->cwd = old_process->cwd;
        for (auto &old_file_descriptor : old_process->file_descriptors) {
            if (old_file_descriptor.get_description())
                new_process->file_descriptors.push_back(old_file_descriptor.duplicate(true));
            else
                new_process->file_descriptors.emplace_back();
        }
        new_process->num_file_descriptors = old_process->num_file_descriptors;
        new_process->first_free_fdnum = old_process->first_free_fdnum;

        new_process->signal_entry = old_process->signal_entry;
        memcpy(new_process->signal_actions, old_process->signal_actions, sizeof(Process::signal_actions));

        klib::strncpy(new_process->name, old_process->name, sizeof(new_process->name));
        klib::strncpy(new_thread->name, new_process->name, sizeof(new_thread->name));

        new_process->set_parent(old_process);

        new_thread->state = Thread::READY;
        sched_list_head.add_before(&new_thread->sched_link);

        return new_thread->tid;
    }

    isize syscall_execve(cpu::syscall::SyscallState *state, const char *path, const char **argv, const char **envp) {
        log_syscall("execve(\"%s\")\n", path);
        Thread *thread = cpu::get_current_thread();
        Process *process = thread->process;

        usize path_length = klib::strlen(path);
        char *path_copy = new char[path_length + 1];
        memcpy(path_copy, path, path_length + 1);

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
            delete[] path_copy;
            for (int i = 0; i < argv_len; i++)
                delete[] argv_copy[i];
            delete[] argv_copy;
            for (int i = 0; i < envp_len; i++)
                delete[] envp_copy[i];
            delete[] envp_copy;
        };

        auto ret = user_exec(process, path_copy, argv_copy, envp_copy, argv_len, envp_len);
        if (ret < 0)
            return ret;

        *state = cpu::syscall::SyscallState();
        state->rcx = thread->gpr_state.rip;
        state->r11 = thread->gpr_state.rflags;

        return 0;
    }

    void syscall_set_fs_base(uptr value) {
        log_syscall("set_fs_base(%#lX)\n", value);
        cpu::write_fs_base(value);
    }

    isize syscall_waitpid(int pid, int *status, int options) {
        log_syscall("waitpid(%d, %#lX, %d)\n", pid, (uptr)status, options);
        if (options & ~(WNOHANG | WUNTRACED | WCONTINUED))
            klib::printf("waitpid: ignoring options %#X\n", options);

        Process *current_process = cpu::get_current_thread()->process;
        Process *target_child = nullptr;

        klib::Vector<Process*> target_children;
        klib::Vector<Event*> events;
        isize which_event = 0;

        auto add_child = [&] (Process *child) -> bool {
            if (child->is_zombie) return true;

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
            return false;
        };

        if (pid == -1) {
            if (current_process->children_list.is_empty())
                return -ECHILD;

            Process *child;
            LIST_FOR_EACH(child, &current_process->children_list, sibling_link) {
                if (add_child(child)) {
                    target_child = child;
                    goto found_child;
                }
            }
        } else if (pid > 0) {
            auto &thread_table = get_thread_table();
            if (pid >= (int)thread_table.size())
                return -ECHILD;

            Thread *target_thread = thread_table[pid];
            if (target_thread == nullptr) return -ECHILD;

            Process *child = target_thread->process;
            if (child->parent != current_process) return -ECHILD;

            if (add_child(child)) {
                target_child = child;
                goto found_child;
            }
        } else {
            klib::printf("waitpid: unhandled pid %d\n", pid);
            return 0;
        }

        which_event = Event::wait(events, options & WNOHANG);
        if (which_event == -EWOULDBLOCK)
            return 0;
        if (which_event < 0)
            return which_event;
        target_child = target_children[which_event];

    found_child:
        int child_pid = target_child->pid;

        if (status)
            *status = target_child->status;

        if (target_child->is_zombie)
            delete target_child;

        return child_pid;
    }

    isize syscall_thread_spawn(void *entry, void *stack) {
        log_syscall("thread_spawn(%#lX, %#lX)\n", (uptr)entry, (uptr)stack);
        Process *process = cpu::get_current_thread()->process;
        Thread *new_thread = new Thread(process, allocate_tid());
        new_thread->init_user((uptr)entry, (uptr)stack);
        klib::strncpy(new_thread->name, process->name, sizeof(new_thread->name));
        enqueue_thread(new_thread);
        return new_thread->tid;
    }

    [[noreturn]] void syscall_thread_exit() {
        log_syscall("thread_exit()\n");
        terminate_self(false);
    }

    mode_t syscall_umask(mode_t mode) {
        log_syscall("umask(%u)\n", mode);
        Process *process = cpu::get_current_thread()->process;
        mode_t old = process->umask;
        process->umask = mode;
        return old;
    }

    isize syscall_thread_getname(int tid, char *name, usize size) {
        log_syscall("thread_getname(%d, %#lX, %lu)\n", tid, (uptr)name, size);
        auto *thread = Thread::get_from_tid(tid);
        klib::strncpy(name, thread->name, klib::min(size, sizeof(thread->name)));
        name[size - 1] = 0;
        return 0;
    }

    isize syscall_thread_setname(int tid, const char *name) {
        log_syscall("thread_setname(%d, \"%s\")\n", tid, name);
        auto *thread = Thread::get_from_tid(tid);
        klib::strncpy(thread->name, name, sizeof(thread->name));
        thread->name[sizeof(thread->name) - 1] = 0;
        return 0;
    }

    void syscall_sched_yield() {
        log_syscall("sched_yield()\n");
        yield();
    }
}
