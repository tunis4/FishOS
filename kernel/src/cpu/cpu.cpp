#include <cpu/cpu.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/interrupts/idt.hpp>
#include <mem/vmm.hpp>
#include <mem/pmm.hpp>
#include <klib/cstdio.hpp>

namespace cpu {
    extern "C" void __syscall_entry();

    // const usize stack_size = 0x10000; // 64 KiB
    const usize stack_size = 0x1000; // FIXME

    static CPU bsp_cpu;

    void smp_init(limine_smp_response *smp_res) {
        klib::printf("CPU: SMP | x2APIC: %s\n", (smp_res->flags & 1) ? "yes" : "no");
        for (u32 i = 0; i < smp_res->cpu_count; i++) {
            auto cpu_info = smp_res->cpus[i];
            auto is_bsp = cpu_info->lapic_id == smp_res->bsp_lapic_id;
            klib::printf("  Core %u%s | Processor ID: %u, LAPIC ID: %u\n", i, is_bsp ? " (BSP)" : "", cpu_info->processor_id, cpu_info->lapic_id);

            CPU *cpu;
            if (is_bsp)
                cpu = get_current_cpu();
            else
                cpu = new CPU();
            cpu->cpu_number = i;
            cpu_info->extra_argument = u64(cpu);

            if (!is_bsp) {
                __atomic_store_n(&cpu_info->goto_address, &init, __ATOMIC_SEQ_CST);
            } else {
                cpu->is_bsp = true;
                init(cpu_info);
            }
        }
    }

    void early_init() {
        load_gdt();
        interrupts::load_idt();

        // hardcode BSP's PAT early so that the framebuffer wont be slow as hell
        // 0: WB  1: WT  2: UC-  3: UC  4: WB  5: WT  6: WC  7: WP
        MSR::write(MSR::IA32_PAT, 0x501040600070406);

        write_gs_base((uptr)(new (&bsp_cpu) CPU()));
    }

    void init(limine_smp_info *info) {
        reload_gdt();
        interrupts::load_idt();

        vmm::kernel_pagemap.activate();

        auto cpu = (CPU*)info->extra_argument;
        cpu->lapic_id = info->lapic_id;

        load_tss(&cpu->tss);

        uptr int_stack_phy = pmm::alloc_pages(stack_size / 0x1000);
        cpu->tss.rsp0 = int_stack_phy + stack_size + vmm::hhdm;

        uptr sched_stack_phy = pmm::alloc_pages(stack_size / 0x1000);
        cpu->tss.ist1 = sched_stack_phy + stack_size + vmm::hhdm;

        // hardcode PAT
        // 0: WB  1: WT  2: UC-  3: UC  4: WB  5: WT  6: WC  7: WP
        MSR::write(MSR::IA32_PAT, 0x501040600070406);

        // enable SSE/SSE2
        u64 cr0 = read_cr0();
        cr0 &= ~(u64(1) << 2);
        cr0 |= u64(1) << 1;
        write_cr0(cr0);
        
        u64 cr4 = read_cr4();
        cr4 |= u64(3) << 9;
        write_cr4(cr4);
        
        // enable syscall/sysret instructions
        MSR::write(MSR::IA32_EFER, MSR::read(MSR::IA32_EFER) | 1);
        
        // make sure interrupt flag gets reset on syscall entry
        MSR::write(MSR::IA32_FMASK, MSR::read(MSR::IA32_FMASK) | 0x200);

        u64 star = 0 | (u64(GDTSegment::KERNEL_CODE_64) << 32) | ((u64(GDTSegment::USER_CODE_64) - 16) << 48);
        MSR::write(MSR::IA32_STAR, star);
        MSR::write(MSR::IA32_LSTAR, (u64)&__syscall_entry);

        write_gs_base((uptr)cpu);

        if (!cpu->is_bsp) {
            asm volatile("cli");
            while (true) asm volatile("hlt");
        }
    }
}
