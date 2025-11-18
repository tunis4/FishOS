#include <dev/tty/pty.hpp>
#include <klib/bitmap.hpp>
#include <klib/cstdio.hpp>

namespace dev::tty {
    constexpr usize PTY_MAX = 256;
    static klib::Bitmap<PTY_MAX> pty_bitmap;

    PseudoTerminalEnd::PseudoTerminalEnd(Terminal *terminal, bool slave)
        : terminal(terminal), pty_event("PseudoTerminalEnd::pty_event"), slave(slave)
    {
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
            if (pty_event.wait() == -EINTR)
                return -EINTR;
        }

        if (packet_mode && count > 0) {
            *(u8*)buf = 0;
            count = ring_buffer.read((char*)buf + 1, count - 1) + 1;
        } else {
            count = ring_buffer.read((char*)buf, count);
        }

        peer->pty_event.trigger();
        return count;
    }

    isize PseudoTerminalEnd::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        terminal->on_write();

        while (peer->ring_buffer.is_full()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (pty_event.wait() == -EINTR)
                return -EINTR;
        }

        if (slave) {
            usize i;
            bool end = false;
            for (i = 0; i < count; i++) {
                terminal->process_output_char(((const char*)buf)[i], [&] (char c) {
                    if (peer->ring_buffer.write(&c, 1) == 0)
                        end = true;
                });
                if (end)
                    break;
            }
            peer->pty_event.trigger();
            return i;
        } else {
            usize i;
            bool end = false;
            for (i = 0; i < count; i++) {
                terminal->process_input_char(((const char*)buf)[i], [&] (char c) {
                    if (peer->ring_buffer.write(&c, 1) == 0)
                        end = true;
                }, [&] (char c) {
                    ring_buffer.write(&c, 1);
                });
                if (end)
                    break;
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
        case TIOCPKT:
            if (slave)
                return -ENOTTY;
            packet_mode = *(int*)arg;
            return 0;
        case TIOCGPKT:
            *(int*)arg = packet_mode;
            return 0;
        default:
            return terminal->tty_ioctl(fd, cmd, arg);
        }
    }

    isize PseudoTerminalMultiplexer::open(vfs::FileDescription *fd) {
        sched::Thread *thread = cpu::get_current_thread();
        isize err = 0;

        Terminal *terminal = new Terminal();
        PseudoTerminalEnd *master = new PseudoTerminalEnd(terminal, false);
        PseudoTerminalEnd *slave = new PseudoTerminalEnd(terminal, true);

        defer {
            if (err < 0) {
                delete terminal;
                delete master;
                delete slave;
            }
        };

        master->peer = slave;
        slave->peer = master;
        if (slave->pts_num == -1)
            return err = -ENOENT;
        master->pts_num = slave->pts_num;

        char pts_path[64];
        klib::snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", slave->pts_num);

        vfs::Entry *entry = vfs::path_to_entry(pts_path);
        if (entry->vnode != nullptr)
            return err = -EEXIST;
        if (entry->parent == nullptr)
            return err = -ENOENT;
        entry->vnode = slave;
        entry->create(vfs::NodeType::CHAR_DEVICE, thread->cred.uids.eid, thread->cred.gids.eid, 0600);

        fd->vnode = master;
        terminal->on_open(fd);
        return 0;
    }
}
