#include <klib/cstdio.hpp>
#include <klib/lock.hpp>
#include <gfx/terminal.hpp>
#include <gfx/framebuffer.hpp>
#include <cpu/cpu.hpp>

static inline usize num_digits(u64 x, u64 base = 10) {
    usize i = 0;
    while (x /= base) i++;
    return i;
}

namespace klib {
    static klib::Spinlock print_lock;

    int putchar(char c) {
        // output to serial port first
        if (c == '\n') {
            cpu::out<u8>(0x3F8, '\r'); // maybe needed
        } else if (c == '\b') {
            cpu::out<u8>(0x3F8, '\b');
            cpu::out<u8>(0x3F8, ' '); // clear the last char
        }
        cpu::out<u8>(0x3F8, c);
        
        if (gfx::is_kernel_terminal_ready()) {
            gfx::kernel_terminal().write_char(c);
        } else { // very simple early terminal for printing boot errors
            // auto &fb = gfx::main_framebuffer;
            // if (fb.addr) {
            //     static usize x = 0;
            //     static usize y = 0;
            //     if (x > fb.width / 8) { x = 0; y++; }
            //     if (y > fb.height / 16) { y = 0; }
            //     if (c == '\n') { x = 0; y++; return c; }
            //     fb.draw_psf_char(c, x, y, 4, 4, ~0, 0);
            //     x++;
            // }
        }
        
        return c;
    }

    template<typename F>
    int printf_template(F put, const char *format, va_list list) {
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

    int vprintf(const char *format, va_list list) {
        InterruptLock interrupt_guard;
        LockGuard<Spinlock> guard(print_lock);
        return printf_template(putchar, format, list);
    }

    int printf(const char *format, ...) {
        va_list list;
        va_start(list, format);
        int i = vprintf(format, list);
        va_end(list);
        return i;
    }

    int snprintf(char *buffer, usize size, const char *format, ...) {
        va_list list;
        va_start(list, format);

        usize i = 0;
        int written = printf_template([&] (char c) {
            if (i < size - 1) {
                buffer[i] = c;
                i++;
            }
        }, format, list);
        buffer[i] = '\0';

        va_end(list);
        return written;
    }

    void panic_vprintf(const char *format, va_list list) {
        print_lock.unlock();
        vprintf(format, list);
    }

    void panic_printf(const char *format, ...) {
        va_list list;
        va_start(list, format);
        panic_vprintf(format, list);
        va_end(list);
    }
}
