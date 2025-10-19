#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <klib/span.hpp>
#include <klib/lock.hpp>

namespace sched {
    struct Thread;

    struct Event {
        struct Listener {
            klib::ListHead listener_link;
            Thread *thread;
            Event *event;
            int which;
        };

        klib::Spinlock lock;
        int pending = 0, num_listeners = 0;
        klib::ListHead listener_list_head;

        Event() {
            listener_list_head.init();
        }

        static isize wait(klib::Span<Event*> events, bool nonblocking = false);
        inline isize wait(bool nonblocking = false) { Event *event = this; return wait(event, nonblocking); }
        isize trigger(bool drop = false, int max_to_wake = klib::NumericLimits<int>::max);
    };
};
