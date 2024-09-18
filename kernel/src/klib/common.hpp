#pragma once

#define __MLIBC_ABI_ONLY

#include <stdint.h>
#include <stddef.h>

#define alloca(type, count) (type*)__builtin_alloca(count * sizeof(type));

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using usize = size_t;
using isize = i64;
using uptr = uintptr_t;
using uint = unsigned int;

using nullptr_t = decltype(nullptr);

namespace klib {
    template<typename T, T v>
    struct IntegralConstant {
        static constexpr T value = v;
    };

    template<bool v>
    using BooleanConstant = IntegralConstant<bool, v>;
    
    using True = BooleanConstant<true>;
    using False = BooleanConstant<false>;

    template<typename> struct IsIntegral : public False {};
    template<> struct IsIntegral<bool> : public True {};
    template<> struct IsIntegral<wchar_t> : public True {};
    template<> struct IsIntegral<char8_t> : public True {};
    template<> struct IsIntegral<char16_t> : public True {};
    template<> struct IsIntegral<char32_t> : public True {};
    template<> struct IsIntegral<u8> : public True {};
    template<> struct IsIntegral<u16> : public True {};
    template<> struct IsIntegral<u32> : public True {};
    template<> struct IsIntegral<u64> : public True {};
    template<> struct IsIntegral<i8> : public True {};
    template<> struct IsIntegral<i16> : public True {};
    template<> struct IsIntegral<i32> : public True {};
    template<> struct IsIntegral<i64> : public True {};
    
    template<typename T> concept Integral = IsIntegral<T>::value;

    template<usize _bits>
    struct __numeric_limits_helper {
        static constexpr usize bits = _bits;
    };

    template<typename> struct NumericLimits {};
    template<> struct NumericLimits<bool> : public __numeric_limits_helper<8> {};
    template<> struct NumericLimits<wchar_t> : public __numeric_limits_helper<16> {};
    template<> struct NumericLimits<char8_t> : public __numeric_limits_helper<8> {};
    template<> struct NumericLimits<char16_t> : public __numeric_limits_helper<16> {};
    template<> struct NumericLimits<char32_t> : public __numeric_limits_helper<32> {};
    template<> struct NumericLimits<u8> : public __numeric_limits_helper<8> {};
    template<> struct NumericLimits<u16> : public __numeric_limits_helper<16> {};
    template<> struct NumericLimits<u32> : public __numeric_limits_helper<32> {};
    template<> struct NumericLimits<u64> : public __numeric_limits_helper<64> {};
    template<> struct NumericLimits<i8> : public __numeric_limits_helper<8> {};
    template<> struct NumericLimits<i16> : public __numeric_limits_helper<16> {};
    template<> struct NumericLimits<i32> : public __numeric_limits_helper<32> {};
    template<> struct NumericLimits<i64> : public __numeric_limits_helper<64> {};
    static_assert(NumericLimits<usize>::bits == NumericLimits<isize>::bits);
    
    template<typename T> struct RemoveCv { using type = T; };
    template<typename T> struct RemoveCv<const T> { using type = T; };
    template<typename T> struct RemoveCv<volatile T> { using type = T; };
    template<typename T> struct RemoveCv<const volatile T> { using type = T; };
    
    template<typename T> struct RemoveConst { using type = T; };
    template<typename T> struct RemoveConst<const T> { using type = T; };
    
    template<typename T> struct RemoveVolatile { using type = T; };
    template<typename T> struct RemoveVolatile<volatile T> { using type = T; };

    template<typename T> struct RemovePointer { using type = T; };
    template<typename T> struct RemovePointer<T*> { using type = T; };
    template<typename T> struct RemovePointer<T* const> { using type = T; };
    template<typename T> struct RemovePointer<T* volatile>  { using type = T; };
    template<typename T> struct RemovePointer<T* const volatile> { using type = T; };

    template<typename T> struct RemoveReference { using type = T; };
    template<typename T> struct RemoveReference<T&> { using type = T; };
    template<typename T> struct RemoveReference<T&&> { using type = T; };
    
    template<typename T> struct IsLValueReference : public False {};
    template<typename T> struct IsLValueReference<T&> : public True {};

    template<typename T>
    constexpr inline T&& forward(typename RemoveReference<T>::type &t) {
        return static_cast<T&&>(t);
    }

    template<typename T>
    constexpr inline T&& forward(typename RemoveReference<T>::type &&t) {
        static_assert(!IsLValueReference<T>::value, "Can not forward an rvalue as an lvalue.");
        return static_cast<T&&>(t);
    }

    template<typename T>
    constexpr auto move(T&& t) {
        return static_cast<typename RemoveReference<T>::type&&>(t); 
    }

    template<typename T>
    constexpr inline void swap(T &a, T &b) {
        T tmp = move(a);
        a = move(b);
        b = move(tmp);
    }

    [[noreturn]] inline void unreachable() { __builtin_unreachable(); }

    inline u32 hash(const char *str) { // djb2a
        u32 hash = 5381;
        for (const char *c = str; *c; c++)
            hash = ((hash << 5) + hash) ^ *c; // hash * 33 ^ str[i]
        return hash;
    }

    struct ScopeExitTag {};

    template<typename Function>
    class ScopeExit final {
        Function function;
    public:
        explicit ScopeExit(Function &&fn) : function(klib::move(fn)) {}
        ~ScopeExit() {
            function();
        }
    };

    template<typename Function>
    auto operator->*(ScopeExitTag, Function &&function) {
        return ScopeExit<Function>{forward<Function>(function)};
    }
}

inline void* operator new(usize, void *p)      { return p; }
inline void* operator new[](usize, void *p)    { return p; }
inline void  operator delete  (void *, void *) { }
inline void  operator delete[](void *, void *) { }

#define CONCAT(a, b) a ## b
#define CONCAT2(a, b) CONCAT(a, b)

#define defer auto CONCAT2(_defer, __LINE__) = ::klib::ScopeExitTag{}->*[&]()
