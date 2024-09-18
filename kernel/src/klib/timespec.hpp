#pragma once

#include <klib/common.hpp>
#include <time.h>

namespace klib {
    struct TimeSpec {
        i64 seconds;
        i64 nanoseconds;

        static TimeSpec from_seconds(u64 s) { return TimeSpec(s, 0); }
        static TimeSpec from_microseconds(u64 µs) { return TimeSpec(µs / 1'000'000, (µs % 1'000'000) * 1'000); }

        struct timespec to_posix() const { return { .tv_sec = seconds, .tv_nsec = nanoseconds }; }
        struct timeval to_timeval() const { return { .tv_sec = seconds, .tv_usec = nanoseconds / 1000 }; }

        bool is_zero() const {
            return seconds == 0 && nanoseconds == 0;
        }

        void add(const TimeSpec &interval) {
            if (this->nanoseconds + interval.nanoseconds > 999'999'999) {
                i64 diff = (this->nanoseconds - interval.nanoseconds) - 1'000'000'000;
                this->nanoseconds = diff;
                this->seconds++;
            } else {
                this->nanoseconds += interval.nanoseconds;
            }
            this->seconds += interval.seconds;
        }

        bool subtract(const TimeSpec &interval) {
            if (interval.nanoseconds > this->nanoseconds) {
                i64 diff = interval.nanoseconds - this->nanoseconds;
                this->nanoseconds = 999'999'999 - diff;
                if (this->seconds == 0) {
                    this->seconds = 0;
                    this->nanoseconds = 0;
                    return true;
                }
                this->seconds--;
            } else {
                this->nanoseconds -= interval.nanoseconds;
            }
            if (interval.seconds > this->seconds) {
                this->seconds = 0;
                this->nanoseconds = 0;
                return true;
            }
            this->seconds -= interval.seconds;
            if (this->is_zero())
                return true;
            return false;
        }
    };
}
