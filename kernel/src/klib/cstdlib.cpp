#include <klib/cstdlib.hpp>
#include <klib/cstdio.hpp>
#include <mem/bump.hpp>

namespace klib {
    void* malloc(usize size) {
        auto ptr = mem::bump::allocate(size);
        // printf("malloc(%ld): %#lX\n", size, (uptr)ptr);
        return ptr;
    }

    void* aligned_alloc(usize size, usize alignment) {
        return mem::bump::allocate(size, alignment);
    }

    void* calloc(usize size) {
        auto ptr = mem::bump::allocate(size);
        memset(ptr, 0, size);
        return ptr;
    }

    void* realloc(void *ptr, usize size) {
        return mem::bump::reallocate(ptr, size);
    }

    void free(void *ptr) {
        // printf("free(): %#lX\n", (uptr)ptr);
        mem::bump::free(ptr);
    }

    extern "C" int atexit(void (*func)()) {
        return 0;
    }
}
