#pragma once

#include <klib/common.hpp>
#include <panic.hpp>

// needs to be in std with these specific names or else compiler complains
namespace std {
    template<typename Promise = void>
    class coroutine_handle;

    template<>
    class coroutine_handle<void> {
        void *handle = nullptr;

    public:
        constexpr coroutine_handle() noexcept = default;
        constexpr coroutine_handle(nullptr_t) noexcept {}

        coroutine_handle& operator=(nullptr_t) noexcept {
            handle = nullptr;
            return *this;
        }

        static constexpr coroutine_handle from_address(void *addr) noexcept {
            coroutine_handle tmp;
            tmp.handle = addr;
            return tmp;
        }

        constexpr void* address() const noexcept { return handle; }
        constexpr explicit operator bool() const noexcept { return handle != nullptr; }
        bool done() const { return __builtin_coro_done(handle); }
        void destroy() const { __builtin_coro_destroy(handle); }
        void operator()() const { resume(); }

        void resume() const {
            ASSERT(!done());
            __builtin_coro_resume(handle);
        }
    };

    template<typename Promise>
    class coroutine_handle {
        void *handle = nullptr;

    public:
        constexpr coroutine_handle() noexcept = default;
        constexpr coroutine_handle(nullptr_t) noexcept {}

        coroutine_handle& operator=(nullptr_t) noexcept {
            handle = nullptr;
            return *this;
        }

        static coroutine_handle from_promise(Promise &promise) {
            coroutine_handle tmp;
            tmp.handle = __builtin_coro_promise(klib::addressof(const_cast<klib::RemoveCv<Promise>::type&>(promise)), alignof(Promise), true);
            return tmp;
        }

        static constexpr coroutine_handle from_address(void *addr) noexcept {
            coroutine_handle tmp;
            tmp.handle = addr;
            return tmp;
        }

        constexpr void* address() const noexcept { return handle; }
        constexpr explicit operator bool() const noexcept { return handle != nullptr; }
        bool done() const { return __builtin_coro_done(handle); }
        void destroy() const { __builtin_coro_destroy(handle); }
        void operator()() const { resume(); }

        void resume() const {
            ASSERT(!done());
            __builtin_coro_resume(handle);
        }

        constexpr operator coroutine_handle<>() const {
            return coroutine_handle<>::from_address(address());
        }

        Promise& promise() const {
            return *static_cast<Promise*>(__builtin_coro_promise(this->handle, alignof(Promise), false));
        }
    };

    struct suspend_never {
        constexpr bool await_ready() const noexcept { return true; }
        constexpr void await_suspend(coroutine_handle<>) const noexcept {}
        constexpr void await_resume() const noexcept {}
    };

    struct suspend_always {
        constexpr bool await_ready() const noexcept { return false; }
        constexpr void await_suspend(coroutine_handle<>) const noexcept {}
        constexpr void await_resume() const noexcept {}
    };

    template<typename... T>
    using void_t = void;

    template<typename T, typename = void>
    struct __coroutine_traits {};

    template<typename T>
    struct __coroutine_traits<T, void_t<typename T::promise_type>> {
        using promise_type = typename T::promise_type;
    };

    template<typename Ret, typename... Args>
    struct coroutine_traits : public __coroutine_traits<Ret> {};
}
