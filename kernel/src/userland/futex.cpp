#include <userland/futex.hpp>
#include <sched/event.hpp>
#include <sched/time.hpp>
#include <sched/sched.hpp>
#include <cpu/syscall/syscall.hpp>
#include <klib/cstdio.hpp>
#include <klib/hashtable.hpp>
#include <linux/futex.h>
#include <errno.h>

namespace userland {
    static klib::HashTable<uptr, sched::Event> futex_hash_table;
    static klib::Spinlock futex_hash_table_lock;

    void init_futex() {
        new (&futex_hash_table) klib::HashTable<uptr, sched::Event>(256);
    }

    isize futex_wait(u32 *uaddr, u32 expected, const klib::TimeSpec *timeout) {
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != expected)
            return -EAGAIN;

        sched::Process *process = cpu::get_current_thread()->process;
        isize phy_addr = process->pagemap->get_physical_addr((uptr)uaddr);
        if (phy_addr < 0) // errno
            return phy_addr;

        sched::Event *events[2];
        {
            klib::LockGuard guard(futex_hash_table_lock);
            auto *futex_event = futex_hash_table[phy_addr];
            if (!futex_event)
                futex_event = futex_hash_table.emplace(phy_addr);
            events[0] = futex_event;
        }

        sched::Timer timer;
        defer { timer.disarm(); };
        if (timeout) {
            timer.remaining = *timeout;
            events[1] = &timer.event;
            timer.arm();
        }

        isize ret = sched::Event::wait({events, usize(timeout ? 2 : 1)});
        if (ret == 1)
            return -ETIMEDOUT;
        return ret;
    }

    isize futex_wake(u32 *uaddr, int max_to_wake) {
        __atomic_load_n(uaddr, __ATOMIC_SEQ_CST); // ensure page is not lazily mapped

        sched::Process *process = cpu::get_current_thread()->process;
        isize phy_addr = process->pagemap->get_physical_addr((uptr)uaddr);
        if (phy_addr < 0) // errno
            return phy_addr;

        sched::Event *event = nullptr;
        {
            klib::LockGuard guard(futex_hash_table_lock);
            event = futex_hash_table[phy_addr];
        }
        if (!event)
            return 0;

        return event->trigger(true, max_to_wake);
    }

    isize syscall_futex(u32 *uaddr, int op, usize arg1, usize arg2, usize arg3, usize arg4) {
        log_syscall("futex(%#lX, %d, %#lX, %#lX, %#lX, %#lX)\n", (uptr)uaddr, op, arg1, arg2, arg3, arg4);
        if (op & FUTEX_PRIVATE_FLAG) // can be ignored since its just a performance optimization
            op &= ~FUTEX_PRIVATE_FLAG;

        switch (op) {
        case FUTEX_WAIT: return futex_wait(uaddr, arg1, (klib::TimeSpec*)arg2);
        case FUTEX_WAKE: return futex_wake(uaddr, arg1);
        default:
            klib::printf("futex: unimplemented op %d\n", op);
            return -ENOSYS;
        }
    }
}
