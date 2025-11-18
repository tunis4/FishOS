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

        isize open(vfs::FileDescription *fd) override;
        void close(vfs::FileDescription *fd) override;
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
        isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg) override;
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return -ESPIPE; }
        isize mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) override { return -EACCES; }
    };

    isize syscall_pipe(int pipefd[2]);
    isize syscall_pipe2(int pipefd[2], int flags);
}
