#pragma once

#include <klib/cstdlib.hpp>

namespace klib {
    template<typename T>
    class Vector {
        T *m_buffer;
        usize m_size;
        usize m_capacity;

    public:
        Vector(usize reserve = 0) : m_size(0), m_capacity(reserve) {
            m_buffer = (T*)(reserve ? klib::malloc(reserve * sizeof(T)) : nullptr);
        }

        ~Vector() {
            klib::free(m_buffer);
        }

        Vector(const Vector &) = delete;
        Vector(const Vector &&) = delete;

        T& operator [](usize index) const {
            return m_buffer[index];
        }

        template<typename... Args>
        T& emplace_back(Args&&... args) {
            if (m_size == m_capacity) {
                m_capacity = m_capacity ? m_capacity * 2 : 1;
                m_buffer = (T*)klib::realloc(m_buffer, m_capacity * sizeof(T));
            }
            return *new (m_buffer + m_size++) T(klib::forward<Args>(args)...);
        }

        T& push_back(const T &t) { return emplace_back(t); }
        T& push_back(T &&t) { return emplace_back(klib::move(t)); }

        inline constexpr T* data() const { return m_buffer; }
        inline constexpr usize size() const { return m_size; }
        inline constexpr usize capacity() const { return m_capacity; }

        void clear() {
            m_size = 0;
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

        iterator begin() { return iterator(m_buffer); }
        iterator end() { return iterator(m_buffer + m_size); }
    };
}
