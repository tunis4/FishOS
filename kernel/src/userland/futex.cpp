#include <userland/futex.hpp>
#include <sched/event.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/cstdio.hpp>

namespace userland {
    isize syscall_futex_wait(int *ptr, int expected, const klib::TimeSpec *timeout) {
#if SYSCALL_TRACE
        klib::printf("futex_wait(%#lX, %d, %#lX)\n", (uptr)ptr, expected, (uptr)timeout);
#endif
        // klib::printf("futex_wait is a stub\n");
        return 0;
    }

    isize syscall_futex_wake(int *ptr) {
#if SYSCALL_TRACE
        klib::printf("futex_wake(%#lX)\n", (uptr)ptr);
#endif
        // klib::printf("futex_wake is a stub\n");
        return 0;
    }
}
