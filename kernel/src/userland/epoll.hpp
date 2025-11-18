#pragma once

#include <fs/vfs.hpp>
#include <klib/vector.hpp>
#include <klib/hashtable.hpp>
#include <sys/epoll.h>

namespace userland {
    struct EPoll final : public vfs::VNode {
        struct RegisteredEvent {
            vfs::FileDescription *description;
            u32 old_revents = 0; // used for EPOLLET
            epoll_event user_events;
        };

        klib::HashTable<usize, usize> key_to_events_index; // key is a mix of fd number and file description pointer
        klib::Vector<RegisteredEvent> events;
        klib::Vector<sched::Event*> wait_events; // starts from 1, event 0 is the timeout
        usize first_free_event = 0;

        EPoll();
        virtual ~EPoll() {}

        isize poll(vfs::FileDescription *fd, isize events) override;

        usize allocate_events_index();
    };

    isize syscall_epoll_create(int size);
    isize syscall_epoll_create1(int flags);
    isize syscall_epoll_ctl(int epfd, int op, int fd, epoll_event *event);
    isize syscall_epoll_wait(int epfd, epoll_event *events, int maxevents, int timeout);
    isize syscall_epoll_pwait(int epfd, epoll_event *events, int maxevents, int timeout, const u64 *sigmask);
    isize syscall_epoll_pwait2(int epfd, epoll_event *events, int maxevents, const klib::TimeSpec *timeout, const u64 *sigmask);
}
