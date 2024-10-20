#pragma once

#include <klib/common.hpp>

namespace mem::bump {
    constexpr usize default_alignment = 16;

    void init(uptr base, usize size);
    void* allocate(usize size, usize alignment = default_alignment);
    void* reallocate(void *ptr, usize size);
    void free(void *ptr);
}
