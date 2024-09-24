#include <sched/event.hpp>
#include <sched/sched.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>

namespace sched {
    isize Event::await(klib::Span<Event*> events, bool nonblocking) {
        klib::InterruptLock guard;
        auto *thread = cpu::get_current_thread();

        for (usize i = 0; i < events.size; i++) {
            auto *event = events[i];
            if (event && event->pending > 0) {
                event->pending--;
                return i;
            }
        }

        if (nonblocking)
            return -EWOULDBLOCK;

        if (thread->pending_signals & ~thread->signal_mask)
            return -EINTR;

        thread->listeners.clear();
        for (usize i = 0; i < events.size; i++) {
            auto *event = events[i];
            if (!event)
                continue;
            auto *listener = new Event::Listener();
            listener->thread = thread;
            listener->which = i;
            event->listener_list_head.add_before(&listener->listener_link);
            event->num_listeners++;
            thread->listeners.push_back(listener);
        }

        dequeue_thread(thread);
        yield();
        if (thread->enqueued_by_signal != -1)
            return -EINTR;

        for (auto *listener : thread->listeners) {
            if (!listener->listener_link.is_invalid())
                listener->listener_link.remove();
            delete listener;
        }
        thread->listeners.clear();

        return thread->which_event;
    }

    isize Event::trigger() {
        klib::InterruptLock guard;

        if (num_listeners == 0) {
            pending++;
            return 0;
        }

        for (klib::ListHead *current = listener_list_head.next; current != &listener_list_head;) {
            auto *listener = LIST_ENTRY(current, Event::Listener, listener_link);
            listener->thread->which_event = listener->which;
            enqueue_thread(listener->thread);
            current = current->next;
            listener->listener_link.remove();
        }

        auto ret = num_listeners;
        num_listeners = 0;
        ASSERT(listener_list_head.is_empty());
        return ret;
    }
}
