#pragma once

#include <klib/timespec.hpp>
#include <linux/futex.h>

namespace userland {
    isize futex_wait(u32 *uaddr, u32 expected, const klib::TimeSpec *timeout);
    isize futex_wake(u32 *uaddr, int max_to_wake);
    isize syscall_futex(u32 *uaddr, int op, usize arg1, usize arg2, usize arg3, usize arg4);
    isize syscall_get_robust_list(int pid, robust_list_head **head_ptr, usize *len_ptr);
    isize syscall_set_robust_list(robust_list_head *head, usize len);
}
