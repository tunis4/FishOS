#include <sched/timer/hpet.hpp>
#include <klib/cstdio.hpp>
#include <acpi/tables.hpp>
#include <panic.hpp>

#define GENERAL_INFO 0
#define GENERAL_CONFIG 0x10
#define MAIN_COUNTER 0xF0
#define TIMER_CONFIG(n) (0x100 + 0x20 * n)
#define TIMER_COMPARATOR(n) (0x108 + 0x20 * n)

// TODO: main counter reads should be atomic
namespace sched::timer::hpet {
    static bool initialized;
    static u64 period, freq;
    static acpi::GenericAddr regs;

    u64 monotonic_time_µs() {
        return regs.read<u64>(MAIN_COUNTER) * (period / 1'000'000) / 1'000;
    }

    void stall_ns(usize ns) {
        usize fs = ns * 1'000'000; // convert to femtoseconds
        usize target = regs.read<u64>(MAIN_COUNTER) + (fs / period);

        while (regs.read<u64>(MAIN_COUNTER) < target);
    }

    bool is_initialized() { return initialized; }

    void init(acpi::HPET *table) {
        regs = table->address;

        auto info = regs.read<u64>(GENERAL_INFO);
        period = info >> 32;
        freq = 1'000'000'000'000'000 / period;
        klib::printf("HPET: Vendor: %04lX, Period: %ld fs, Freq: %ld Hz\n", (info >> 16) & 0xFFFF, period, freq);

        regs.write<u64>(GENERAL_CONFIG, 0); // disable hpet
        regs.write<u64>(MAIN_COUNTER, 0); // set counter to 0
        regs.write<u64>(GENERAL_CONFIG, 1); // enable hpet

        initialized = true;
        klib::printf("HPET: Initialized\n");
    }
}
