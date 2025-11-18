#pragma once

#include <klib/common.hpp>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

namespace userland {
    isize syscall_uname(struct utsname *buf);
    isize syscall_sysinfo(struct sysinfo *buf);
}
