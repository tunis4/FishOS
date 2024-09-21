#include <userland/pipe.hpp>
#include <cpu/cpu.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>

namespace userland {
    Pipe::Pipe() {
        type = vfs::NodeType::FIFO;
        event = &pipe_event;
    }

    isize Pipe::open(vfs::FileDescription *fd) {
        if (fd->can_read())
            readers++;
        if (fd->can_write())
            writers++;
        return 0;
    }

    void Pipe::close(vfs::FileDescription *fd) {
        if (fd->can_read())
            readers--;
        if (fd->can_write())
            writers--;
        pipe_event.trigger();
    }

    isize Pipe::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        while (ring_buffer.is_empty()) {
            if (writers == 0)
                return 0;
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (pipe_event.await() == -EINTR)
                return -EINTR;
        }
        count = ring_buffer.read((u8*)buf, count);
        pipe_event.trigger();
        return count;
    }

    isize Pipe::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        while (true) {
            if (readers == 0)
                return -EPIPE;
            if (!ring_buffer.is_full())
                break;
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (pipe_event.await() == -EINTR)
                return -EINTR;
        }
        count = ring_buffer.write((const u8*)buf, count);
        pipe_event.trigger();
        return count;
    }

    isize Pipe::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN) {
            if (writers == 0)
                revents |= POLLHUP;
            else if (!ring_buffer.is_empty())
                revents |= POLLIN;
        }
        if (events & POLLOUT) {
            if (readers == 0)
                revents |= POLLERR;
            else if (!ring_buffer.is_full())
                revents |= POLLOUT;
        }
        return revents;
    }

    isize syscall_pipe(int pipefd[2], int flags) {
#if SYSCALL_TRACE
        klib::printf("pipe(%#lX, %d)\n", (uptr)pipefd, flags);
#endif
        sched::Process *process = cpu::get_current_thread()->process;

        if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
            klib::printf("unsupported flags for pipe: %d\n", flags);
            return -EINVAL;
        }

        auto *pipe = new Pipe();

        pipefd[0] = process->allocate_fdnum();
        auto *read_description = new vfs::FileDescription(pipe, O_RDONLY | (flags & O_NONBLOCK));
        pipe->open(read_description);
        process->file_descriptors[pipefd[0]].init(read_description, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);

        pipefd[1] = process->allocate_fdnum();
        auto *write_description = new vfs::FileDescription(pipe, O_WRONLY | (flags & O_NONBLOCK));
        pipe->open(write_description);
        process->file_descriptors[pipefd[1]].init(write_description, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);

        return 0;
    }
}
