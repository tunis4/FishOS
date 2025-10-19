#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <panic.hpp>

#define DETECT_DEADLOCK 1

namespace klib {
    struct Spinlock {
        volatile bool locked = false;
#if DETECT_DEADLOCK
        u32 i = 0;
#endif

        inline void lock() {
            while (__atomic_test_and_set(&this->locked, __ATOMIC_ACQUIRE)) {
#if DETECT_DEADLOCK
                i++;
                if (i >= 100000000)
                    panic("Deadlock detected");
#endif
                asm volatile("pause");
            }
        }

        inline void unlock() {
#if DETECT_DEADLOCK
            i = 0;
#endif
            __atomic_clear(&this->locked, __ATOMIC_RELEASE);
        }
    };

    template<class L>
    concept BasicLockable = requires(L l) {
        l.lock();
        l.unlock();
    };

    template<BasicLockable L>
    class LockGuard {
        L &guarded_lock;

    public:
        explicit LockGuard(L &l) : guarded_lock(l) { lock(); }
        ~LockGuard() { unlock(); }

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator =(const LockGuard&) = delete;

        void lock() { guarded_lock.lock(); }
        void unlock() { guarded_lock.unlock(); }
    };

    class InterruptLock {
        bool old_state;

    public:
        explicit InterruptLock() {
            old_state = cpu::get_interrupt_state();
            cpu::toggle_interrupts(false);
        }

        ~InterruptLock() {
            cpu::toggle_interrupts(old_state);
        }

        InterruptLock(const InterruptLock&) = delete;
        InterruptLock& operator =(const InterruptLock&) = delete;
    };
}
