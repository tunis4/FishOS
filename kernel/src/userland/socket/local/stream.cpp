#include <userland/socket/local/stream.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <sys/un.h>
#include <errno.h>

#undef CMSG_NXTHDR
#define __CMSG_NEXT(c) ((char *)(c) + CMSG_ALIGN((c)->cmsg_len))
#define __MHDR_LIMIT(m) ((char *)(m)->msg_control + (m)->msg_controllen)
#define CMSG_NXTHDR(m, c) \
    ((c)->cmsg_len < sizeof(struct cmsghdr) || \
        (ptrdiff_t)(sizeof(struct cmsghdr) + CMSG_ALIGN((c)->cmsg_len)) \
            >= __MHDR_LIMIT(m) - (char *)(c) \
    ? (struct cmsghdr *)0 : (struct cmsghdr *)__CMSG_NEXT(c))

namespace socket {
    LocalStreamSocket::LocalStreamSocket(bool is_seqpacket) : socket_event("LocalStreamSocket::socket_event") {
        this->is_seqpacket = is_seqpacket;
        socket_family = AF_LOCAL;
        socket_type = is_seqpacket ? SOCK_SEQPACKET : SOCK_STREAM;
        event = &socket_event;
        address.sun_family = socket_family;
        pending_list.init();
    }

    LocalStreamSocket::~LocalStreamSocket() {
        if (peer)
            peer->peer = nullptr;
        if (ring_buffer) {
            delete ring_buffer;
            ring_buffer = nullptr;
        }
    }

    void LocalStreamSocket::close(vfs::FileDescription *fd) {
        if (peer)
            peer->peer = nullptr;
        if (ring_buffer) {
            delete ring_buffer;
            ring_buffer = nullptr;
        }
    }

    isize LocalStreamSocket::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN) {
            if (ring_buffer && !ring_buffer->is_empty())
                revents |= POLLIN;
            if (max_pending > 0 && !pending_list.is_empty())
                revents |= POLLIN;
        }
        if (events & POLLOUT)
            if (peer && peer->ring_buffer && !peer->ring_buffer->is_full())
                revents |= POLLOUT;
        return revents;
    }

    isize LocalStreamSocket::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case FIONREAD:
            if (!ring_buffer)
                return -EINVAL;
            if (is_seqpacket)
                klib::printf("LocalStreamSocket::ioctl: FIONREAD does not give correct value for SOCK_SEQPACKET\n");
            *(int*)arg = ring_buffer->data_count();
            return 0;
        default:
            return vfs::VNode::ioctl(fd, cmd, arg);
        }
    }

    isize LocalStreamSocket::bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;
        auto *addr = (sockaddr_un*)addr_ptr;

        log_syscall("LocalStreamSocket::bind(\"%s\")\n", addr->sun_path);

        vfs::Entry *entry = vfs::path_to_entry(addr->sun_path, process->cwd);
        if (entry->vnode != nullptr) return -EADDRINUSE;
        if (entry->parent == nullptr) return -ENOENT;

        entry->vnode = this;
        entry->create(vfs::NodeType::SOCKET, thread->cred.uids.eid, thread->cred.gids.eid, 0777 & ~process->umask);
        memset(&this->address, 0, sizeof(sockaddr_un));
        memcpy(&this->address, addr, get_local_addr_length(addr));
        return 0;
    }

    isize LocalStreamSocket::connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;
        auto *addr = (sockaddr_un*)addr_ptr;
        if (addr->sun_family != AF_LOCAL) return -EAFNOSUPPORT;

        log_syscall("LocalStreamSocket::connect(\"%s\")\n", addr->sun_path);

        vfs::Entry *entry = vfs::path_to_entry(addr->sun_path, process->cwd);
        if (entry->vnode == nullptr) return -ENOENT;
        if (entry->vnode->node_type != vfs::NodeType::SOCKET) return -ECONNREFUSED;

        LocalStreamSocket *target = (LocalStreamSocket*)entry->vnode;
        {
            klib::SpinlockGuard guard(target->pending_lock);
            if (target->num_pending >= target->max_pending)
                return -ECONNREFUSED;
            target->pending_list.add_before(&pending_list);
        }

        target->socket_event.trigger();
        if (socket_event.wait() == -EINTR)
            return -EINTR;

        credentials.pid = process->pid;
        credentials.uid = thread->cred.uids.eid;
        credentials.gid = thread->cred.gids.eid;

        return 0;
    }

    isize LocalStreamSocket::listen(vfs::FileDescription *fd, int backlog) {
        max_pending = backlog;

        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;
        credentials.pid = process->pid;
        credentials.uid = thread->cred.uids.eid;
        credentials.gid = thread->cred.gids.eid;
        return 0;
    }

    isize LocalStreamSocket::accept(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
        klib::SpinlockGuard guard(pending_lock);

        while (pending_list.is_empty()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            guard.unlock();
            if (socket_event.wait() == -EINTR)
                return -EINTR;
            guard.lock();
        }

        LocalStreamSocket *accepted_peer = LIST_ENTRY(pending_list.next, LocalStreamSocket, pending_list);
        accepted_peer->pending_list.remove();

        if (addr_ptr && addr_length) {
            *addr_length = get_local_addr_length(&accepted_peer->address);
            memcpy(addr_ptr, &accepted_peer->address, *addr_length);
        }

        auto *connected_socket = new LocalStreamSocket(is_seqpacket);
        connected_socket->peer = accepted_peer;
        connected_socket->ring_buffer = new RingBuffer();
        memset(&connected_socket->address, 0, sizeof(sockaddr_un));
        memcpy(&connected_socket->address, &this->address, get_local_addr_length(&this->address));

        accepted_peer->peer = connected_socket;
        if (!accepted_peer->ring_buffer)
            accepted_peer->ring_buffer = new RingBuffer();
        accepted_peer->socket_event.trigger();

        return connected_socket->create_socket_fd(flags);
    }

    isize LocalStreamSocket::recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) {
        if (flags & ~(MSG_CMSG_CLOEXEC | MSG_DONTWAIT))
            klib::printf("LocalStreamSocket::recvmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_namelen != 0)
            klib::printf("LocalStreamSocket::recvmsg: hdr->msg_name unsupported\n");

        hdr->msg_flags = 0;

        if (hdr->msg_controllen > 0) {
            usize ancillary_transferred = 0;
            cmsghdr *cmsg = nullptr;
            while (ancillary_ring_buffer.read(&cmsg, 1)) {
                // klib::printf("socket %#lX has control from peer %#lX\n", (uptr)this, (uptr)peer);
                if (ancillary_transferred + CMSG_ALIGN(cmsg->cmsg_len) <= hdr->msg_controllen) {
                    memcpy((u8*)hdr->msg_control + ancillary_transferred, cmsg, cmsg->cmsg_len);
                    ancillary_transferred += CMSG_ALIGN(cmsg->cmsg_len);
                    // klib::printf("socket %#lX received control from peer %#lX\n", (uptr)this, (uptr)peer);
                } else {
                    hdr->msg_flags |= MSG_CTRUNC;
                }
                klib::free(cmsg);
            }
            hdr->msg_controllen = ancillary_transferred;
        }

        if (!peer || !ring_buffer)
            return -ENOTCONN;

        while (ring_buffer->is_empty()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        u32 packet_length = 0;
        if (is_seqpacket) {
            isize ret = ring_buffer->read((u8*)&packet_length, sizeof(packet_length));
            ASSERT(ret == sizeof(packet_length));
        }

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            if (iov->iov_len == 0) continue;

            usize amount_to_read = iov->iov_len;
            if (is_seqpacket) {
                amount_to_read = klib::min(amount_to_read, packet_length - transferred);
                if (amount_to_read == 0)
                    break;
            }

            transferred += ring_buffer->read((u8*)iov->iov_base, amount_to_read);
            if (ring_buffer->is_empty())
                break;
        }

        if (is_seqpacket) {
            ASSERT(transferred <= packet_length);
            if (transferred < packet_length) {
                ring_buffer->truncate(packet_length - transferred);
                hdr->msg_flags |= MSG_TRUNC;
            }
        }

        peer->socket_event.trigger(true);
        return transferred;
    }

    isize LocalStreamSocket::sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) {
        if (flags & ~(MSG_NOSIGNAL | MSG_DONTWAIT))
            klib::printf("LocalStreamSocket::sendmsg: unsupported flags %#X\n", flags);

        if (hdr->msg_namelen != 0)
            return -EISCONN;

        for (cmsghdr *cmsg = CMSG_FIRSTHDR(hdr); cmsg; cmsg = CMSG_NXTHDR((msghdr*)hdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
                cmsghdr *copied_cmsg = (cmsghdr*)klib::calloc(CMSG_ALIGN(cmsg->cmsg_len));
                memcpy(copied_cmsg, cmsg, cmsg->cmsg_len);
                peer->ancillary_ring_buffer.write(&copied_cmsg, 1);
                // klib::printf("socket %#lX sent control to peer %#lX\n", (uptr)this, (uptr)peer);
            } else {
                klib::printf("LocalStreamSocket::sendmsg: unsupported cmsg (level: %d, type: %d)\n", cmsg->cmsg_level, cmsg->cmsg_type);
            }
        }

        if (!peer || !peer->ring_buffer)
            return -ENOTCONN;

        while (peer->ring_buffer->is_full()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        if (is_seqpacket) {
            u32 packet_length = 0;
            for (usize i = 0; i < hdr->msg_iovlen; i++) {
                struct iovec *iov = &hdr->msg_iov[i];
                packet_length += iov->iov_len;
            }

            isize ret = ring_buffer->write((u8*)&packet_length, sizeof(packet_length));
            ASSERT(ret == sizeof(packet_length));
        }

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            if (iov->iov_len == 0) continue;
            transferred += peer->ring_buffer->write((u8*)iov->iov_base, iov->iov_len);
            if (peer->ring_buffer->is_full())
                break;
        }
        peer->socket_event.trigger(true);
        return transferred;
    }

    isize LocalStreamSocket::shutdown(vfs::FileDescription *fd, int how) {
        return -ENOSYS;
    }

    isize LocalStreamSocket::getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) {
        if (layer == SOL_SOCKET) {
            switch (number) {
            case SO_PASSCRED:
                *(int*)buffer = passcred;
                *size = sizeof(int);
                return 0;
            case SO_PEERCRED:
                *(ucred*)buffer = peer->credentials;
                *size = sizeof(ucred);
                return 0;
            case SO_SNDBUF:
            case SO_RCVBUF:
                *(int*)buffer = ring_buffer->size;
                *size = sizeof(int);
                return 0;
            case SO_KEEPALIVE:
                *(int*)buffer = 0;
                *size = sizeof(int);
                return 0;
            case SO_PEERSEC:
                return -ENOPROTOOPT;
            }
        }
        klib::printf("LocalStreamSocket::getsockopt: unsupported option layer %d, number %d\n", layer, number);
        return -ENOPROTOOPT;
    }

    isize LocalStreamSocket::setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) {
        if (layer == SOL_SOCKET && number == SO_PASSCRED) {
            passcred = *(int*)buffer;
        } else if (layer == SOL_SOCKET && (number == SO_SNDBUF || number == SO_RCVBUF)) {
            // no-op
        } else if (layer == 6) {
            return -EOPNOTSUPP;
        } else {
            klib::printf("LocalStreamSocket::setsockopt: unsupported option layer %d, number %d\n", layer, number);
            return -ENOPROTOOPT;
        }
        return 0;
    }

    isize LocalStreamSocket::getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        socklen_t this_length = get_local_addr_length(&this->address);
        memcpy(addr_ptr, &this->address, klib::min(this_length, *addr_length));
        *addr_length = this_length;
        return 0;
    }

    isize LocalStreamSocket::getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        socklen_t peer_length = get_local_addr_length(&peer->address);
        memcpy(addr_ptr, &peer->address, klib::min(peer_length, *addr_length));
        *addr_length = peer_length;
        return 0;
    }
}
