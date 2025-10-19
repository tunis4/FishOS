#pragma once

#include <fs/vfs.hpp>

namespace userland {
    struct EventFD final : public vfs::VNode {
        static constexpr u64 COUNTER_MAX = 0xfffffffffffffffe;

        sched::Event eventfd_event;
        u64 counter;
        bool is_semaphore = false;

        EventFD();
        virtual ~EventFD() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
    };

    isize syscall_eventfd_create(uint initval, int flags);
}
