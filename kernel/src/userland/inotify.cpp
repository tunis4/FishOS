#include <userland/inotify.hpp>
#include <cpu/cpu.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <sys/inotify.h>

namespace userland {
    INotify::INotify() {
        node_type = vfs::NodeType::INOTIFY;
        event = &inotify_event;
    }

    isize INotify::open(vfs::FileDescription *fd) {
        return 0;
    }

    void INotify::close(vfs::FileDescription *fd) {
    }

    isize INotify::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        while (ring_buffer.is_empty()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (inotify_event.wait() == -EINTR)
                return -EINTR;
        }
        count = ring_buffer.read((u8*)buf, count);
        return count;
    }

    isize INotify::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (!ring_buffer.is_empty())
                revents |= POLLIN;
        return revents;
    }

    isize syscall_inotify_create(int flags) {
        log_syscall("inotify_create(%#X)\n", flags);
        sched::Process *process = cpu::get_current_thread()->process;

        if (flags & ~(IN_NONBLOCK | IN_NONBLOCK))
            return -EINVAL;

        auto *inotify = new INotify();

        int ifd = process->allocate_fdnum();
        auto *description = new vfs::FileDescription(inotify, O_RDONLY | (flags & O_NONBLOCK));
        inotify->open(description);
        process->file_descriptors[ifd].init(description, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
        return ifd;
    }

    isize syscall_inotify_add_watch(int ifd, const char *path, u32 mask) {
        log_syscall("inotify_add_watch(%d, \"%s\", %#X)\n", ifd, path, mask);
        auto *description = vfs::get_file_description(ifd);
        if (!description) return -EBADF;
        if (description->vnode->node_type != vfs::NodeType::INOTIFY) return -EINVAL;
        INotify *inotify = (INotify*)description->vnode;
        return inotify->wd++;
    }

    isize syscall_inotify_rm_watch(int ifd, int wd) {
        log_syscall("inotify_rm_watch(%d, %d)\n", ifd, wd);
        auto *description = vfs::get_file_description(ifd);
        if (!description) return -EBADF;
        if (description->vnode->node_type != vfs::NodeType::INOTIFY) return -EINVAL;
        INotify *inotify = (INotify*)description->vnode;
        return 0;
    }
}
