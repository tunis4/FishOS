#include <klib/cstdlib.hpp>
#include <klib/lock.hpp>
#include <panic.hpp>

extern "C" {
    uptr __stack_chk_guard = 0xED1A449A97A8154E;
    void __stack_chk_fail() {
        panic("Stack smashing detected");
    }
}

static klib::Spinlock static_init_lock;
static bool static_init_old_interrupt_state;

using __guard = u64;
#define GUARD_INITIALIZED (1 << 0)
#define GUARD_IN_USE (1 << 1)

namespace __cxxabiv1  {
    extern "C" i32 __cxa_guard_acquire(__guard *g) {
        if (*g)
            return 0;

        static_init_old_interrupt_state = cpu::get_interrupt_state();
        cpu::toggle_interrupts(false);
        static_init_lock.lock();
        if (*g & GUARD_INITIALIZED) {
            static_init_lock.unlock();
            cpu::toggle_interrupts(static_init_old_interrupt_state);
            return 0;
        }

        if (*g & GUARD_IN_USE)
            panic("__cxa_guard_acquire: guard in use");

        *g |= GUARD_IN_USE;
        return 1;
    }

    extern "C" void __cxa_guard_release(__guard *g) {
        *g |= GUARD_INITIALIZED;
        static_init_lock.unlock();
        cpu::toggle_interrupts(static_init_old_interrupt_state);
    }

    extern "C" void __cxa_guard_abort(__guard *g) {
        panic("__cxa_guard_abort called");
    }
}

extern "C" void __cxa_pure_virtual() {
    panic("__cxa_pure_virtual called");
}
 
void* operator new(usize size) {
    return klib::malloc(size);
}
 
void* operator new[](usize size) {
    return klib::malloc(size);
}
 
void operator delete(void *ptr) {
    klib::free(ptr);
}
 
void operator delete[](void *ptr) {
    klib::free(ptr);
}

void operator delete(void *ptr, usize size) {
    ::operator delete(ptr);
    memset(ptr, 0xAE, size);
}

void operator delete[](void *ptr, usize size) {
    ::operator delete[](ptr);
}
