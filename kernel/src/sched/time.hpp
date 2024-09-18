#pragma once

#include <sched/event.hpp>
#include <klib/timespec.hpp>
#include <klib/list.hpp>
#include <limine.hpp>

namespace sched {
    extern klib::TimeSpec monotonic_clock;
    extern klib::TimeSpec realtime_clock;

    struct Timer {
        klib::TimeSpec remaining;
        Event event;
        klib::ListHead armed_timers_link;
        bool fired = false;

        Timer() {
            armed_timers_link.init();
        }

        Timer(const klib::TimeSpec &time) : remaining(time) {
            armed_timers_link.init();
        }

        void arm();
        void disarm();
    };

    void init_time(limine_boot_time_response *boot_time_res);
    void update_time(klib::TimeSpec interval);

    isize syscall_sleep(const klib::TimeSpec *duration, klib::TimeSpec *remaining);
    isize syscall_clock_gettime(clockid_t clock_id, klib::TimeSpec *time);
    isize syscall_clock_getres(clockid_t clock_id, klib::TimeSpec *res);
}
