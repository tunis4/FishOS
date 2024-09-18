#pragma once

#include <dev/input/input.hpp>
#include <dev/device.hpp>

namespace dev::input {
    struct EventDevNode final : public CharDevNode {
        InputDevice *input_device;

        EventDevNode(InputDevice *input_device);
        virtual ~EventDevNode();

        virtual isize open(vfs::FileDescription *fd);
        virtual void close(vfs::FileDescription *fd);
        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset);
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) { return count; }
        virtual isize poll(vfs::FileDescription *fd, isize events);
        virtual isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg);
    };
}
