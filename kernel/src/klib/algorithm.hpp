#pragma once

#include <klib/common.hpp>

namespace klib {
    template<Integral A, Integral B>
    inline constexpr auto min(const A a, const B b) {
        return (b < a) ? b : a;
    }

    template<Integral A, Integral B>
    inline constexpr auto max(const A a, const B b) {
        return (a < b) ? b : a;
    }

    template<Integral V, Integral L, Integral H>
    inline constexpr auto clamp(const V v, const L low, const H high) {
        return min(max(v, low), high);
    }

    template<Integral T>
    inline constexpr const T align_up(const T i, usize alignment) {
        return i % alignment ? ((i / alignment) + 1) * alignment : i;
    }

    template<Integral T>
    inline constexpr const T align_down(const T i, usize alignment) {
        return i % alignment ? (i / alignment) * alignment : i;
    }

    template<Integral T>
    inline constexpr usize bits_to(usize i) {
        return align_up(i, sizeof(T) * 8) / (sizeof(T) * 8);
    }

    inline constexpr usize num_digits(u64 x, u64 base = 10) {
        usize i = 0;
        while (x /= base) i++;
        return i;
    }
}
