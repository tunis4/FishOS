#include <klib/cstdio.hpp>
#include <klib/lock.hpp>
#include <gfx/terminal.hpp>
#include <gfx/framebuffer.hpp>
#include <cpu/cpu.hpp>

namespace klib {
    klib::Spinlock print_lock;

    int serial_putchar(char c) {
        if (c == '\n')
            cpu::out<u8>(0x3F8, '\r'); // maybe needed
        cpu::out<u8>(0x3F8, c);
        return c;
    }

    int putchar(char c) {
        serial_putchar(c);
        if (gfx::kernel_terminal_enabled)
            gfx::kernel_terminal().write_char(c);
        return c;
    }

    int vprintf_unlocked(const char *format, va_list list) {
        return vprintf_template(putchar, format, list);
    }

    int printf_unlocked(const char *format, ...) {
        va_list list;
        va_start(list, format);
        int i = vprintf_unlocked(format, list);
        va_end(list);
        return i;
    }

    int vprintf(const char *format, va_list list) {
        PrintGuard print_guard;
        return vprintf_unlocked(format, list);
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
        int written = vprintf_template([&] (char c) {
            if (i < size - 1) {
                buffer[i] = c;
                i++;
            }
        }, format, list);
        buffer[i] = '\0';

        va_end(list);
        return written;
    }
}
