#pragma once

#include <fs/vfs.hpp>
#include <klib/ring_buffer.hpp>

namespace userland {
    struct Pipe final : public vfs::VNode {
        static constexpr usize capacity = 0x1000;

        usize readers = 0, writers = 0;
        sched::Event pipe_event;
        klib::RingBuffer<u8, capacity> ring_buffer;

        Pipe();
        virtual ~Pipe() {}

        virtual isize  open(vfs::FileDescription *fd);
        virtual  void close(vfs::FileDescription *fd);
        virtual isize  read(vfs::FileDescription *fd, void *buf, usize count, usize offset);
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset);
        virtual isize  poll(vfs::FileDescription *fd, isize events);
        virtual isize  seek(vfs::FileDescription *fd, usize position, isize offset, int whence) { return -ESPIPE; }
    };

    isize syscall_pipe(int pipefd[2], int flags);
}
