#pragma once

#include <klib/common.hpp>

namespace userland {
    isize syscall_gettid();
    isize syscall_getpid();
    isize syscall_getppid();
    isize syscall_getpgid(int pid);
    isize syscall_setpgid(int pid, int pgid);
    isize syscall_getsid(int pid);
    isize syscall_setsid();
}
