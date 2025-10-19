#include <sched/event.hpp>
#include <sched/sched.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>

namespace sched {
    static void lock_events(klib::Span<Event*> &events) {
        for (auto *event : events)
            event->lock.lock();
    }

    static void unlock_events(klib::Span<Event*> &events) {
        for (auto *event : events)
            event->lock.unlock();
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

        thread->listeners.clear();
        for (usize i = 0; i < events.size; i++) {
            auto *event = events[i];
            if (!event)
                continue;
            auto *listener = new Event::Listener();
            listener->thread = thread;
            listener->event = event;
            listener->which = i;
            event->listener_list_head.add_before(&listener->listener_link);
            event->num_listeners++;
            thread->listeners.push_back(listener);
        }

        dequeue_thread(thread);
        unlock_events(events);
        yield();
        lock_events(events);
        if (thread->enqueued_by_signal != -1)
            return -EINTR;

        thread->clear_listeners();
        return thread->which_event;
    }

    isize Event::trigger(bool drop, int max_to_wake) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(this->lock);

        if (num_listeners == 0) {
            if (!drop)
                pending++;
            return 0;
        }

        int num_woken_up = 0;
        Event::Listener *listener;
        LIST_FOR_EACH_SAFE(listener, &listener_list_head, listener_link) {
            listener->thread->which_event = listener->which;
            enqueue_thread(listener->thread);
            listener->listener_link.remove();
            num_listeners--;
            num_woken_up++;
            if (num_woken_up >= max_to_wake)
                break;
        }

        if (num_listeners == 0)
            ASSERT(listener_list_head.is_empty());

        return num_woken_up;
    }
}
