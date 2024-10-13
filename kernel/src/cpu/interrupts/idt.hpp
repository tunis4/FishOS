#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>

namespace cpu::interrupts {
    struct [[gnu::packed]] IDTR {
        u16 limit;
        u64 base;
    };

    enum class IDTType {
        INTERRUPT = 0b1110,
        TRAP = 0b1111
    };

    struct [[gnu::packed]] IDTEntry {
        u16 offset1;
        u16 selector;
        u16 attributes;
        u16 offset2;
        u32 offset3;
        u32 reserved;
    };

    struct ISR {
        using Handler = void (*)(void *priv, InterruptState* frame);

        Handler handler;
        void *priv;
    };

    u8 allocate_vector();
    void set_isr(u8 vec, ISR::Handler handler, void *priv);
    void load_idt();
}
