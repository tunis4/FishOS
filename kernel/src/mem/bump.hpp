#pragma once

#include <klib/common.hpp>

namespace mem::bump {
    constexpr usize alignment = 16;

    void init(uptr base, usize size);
    void* allocate(usize size);
    void* reallocate(void *ptr, usize size);
    void free(void *ptr);
}
