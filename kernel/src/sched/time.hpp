#pragma once

#include <sched/event.hpp>
#include <klib/timespec.hpp>
#include <klib/list.hpp>
#include <limine.hpp>

namespace sched {
    struct Timer {
        using Callback = void(void*);

        klib::TimeSpec remaining;
        Event event;
        klib::ListHead armed_timers_link;
        Callback *callback = nullptr;
        void *callback_data;
        bool fired = false;

        Timer() : event("Timer::event") {}

        void arm(const klib::TimeSpec &time, Callback *callback = nullptr, void *callback_data = nullptr);
        void disarm();
    };

    void init_time(limine_boot_time_response *boot_time_res);
    void update_time(klib::TimeSpec interval);

    [[nodiscard]] klib::TimeSpec get_clock(clockid_t clock_id);

    isize syscall_time(time_t *t);
    isize syscall_gettimeofday(timeval *tv);
    isize syscall_clock_gettime(clockid_t clock_id, klib::TimeSpec *time);
    isize syscall_clock_getres(clockid_t clock_id, klib::TimeSpec *res);
    isize syscall_nanosleep(const klib::TimeSpec *duration, klib::TimeSpec *remaining);
    isize syscall_clock_nanosleep(clockid_t clock_id, int flags, const klib::TimeSpec *duration, klib::TimeSpec *remaining);
}
