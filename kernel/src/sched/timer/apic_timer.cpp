#include <sched/timer/apic_timer.hpp>
#include <sched/timer/hpet.hpp>
#include <sched/timer/pit.hpp>
#include <sched/time.hpp>
#include <sched/sched.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <cpu/interrupts/apic.hpp>
#include <cpu/interrupts/idt.hpp>
#include <cpu/cpu.hpp>

using namespace cpu::interrupts;

namespace sched::timer::apic_timer {
    usize freq = 0;
    u8 vector = 0;
    usize previous_interval = 0;

    static void interrupt(void *priv, cpu::InterruptState *state) {
        stop();
        update_time(klib::TimeSpec::from_microseconds(previous_interval));
        usize interval = sched::scheduler_isr(priv, state);
        cpu::interrupts::eoi();
        oneshot(interval);
    }

    void stop() {
        LAPIC::write_reg(LAPIC::TIMER_INITIAL, 0);
        // LAPIC::mask_vector(LAPIC::LVT_TIMER);
    }

    void oneshot(usize µs) {
        stop();
        previous_interval = µs;

        u32 ticks = (µs * freq) / 1000000;
        // LAPIC::set_vector(LAPIC::LVT_TIMER, vector, false, false, false, false);
        LAPIC::write_reg(LAPIC::TIMER_DIVIDE, 0b1011); // divide by 1
        LAPIC::write_reg(LAPIC::TIMER_INITIAL, ticks);
    }

    u64 µs_since_interrupt() {
        u64 initial_ticks = LAPIC::read_reg(LAPIC::TIMER_INITIAL);
        u64 current_ticks = LAPIC::read_reg(LAPIC::TIMER_CURRENT);
        return ((initial_ticks - current_ticks) * 1000000) / freq;
    }

    void self_interrupt() {
        previous_interval = µs_since_interrupt();
        LAPIC::send_ipi(cpu::get_current_cpu()->lapic_id, vector);
    }

    void init() {
        vector = allocate_vector();
        set_isr(vector, interrupt, nullptr);

        stop();

        LAPIC::set_vector(LAPIC::LVT_TIMER, vector, false, false, false, true);
        LAPIC::write_reg(LAPIC::TIMER_DIVIDE, 3); // divide by 16

        if (hpet::is_initialized()) {
            klib::printf("APIC Timer: Using HPET for calibration\n");
            LAPIC::write_reg(LAPIC::TIMER_INITIAL, 0xFFFFFFFF);
            hpet::stall_ms(100);
        } else {
            klib::printf("APIC Timer: Using PIT for calibration\n");
            pit::prepare_sleep(100);
            LAPIC::write_reg(LAPIC::TIMER_INITIAL, 0xFFFFFFFF);
            pit::perform_sleep();
        }

        freq = (0xFFFFFFFF - LAPIC::read_reg(LAPIC::TIMER_CURRENT)) * 10 * 16; 

        stop();

        klib::printf("APIC Timer: Freq: %ld\n", freq);

        LAPIC::unmask_vector(LAPIC::LVT_TIMER);

        // LAPIC::write_reg(LAPIC::TIMER_DIVIDE, 3); // divide by 16
        // LAPIC::write_reg(LAPIC::TIMER_INITIAL, freq * 4 / 16); // try to sleep 4 seconds with apic timer

        // while (LAPIC::read_reg(LAPIC::TIMER_CURRENT))
        //     asm volatile("pause");
    }
}