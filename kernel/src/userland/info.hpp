#pragma once

#include <klib/common.hpp>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

namespace userland {
    isize syscall_uname(utsname *buf);
    isize syscall_sysinfo(sysinfo *buf);
}
