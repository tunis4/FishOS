#include <sched/event.hpp>
#include <sched/sched.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>

#define EVENT_TRACE 0

#if EVENT_TRACE
#   define trace_event(format, ...) klib::printf(format __VA_OPT__(,) __VA_ARGS__);
#else
#   define trace_event(format, ...)
#endif

namespace sched {
    Event::Listener::Listener(Thread *thread, Event *event, int which) : thread(thread), event(event), which(which) {}

    // FIXME: deadlock if events contains duplicates
    static void lock_events(klib::Span<Event*> &events) {
        // for (auto *event : events)
        //     if (event)
        //         event->lock.lock();
    }

    static void unlock_events(klib::Span<Event*> &events) {
        // for (auto *event : events)
        //     if (event)
        //         event->lock.unlock();
    }

    isize Event::wait(klib::Span<Event*> events, bool nonblocking) {
        klib::InterruptLock guard;
        auto *thread = cpu::get_current_thread();

        if (thread->has_pending_signals())
            return -EINTR;

        lock_events(events);
        defer { unlock_events(events); };

        for (usize i = 0; i < events.size; i++) {
            auto *event = events[i];
            if (event && event->pending > 0) {
                event->pending--;
                return i;
            }
        }

        if (nonblocking)
            return -EWOULDBLOCK;

        ASSERT(thread->listeners.size() == 0);
        for (usize i = 0; i < events.size; i++) {
            auto *event = events[i];
            if (!event) continue;

            Listener &listener = thread->listeners.emplace_back(thread, event, i);
            event->listener_list_head.add_before(&listener.listener_link);
            event->num_listeners++;

            trace_event("[%d %s] Waiting for event %s\n", thread->tid, thread->name, event->debug_name);
        }

        dequeue_thread(thread);
        unlock_events(events);
        yield();
        lock_events(events);

        thread->clear_listeners();

        if (thread->enqueued_by_signal != -1)
            return -EINTR;
        return thread->which_event;
    }

    isize Event::trigger(bool drop, int max_to_wake) {
        klib::SpinlockGuard guard(this->lock);

        [[maybe_unused]] auto *thread = cpu::get_current_thread();
        trace_event("[%d %s] Triggering event %s\n", thread->tid, thread->name, this->debug_name);
        
        if (num_listeners == 0) {
            if (!drop)
                pending++;
            return 0;
        }

        int num_woken_up = 0;
        Event::Listener *listener;
        LIST_FOR_EACH_SAFE(listener, &listener_list_head, listener_link) {
            trace_event("[%d %s] Waking thread %s\n", thread->tid, thread->name, listener->thread->name);

            listener->thread->which_event = listener->which;
            enqueue_thread(listener->thread);

            num_woken_up++;
            if (num_woken_up >= max_to_wake)
                break;
        }

        if (num_listeners == 0)
            ASSERT(listener_list_head.is_empty());

        return num_woken_up;
    }
}
