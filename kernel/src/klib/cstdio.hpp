#pragma once

#include <stdarg.h>
#include <klib/common.hpp>
#include <klib/lock.hpp>
#include <klib/algorithm.hpp>

namespace klib {
    int putchar(char c);
    [[gnu::format(printf, 1, 0)]] int vprintf(const char *format, va_list list);
    [[gnu::format(printf, 1, 2)]] int printf(const char *format, ...);
    [[gnu::format(printf, 3, 4)]] int snprintf(char *buffer, usize size, const char *format, ...);
    [[gnu::format(printf, 1, 0)]] int vprintf_unlocked(const char *format, va_list list);
    [[gnu::format(printf, 1, 2)]] int printf_unlocked(const char *format, ...);

    extern klib::Spinlock print_lock;

    struct PrintGuard {
        InterruptLock interrupt_guard;
        LockGuard<Spinlock> lock_guard;

        PrintGuard() : interrupt_guard(), lock_guard(print_lock) {}
    };

    template<typename F> concept Putchar = requires(F f) { f(' '); };

    template<Putchar Put>
    [[gnu::format(printf, 2, 0)]] constexpr inline int vprintf_template(Put put, const char *format, va_list list) {
        int written = 0;
        for (int i = 0; format[i]; i++) {
            bool alt_form = false;
            bool long_int = false;
            usize zero_pad = 0;
            usize s_length = 0;

            if (format[i] == '%') {
                i++;

                while (1) {
                    switch (format[i]) {
                    case '#':
                        alt_form = true;
                        i++;
                        break;
                    case '0':
                        i++;
                        zero_pad = format[i] - '0';
                        i++;
                        if (format[i] >= '0' && format[i] <= '9') {
                            zero_pad = ((format[i - 1] - '0') * 10) + (format[i] - '0');
                            i++;
                        }
                        break;
                    case 'l':
                        long_int = true;
                        i++;
                        break;
                    case '.':
                        i++;
                        if (format[i] == '*') {
                            s_length = va_arg(list, usize);
                            i++;
                        }
                        break;
                    case 'x':
                    case 'X': {
                        if (alt_form) {
                            put('0');
                            put('x');
                            written += 2;
                        }
                        const char *low = "0123456789abcdef";
                        const char *high = "0123456789ABCDEF";
                        const char *used = format[i] == 'X' ? high : low;
                        u64 value = 0;
                        if (long_int) value = va_arg(list, u64);
                        else value = va_arg(list, u32);
                        usize digits = num_digits(value, 16);
                        if (zero_pad && zero_pad > digits) {
                            for (usize j = 0; j < zero_pad - digits - 1; j++)
                                put('0');
                            written += zero_pad - digits - 1;
                        }
                        for (int di = digits; di >= 0; di--) {
                            u64 v = value;
                            for (int dii = 0; dii < di; dii++) v /= 16;
                            u64 d = v % 16;
                            put(used[d]);
                            written++;
                        }
                        goto end;
                    }
                    case 'd': {
                        u64 value = 0;
                        if (long_int) {
                            value = va_arg(list, u64);
                            if ((i64)value < 0) {
                                put('-');
                                value = -(i64)value;
                            }
                        } else {
                            value = va_arg(list, u32);
                            if ((i32)value < 0) {
                                put('-');
                                value = -(i32)value;
                            }
                        }
                        usize digits = num_digits(value);
                        if (zero_pad && zero_pad > digits) {
                            for (usize j = 0; j < zero_pad - digits - 1; j++)
                                put('0');
                            written += zero_pad - digits - 1;
                        }
                        for (int di = digits; di >= 0; di--) {
                            u64 v = value;
                            for (int dii = 0; dii < di; dii++) v /= 10;
                            u64 d = v % 10;
                            put('0' + d);
                            written++;
                        }
                        goto end;
                    }
                    case 'u': {
                        u64 value = 0;
                        if (long_int) value = va_arg(list, u64);
                        else value = va_arg(list, u32);
                        usize digits = num_digits(value);
                        if (zero_pad && zero_pad > digits) {
                            for (usize j = 0; j < zero_pad - digits - 1; j++)
                                put('0');
                            written += zero_pad - digits - 1;
                        }
                        for (int di = digits; di >= 0; di--) {
                            u64 v = value;
                            for (int dii = 0; dii < di; dii++) v /= 10;
                            u64 d = v % 10;
                            put('0' + d);
                            written++;
                        }
                        goto end;
                    }
                    case 'c': {
                        char c = va_arg(list, int);
                        put(c);
                        written++;
                        goto end;
                    }
                    case 's': {
                        const char *str = va_arg(list, const char*);
                        if (s_length) {
                            for (usize i = 0; i < s_length; i++) {
                                put(str[i]);
                                written++;
                            }
                        } else {
                            for (usize i = 0; str[i]; i++) {
                                put(str[i]);
                                written++;
                            }
                        }
                        goto end;
                    }
                    }
                }
            end:
                continue;
            }

            written++;
            put(format[i]);
        }
        return written;
    }

    template<Putchar Put>
    [[gnu::format(printf, 2, 3)]] constexpr inline int printf_template(Put put, const char *format, ...) {
        va_list list;
        va_start(list, format);
        int i = vprintf_template(put, format, list);
        va_end(list);
        return i;
    }
}
