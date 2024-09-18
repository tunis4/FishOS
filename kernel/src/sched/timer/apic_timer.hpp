#pragma once

#include <klib/common.hpp>

namespace sched::timer::apic_timer {
    extern usize freq;
    extern u8 vector;

    void stop();
    void oneshot(usize µs);
    void init();
}
