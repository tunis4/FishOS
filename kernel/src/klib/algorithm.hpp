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

    template<Integral T>
    inline T* qsort_partition(T *low, T *high) {
        T pivot = *(low + (high - low) / 2);

        while (true) {
            while (*low < pivot)
                ++low;

            do {
                --high;
            } while (pivot < *high);

            if (low >= high)
                return low;
            swap(*low, *high);
            ++low;
        }
    }

    // qsort implementation from https://stackoverflow.com/a/66288047
    template<Integral T>
    inline void qsort(T *begin, T *end) {
        if (end - begin < 2)
            return;

        T *p = qsort_partition(begin, end);
        qsort(begin, p);
        qsort(p, end);
    }
}
