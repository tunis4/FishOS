#pragma once

#include <klib/common.hpp>
#include <sys/stat.h>

namespace userland {
    isize syscall_gettid();
    isize syscall_getpid();
    isize syscall_getppid();
    isize syscall_getpgid(int pid);
    isize syscall_getpgrp();
    isize syscall_setpgid(int pid, int pgid);
    isize syscall_getsid(int pid);
    isize syscall_setsid();
}
