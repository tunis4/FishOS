#pragma once

#include <klib/coroutine.hpp>
#include <sched/event.hpp>

namespace klib {
    template<typename Result = void>
    struct [[nodiscard]] Awaitable {
        using result_type = Result;

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }

            template<typename P>
            auto await_suspend(std::coroutine_handle<P> handle) noexcept {
                return handle.promise().continuation;
            }

            void await_resume() const noexcept {}
        };

        struct Promise {
            std::coroutine_handle<> continuation;
            Result result;

            Awaitable get_return_object() {
                return Awaitable { std::coroutine_handle<Promise>::from_promise(*this) };
            }

            void unhandled_exception() noexcept {}

            void return_value(Result &&res) noexcept { result = move(res); }

            std::suspend_always initial_suspend() noexcept { return {}; }
            FinalAwaiter final_suspend() noexcept { return {}; }
        };
        using promise_type = Promise;

        Awaitable() = default;

        ~Awaitable() {
            if (coroutine)
                coroutine.destroy();
        }

        struct Awaiter {
            std::coroutine_handle<Promise> handle;

            bool await_ready() const noexcept { return !handle || handle.done(); }

            auto await_suspend(std::coroutine_handle<> calling) noexcept {
                handle.promise().continuation = calling;
                return handle;
            }

            template<typename T = Result>
            requires(is_same<T, void>)
            void await_resume() noexcept {}

            template<typename T = Result>
            requires(!is_same<T, void>)
            T await_resume() noexcept { return move(handle.promise().result); }
        };

        auto operator co_await() noexcept { return Awaiter { coroutine }; }

        Awaitable(Awaitable &&other) : coroutine(other.coroutine) {
            other.coroutine = nullptr;
        }

        Awaitable(const Awaitable&) = delete;
        Awaitable& operator=(const Awaitable&) = delete;

    private:
        explicit Awaitable(std::coroutine_handle<Promise> coroutine) : coroutine(coroutine) {}

        std::coroutine_handle<Promise> coroutine;
    };

    template<>
    struct Awaitable<void>::Promise {
        std::coroutine_handle<> continuation;

        Awaitable get_return_object() {
            return Awaitable { std::coroutine_handle<Promise>::from_promise(*this) };
        }

        void unhandled_exception() noexcept {}

        void return_void() noexcept {}

        std::suspend_always initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
    };

    template<typename R>
    struct RequestCallback {
        using Callback = void(R result, void *data);

        void *data = nullptr;
        Callback *callback = nullptr;

        void set_callback(void *new_data, Callback *new_callback) {
            data = new_data;
            callback = new_callback;
        }

        void invoke(R result) {
            if (callback)
                callback(result, data);
        }
    };

    template<typename R>
    class [[nodiscard]] RootAwaitable {
        RequestCallback<R> *callback = nullptr;
        R result;
        bool has_result;
        std::coroutine_handle<> handle = nullptr;

        static void callback_handler(R result, void *data) {
            auto *awaitable = (RootAwaitable<R>*)data;
            awaitable->result = result;
            awaitable->has_result = true;
            if (awaitable->handle)
                awaitable->handle.resume();
        }

    public:
        using result_type = R;

        RootAwaitable() = delete;

        RootAwaitable(RequestCallback<R> *callback) : callback(callback), has_result(false) {
            callback->set_callback(this, &callback_handler);
        }

        RootAwaitable(R result) : result(result), has_result(true) {}

        RootAwaitable(RootAwaitable &&other) {
            swap(this->handle, other.handle);
            this->has_result = other.has_result;
            this->callback = nullptr;
            if (this->has_result) {
                this->result = other.result;
            } else {
                swap(this->callback, other.callback);
                this->callback->set_callback(this, &callback_handler);
            }
        }

        RootAwaitable(const RootAwaitable&) = delete;
        RootAwaitable& operator=(const RootAwaitable&) = delete;

        auto operator co_await() {
            struct Awaiter {
                RootAwaitable<R> &awaitable;

                bool await_ready() const noexcept {
                    return awaitable.has_result;
                }

                void await_suspend(std::coroutine_handle<> handle) noexcept {
                    awaitable.handle = handle;
                }

                R await_resume() const noexcept {
                    return awaitable.result;
                }
            };
            return Awaiter { *this };
        }
    };

    template<typename Result>
    struct [[nodiscard]] SyncWaitTask {
        struct Promise;

        struct CompletionNotifier {
            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<Promise> coroutine) noexcept {
                coroutine.promise().event->trigger();
            }

            void await_resume() noexcept {}
        };

        struct Promise {
            sched::Event *event;
            Result result;

            void start(sched::Event *new_event) {
                event = new_event;
                std::coroutine_handle<Promise>::from_promise(*this).resume();
            }

            auto get_return_object() noexcept {
                return std::coroutine_handle<Promise>::from_promise(*this);
            }

            void unhandled_exception() noexcept { unreachable(); }
            void return_void() noexcept { unreachable(); }

            auto yield_value(Result result) noexcept {
                this->result = result;
                return final_suspend();
            }

            std::suspend_always initial_suspend() noexcept { return {}; }
            auto final_suspend() noexcept { return CompletionNotifier {}; }
        };
        using promise_type = Promise;

        SyncWaitTask(std::coroutine_handle<Promise> coroutine) : coroutine(coroutine) {}
        ~SyncWaitTask() { if (coroutine) coroutine.destroy(); }

        void start(sched::Event *event) {
            coroutine.promise().start(event);
        }

        template<typename T = Result>
        requires(!is_same<T, void>)
        Result result() {
            return coroutine.promise().result;
        }

    private:
        std::coroutine_handle<Promise> coroutine;
    };

    template<>
    struct SyncWaitTask<void>::Promise {
        sched::Event *event;

        void start(sched::Event *new_event) {
            event = new_event;
            std::coroutine_handle<Promise>::from_promise(*this).resume();
        }

        auto get_return_object() noexcept {
            return std::coroutine_handle<Promise>::from_promise(*this);
        }

        void unhandled_exception() noexcept {}
        void return_void() noexcept {}

        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept { return CompletionNotifier {}; }
    };

    template<typename A>
    requires(is_same<typename A::result_type, void>)
    SyncWaitTask<void> make_sync_wait_task(A &&awaitable) {
        co_await forward<A>(awaitable);
    }

    template<typename A>
    requires(!is_same<typename A::result_type, void>)
    SyncWaitTask<typename A::result_type> make_sync_wait_task(A &&awaitable) {
        co_yield co_await forward<A>(awaitable);
    }

    template<typename A>
    A::result_type sync(A &&awaitable) {
        auto task = make_sync_wait_task(forward<A>(awaitable));
        sched::Event event;
        task.start(&event);
        event.wait();
        if constexpr (!is_same<typename A::result_type, void>)
            return task.result();
    }
}
