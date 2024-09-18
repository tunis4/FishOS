#pragma once

[[noreturn]] static inline void abort() {
    asm volatile("cli");
    while (true) asm volatile("hlt");
}

[[noreturn]] void panic(const char *format, ...);

#ifndef NDEBUG
#define ASSERT(x) do { if (!(x)) [[unlikely]] panic("Assertion \"%s\" failed in %s at %s:%d", #x, __PRETTY_FUNCTION__, __FILE__, __LINE__); } while (false)
#else
#define ASSERT(x) do { static_cast<void>(x); } while (false)
#endif
