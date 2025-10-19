#pragma once

#include <klib/list.hpp>
#include <klib/async.hpp>
#include <sched/event.hpp>

namespace dev {
    struct IoTask {
        using Function = klib::Awaitable<void>(void *priv1, void *priv2);

        Function *function;
        void *priv1, *priv2;
        klib::ListHead task_link;
    };

    struct IoService {
        klib::ListHead task_list;
        klib::Spinlock task_list_lock;

        sched::Thread *thread;
        sched::Event queue_event;

        IoService();

        void push(IoTask::Function *function, void *priv1, void *priv2);
        IoTask* pop();

        void thread_loop();
    };

    extern IoService io_service;

    void init_io_service();
}
