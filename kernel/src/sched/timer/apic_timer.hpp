#pragma once

#include <klib/common.hpp>
#include <klib/timespec.hpp>

namespace sched::timer::apic_timer {
    extern usize freq;
    extern u8 vector;

    void stop();
    void oneshot(usize µs);
    u64 µs_since_interrupt();
    void self_interrupt();
    void init();
}
