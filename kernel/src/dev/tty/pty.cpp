#include <dev/tty/pty.hpp>
#include <klib/bitmap.hpp>
#include <klib/cstdio.hpp>

namespace dev::tty {
    constexpr usize PTY_MAX = 256;
    static klib::Bitmap<PTY_MAX> pty_bitmap;

    PseudoTerminalEnd::PseudoTerminalEnd(Terminal *terminal, bool slave) : terminal(terminal), slave(slave) {
        event = &pty_event;
        if (slave) {
            for (usize i = 0; i < PTY_MAX; i++) {
                if (pty_bitmap.get(i) == false) {
                    pty_bitmap.set(i, true);
                    pts_num = i;
                    break;
                }
            }
        }
    }

    isize PseudoTerminalEnd::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        terminal->on_read();

        while (ring_buffer.is_empty()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (pty_event.await() == -EINTR)
                return -EINTR;
        }

        count = ring_buffer.read((char*)buf, count);
        peer->pty_event.trigger();
        return count;
    }

    isize PseudoTerminalEnd::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        terminal->on_write();

        while (peer->ring_buffer.is_full()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (pty_event.await() == -EINTR)
                return -EINTR;
        }

        if (slave) {
            usize i;
            bool end = false;
            for (i = 0; i < count; i++) {
                if (end)
                    break;
                terminal->process_output_char(((const char*)buf)[i], [&] (char c) {
                    if (peer->ring_buffer.write(&c, 1) == 0)
                        end = true;
                });
            }
            peer->pty_event.trigger();
            return i;
        } else {
            usize i;
            bool end = false;
            for (i = 0; i < count; i++) {
                if (end)
                    break;
                terminal->process_input_char(((const char*)buf)[i], [&] (char c) {
                    if (peer->ring_buffer.write(&c, 1) == 0)
                        end = true;
                }, [&] (char c) {
                    ring_buffer.write(&c, 1);
                });
            }
            peer->pty_event.trigger();
            return i;
        }
    }

    isize PseudoTerminalEnd::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (!ring_buffer.is_empty())
                revents |= POLLIN;
        if (events & POLLOUT)
            if (!peer->ring_buffer.is_full())
                revents |= POLLOUT;
        return revents;
    }

    isize PseudoTerminalEnd::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case TIOCGPTN:
            *(int*)arg = pts_num;
            return 0;
        case TIOCSPTLCK:
            return 0;
        default:
            return terminal->tty_ioctl(fd, cmd, arg);
        }
    }

    isize PseudoTerminalMultiplexer::open(vfs::FileDescription *fd) {
        Terminal *terminal = new Terminal();
        PseudoTerminalEnd *master = new PseudoTerminalEnd(terminal, false);
        PseudoTerminalEnd *slave = new PseudoTerminalEnd(terminal, true);
        master->peer = slave;
        slave->peer = master;
        master->pts_num = slave->pts_num;

        char pts_path[64];
        klib::snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", slave->pts_num);

        vfs::Entry *entry = vfs::path_to_entry(pts_path);
        if (entry->vnode != nullptr)
            return -EEXIST;
        if (entry->parent == nullptr)
            return -ENOENT;
        entry->vnode = slave;
        entry->create();

        fd->vnode = master;
        terminal->on_open(fd);
        return 0;
    }
}
