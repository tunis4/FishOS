#pragma once

#include <klib/common.hpp>
#include <klib/algorithm.hpp>

namespace klib {
    template<usize size>
    struct Bitmap {
        static constexpr usize bits_per_usize = sizeof(usize) * 8;

        usize data[bits_to<usize>(size)] = {};

        Bitmap() {}

        inline bool get(usize index) const {
            return (data[index / bits_per_usize] >> (index % bits_per_usize)) & 1;
        }

        inline void set(usize index, bool value) {
            usize d = index / bits_per_usize;
            usize r = index % bits_per_usize;
            if (value)
                data[d] |= (usize)1 << r;
            else
                data[d] &= ~((usize)1 << r);
        }
    };
}
