#pragma once

#include <dev/tty/tty.hpp>
#include <klib/ring_buffer.hpp>

namespace dev::tty {
    struct PseudoTerminalEnd final : public CharDevNode {
        Terminal *terminal;
        PseudoTerminalEnd *peer;
        sched::Event pty_event;
        klib::RingBuffer<char, 0x1000> ring_buffer;
        int pts_num;
        bool slave;

        PseudoTerminalEnd(Terminal *terminal, bool slave);
        virtual ~PseudoTerminalEnd() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
        isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg) override;
    };

    struct PseudoTerminalMultiplexer final : public CharDevNode {
        isize open(vfs::FileDescription *fd) override;
    };
}
