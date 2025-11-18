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
#include <userland/futex.hpp>
#include <gfx/framebuffer.hpp>
#include <dev/tty/console.hpp>
#include <fs/procfs.hpp>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <asm/prctl.h>
#include <linux/prctl.h>

namespace sched {
    constexpr usize sched_freq = 200; // Hz
    constexpr usize kernel_stack_size = 64 * 1024;
    constexpr usize user_stack_size = 8 * 1024 * 1024;
    constexpr usize user_binary_base = 0x560000000000;
    constexpr usize user_linker_base = 0x7e0000000000;
    constexpr usize user_mmap_base = 0x7f0000000000;

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

    Process::Process() :
        zombie_event("Process::zombie_event"),
        stopped_event("Process::stopped_event"),
        continued_event("Process::continued_event")
    {
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

        procfs_dir->remove();
    }

    Thread* Thread::get_from_tid(int tid) {
        auto &thread_table = get_thread_table();
        if (tid <= 0 || tid >= (int)thread_table.size())
            return nullptr;
        return thread_table[tid];
    }

    void Thread::send_signal(int signal) {
        if (signal >= 32)
            klib::printf("send_signal: real-time signals not supported correctly (signal %d)\n", signal);
        pending_signals |= userland::get_signal_bit(signal);
        enqueue_thread(this, signal);
    }

    bool Thread::has_pending_signals() {
        return pending_signals & ~signal_mask;
    }

    void Thread::clear_listeners() {
        klib::InterruptLock interrupt_guard;
        for (auto &listener : listeners) {
            if (listener.event) {
                listener.listener_link.remove();
                listener.event->num_listeners--;
                listener.event = nullptr;
            }
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

        if (this == init_process)
            panic("Init process died");

        is_zombie = true;
        if (terminate_signal != -1) {
            wait_status = terminate_signal;
            wait_code = CLD_KILLED;
        }

        Process *child;
        LIST_FOR_EACH_SAFE(child, &children_list, sibling_link) {
            child->set_parent(init_process);
        }

        for (usize i = 0; i < file_descriptors.size(); i++)
            if (file_descriptors[i].get_description() != nullptr)
                file_descriptors[i].close(this, i);

        delete pagemap;

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

    Thread* new_kernel_thread(void (*func)(), bool enqueue, const char *name) {
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

        klib::strncpy(thread->name, name, sizeof(thread->name));

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
        vfs::Entry *executable;
        {
            vfs::Entry *starting_point = path[0] != '/' ? process->cwd : nullptr;
            auto *entry = vfs::path_to_entry(path, starting_point);
            if (entry->vnode == nullptr)
                return -ENOENT;
            if (entry->vnode->node_type != vfs::NodeType::REGULAR)
                return -EACCES;
            executable = entry;
        }

        Thread *thread = process->get_main_thread();
        asm volatile("cli");

        decltype(thread->name) old_name;
        klib::strncpy(old_name, thread->name, sizeof(thread->name));
        klib::strncpy(thread->name, executable->name, 16);

        isize err = 0;
        auto *old_pagemap = process->pagemap;
        auto old_mmap_anon_base = process->mmap_anon_base;
        defer {
            if (err < 0 && process->pid != 1) {
                process->pagemap = old_pagemap;
                process->mmap_anon_base = old_mmap_anon_base;
                old_pagemap->activate();
                klib::strncpy(thread->name, old_name, sizeof(thread->name));
            }
        };

        process->pagemap = new mem::Pagemap();
        memset(process->pagemap->pml4, 0, 0x1000);
        process->pagemap->map_kernel();
        process->mmap_anon_base = 0;

        process->pagemap->activate();

        elf::Auxval auxv {}, ld_auxv {};
        char *ld_path = nullptr;
        defer { if (ld_path) delete[] ld_path; };
        char *interpreter_path = nullptr;
        defer { if (interpreter_path) delete[] interpreter_path; };
        char *interpreter_arg = nullptr;
        defer { if (interpreter_arg) delete[] interpreter_arg; };

        if (err = elf::load(process->pagemap, executable->vnode, user_binary_base, &ld_path, &auxv, &process->mmap_anon_base); err < 0) {
            // parse shebang
            char buf[256] = {};
            executable->vnode->read(nullptr, buf, sizeof(buf), 0);
            bool had_newline = false;
            for (usize i = 0; i < sizeof(buf); i++) {
                if (buf[i] == '\n') {
                    had_newline = true;
                    buf[i] = '\0';
                    break;
                }
            }
            if (!had_newline || buf[0] != '#' || buf[1] != '!')
                return err;

            usize i = 2;
            for (; buf[i] == ' '; i++); // skip spaces
            if (!buf[i]) return err;

            char *interpreter_path_start = buf + i;
            for (; buf[i] && buf[i] != ' '; i++); // skip until theres a space
            usize interpreter_path_len = buf + i - interpreter_path_start;
            if (interpreter_path_len == 0)
                return err;

            interpreter_path = new char[interpreter_path_len + 1];
            memcpy(interpreter_path, interpreter_path_start, interpreter_path_len);
            interpreter_path[interpreter_path_len] = '\0';

            char *interpreter_arg_start = buf + i;
            for (; buf[i] && buf[i] != ' '; i++); // skip until theres a space
            usize interpreter_arg_len = buf + i - interpreter_arg_start;

            if (interpreter_arg_len > 0) {
                interpreter_arg = new char[interpreter_arg_len + 1];
                memcpy(interpreter_arg, interpreter_arg_start, interpreter_arg_len);
                interpreter_arg[interpreter_arg_len] = '\0';
            }

            vfs::Entry *starting_point = interpreter_path[0] != '/' ? process->cwd : nullptr;
            auto *entry = vfs::path_to_entry(interpreter_path, starting_point);
            if (entry->vnode == nullptr) return err = -ENOENT;
            if (entry->vnode->node_type != vfs::NodeType::REGULAR) return err = -EACCES;
            executable = entry;

            if (err = elf::load(process->pagemap, executable->vnode, user_binary_base, &ld_path, &auxv, &process->mmap_anon_base); err < 0)
                return err;
        }

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

            if (err = elf::load(process->pagemap, ld_file, user_linker_base, nullptr, &ld_auxv, &process->mmap_anon_base); err < 0) {
                if (err == -ENOEXEC)
                    err = -ELIBBAD;
                return err;
            }
        }

        process->mmap_anon_base = user_mmap_base;

        process->pagemap->map_anonymous(process->mmap_anon_base, user_stack_size, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NO_EXECUTE);
        process->mmap_anon_base += user_stack_size;
        thread->user_stack = process->mmap_anon_base;
        process->mmap_anon_base += 0x10000; // guard

        process->exe = executable;

        uptr entry = ld_path ? ld_auxv.at_entry : auxv.at_entry;
        thread->init_user(entry, thread->user_stack);

        cpu::write_fs_base(thread->fs_base);

        uptr *stack = (uptr*)thread->user_stack;

        stack = (uptr*)((uptr)stack - 16);
        uptr *random_data = stack;
        memset(random_data, 0xAF, 16);

        usize path_length = klib::strlen(path);
        stack = (uptr*)((uptr)stack - path_length - 1);
        uptr *execfn = stack;
        memcpy(execfn, path, path_length);

        for (int i = 0; i < envp_len; i++) {
            usize length = klib::strlen(envp[i]);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, envp[i], length);
        }

        int num_interpreter_args = (interpreter_path ? 1 : 0) + (interpreter_arg ? 1 : 0);
        if (interpreter_path) {
            usize length = klib::strlen(interpreter_path);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, interpreter_path, length);
        }
        if (interpreter_arg) {
            usize length = klib::strlen(interpreter_arg);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, interpreter_arg, length);
        }

        for (int i = 0; i < argv_len; i++) {
            usize length = klib::strlen(argv[i]);
            stack = (uptr*)((uptr)stack - length - 1);
            memcpy(stack, argv[i], length);
        }

        stack = (uptr*)klib::align_down((uptr)stack, 16);
        if (((argv_len + envp_len + num_interpreter_args + 1) & 1) != 0) {
            stack--;
        }

        *(--stack) = 0; *(--stack) = 0;
        stack -= 2; stack[0] = AT_SECURE; stack[1] = 0;
        stack -= 2; stack[0] = AT_ENTRY;  stack[1] = auxv.at_entry;
        stack -= 2; stack[0] = AT_PHDR;   stack[1] = auxv.at_phdr;
        stack -= 2; stack[0] = AT_PHENT;  stack[1] = auxv.at_phent;
        stack -= 2; stack[0] = AT_PHNUM;  stack[1] = auxv.at_phnum;
        stack -= 2; stack[0] = AT_PAGESZ; stack[1] = 0x1000;
        stack -= 2; stack[0] = AT_EXECFN; stack[1] = (uptr)execfn;
        stack -= 2; stack[0] = AT_RANDOM; stack[1] = (uptr)random_data;

        uptr old_stack = thread->user_stack;
        old_stack -= path_length + 1;
        old_stack -= 16;

        *(--stack) = 0;
        stack -= envp_len;
        for (int i = 0; i < envp_len; i++) {
            old_stack -= klib::strlen(envp[i]) + 1;
            stack[i] = old_stack;
        }

        *(--stack) = 0;
        stack -= argv_len + num_interpreter_args;
        if (interpreter_path) {
            old_stack -= klib::strlen(interpreter_path) + 1;
            stack[0] = old_stack;
        }
        if (interpreter_arg) {
            old_stack -= klib::strlen(interpreter_arg) + 1;
            stack[1] = old_stack;
        }

        for (int i = 0; i < argv_len; i++) {
            old_stack -= klib::strlen(argv[i]) + 1;
            stack[i + num_interpreter_args] = old_stack;
        }

        *(--stack) = argv_len + num_interpreter_args;

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

        process->has_performed_execve = true;

        delete old_pagemap;

        return 0;
    }

    Process* create_init_process(const char *path, int argc, char **argv) {
        klib::InterruptLock guard;

        init_process = new Process();
        init_process->pid = 1;
        init_process->parent = init_process;

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

        thread->cred.uids.rid = 0; thread->cred.uids.eid = 0; thread->cred.uids.fsid = 0; thread->cred.uids.sid = 0;
        thread->cred.gids.rid = 0; thread->cred.gids.eid = 0; thread->cred.gids.fsid = 0; thread->cred.gids.sid = 0;

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
        auto ret = user_exec(init_process, path, argv, envp, argc, sizeof(envp) / sizeof(char*) - 1);
        ASSERT(ret >= 0);

        procfs::create_process_dir(init_process);

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
        }, false, "Idle thread");
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
            thread->process->wait_status = stop_signal;
            thread->process->wait_code = CLD_STOPPED;
            thread->process->stopped_event.trigger();
            if (!(thread->process->parent->signal_actions[SIGCHLD].flags & SA_NOCLDSTOP))
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
        if (thread->process->num_living_threads == 0) {
            thread->process->zombify(terminate_signal);
        } else if (thread->clear_child_tid != 0) {
            ASSERT(mem::vmm->active_pagemap == thread->process->pagemap);
            *(pid_t*)thread->clear_child_tid = 0;
            userland::futex_wake((u32*)thread->clear_child_tid, 1);
        }
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
        __atomic_test_and_set(&thread->yield_await, __ATOMIC_ACQUIRE);

        reschedule_self();

        cpu::toggle_interrupts(true);
        while (__atomic_test_and_set(&thread->yield_await, __ATOMIC_ACQUIRE))
            asm volatile("pause");
        __atomic_clear(&thread->yield_await, __ATOMIC_RELEASE);
    }

    void reschedule_self() {
        timer::apic_timer::self_interrupt();
    }

    usize scheduler_isr(void *priv, cpu::InterruptState *gpr_state) {
        cpu::CPU *cpu = cpu::get_current_cpu();
        Thread *current_thread = cpu->running_thread;
    retry:
        if (current_thread) {
            __atomic_clear(&current_thread->yield_await, __ATOMIC_RELEASE);

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

        if (current_thread->enqueued_by_signal == SIGCONT) {
            if (current_thread->state == Thread::STOPPED) {
                current_thread->state = Thread::READY;
                current_thread->process->wait_status = SIGCONT;
                current_thread->process->wait_code = CLD_CONTINUED;
                current_thread->process->continued_event.trigger();
            }
        }

        if ((current_thread->gpr_state.cs & 3) == 3) {
            if (current_thread->entering_signal)
                userland::dispatch_pending_signal(current_thread);
            else if (current_thread->exiting_signal)
                userland::return_from_signal(current_thread);
        }

        if (current_thread->set_child_tid) {
            *(pid_t*)current_thread->set_child_tid = current_thread->tid;
            current_thread->set_child_tid = 0;
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

    void debug_print_threads() {
        klib::InterruptLock interrupt_guard;
        auto &thread_table = get_thread_table();
        for (Thread *thread : thread_table) {
            if (!thread) continue;

            const char *state;
            switch (thread->state) {
            case sched::Thread::READY:
            case sched::Thread::RUNNING: state = "R (running)"; break;
            case sched::Thread::BLOCKED: state = "S (sleeping)"; break;
            case sched::Thread::STOPPED: state = "T (stopped)"; break;
            case sched::Thread::ZOMBIE:  state = "Z (zombie)"; break;
            }

            klib::printf("TID: %3d, State: %s, Name: %s\n", thread->tid, state, thread->name);
            for (Event::Listener &listener : thread->listeners) {
                klib::printf("    Listening to %s\n", listener.event->debug_name);
            }
        }
    }

    [[noreturn]] void syscall_exit(int status) {
        log_syscall("exit(%d)\n", status);
        klib::printf("exit: status is ignored\n");
        terminate_self(false);
    }

    [[noreturn]] void syscall_exit_group(int status) {
        log_syscall("exit_group(%d)\n", status);
        Process *process = cpu::get_current_thread()->process;
        process->wait_status = status;
        process->wait_code = CLD_EXITED;
        terminate_self(true);
    }

    static constexpr usize thread_clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    static constexpr usize supported_generic_clone_flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_PARENT_SETTID | CLONE_SETTLS | CLONE_CLEAR_SIGHAND;

    static isize clone_impl(clone_args *clone_args, usize size) {
        usize flags = clone_args->flags;
        if ((flags & CLONE_SIGHAND) && (flags & CLONE_CLEAR_SIGHAND))
            return -EINVAL;
        if ((flags & CLONE_VM) && (flags & CLONE_VFORK)) {
            klib::printf("clone: vfork is implemented as fork which may be incorrect\n");
            flags &= ~(CLONE_VM | CLONE_VFORK);
        }

        bool is_spawning_thread = false;
        if (flags & thread_clone_flags) {
            is_spawning_thread = true;
            flags &= ~thread_clone_flags;
        }

        if (flags & ~supported_generic_clone_flags) {
            klib::printf("clone: unsupported flags %#lX\n", flags);
            return -EINVAL;
        }

        if (!is_spawning_thread && clone_args->exit_signal != SIGCHLD)
            klib::printf("clone: unsupported exit signal %llu\n", clone_args->exit_signal);

        auto *cpu = cpu::get_current_cpu();
        Thread *old_thread = cpu->running_thread;
        Process *old_process = old_thread->process;
        auto *state = old_thread->syscall_state;

        Thread *new_thread = nullptr;
        Process *new_process = nullptr;
        if (is_spawning_thread) {
            new_thread = new Thread(old_process, allocate_tid());
        } else {
            new_process = new Process();
            new_thread = new Thread(new_process, new_process->pid);
        }

        new_thread->running_on = 0;

        memcpy(&new_thread->gpr_state, state, sizeof(cpu::syscall::SyscallState)); // the top part of the syscall state and the interrupt state are the same
        new_thread->gpr_state.cs = u64(cpu::GDTSegment::USER_CODE_64) | 3;
        new_thread->gpr_state.ds = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.es = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.ss = u64(cpu::GDTSegment::USER_DATA_64) | 3;
        new_thread->gpr_state.rflags = state->r11;
        new_thread->gpr_state.rip = state->rcx;
        new_thread->gpr_state.rax = 0; // return value of the syscall for the new thread
        new_thread->gs_base = cpu::read_kernel_gs_base(); // is actually the thread's gs base
        new_thread->fs_base = cpu::read_fs_base();
        new_thread->extended_state = klib::aligned_alloc(cpu::extended_state_size, 64);
        memcpy(new_thread->extended_state, old_thread->extended_state, cpu::extended_state_size);

        if (clone_args->stack_size > 0) {
            uptr new_stack = clone_args->stack + clone_args->stack_size;
            new_thread->gpr_state.rsp = new_stack;
            new_thread->user_stack = new_stack;
            new_thread->saved_user_stack = new_stack;
        } else {
            new_thread->gpr_state.rsp = cpu->user_stack;
            new_thread->user_stack = old_thread->user_stack;
            new_thread->saved_user_stack = cpu->user_stack;
        }

        void *kernel_stack = klib::malloc(kernel_stack_size);
        memset(kernel_stack, 0, kernel_stack_size);
        new_thread->kernel_stack = (uptr)kernel_stack + kernel_stack_size;
        new_thread->saved_kernel_stack = new_thread->kernel_stack;

        new_thread->signal_mask = old_thread->signal_mask;
        new_thread->signal_alt_stack = old_thread->signal_alt_stack;

        new_thread->cred = old_thread->cred;

        if (new_process) {
            new_process->pagemap = old_process->pagemap->fork();
            new_process->mmap_anon_base = old_process->mmap_anon_base;

            new_process->exe = old_process->exe;
            new_process->cwd = old_process->cwd;
            for (auto &old_file_descriptor : old_process->file_descriptors) {
                if (old_file_descriptor.get_description())
                    new_process->file_descriptors.push_back(old_file_descriptor.duplicate(true));
                else
                    new_process->file_descriptors.emplace_back();
            }
            new_process->num_file_descriptors = old_process->num_file_descriptors;
            new_process->first_free_fdnum = old_process->first_free_fdnum;

            memcpy(new_process->signal_actions, old_process->signal_actions, sizeof(Process::signal_actions));
            if (flags & CLONE_CLEAR_SIGHAND) {
                for (usize i = 0; i < sizeof(Process::signal_actions) / sizeof(userland::KernelSigaction); i++) {
                    auto *sigaction = &new_process->signal_actions[i];
                    if (sigaction->handler != SIG_DFL && sigaction->handler != SIG_IGN)
                        sigaction->handler = SIG_DFL;
                }
            }

            new_process->set_parent(old_process);
            procfs::create_process_dir(new_process);
        }

        if (flags & CLONE_CHILD_CLEARTID)
            new_thread->clear_child_tid = clone_args->child_tid;
        if (flags & CLONE_CHILD_SETTID)
            new_thread->set_child_tid = clone_args->child_tid;
        if (flags & CLONE_PARENT_SETTID)
            *(pid_t*)clone_args->parent_tid = new_thread->tid;
        if (flags & CLONE_SETTLS)
            new_thread->fs_base = clone_args->tls;

        klib::strncpy(new_thread->name, old_thread->name, sizeof(new_thread->name));

        new_thread->state = Thread::READY;
        sched_list_head.add_before(&new_thread->sched_link);

        return new_thread->tid;
    }

    isize syscall_fork() {
        log_syscall("fork()\n");
        clone_args args = {
            .exit_signal = SIGCHLD
        };
        return clone_impl(&args, sizeof(args));
    }

    isize syscall_vfork() {
        log_syscall("vfork()\n");
        clone_args args = {
            .flags = CLONE_VFORK | CLONE_VM,
            .exit_signal = SIGCHLD
        };
        return clone_impl(&args, sizeof(args));
    }

    isize syscall_clone(u64 flags, uptr stack, int *parent_tid, int *child_tid, u64 tls) {
        log_syscall("clone(%#lX, %#lX, %#lX, %#lX, %#lX)\n", flags, stack, (uptr)parent_tid, (uptr)child_tid, tls);
        clone_args args = {
            .flags = flags & ~CSIGNAL,
            .pidfd = (u64)parent_tid,
            .child_tid = (u64)child_tid,
            .parent_tid = (u64)parent_tid,
            .exit_signal = flags & CSIGNAL,
            .stack = stack,
            .tls = tls
        };
        return clone_impl(&args, sizeof(args));
    }

    isize syscall_clone3(clone_args *clone_args, usize size) {
        log_syscall("clone3(%#lX, %#lX)\n", (uptr)clone_args, size);
        return clone_impl(clone_args, size);
    }

    isize syscall_execve(const char *path, const char **argv, const char **envp) {
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

        auto *state = thread->syscall_state;
        *state = cpu::syscall::SyscallState();
        state->rcx = thread->gpr_state.rip;
        state->r11 = thread->gpr_state.rflags;

        return 0;
    }

    isize syscall_arch_prctl(int op, uptr addr) {
        log_syscall("arch_prctl(%d, %#lX)\n", op, addr);
        switch (op) {
        case ARCH_SET_FS: cpu::write_fs_base(addr); return 0;
        case ARCH_GET_FS: *(uptr*)addr = cpu::read_fs_base(); return 0;
        default:
            klib::printf("arch_prctl: unimplemented op %d\n", op);
            return -EINVAL;
        }
    }

    static isize waitid_impl(idtype_t idtype, id_t id, siginfo_t *infop, int options, struct rusage *rusage) {
        if (int unsupported_options = (options & ~(WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED)))
            klib::printf("waitid: unsupported options %#X\n", unsupported_options);
        if (rusage) {
            klib::printf("waitid: rusage not implemented\n");
            memset(rusage, 0, sizeof(struct rusage));
        }

        if (!(options & (WEXITED | WSTOPPED | WCONTINUED)))
            return -EINVAL;

        Process *current_process = cpu::get_current_thread()->process;

        klib::Vector<Process*> target_children;
        klib::Vector<Event*> events;

        auto add_child = [&] (Process *child) {
            if (options & WEXITED) {
                target_children.push_back(child);
                events.push_back(&child->zombie_event);
            }
            if (options & WUNTRACED) {
                target_children.push_back(child);
                events.push_back(&child->stopped_event);
            }
            if (options & WCONTINUED) {
                target_children.push_back(child);
                events.push_back(&child->continued_event);
            }
        };

        if (idtype == P_ALL) {
            if (current_process->children_list.is_empty())
                return -ECHILD;

            Process *child;
            LIST_FOR_EACH(child, &current_process->children_list, sibling_link)
                add_child(child);
        } else if (idtype == P_PID) {
            auto &thread_table = get_thread_table();
            if (id >= thread_table.size())
                return -ECHILD;

            Thread *target_thread = thread_table[id];
            if (target_thread == nullptr) return -ECHILD;

            Process *child = target_thread->process;
            if (child->parent != current_process) return -ECHILD;

            add_child(child);
        } else {
            klib::printf("waitid: unsupported idtype %d\n", idtype);
            return 0;
        }

        while (true) {
            Process *target_child = nullptr;
            for (Process *child : target_children) {
                if (child->wait_code != 0) {
                    target_child = child;
                    break;
                }
            }

            if (target_child) {
                int child_pid = target_child->pid;

                if (infop) {
                    siginfo_t siginfo {};
                    siginfo.si_pid = child_pid;
                    siginfo.si_uid = target_child->get_main_thread()->cred.uids.rid;
                    siginfo.si_signo = SIGCHLD;
                    siginfo.si_status = target_child->wait_status;
                    siginfo.si_code = target_child->wait_code;
                    *infop = siginfo;
                }

                if (!(options & WNOWAIT)) {
                    if (target_child->is_zombie)
                        delete target_child;
                }

                return child_pid;
            }

            if (options & WNOHANG) {
                if (infop) {
                    siginfo_t siginfo {};
                    *infop = siginfo;
                }
                return 0;
            }
            if (sched::Event::wait(events) == -EINTR)
                return -EINTR;
        }
    }

    isize syscall_wait4(int pid, int *status, int options, struct rusage *rusage) {
        log_syscall("wait4(%d, %#lX, %d, %#lX)\n", pid, (uptr)status, options, (uptr)rusage);
        if (options & ~(WNOHANG | WUNTRACED | WCONTINUED | __WCLONE | __WALL | __WNOTHREAD))
            return -EINVAL;
        int waitid_options = WEXITED;
        waitid_options |= options & ~WUNTRACED;
        if (options & WUNTRACED) waitid_options |= WSTOPPED; // FIXME: not exactly equivalent

        idtype_t idtype;
        id_t id;
        if (pid < -1) {
            idtype = P_PGID;
            id = -pid;
        } else if (pid == -1) {
            idtype = P_ALL;
            id = 0;
        } else if (pid == 0) {
            idtype = P_PGID;
            id = cpu::get_current_thread()->process->group->leader_process->pid;
        } else { // pid > 0
            idtype = P_PID;
            id = pid;
        }

        siginfo_t info;
        isize ret = waitid_impl(idtype, id, &info, waitid_options, rusage);
        if (ret < 0)
            return ret;
        if (status) {
            if (info.si_pid != 0) {
                switch (info.si_code) {
                case CLD_EXITED: *status = W_EXITCODE(info.si_status, 0); break;
                case CLD_KILLED: *status = W_EXITCODE(0, info.si_status); break;
                case CLD_STOPPED: *status = __W_STOPCODE(info.si_status); break;
                case CLD_CONTINUED: *status = __W_CONTINUED; break;
                default: klib::printf("wait4: unexpected si_code %d\n", info.si_code); *status = 0;
                }
            } else {
                *status = 0;
            }
        }
        return info.si_pid;
    }

    isize syscall_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options, struct rusage *rusage) {
        log_syscall("waitid(%d, %d, %#lX, %d, %#lX)\n", idtype, id, (uptr)infop, options, (uptr)rusage);
        return waitid_impl(idtype, id, infop, options, rusage);
    }

    isize syscall_set_tid_address(int *tidptr) {
        log_syscall("set_tid_address(%#lX)\n", (uptr)tidptr);
        Thread *thread = cpu::get_current_thread();
        thread->clear_child_tid = (uptr)tidptr;
        return thread->tid;
    }

    mode_t syscall_umask(mode_t mode) {
        log_syscall("umask(%u)\n", mode);
        Process *process = cpu::get_current_thread()->process;
        mode_t old = process->umask;
        process->umask = mode & 0777;
        return old;
    }

    void syscall_sched_yield() {
        log_syscall("sched_yield()\n");
        yield();
    }

    isize syscall_sched_getaffinity(int pid, usize cpusetsize, cpu_set_t *mask) {
        log_syscall("sched_getaffinity(%d, %#lX, %#lX)\n", pid, cpusetsize, (uptr)mask);
        CPU_ZERO_S(cpusetsize, mask);
        CPU_SET(0, mask);
        return cpusetsize;
    }

    isize syscall_getcpu(uint *cpu, uint *node) {
        log_syscall("getcpu(%#lX, %#lX)\n", (uptr)cpu, (uptr)node);
        if (cpu) *cpu = 0;
        if (node) *node = 0;
        return 0;
    }

    isize syscall_prlimit64(int pid, uint resource, const rlimit64 *new_limit, rlimit64 *old_limit) {
        log_syscall("prlimit64(%d, %u, %#lX, %#lX)\n", pid, resource, (uptr)new_limit, (uptr)old_limit);

        if (new_limit) {
            klib::printf("prlimit64: setting new limit is unsupported (resource type: %u)\n", resource);
            return 0;
        }
        if (old_limit == nullptr)
            return 0;

        switch (resource) {
        case RLIMIT_NOFILE:  *old_limit = { .rlim_cur =            1024, .rlim_max =            1024 }; return 0;
        case RLIMIT_STACK:   *old_limit = { .rlim_cur = 8 * 1024 * 1024, .rlim_max = 8 * 1024 * 1024 }; return 0;
        case RLIMIT_NPROC:   *old_limit = { .rlim_cur =          126047, .rlim_max =          126047 }; return 0;
        case RLIMIT_AS:      *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_RSS:     *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_CPU:     *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_FSIZE:   *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_DATA:    *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_LOCKS:   *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_MEMLOCK: *old_limit = { .rlim_cur = RLIM64_INFINITY, .rlim_max = RLIM64_INFINITY }; return 0;
        case RLIMIT_CORE:    *old_limit = { .rlim_cur =               0, .rlim_max =               0 }; return 0;
        default:
            klib::printf("prlimit64: unsupported resource type %u\n", resource);
            return -EINVAL;
        }
    }

    isize syscall_prctl(int op, usize arg1, usize arg2, usize arg3, usize arg4) {
        log_syscall("prctl(%d, %#lX, %#lX, %#lX, %#lX)\n", op, arg1, arg2, arg3, arg4);
        Thread *thread = cpu::get_current_thread();
        Process *process = thread->process;

        switch (op) {
        case PR_GET_DUMPABLE: return process->dumpable; 
        case PR_SET_DUMPABLE: process->dumpable = arg1; return 0; 
        case PR_GET_NAME: {
            char *name = (char*)arg1;
            klib::strncpy(name, thread->name, 16);
            name[15] = 0;
        } return 0;
        case PR_SET_NAME: {
            const char *name = (const char*)arg1;
            klib::strncpy(thread->name, name, 16);
            thread->name[15] = 0;
        } return 0;
        case PR_CAPBSET_READ: return 1;
        default:
            klib::printf("prctl: unimplemented op %d\n", op);
            return -EINVAL;
        }
    }
}
