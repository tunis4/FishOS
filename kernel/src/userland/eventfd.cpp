#include <userland/eventfd.hpp>
#include <cpu/cpu.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <sys/eventfd.h>

namespace userland {
    EventFD::EventFD() : eventfd_event("EventFD::eventfd_event") {
        node_type = vfs::NodeType::EVENTFD;
        event = &eventfd_event;
    }

    isize EventFD::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        if (count < 8)
            return -EINVAL;

        while (this->counter == 0) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (eventfd_event.wait() == -EINTR)
                return -EINTR;
        }

        if (is_semaphore) {
            *(u64*)buf = 1;
            this->counter--;
        } else {
            *(u64*)buf = this->counter;
            this->counter = 0;
        }

        eventfd_event.trigger(true);
        return 8;
    }

    isize EventFD::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        if (count < 8) return -EINVAL;
        u64 to_add = *(u64*)buf;
        if (to_add == 0xffffffffffffffff) return -EINVAL;

        while (this->counter > COUNTER_MAX - to_add) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            if (eventfd_event.wait() == -EINTR)
                return -EINTR;
        }

        this->counter += to_add;
        eventfd_event.trigger(true);
        return 8;
    }

    isize EventFD::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (this->counter > 0)
                revents |= POLLIN;
        if (events & POLLOUT)
            if (this->counter < COUNTER_MAX)
                revents |= POLLOUT;
        return revents;
    }

    static isize eventfd_impl(uint initval, int flags) {
        sched::Process *process = cpu::get_current_thread()->process;

        if (flags & ~(EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE))
            return -EINVAL;

        auto *eventfd = new EventFD();
        eventfd->counter = initval;
        if (flags & EFD_SEMAPHORE)
            eventfd->is_semaphore = true;

        int efd = process->allocate_fdnum();
        auto *description = new vfs::FileDescription(eventfd, O_RDWR | (flags & EFD_NONBLOCK) ? O_NONBLOCK : 0);
        process->file_descriptors[efd].init(description, (flags & EFD_CLOEXEC) ? FD_CLOEXEC : 0);
        return efd;
    }

    isize syscall_eventfd2(uint initval, int flags) {
        log_syscall("eventfd2(%#X, %#X)\n", initval, flags);
        return eventfd_impl(initval, flags);
    }

    isize syscall_eventfd(uint initval) {
        log_syscall("eventfd(%#X)\n", initval);
        return eventfd_impl(initval, 0);
    }
}
