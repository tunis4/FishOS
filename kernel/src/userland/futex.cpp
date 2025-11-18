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
    static klib::HashTable<uptr, sched::Event> futex_hash_table(256);
    static klib::Spinlock futex_hash_table_lock;

    isize futex_wait(u32 *uaddr, u32 expected, const klib::TimeSpec *timeout) {
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != expected)
            return -EAGAIN;

        sched::Process *process = cpu::get_current_thread()->process;
        isize phy_addr = process->pagemap->get_physical_addr((uptr)uaddr);
        if (phy_addr < 0) // errno
            return phy_addr;

        sched::Event *events[2];
        {
            klib::SpinlockGuard guard(futex_hash_table_lock);
            auto *futex_event = futex_hash_table[phy_addr];
            if (!futex_event)
                futex_event = futex_hash_table.emplace(phy_addr, "futex event");
            events[0] = futex_event;
        }

        sched::Timer timer;
        defer { timer.disarm(); };
        if (timeout) {
            events[1] = &timer.event;
            timer.arm(*timeout);
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
            klib::SpinlockGuard guard(futex_hash_table_lock);
            event = futex_hash_table[phy_addr];
        }
        if (!event)
            return 0;

        return event->trigger(true, max_to_wake);
    }

    isize futex_wake_op(u32 *uaddr1, int max_to_wake1, u32 *uaddr2, int max_to_wake2, u32 op_encoded) {
        klib::InterruptLock interrupt_guard;

        u32 op = (op_encoded >> 28) & 0xf;
        u32 op_arg = (op_encoded >> 24) & 0xf;
        u32 cmp = (op_encoded >> 12) & 0xfff;
        u32 cmp_arg = op_encoded & 0xfff;
        u32 old_val = 0;

        if (op & FUTEX_OP_OPARG_SHIFT) {
            op_arg = 1 << op_arg;
            op &= ~FUTEX_OP_OPARG_SHIFT;
        }

        switch (op) {
        case FUTEX_OP_SET:  old_val = __atomic_exchange_n(uaddr2, op_arg, __ATOMIC_SEQ_CST); break;
        case FUTEX_OP_ADD:  old_val = __atomic_fetch_add( uaddr2, op_arg, __ATOMIC_SEQ_CST); break;
        case FUTEX_OP_OR:   old_val = __atomic_fetch_or(  uaddr2, op_arg, __ATOMIC_SEQ_CST); break;
        case FUTEX_OP_ANDN: old_val = __atomic_fetch_nand(uaddr2, op_arg, __ATOMIC_SEQ_CST); break;
        case FUTEX_OP_XOR:  old_val = __atomic_fetch_xor( uaddr2, op_arg, __ATOMIC_SEQ_CST); break;
        default: return -EINVAL;
        }

        isize ret1 = futex_wake(uaddr1, max_to_wake1);
        if (ret1 < 0) return ret1;

        bool cmp_result = false;
        switch (cmp) {
        case FUTEX_OP_CMP_EQ: cmp_result = old_val == cmp_arg; break;
        case FUTEX_OP_CMP_NE: cmp_result = old_val != cmp_arg; break;
        case FUTEX_OP_CMP_LT: cmp_result = old_val <  cmp_arg; break;
        case FUTEX_OP_CMP_LE: cmp_result = old_val <= cmp_arg; break;
        case FUTEX_OP_CMP_GT: cmp_result = old_val >  cmp_arg; break;
        case FUTEX_OP_CMP_GE: cmp_result = old_val >= cmp_arg; break;
        default: return -EINVAL;
        }

        isize ret2 = 0;
        if (cmp_result) {
            ret2 = futex_wake(uaddr2, max_to_wake2);
            if (ret2 < 0) return ret2;
        }

        return ret1 + ret2;
    }

    isize futex_wait_bitset(u32 *uaddr, u32 expected, const klib::TimeSpec *absolute_timeout, u32 mask, bool is_clock_realtime) {
        if (mask != FUTEX_BITSET_MATCH_ANY) {
            klib::printf("futex: FUTEX_WAIT_BITSET not implemented properly\n");
            return -ENOSYS;
        }

        if (absolute_timeout) {
            auto current_time = sched::get_clock(is_clock_realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC);
            klib::TimeSpec relative_timeout = *absolute_timeout - current_time;
            if (relative_timeout.is_zero())
                return -ETIMEDOUT;
            return futex_wait(uaddr, expected, &relative_timeout);
        } else {
            return futex_wait(uaddr, expected, nullptr);
        }
    }

    isize futex_wake_bitset(u32 *uaddr, int max_to_wake, u32 mask) {
        if (mask != FUTEX_BITSET_MATCH_ANY) {
            klib::printf("futex: FUTEX_WAKE_BITSET not implemented properly\n");
            return -ENOSYS;
        }
        return futex_wake(uaddr, max_to_wake);
    }

    isize syscall_futex(u32 *uaddr, int op, usize arg1, usize arg2, usize arg3, usize arg4) {
        log_syscall("futex(%#lX, %d, %#lX, %#lX, %#lX, %#lX)\n", (uptr)uaddr, op, arg1, arg2, arg3, arg4);
        if (op & FUTEX_PRIVATE_FLAG) // can be ignored since its just a performance optimization
            op &= ~FUTEX_PRIVATE_FLAG;

        bool is_clock_realtime = (op & FUTEX_CLOCK_REALTIME);

        switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAIT:
            if (is_clock_realtime)
                klib::printf("futex: FUTEX_CLOCK_REALTIME not supported for FUTEX_WAIT\n");
            return futex_wait(uaddr, arg1, (klib::TimeSpec*)arg2);
        case FUTEX_WAKE: return futex_wake(uaddr, arg1);
        case FUTEX_WAIT_BITSET: return futex_wait_bitset(uaddr, arg1, (klib::TimeSpec*)arg2, arg4, is_clock_realtime);
        case FUTEX_WAKE_BITSET: return futex_wake_bitset(uaddr, arg1, arg4);
        case FUTEX_WAKE_OP: return futex_wake_op(uaddr, arg1, (u32*)arg3, arg2, arg4);
        default:
            klib::printf("futex: unimplemented op %d\n", op);
            return -ENOSYS;
        }
    }

    isize syscall_get_robust_list(int pid, robust_list_head **head_ptr, usize *len_ptr) {
        log_syscall("get_robust_list(%d, %#lX, %#lX)\n", pid, (uptr)head_ptr, (uptr)len_ptr);
        return -ENOSYS;
    }

    isize syscall_set_robust_list(robust_list_head *head, usize len) {
        log_syscall("set_robust_list(%#lX, %#lX)\n", (uptr)head, len);
        return -ENOSYS;
    }
}
