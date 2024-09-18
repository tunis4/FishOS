#pragma once

#include <klib/common.hpp>

namespace klib {
    template<Integral T> 
    inline constexpr const T min(const T a, const T b) {
        return (b < a) ? b : a;
    }

    template<Integral T> 
    inline constexpr const T max(const T a, const T b) {
        return (a < b) ? b : a;
    }

    template<Integral T> 
    inline constexpr const T clamp(const T v, const T low, const T high) {
        return min(max(v, low), high);
    }

    template<Integral T, T alignment>
    inline constexpr const T align_up(const T i) {
        return i % alignment ? ((i / alignment) + 1) * alignment : i;
    }

    template<Integral T, T alignment>
    inline constexpr const T align_down(const T i) {
        return i % alignment ? (i / alignment) * alignment : i;
    }

    template<Integral T>
    inline constexpr usize bits_to(usize i) {
        return align_up<usize, sizeof(T) * 8>(i) / (sizeof(T) * 8);
    }
}
