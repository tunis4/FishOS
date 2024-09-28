#pragma once

#include <stdarg.h>
#include <klib/common.hpp>

namespace klib {
    int putchar(char c);
    [[gnu::format(printf, 1, 0)]] int vprintf(const char *format, va_list list);
    [[gnu::format(printf, 1, 2)]] int printf(const char *format, ...);
    [[gnu::format(printf, 3, 4)]] int snprintf(char *buffer, usize size, const char *format, ...);
    [[gnu::format(printf, 1, 0)]] void panic_vprintf(const char *format, va_list list);
    [[gnu::format(printf, 1, 2)]] void panic_printf(const char *format, ...);
}
