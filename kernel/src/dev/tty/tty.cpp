#include <dev/tty/tty.hpp>

namespace dev::tty {
    Terminal::Terminal() {
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

    Terminal::~Terminal() {}

    void Terminal::on_open(vfs::FileDescription *fd) {
        if (!(fd->flags & O_NOCTTY))
            set_controlling_terminal(cpu::get_current_thread()->process, fd->vnode);
    }

    void Terminal::on_read() {
        // auto *process_group = cpu::get_current_thread()->process->process_group_leader;
        // if (process_group != foreground_process_group)
        //     process_group->send_process_group_signal(SIGTTIN);
    }

    void Terminal::on_write() {
        // auto *process_group = cpu::get_current_thread()->process->process_group_leader;
        // if (termios.c_lflag & TOSTOP)
        //     if (process_group != foreground_process_group)
        //         process_group->send_process_group_signal(SIGTTOU);
    }

    isize Terminal::tty_ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case TIOCGWINSZ: {
            memcpy(arg, &winsize, sizeof(winsize));
        } return 0;
        case TIOCSWINSZ: {
            memcpy(&winsize, arg, sizeof(winsize));
        } return 0;
        case TCGETS: {
            memcpy(arg, &termios, sizeof(termios));
        } return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            memcpy(&termios, arg, sizeof(termios));
        } return 0;
        case TIOCGPGRP: {
            *(int*)arg = foreground_process_group->pid;
        } return 0;
        case TIOCSPGRP: {
            auto *thread = sched::Thread::get_from_tid(*(int*)arg);
            if (!thread)
                return -ESRCH;
            foreground_process_group = thread->process;
        } return 0;
        case TIOCGSID: {
            *(int*)arg = session->pid;
        } return 0;
        case TIOCSCTTY: {
            set_controlling_terminal(cpu::get_current_thread()->process, fd->vnode);
        } return 0;
        default:
            return -EINVAL;
        }
    }

    void Terminal::set_controlling_terminal(sched::Process *process, vfs::VNode *vnode) {
        if (process != process->session_leader)
            return;
        process->controlling_terminal = vnode;
        session = process;
        foreground_process_group = process;
    }

    isize TTYDevNode::open(vfs::FileDescription *fd) {
        auto *controlling_terminal = cpu::get_current_thread()->process->session_leader->controlling_terminal;
        if (!controlling_terminal)
            return -ENOENT;
        fd->vnode = controlling_terminal;
        return 0;
    }
}
