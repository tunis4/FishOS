#pragma once

#include <klib/common.hpp>
#include <acpi/tables.hpp>

namespace sched::timer::hpet {
    void init(acpi::HPET *table);
    bool is_initialized();

    u64 monotonic_time_µs();

    void stall_ns(usize ns);
    inline void stall_µs(usize µs) { stall_ns(µs * 1'000); }
    inline void stall_ms(usize ms) { stall_ns(ms * 1'000'000); }
}
