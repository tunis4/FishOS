#include <dev/tty/console.hpp>
#include <sched/sched.hpp>

namespace dev::tty {
    ConsoleDevNode::ConsoleDevNode() {
        keyboard = input::main_keyboard;
        if (keyboard) {
            event = &keyboard->event;
            keyboard_listener = keyboard->create_listener("console_listener", [] (void *priv) {
                auto *self = (ConsoleDevNode*)priv;
                input::InputEvent input_event;
                while (self->keyboard_listener->event_buffer.read(&input_event, 1))
                    self->process_input_event(input_event);
            }, this);
        }

        auto &term = gfx::kernel_terminal();
        winsize.ws_col = term.width_chars;
        winsize.ws_row = term.height_chars;
        winsize.ws_xpixel = term.actual_width;
        winsize.ws_ypixel = term.actual_height;
    }

    ConsoleDevNode::~ConsoleDevNode() {
        if (keyboard)
            keyboard->remove_listener(keyboard_listener);
    }

    isize ConsoleDevNode::open(vfs::FileDescription *fd) {
        Terminal::on_open(fd);
        return 0;
    }

    isize ConsoleDevNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        Terminal::on_read();
        while (input_buffer.is_empty())
            if (event->wait() == -EINTR)
                return -EINTR;
        return input_buffer.read((char*)buf, count);
    }

    isize ConsoleDevNode::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        Terminal::on_write();
        return klib::printf("%.*s", (int)count, (char*)buf);
    }

    isize ConsoleDevNode::poll(vfs::FileDescription *fd, isize events) {
        if (events & POLLIN)
            if (!input_buffer.is_empty())
                return POLLIN;
        return 0;
    }

    isize ConsoleDevNode::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        return tty_ioctl(fd, cmd, arg);
    }

    static const char keycode_map[128] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', '-', '=', '\b',
        '\t',
        'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, // 29 ctrl
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0, // 42 left shift
        '\\', 'z', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '/', 0, // 54 right shift
        '*',
        0, // 56 alt
        ' ',
        0, // 58 caps lock
        0, // 59 F1 key
        0, 0, 0, 0, 0, 0, 0, 0,
        0, // 68 F10 key
        0, // 69 num lock
        0, // 70 scroll lock
        0, // 71 home key
        0, // 72 up arrow
        0, // 73 page up
        '-',
        0, // 75 left arrow
        0,
        0, // 77 right arrow
        '+',
        0, // 79 end key
        0, // 80 down arrow
        0, // 81 page down
        0, // 82 insert key
        0, // 83 delete key
        0, 0, 0,
        0, // 87 F11 key
        0, // 88 F12 key
        0
    };

    void ConsoleDevNode::process_input_event(input::InputEvent &input_event) {
        if (!gfx::kernel_terminal_enabled)
            return;
 
        if (input_event.type != EV_KEY || input_event.value == 0)
            return;
        
        switch (input_event.code) {
        case KEY_UP:    input_buffer.write_truncate("\e[A", 3); break;
        case KEY_LEFT:  input_buffer.write_truncate("\e[D", 3); break;
        case KEY_RIGHT: input_buffer.write_truncate("\e[C", 3); break;
        case KEY_DOWN:  input_buffer.write_truncate("\e[B", 3); break;
        default:
            char c = keycode_map[input_event.code];
            if (!c)
                break;

            if (keyboard->is_shift()) {
                switch (c) {
                case  '1': c =  '!'; break;
                case  '2': c =  '@'; break;
                case  '3': c =  '#'; break;
                case  '4': c =  '$'; break;
                case  '5': c =  '%'; break;
                case  '6': c =  '^'; break;
                case  '7': c =  '&'; break;
                case  '8': c =  '*'; break;
                case  '9': c =  '('; break;
                case  '0': c =  ')'; break;
                case  '/': c =  '?'; break;
                case '\\': c =  '|'; break;
                case  ',': c =  '<'; break;
                case  '.': c =  '>'; break;
                case '\'': c = '\"'; break;
                case  ']': c =  '}'; break;
                case  '[': c =  '{'; break;
                case  ';': c =  ':'; break;
                case  '-': c =  '_'; break;
                case  '=': c =  '+'; break;
                }
            }

            if (keyboard->is_caps() && c >= 'a' && c <= 'z')
                c = c - 'a' + 'A';

            if (keyboard->is_ctrl())
                c = c - 'a' + 'A' - 0x40;

            process_input_char(c, [this] (char c) {
                input_buffer.write_truncate(&c, 1);
            }, klib::putchar);
        }
    }
}
