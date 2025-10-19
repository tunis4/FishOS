#include <dev/io_service.hpp>
#include <sched/sched.hpp>

namespace dev {
    IoService io_service;

    void IoService::push(IoTask::Function *function, void *priv1, void *priv2) {
        IoTask *task = new IoTask(function, priv1, priv2);

        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(task_list_lock);
        task_list.add_before(&task->task_link);
        queue_event.trigger(true);
    }

    IoTask* IoService::pop() {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(task_list_lock);

        if (task_list.is_empty())
            return nullptr;
        IoTask *task = LIST_HEAD(&task_list, IoTask, task_link);
        task->task_link.remove();
        return task;
    }

    IoService::IoService() {
        task_list.init();
    }

    void IoService::thread_loop() {
        while (true) {
            if (auto *task = pop()) {
                klib::sync(task->function(task->priv1, task->priv2));
                delete task;
            } else {
                queue_event.wait();
            }
        }
    }

    void init_io_service() {
        io_service.thread = sched::new_kernel_thread([] () {
            io_service.thread_loop();
        }, true);
    }
}
