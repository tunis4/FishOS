#include <dev/console.hpp>

namespace dev {
    ConsoleDevNode::ConsoleDevNode() {
        keyboard = input::main_keyboard;
        event = &keyboard->event;
        keyboard_listener = keyboard->create_listener("console_listener", [] (void *priv) {
            auto *self = (ConsoleDevNode*)priv;
            input::InputEvent input_event;
            while (self->keyboard_listener->event_buffer.read(&input_event, 1))
                self->process_input_event(input_event);
        }, this);

        termios.c_iflag = ICRNL;
        termios.c_oflag = ONLCR;
        termios.c_lflag = ECHO | ICANON | ISIG;
        termios.c_cflag = 0;
        termios.ibaud = 38400;
        termios.obaud = 38400;
        termios.c_cc[VMIN] = 1;
        termios.c_cc[VINTR] = 0x03;
        termios.c_cc[VQUIT] = 0x1c;
        termios.c_cc[VERASE] = '\b';
        termios.c_cc[VKILL] = 0x15;
        termios.c_cc[VEOF] = 0x04;
        termios.c_cc[VSTART] = 0x11;
        termios.c_cc[VSTOP] = 0x13;
        termios.c_cc[VSUSP] = 0x1a;
    }

    ConsoleDevNode::~ConsoleDevNode() {
        keyboard->remove_listener(keyboard_listener);
    }

    isize ConsoleDevNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        while (input_buffer.is_empty())
            event->await();
        return input_buffer.read((char*)buf, count);
    }

    isize ConsoleDevNode::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        return klib::printf("%.*s", (int)count, (char*)buf);
    }

    isize ConsoleDevNode::poll(vfs::FileDescription *fd, isize events) {
        if (events & POLLIN)
            if (!input_buffer.is_empty())
                return POLLIN;
        return 0;
    }

    isize ConsoleDevNode::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case TIOCGWINSZ: {
            struct winsize *ws = (struct winsize*)arg;
            auto &terminal = gfx::kernel_terminal();
            ws->ws_col = terminal.width_chars;
            ws->ws_row = terminal.height_chars;
            ws->ws_xpixel = terminal.actual_width;
            ws->ws_ypixel = terminal.actual_height;
        } return 0;
        case TCGETS: {
            memcpy(arg, &termios, sizeof(termios));
        } return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            memcpy(&termios, arg, sizeof(termios));
        } return 0;
        default:
            return -EINVAL;
        }
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

            if (keyboard->is_caps() && c >= 'a' && c <= 'z')
                c = c - 'a' + 'A';

            if (keyboard->is_shift() && c == '1') c = '!';
            if (keyboard->is_shift() && c == '2') c = '@';
            if (keyboard->is_shift() && c == '3') c = '#';
            if (keyboard->is_shift() && c == '4') c = '$';
            if (keyboard->is_shift() && c == '5') c = '%';
            if (keyboard->is_shift() && c == '6') c = '^';
            if (keyboard->is_shift() && c == '7') c = '&';
            if (keyboard->is_shift() && c == '8') c = '*';
            if (keyboard->is_shift() && c == '9') c = '(';
            if (keyboard->is_shift() && c == '0') c = ')';
            if (keyboard->is_shift() && c == '/') c = '?';
            if (keyboard->is_shift() && c == '\\') c = '|';
            if (keyboard->is_shift() && c == ',') c = '<';
            if (keyboard->is_shift() && c == '.') c = '>';
            if (keyboard->is_shift() && c == '\'') c = '\"';
            if (keyboard->is_shift() && c == '[') c = '{';
            if (keyboard->is_shift() && c == ']') c = '}';
            if (keyboard->is_shift() && c == ';') c = ':';
            if (keyboard->is_shift() && c == '-') c = '_';
            if (keyboard->is_shift() && c == '=') c = '+';

            if (keyboard->is_ctrl()) c = c - 'a' + 'A' - 0x40;

            input_buffer.write_truncate(&c, 1);
            if (termios.c_lflag & ECHO)
                klib::putchar(c);
        }
    }
}
