#pragma once

#include <klib/common.hpp>
#include <klib/cstring.hpp>

namespace klib {
    void* malloc(usize size);
    void* calloc(usize size);
    void* realloc(void *ptr, usize size);
    void free(void *ptr);
}
