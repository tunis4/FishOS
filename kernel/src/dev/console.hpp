#pragma once

#include <dev/device.hpp>
#include <dev/input/input.hpp>
#include <gfx/terminal.hpp>
#include <klib/cstdio.hpp>
#include <termios.h>

namespace dev {
    struct ConsoleDevNode final : public CharDevNode {
        struct termios termios {};
        sched::Process *foreground_process_group;
        sched::Process *session;

        input::KeyboardDevice *keyboard;
        input::InputListener *keyboard_listener;
        klib::RingBuffer<char, 512> input_buffer;

        ConsoleDevNode();
        ~ConsoleDevNode();

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
        isize ioctl(vfs::FileDescription *fd, usize cmd, void *arg) override;
    
    private:
        void process_input_event(input::InputEvent &input_event);
    };
}
