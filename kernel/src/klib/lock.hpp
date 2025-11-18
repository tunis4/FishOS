#pragma once

#include <klib/common.hpp>
#include <cpu/cpu.hpp>
#include <panic.hpp>

#define SPINLOCK_DEBUG 1

namespace klib {
#if SPINLOCK_DEBUG
    [[gnu::format(printf, 1, 2)]] int printf_unlocked(const char *format, ...);
#endif

    struct Spinlock {
        volatile bool locked = false;
#if SPINLOCK_DEBUG
        u32 i = 0;
        StackFrame *locker_frame = nullptr;
#endif

        inline void lock() {
#if SPINLOCK_DEBUG
            if (cpu::get_interrupt_state() == true)
                panic("Attempted to lock spinlock while interrupts are enabled");
#endif
            while (__atomic_test_and_set(&this->locked, __ATOMIC_ACQUIRE)) {
#if SPINLOCK_DEBUG
                i++;
                if (i >= 100000000) {
                    klib::printf_unlocked("\nLocker Stacktrace:\n");
                    while (true) {
                        if (locker_frame == nullptr || locker_frame->ip == 0)
                            break;
                        
                        klib::printf_unlocked("%#lX\n", locker_frame->ip);
                        locker_frame = locker_frame->next;
                    }
                    panic("Deadlock detected");
                }
#endif
                asm volatile("pause");
            }
#if SPINLOCK_DEBUG
            locker_frame = (StackFrame*)__builtin_frame_address(0);
#endif
        }

        inline void unlock() {
#if SPINLOCK_DEBUG
            i = 0;
            locker_frame = nullptr;
#endif
            __atomic_clear(&this->locked, __ATOMIC_RELEASE);
        }
    };

    template<class L>
    concept BasicLockable = requires(L l) {
        l.lock();
        l.unlock();
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

    template<BasicLockable L>
    class SpinlockGuard {
        L &guarded_lock;
        InterruptLock interrupt_lock;

    public:
        explicit SpinlockGuard(L &l) : guarded_lock(l) { lock(); }
        ~SpinlockGuard() { unlock(); }

        SpinlockGuard(const SpinlockGuard&) = delete;
        SpinlockGuard& operator =(const SpinlockGuard&) = delete;

        void lock() { guarded_lock.lock(); }
        void unlock() { guarded_lock.unlock(); }
    };
}
