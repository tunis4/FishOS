#pragma once

#include <klib/timespec.hpp>

namespace userland {
    void init_futex();

    isize futex_wait(u32 *uaddr, u32 expected, const klib::TimeSpec *timeout);
    isize futex_wake(u32 *uaddr, int max_to_wake);
    isize syscall_futex(u32 *uaddr, int op, usize arg1, usize arg2, usize arg3, usize arg4);
}
