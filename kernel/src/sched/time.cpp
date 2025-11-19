#include <sched/time.hpp>
#include <sched/sched.hpp>
#include <sched/timer/apic_timer.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>
#include <errno.h>

namespace sched {
    static klib::ListHead armed_timers_list;
    static klib::Spinlock armed_timers_lock;

    static klib::TimeSpec monotonic_clock;
    static klib::TimeSpec realtime_clock;

    void Timer::arm(const klib::TimeSpec &time, Callback *callback, void *callback_data) {
        this->remaining = time;
        this->fired = false;
        if (callback) {
            this->callback = callback;
            this->callback_data = callback_data;
        }

        klib::SpinlockGuard guard(armed_timers_lock);
        armed_timers_list.add_before(&this->armed_timers_link);
    }

    void Timer::disarm() {
        this->remaining = {};
        this->interval = {};
        if (callback) {
            this->callback = nullptr;
            this->callback_data = nullptr;
        }

        klib::SpinlockGuard guard(armed_timers_lock);
        if (!this->armed_timers_link.is_invalid())
            this->armed_timers_link.remove();
    }

    void init_time(limine_boot_time_response *boot_time_res) {
        armed_timers_list.init();
        i64 epoch = boot_time_res ? boot_time_res->boot_time : 0;
        monotonic_clock = klib::TimeSpec::from_seconds(0);
        realtime_clock = klib::TimeSpec::from_seconds(epoch);
    }

    void update_time(klib::TimeSpec interval) {
        monotonic_clock += interval;
        realtime_clock += interval;

        klib::SpinlockGuard guard(armed_timers_lock);
        Timer *timer;
        LIST_FOR_EACH_SAFE(timer, &armed_timers_list, armed_timers_link) {
            if (timer->fired)
                continue;

            timer->remaining -= interval;
            if (timer->remaining.is_zero()) {
                if (timer->interval.is_zero()) {
                    timer->fired = true;
                    timer->event.trigger();
                    timer->armed_timers_link.remove();
                } else {
                    timer->remaining = timer->interval;
                }
                if (timer->callback)
                    timer->callback(timer->callback_data);
            }
        }
    }

    klib::TimeSpec get_clock(clockid_t clock_id) {
        auto current_interval = klib::TimeSpec::from_microseconds(sched::timer::apic_timer::µs_since_interrupt());
        switch (clock_id) {
        case CLOCK_BOOTTIME:
        case CLOCK_MONOTONIC: return monotonic_clock + current_interval;
        case CLOCK_REALTIME: return realtime_clock + current_interval;
        default: return { 0, 0 };
        }
    }

    isize syscall_time(time_t *t) {
        log_syscall("time(%#lX)\n", (uptr)t);
        time_t time = get_clock(CLOCK_REALTIME).seconds;
        if (t) *t = time;
        return time;
    }

    isize syscall_gettimeofday(timeval *tv) {
        log_syscall("gettimeofday(%#lX)\n", (uptr)tv);
        *tv = get_clock(CLOCK_REALTIME).to_timeval();
        return 0;
    }

    isize syscall_clock_gettime(clockid_t clock_id, klib::TimeSpec *time) {
        log_syscall("clock_gettime(%d, %#lX)\n", clock_id, (uptr)time);
        if (clock_id == CLOCK_MONOTONIC_COARSE || clock_id == CLOCK_MONOTONIC_RAW || clock_id == CLOCK_BOOTTIME)
            clock_id = CLOCK_MONOTONIC;
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
            return -EINVAL;
        *time = get_clock(clock_id);
        return 0;
    }

    isize syscall_clock_getres(clockid_t clock_id, klib::TimeSpec *res) {
        log_syscall("clock_getres(%d, %#lX)\n", clock_id, (uptr)res);
        if (clock_id == CLOCK_MONOTONIC_COARSE || clock_id == CLOCK_MONOTONIC_RAW || clock_id == CLOCK_BOOTTIME)
            clock_id = CLOCK_MONOTONIC;
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
            return -EINVAL;
        *res = klib::TimeSpec::from_microseconds(10);
        return 0;
    }

    static isize clock_nanosleep_impl(clockid_t clockid, int flags, const klib::TimeSpec *duration, klib::TimeSpec *remaining) {
        if (flags & TIMER_ABSTIME)
            klib::printf("nanosleep: TIMER_ABSTIME unsupported\n");

        Timer timer;
        timer.arm(*duration);
        auto ret = timer.event.wait();
        timer.disarm();
        if (remaining)
            *remaining = timer.remaining;
        if (ret == -EINTR)
            return -EINTR;
        ASSERT(timer.fired);
        return 0;
    }

    isize syscall_nanosleep(const klib::TimeSpec *duration, klib::TimeSpec *remaining) {
        log_syscall("nanosleep({ %lu, %lu })\n", duration->seconds, duration->nanoseconds);
        return clock_nanosleep_impl(CLOCK_MONOTONIC, 0, duration, remaining);
    }

    isize syscall_clock_nanosleep(clockid_t clock_id, int flags, const klib::TimeSpec *duration, klib::TimeSpec *remaining) {
        log_syscall("clock_nanosleep(%d, %d, { %lu, %lu })\n", clock_id, flags, duration->seconds, duration->nanoseconds);
        return clock_nanosleep_impl(clock_id, flags, duration, remaining);
    }

    isize syscall_getitimer(int which, itimerval *curr_value) {
        log_syscall("getitimer(%d, %#lX)\n", which, (uptr)curr_value);
        if (which != ITIMER_REAL) {
            klib::printf("getitimer: itimer other than ITIMER_REAL is not supported\n");
            return -EINVAL;
        }

        sched::Process *process = cpu::get_current_thread()->process;
        curr_value->it_interval = process->itimer_real.interval.to_timeval();
        curr_value->it_value = process->itimer_real.remaining.to_timeval();
        return 0;
    }

    static isize setitimer_impl(int which, const itimerval *new_value, itimerval *old_value) {
        sched::Process *process = cpu::get_current_thread()->process;
        if (which != ITIMER_REAL) {
            klib::printf("setitimer: itimer other than ITIMER_REAL is not supported\n");
            return -EINVAL;
        }
        itimerval new_timerval = {};
        if (new_value) {
            if (new_value->it_value.tv_usec > 999'999) return -EINVAL;
            if (new_value->it_interval.tv_usec > 999'999) return -EINVAL;
            new_timerval = *new_value;
        }
        auto timer_value = klib::TimeSpec::from_timeval(new_timerval.it_value);
        auto timer_interval = klib::TimeSpec::from_timeval(new_timerval.it_interval);

        if (old_value) {
            old_value->it_interval = process->itimer_real.interval.to_timeval();
            old_value->it_value = process->itimer_real.remaining.to_timeval();
        }

        process->itimer_real.disarm();
        if (timer_value.is_zero())
            return 0;

        process->itimer_real.interval = timer_interval;
        process->itimer_real.arm(timer_value, [] (void *data) {
            auto *process = (sched::Process*)data;
            process->send_signal(SIGALRM);
        }, process);
        return 0;
    }

    isize syscall_setitimer(int which, const itimerval *new_value, itimerval *old_value) {
        log_syscall("setitimer(%d, %#lX, %#lX)\n", which, (uptr)new_value, (uptr)old_value);
        return setitimer_impl(which, new_value, old_value);
    }

    isize syscall_alarm(uint seconds) {
        log_syscall("alarm(%u)\n", seconds);
        itimerval new_value = {}, old_value;
        new_value.it_value.tv_sec = seconds;
        setitimer_impl(ITIMER_REAL, &new_value, &old_value);
        return old_value.it_value.tv_sec;
    }
}
