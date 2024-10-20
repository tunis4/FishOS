#pragma once

#include <klib/common.hpp>
#include <klib/cstring.hpp>

namespace klib {
    void* malloc(usize size);
    void* aligned_alloc(usize size, usize alignment);
    void* calloc(usize size);
    void* realloc(void *ptr, usize size);
    void free(void *ptr);
}
