#include <cpu/interrupts/idt.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/cstdio.hpp>
#include <klib/bitmap.hpp>
#include <sched/sched.hpp>
#include <panic.hpp>

namespace cpu::interrupts {
    [[gnu::aligned(16)]] static IDTEntry idt[256];
    static IDTR idtr;
    static ISR isr_table[256];
    static klib::Bitmap<256> idt_bitmap;

    extern "C" void (*__idt_wrappers[256])();

    u8 allocate_vector() {
        for (u8 i = 32; i; i++) {
            if (idt_bitmap.get(i) == false) {
                idt_bitmap.set(i, true);
                return i;
            }
        }
        panic("Failed to allocate interrupt");
    }

    static void load_idt_entry(u8 index, void (*wrapper)(), IDTType type) {
        auto *entry = &idt[index];
        entry->offset1 = (u64)wrapper & 0xFFFF;
        entry->offset2 = ((u64)wrapper & 0xFFFF0000) >> 16;
        entry->offset3 = ((u64)wrapper & 0xFFFFFFFF00000000) >> 32;
        entry->selector = u16(GDTSegment::KERNEL_CODE_64);
        entry->attributes = (1 << 15) | (int(type) << 8); // set present bit and type
        entry->reserved = 0;
    }

    void set_isr(u8 vec, ISR::Handler handler, void *priv) {
        isr_table[vec].handler = handler;
        isr_table[vec].priv = priv;
    }

    const char *exception_strings[] = {
        "Division by 0",
        "Debug",
        "NMI",
        "Breakpoint",
        "Overflow",
        "Bound range exceeded",
        "Invalid opcode",
        "Device not available",
        "Double fault",
        "???",
        "Invalid TSS",
        "Segment not present",
        "Stack-segment fault",
        "General protection fault",
        "Page fault",
        "???",
        "x87 exception",
        "Alignment check",
        "Machine check",
        "SIMD exception",
        "Virtualisation",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "???",
        "Security"
    };

    static void exception_handler(void *priv, InterruptState *state) {
        u8 vec = (u64)priv;
        const char *err_name = vec < 31 ? exception_strings[vec] : "Not an exception";
        if ((state->cs & 3) == 3) {
            sched::Thread *thread = cpu::get_current_thread();

            int signal = SIGSEGV;
            if (vec == 0 || vec == 19) signal = SIGFPE;
            else if (vec == 6) signal = SIGILL;
            else if (vec == 13) signal = SIGBUS;
            else if (vec != 14) klib::printf("cpu: unexpected exception %#X in user mode, sending SIGSEGV\n", vec);

            thread->send_signal(signal);

            memcpy(&thread->gpr_state, state, sizeof(cpu::InterruptState));
            if (thread->extended_state)
                cpu::save_extended_state(thread->extended_state);
            thread->gs_base = cpu::read_kernel_gs_base();
            thread->fs_base = cpu::read_fs_base();
            thread->saved_user_stack = cpu::get_current_cpu()->user_stack;
            thread->saved_kernel_stack = cpu::get_current_cpu()->kernel_stack;

            thread->entering_signal = true;
            userland::dispatch_pending_signal(thread);
            memcpy(state, &thread->gpr_state, sizeof(cpu::InterruptState));
            if (thread->extended_state)
                cpu::restore_extended_state(thread->extended_state);
            return;
        }
        klib::printf("\nCPU Exception: %s (%#X)\n", err_name, vec);
        if (state->err) klib::printf("Error code: %#04lX\n", state->err);
        if (vec == 0xE) klib::printf("CR2=%016lX\n", cpu::read_cr2());
        klib::printf("RAX=%016lX RBX=%016lX RCX=%016lX RDX=%016lX\n", state->rax, state->rbx, state->rcx, state->rdx);
        klib::printf("RSI=%016lX RDI=%016lX RBP=%016lX RSP=%016lX\n", state->rsi, state->rdi, state->rbp, state->rsp);
        klib::printf(" R8=%016lX  R9=%016lX R10=%016lX R11=%016lX\n", state->r8,  state->r9,  state->r10, state->r11);
        klib::printf("R12=%016lX R13=%016lX R14=%016lX R15=%016lX\n", state->r12, state->r13, state->r14, state->r15);
        klib::printf("RIP=%016lX RFLAGS=%016lX\n", state->rip, state->rflags);
        klib::printf("ES=%016lX CS=%016lX SS=%016lX DS=%016lX\n", state->es, state->cs, state->ss, state->ds);
        klib::printf("FS=%016lX GS=%016lX KERNEL_GS=%016lX\n", cpu::read_fs_base(), cpu::read_gs_base(), cpu::read_kernel_gs_base());
        klib::printf("\nStacktrace:\n");
        StackFrame *frame = (StackFrame*)state->rbp;
        while (true) {
            if (frame == nullptr || frame->ip == 0)
                break;
            
            klib::printf("%#lX\n", frame->ip);
            frame = frame->next;
        }
        klib::printf("Kernel Panic: Cannot recover from CPU exception that happened in the kernel\n");
        abort();
    }

    static void page_fault_handler(void *priv, InterruptState *state) {
        u64 cr2 = cpu::read_cr2();
        auto *pagemap = mem::vmm->active_pagemap;
        if (cr2 >= 0xFFFF800000000000)
            pagemap = &mem::vmm->kernel_pagemap;
        if (pagemap->handle_page_fault(cr2) < 0)
            exception_handler(priv, state);
    }

    extern "C" void __idt_handler_common(u64 vec, InterruptState *state) {
        ISR *isr = &isr_table[vec];
        isr->handler(isr->priv, state);
    }

    void load_idt() {
        for (usize i = 0; i < 256; i++)
            load_idt_entry(i, __idt_wrappers[i], IDTType::INTERRUPT);
        
        for (usize i = 0; i < 32; i++) {
            set_isr(i, i == 0xE ? page_fault_handler : exception_handler, (void*)i);
            idt_bitmap.set(i, true);
        }

        idtr.limit = sizeof(idt) - 1;
        idtr.base = (u64)&idt;
        asm volatile("cli");
        asm volatile("lidt %0" : : "m" (idtr));
        asm volatile("sti");
    }
}
