#pragma once

#include <klib/timespec.hpp>

namespace userland {
    isize syscall_futex_wait(int *ptr, int expected, const klib::TimeSpec *timeout);
    isize syscall_futex_wake(int *ptr);
}
