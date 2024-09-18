#include <sched/timer/pit.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <cpu/cpu.hpp>

using namespace cpu;

namespace sched::timer::pit {
    static klib::Spinlock pit_lock;
    static bool sleeping = false;
    static usize sleep_ticks = 0;

    static void irq(u64 vec, InterruptState *state) {
        if (sleeping) sleep_ticks--;
        interrupts::eoi();
    }

    void set_reload(u16 count) {
        klib::LockGuard guard(pit_lock);
        out<u8>(0x43, 0x34); // channel 0, lo/hi access mode, mode 2 (rate generator)
        out<u8>(0x40, count & 0xFF);
        out<u8>(0x40, count >> 8);
    }

    void set_divide(u16 divide) {
        klib::LockGuard guard(pit_lock);
        out<u8>(0x43, 0x36); // channel 0, lo/hi access mode, mode 3 (square wave generator)
        out<u8>(0x40, divide & 0xFF);
        out<u8>(0x40, divide >> 8);
    }

    u16 get_current_count() {
        klib::LockGuard guard(pit_lock);
        out<u8>(0x43, 0);
        u8 lo = in<u8>(0x40);
        u8 hi = in<u8>(0x40);
        return (hi << 8) | lo;
    }

    void prepare_sleep(usize ms) {
        sleep_ticks = (ms * 14565) / 100000;
        set_divide(8192);
    }

    void perform_sleep() {
        sleeping = true;
        while (sleep_ticks)
            asm volatile("pause");
        sleeping = false;
    }

    void init() {
        interrupts::register_irq(0, irq);
    }
}
