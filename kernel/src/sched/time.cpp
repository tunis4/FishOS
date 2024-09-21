#include <sched/time.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>
#include <errno.h>

namespace sched {
    static klib::ListHead armed_timers_list;
    static klib::Spinlock armed_timers_lock;

    klib::TimeSpec monotonic_clock;
    klib::TimeSpec realtime_clock;

    void Timer::arm() {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(armed_timers_lock);
        this->fired = false;
        armed_timers_list.add_before(&this->armed_timers_link);
    }

    void Timer::disarm() {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(armed_timers_lock);
        this->armed_timers_link.remove();
    }

    void init_time(limine_boot_time_response *boot_time_res) {
        armed_timers_list.init();
        i64 epoch = boot_time_res ? boot_time_res->boot_time : 0;
        monotonic_clock = klib::TimeSpec::from_seconds(0);
        realtime_clock = klib::TimeSpec::from_seconds(epoch);
    }

    void update_time(klib::TimeSpec interval) {
        monotonic_clock.add(interval);
        realtime_clock.add(interval);

        Timer *timer;
        LIST_FOR_EACH(timer, &armed_timers_list, armed_timers_link) {
            if (timer->fired)
                continue;

            if (timer->remaining.subtract(interval)) {
                timer->fired = true;
                timer->event.trigger();
            }
        }
    }

    isize syscall_sleep(const klib::TimeSpec *duration, klib::TimeSpec *remaining) {
#if SYSCALL_TRACE
        klib::printf("sleep({ %lu, %lu })\n", duration->seconds, duration->nanoseconds);
#endif
        Timer timer(*duration);
        timer.arm();
        auto ret = timer.event.await();
        timer.disarm();
        if (remaining)
            *remaining = timer.remaining;
        if (ret == -EINTR)
            return -EINTR;
        ASSERT(timer.fired);
        return 0;
    }

    isize syscall_clock_gettime(clockid_t clock_id, klib::TimeSpec *time) {
#if SYSCALL_TRACE
        klib::printf("clock_gettime(%d, %#lX)\n", clock_id, (uptr)time);
#endif
        if (clock_id == CLOCK_REALTIME)
            *time = realtime_clock;
        else if (clock_id == CLOCK_MONOTONIC)
            *time = monotonic_clock;
        else
            return -EINVAL;
        return 0;
    }

    isize syscall_clock_getres(clockid_t clock_id, klib::TimeSpec *res) {
#if SYSCALL_TRACE
        klib::printf("clock_getres(%d, %#lX)\n", clock_id, (uptr)res);
#endif
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
            return -EINVAL;
        *res = klib::TimeSpec::from_microseconds(10);
        return 0;
    }
}
