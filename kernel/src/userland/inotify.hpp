#pragma once

#include <fs/vfs.hpp>
#include <klib/ring_buffer.hpp>

namespace userland {
    struct INotify final : public vfs::VNode {
        static constexpr usize capacity = 0x1000;

        sched::Event inotify_event;
        klib::RingBuffer<u8, capacity> ring_buffer;
        int wd = 0;

        INotify();
        virtual ~INotify() {}

        isize open(vfs::FileDescription *fd) override;
        void close(vfs::FileDescription *fd) override;
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
    };

    isize syscall_inotify_init1(int flags);
    isize syscall_inotify_add_watch(int ifd, const char *path, u32 mask);
    isize syscall_inotify_rm_watch(int ifd, int wd);
}
