#pragma once

#include <dev/input/input.hpp>
#include <dev/device.hpp>

namespace dev::input {
    struct EventDevNode final : public CharDevNode {
        InputDevice *input_device;

        EventDevNode(InputDevice *input_device);
        virtual ~EventDevNode();

        isize open(vfs::FileDescription *fd) override;
        void close(vfs::FileDescription *fd) override;
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override { return count; }
        isize poll(vfs::FileDescription *fd, isize events) override;
        isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg) override;
    };
}
