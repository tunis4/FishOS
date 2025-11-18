// This file uses code from the Astral project
// See NOTICE.md for the license of Astral

#pragma once

#include <klib/algorithm.hpp>
#include <klib/cstring.hpp>

namespace klib {
    template<typename T, usize s>
    struct RingBuffer {
        static constexpr usize size = s;

        usize read_index = 0, write_index = 0;
        T data[size];

        RingBuffer() {}

        usize data_count() const { return write_index - read_index; }
        usize free_count() const { return size - data_count(); }
        bool is_empty() const { return data_count() == 0; }
        bool is_full() const { return data_count() == size; }

        usize read(T *buffer, usize count) {
            count = min(count, data_count());
            if (count == 0) return 0;
            usize first_pass_offset = read_index % size;
            usize first_pass_remaining = size - first_pass_offset;
            usize first_pass_count = min(count, first_pass_remaining);

            memcpy(buffer, data + first_pass_offset, first_pass_count * sizeof(T));

            if (first_pass_count < count)
                memcpy(buffer + first_pass_count, data, (count - first_pass_count) * sizeof(T));

            read_index += count;
            return count;
        }

        usize write(const T *buffer, usize count) {
            count = min(count, free_count());
            usize first_pass_offset = write_index % size;
            usize first_pass_remaining = size - first_pass_offset;
            usize first_pass_count = min(count, first_pass_remaining);

            memcpy(data + first_pass_offset, buffer, first_pass_count * sizeof(T));

            if (first_pass_count < count)
                memcpy(data, buffer + first_pass_count, (count - first_pass_count) * sizeof(T));

            write_index += count;
            return count;
        }

        usize truncate(usize count) {
            count = min(count, data_count());
            read_index += count;
            return count;
        }

        usize write_truncate(const T *buffer, usize count) {
            if (free_count() < count)
                truncate(count);
            return write(buffer, count);
        }

        // read first element without removing it
        usize peek(T *buffer) {
            if (is_empty())
                return 0;
            memcpy(buffer, data + (read_index % size), sizeof(T));
            return 1;
        }
    };
}
