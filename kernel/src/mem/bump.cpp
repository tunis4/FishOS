#include <mem/bump.hpp>
#include <klib/cstring.hpp>
#include <klib/lock.hpp>
#include <klib/algorithm.hpp>

namespace mem::bump {
    static klib::Spinlock alloc_lock;

    static uptr alloc_base;
    static usize total_size;
    static usize alloc_ptr;

    void init(uptr base, usize size) {
        alloc_base = base;
        total_size = size;
    }

    void* allocate(usize size, usize alignment) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(alloc_lock);

        if (size != 0) {
            uptr ret = klib::align_up(alloc_base + alloc_ptr, alignment);
            alloc_ptr = (ret - alloc_base) + size;
            ASSERT(alloc_ptr < total_size);
            return (void*)ret;
        }

        return nullptr;
    }

    void free(void *ptr) {}

    void* reallocate(void *ptr, usize size) {
        if (ptr == nullptr)
            return allocate(size);

        if (size == 0) {
            free(ptr);
            return nullptr;
        }

        void *newptr = allocate(size);
        memcpy(newptr, ptr, size);
        free(ptr);
        return newptr;

        return nullptr;
    }
}
