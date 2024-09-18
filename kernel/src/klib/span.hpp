#pragma once

#include <klib/common.hpp>

namespace klib {
    template<typename T>
    struct Span {
        T *data;
        usize size;

        template<usize N>
        constexpr Span(T (&arr)[N]) {
            data = arr;
            size = N;
        }

        constexpr Span(T *arr, usize num) {
            data = arr;
            size = num;
        }

        constexpr Span(T &single) {
            data = &single;
            size = 1;
        }

        template<typename V>
        constexpr Span(V &v) {
            data = v.data();
            size = v.size();
        }

        ~Span() {}

        Span(const Span &) = delete;
        Span(const Span &&) = delete;

        T& operator [](usize index) const {
            return data[index];
        }

        struct iterator {
            using value_type = T;
            using pointer = T*;
            using reference = T&;

            explicit iterator(pointer ptr) : ptr(ptr) {}
            reference operator *() const { return *ptr; }
            pointer operator ->() const { return ptr; }

            iterator& operator ++() { ++ptr; return *this; }  
            iterator operator ++(int) { iterator tmp = *this; ++ptr; return tmp; }
            iterator& operator --() { --ptr; return *this; }  
            iterator operator --(int) { iterator tmp = *this; --ptr; return tmp; }

            friend bool operator ==(const iterator &a, const iterator &b) { return a.ptr == b.ptr; }
            friend bool operator !=(const iterator &a, const iterator &b) { return a.ptr != b.ptr; }
            friend usize operator -(const iterator &a, const iterator &b) { return a.ptr - b.ptr; }
            friend bool operator <(const iterator &a, const iterator &b) { return a.ptr < b.ptr; }

        private:
            pointer ptr;
        };
        
        iterator begin() { return iterator(data); }
        iterator end() { return iterator(data + size); }
    };
}
