#include <userland/epoll.hpp>
#include <cpu/cpu.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <klib/cstdio.hpp>

namespace userland {
    EPoll::EPoll() : key_to_events_index(16) {
        node_type = vfs::NodeType::EPOLL;
        // event = &poll_event;
        wait_events.push_back(nullptr);
    }

    isize EPoll::poll(vfs::FileDescription *fd, isize events) {
        klib::printf("epoll: poll on epoll fd is not supported correctly\n");
        isize revents = 0;
        if (events & POLLIN)
            revents |= POLLIN;
        // if (events & POLLIN) {
        //     for (nfds_t i = 0; i < this->descriptions.size(); i++) {
        //         auto *description = this->descriptions[i];
        //         auto *user_event = &this->user_events[i];
        //         if (!description)
        //             continue;
        //         if (description->vnode->poll(description, user_event->events)) {
        //             revents |= POLLIN;
        //             break;
        //         }
        //     }
        // }
        return revents;
    }

    usize EPoll::allocate_events_index() {
        for (usize i = first_free_event; i < events.size(); i++) {
            if (events[i].description == nullptr) {
                first_free_event = i + 1;
                return i;
            }
        }
        first_free_event = events.size();
        events.emplace_back();
        wait_events.push_back(nullptr);
        return events.size() - 1;
    }

    isize epoll_create1_impl(int flags) {
        sched::Process *process = cpu::get_current_thread()->process;

        if (flags & ~EPOLL_CLOEXEC)
            return -EINVAL;

        auto *epoll = new EPoll();

        int epfd = process->allocate_fdnum();
        auto *description = new vfs::FileDescription(epoll, O_RDONLY);
        epoll->open(description);
        process->file_descriptors[epfd].init(description, (flags & EPOLL_CLOEXEC) ? FD_CLOEXEC : 0);
        return epfd;
    }

    isize syscall_epoll_create(int size) {
        log_syscall("epoll_create(%#X)\n", size);
        if (size <= 0)
            return -EINVAL;
        return epoll_create1_impl(0);
    }

    isize syscall_epoll_create1(int flags) {
        log_syscall("epoll_create1(%#X)\n", flags);
        return epoll_create1_impl(flags);
    }

    static u64 calculate_key(int fd, vfs::FileDescription *description) {
        return klib::hash_combine(klib::hash(fd), klib::hash((uptr)description));
    }

    isize syscall_epoll_ctl(int epfd, int op, int fd, epoll_event *user_event) {
        log_syscall("epoll_ctl(%d, %d, %d, %#lX)\n", epfd, op, fd, (uptr)user_event);

        auto *description = vfs::get_file_description(epfd);
        if (!description) return -EBADF;
        if (description->vnode->node_type != vfs::NodeType::EPOLL) return -EINVAL;
        EPoll *epoll = (EPoll*)description->vnode;

        auto *target_description = vfs::get_file_description(fd);
        if (!target_description) return -EBADF;
        u64 fd_key = calculate_key(fd, target_description);

        if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD)
            if ((user_event->events & EPOLLONESHOT) || (user_event->events & EPOLLWAKEUP) || (user_event->events & EPOLLEXCLUSIVE))
                klib::printf("epoll: unsupported event flags %#X\n", user_event->events);

        klib::InterruptLock interrupt_guard;

        usize *index_ptr = epoll->key_to_events_index[fd_key];
        switch (op) {
        case EPOLL_CTL_ADD: {
            if (index_ptr) return -EEXIST;
            usize index = epoll->allocate_events_index();
            epoll->events[index] = {
                .description = target_description,
                .old_revents = 0,
                .user_events = *user_event,
            };
            epoll->wait_events[index + 1] = target_description->vnode->event;
            epoll->key_to_events_index.emplace(fd_key, index);
        } break;
        case EPOLL_CTL_MOD: {
            if (!index_ptr) return -ENOENT;
            usize index = *index_ptr;
            epoll->events[index].user_events = *user_event;
        } break;
        case EPOLL_CTL_DEL: {
            if (!index_ptr) return -ENOENT;
            usize index = *index_ptr;
            epoll->events[index] = {
                .description = nullptr,
                .old_revents = 0,
                .user_events = {},
            };
            epoll->wait_events[index + 1] = nullptr;
            epoll->key_to_events_index.remove(fd_key);
            if (epoll->first_free_event > index)
                epoll->first_free_event = index;
        } break;
        default: return -EINVAL;
        }

        return 0;
    }

    isize epoll_pwait2_impl(int epfd, epoll_event *events, int maxevents, const klib::TimeSpec *timeout, const u64 *sigmask) {
        if (maxevents <= 0) return -EINVAL;
        auto *epoll_description = vfs::get_file_description(epfd);
        if (!epoll_description) return -EBADF;
        if (epoll_description->vnode->node_type != vfs::NodeType::EPOLL) return -EINVAL;
        EPoll *epoll = (EPoll*)epoll_description->vnode;

        auto *thread = cpu::get_current_thread();
        if (sigmask) {
            ASSERT(thread->has_poll_saved_signal_mask == false);
            thread->has_poll_saved_signal_mask = true;
            thread->poll_saved_signal_mask = thread->signal_mask;
            thread->signal_mask = *sigmask;
            if (thread->has_pending_signals())
                return -EINTR;
        }

        sched::Timer timer;
        defer { timer.disarm(); };
        epoll->wait_events[0] = nullptr;
        bool block = true;
        if (timeout) {
            if (timeout->is_zero()) {
                block = false;
            } else {
                epoll->wait_events[0] = &timer.event;
                timer.arm(*timeout);
            }
        }

        int num_ready = 0;
        while (true) {
            for (nfds_t i = 0; i < epoll->events.size(); i++) {
                auto &event = epoll->events[i];
                if (event.description == nullptr)
                    continue;

                isize new_revents = event.description->vnode->poll(event.description, event.user_events.events);

                isize revents = new_revents;
                if (event.user_events.events & EPOLLET)
                    revents &= ~event.old_revents;

                event.old_revents = new_revents;

                if (revents) {
                    events[num_ready].events = revents;
                    events[num_ready].data = event.user_events.data;
                    num_ready++;
                    if (num_ready >= maxevents)
                        break;
                }
            }

            if (num_ready > 0)
                return num_ready;
            if (!block)
                return 0;
            if (sched::Event::wait(epoll->wait_events) == -EINTR)
                return -EINTR;
            if (timer.fired)
                return 0;
        }
    }

    isize syscall_epoll_wait(int epfd, epoll_event *events, int maxevents, int timeout) {
        log_syscall("epoll_wait(%d, %#lX, %d, %d)\n", epfd, (uptr)events, maxevents, timeout);
        if (timeout >= 0) {
            klib::TimeSpec ts;
            ts.seconds = timeout / 1000;
            ts.nanoseconds = (timeout % 1000) * 1000000;
            return epoll_pwait2_impl(epfd, events, maxevents, &ts, nullptr);
        }
        return epoll_pwait2_impl(epfd, events, maxevents, nullptr, nullptr);
    }

    isize syscall_epoll_pwait(int epfd, epoll_event *events, int maxevents, int timeout, const u64 *sigmask) {
        log_syscall("epoll_pwait(%d, %#lX, %d, %d, %#lX)\n", epfd, (uptr)events, maxevents, timeout, (uptr)sigmask);
        if (timeout >= 0) {
            klib::TimeSpec ts;
            ts.seconds = timeout / 1000;
            ts.nanoseconds = (timeout % 1000) * 1000000;
            return epoll_pwait2_impl(epfd, events, maxevents, &ts, sigmask);
        }
        return epoll_pwait2_impl(epfd, events, maxevents, nullptr, sigmask);
    }

    isize syscall_epoll_pwait2(int epfd, epoll_event *events, int maxevents, const klib::TimeSpec *timeout, const u64 *sigmask) {
        log_syscall("epoll_pwait2(%d, %#lX, %d, %#lX, %#lX)\n", epfd, (uptr)events, maxevents, (uptr)timeout, (uptr)sigmask);
        return epoll_pwait2_impl(epfd, events, maxevents, timeout, sigmask);
    }
}
