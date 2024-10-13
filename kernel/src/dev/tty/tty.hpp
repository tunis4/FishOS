#pragma once

#include <dev/devnode.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <termios.h>

namespace dev::tty {
    struct Terminal {
        struct termios termios;
        struct winsize winsize;
        sched::Process *foreground_process_group;
        sched::Process *session;

        Terminal();
        virtual ~Terminal();

        void on_open(vfs::FileDescription *fd);
        void on_read();
        void on_write();
        isize tty_ioctl(vfs::FileDescription *fd, usize cmd, void *arg);
        void set_controlling_terminal(sched::Process *process, vfs::VNode *vnode);

        template<typename Put>
        void process_output_char(char c, Put &&put) {
		    if (c == '\n' && (termios.c_oflag & ONLCR))
                put('\r');
            put(c);
        }

        template<typename Put, typename Echo>
        void process_input_char(char c, Put &&put, Echo &&echo) {
            if (c == '\r' && (termios.c_iflag & IGNCR))
                return;
            if (c == '\r' && (termios.c_iflag & ICRNL))
                put('\n');
            else if (c == '\n' && (termios.c_iflag & INLCR))
                put('\r');

            // if ((termios.c_lflag & ECHO) && (termios.c_lflag & ECHOCTL)) {
            //     echo('^');
            //     echo(c - 'a' + 'A');
            // }

            if (termios.c_lflag & ISIG) {
                int signal = -1;
                if (c == termios.c_cc[VINTR])
                    signal = SIGINT;
                else if (c == termios.c_cc[VQUIT])
                    signal = SIGQUIT;
                else if (c == termios.c_cc[VSUSP])
                    signal = SIGTSTP;

                if (signal != -1) {
                    foreground_process_group->send_process_group_signal(signal);
                    return;
                }
            }

            put(c);
        }
    };

    struct TTYDevNode final : public CharDevNode {
        isize open(vfs::FileDescription *fd) override;
    };
}
