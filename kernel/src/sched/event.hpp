#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <klib/span.hpp>

namespace sched {
    struct Thread;

    struct Event {
        struct Listener {
            klib::ListHead listener_link;
            Thread *thread;
            usize which;
        };

        usize pending = 0, num_listeners = 0;
        klib::ListHead listener_list_head;

        Event() {
            listener_list_head.init();
        }

        static isize await(klib::Span<Event*> events);
        inline isize await() { Event *event = this; return await(event); }
        isize trigger();
    };
};
