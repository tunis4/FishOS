#include <userland/socket/local/datagram.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <sys/un.h>
#include <errno.h>

namespace socket {
    LocalDatagramSocket::Datagram* LocalDatagramSocket::Datagram::construct(LocalDatagramSocket *from, u32 length) {
        auto *datagram = (Datagram*)klib::malloc(sizeof(Datagram) + length);
        datagram->from = from;
        datagram->length = length;
        return datagram;
    }

    LocalDatagramSocket::LocalDatagramSocket() : socket_event("LocalDatagramSocket::socket_event") {
        socket_family = AF_LOCAL;
        socket_type = SOCK_DGRAM;
        event = &socket_event;
        address.sun_family = socket_family;
        ring_buffer = new RingBuffer();
    }

    LocalDatagramSocket::~LocalDatagramSocket() {
        while (!ring_buffer->is_empty()) {
            Datagram *datagram;
            ring_buffer->read(&datagram, 1);
            klib::free(datagram);
        }
        delete ring_buffer;
    }

    isize LocalDatagramSocket::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (ring_buffer && !ring_buffer->is_empty())
                revents |= POLLIN;
        if (events & POLLOUT)
            if (connected && connected->ring_buffer && !connected->ring_buffer->is_full())
                revents |= POLLOUT;
        return revents;
    }

    isize LocalDatagramSocket::ioctl(vfs::FileDescription *fd, usize cmd, void *arg) {
        switch (cmd) {
        case FIONREAD:
            if (ring_buffer->is_empty()) {
                *(int*)arg = 0;
            } else {
                Datagram *datagram;
                ring_buffer->peek(&datagram);
                *(int*)arg = datagram->length;
            }
            return 0;
        case TIOCOUTQ:
            klib::printf("LocalDatagramSocket::ioctl: TIOCOUTQ is unimplemented\n");
            return -EINVAL;
        default:
            return vfs::VNode::ioctl(fd, cmd, arg);
        }
    }

    isize LocalDatagramSocket::bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;
        auto *addr = (sockaddr_un*)addr_ptr;

        log_syscall("LocalDatagramSocket::bind(\"%s\")\n", addr->sun_path);

        vfs::Entry *entry = vfs::path_to_entry(addr->sun_path, process->cwd);
        if (entry->vnode != nullptr) return -EADDRINUSE;
        if (entry->parent == nullptr) return -ENOENT;

        entry->vnode = this;
        entry->create(vfs::NodeType::SOCKET, thread->cred.uids.eid, thread->cred.gids.eid, 0777 & ~process->umask);
        memset(&this->address, 0, sizeof(sockaddr_un));
        memcpy(&this->address, addr, get_local_addr_length(addr));
        return 0;
    }

    isize LocalDatagramSocket::connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Process *process = cpu::get_current_thread()->process;
        auto *addr = (sockaddr_un*)addr_ptr;
        if (addr->sun_family != AF_LOCAL) return -EAFNOSUPPORT;

        log_syscall("LocalDatagramSocket::connect(\"%s\")\n", addr->sun_path);

        vfs::Entry *entry = vfs::path_to_entry(addr->sun_path, process->cwd);
        if (entry->vnode == nullptr) return -ENOENT;
        if (entry->vnode->node_type != vfs::NodeType::SOCKET) return -ECONNREFUSED;
        Socket *target = (Socket*)entry->vnode;
        if (target->socket_family != AF_LOCAL || target->socket_type != SOCK_DGRAM) return -EPROTOTYPE;

        connected = (LocalDatagramSocket*)target;
        return 0;
    }

    isize LocalDatagramSocket::recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) {
        if (flags & ~(MSG_DONTWAIT))
            klib::printf("LocalDatagramSocket::recvmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("LocalDatagramSocket::recvmsg: hdr->msg_control unsupported\n");
        if (hdr->msg_namelen != 0)
            klib::printf("LocalDatagramSocket::recvmsg: hdr->msg_name unsupported\n");

        hdr->msg_controllen = 0;
        hdr->msg_flags = 0;

        while (ring_buffer->is_empty()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        Datagram *datagram;
        defer { klib::free(datagram); };

        usize datagrams_read = ring_buffer->read(&datagram, 1);
        ASSERT(datagrams_read == 1);

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            if (iov->iov_len == 0) continue;
            usize count = klib::min(datagram->length - transferred, iov->iov_len);
            memcpy(iov->iov_base, datagram->data + transferred, count);
            transferred += count;
            if (count < iov->iov_len)
                break;
        }

        if (transferred < datagram->length)
            hdr->msg_flags |= MSG_TRUNC;

        datagram->from->socket_event.trigger();
        return transferred;
    }

    isize LocalDatagramSocket::sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) {
        if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) // FIXME: ignoring MSG_NOSIGNAL
            klib::printf("LocalDatagramSocket::sendmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("LocalDatagramSocket::sendmsg: hdr->msg_control unsupported\n");

        LocalDatagramSocket *target = connected;
        if (!target) {
            if (hdr->msg_namelen == 0)
                return -EDESTADDRREQ;

            auto *addr = (sockaddr_un*)hdr->msg_name;
            if (addr->sun_family != AF_LOCAL) return -EAFNOSUPPORT;

            sched::Process *process = cpu::get_current_thread()->process;
            vfs::Entry *entry = vfs::path_to_entry(addr->sun_path, process->cwd);
            if (entry->vnode == nullptr) return -ENOENT;
            if (entry->vnode->node_type != vfs::NodeType::SOCKET) return -ECONNREFUSED;

            target = (LocalDatagramSocket*)entry->vnode;
            if (target->socket_family != AF_LOCAL || target->socket_type != SOCK_DGRAM) return -EPROTOTYPE;
        }

        while (target->ring_buffer->is_full()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        usize count = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            count += iov->iov_len;
        }

        Datagram *datagram = Datagram::construct(this, count);

        usize copied = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            if (iov->iov_len == 0) continue;
            memcpy(datagram->data + copied, iov->iov_base, iov->iov_len);
            copied += iov->iov_len;
        }

        usize datagrams_written = target->ring_buffer->write(&datagram, 1);
        ASSERT(datagrams_written == 1);

        target->socket_event.trigger();
        return count;
    }

    isize LocalDatagramSocket::shutdown(vfs::FileDescription *fd, int how) {
        return -ENOSYS;
    }

    isize LocalDatagramSocket::getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) {
        klib::printf("LocalDatagramSocket::getsockopt: unsupported option layer %d, number %d\n", layer, number);
        return -ENOPROTOOPT;
    }

    isize LocalDatagramSocket::setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) {
        klib::printf("LocalDatagramSocket::setsockopt: unsupported option layer %d, number %d\n", layer, number);
        return -ENOPROTOOPT;
    }

    isize LocalDatagramSocket::getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        socklen_t this_length = get_local_addr_length(&this->address);
        memcpy(addr_ptr, &this->address, klib::min(this_length, *addr_length));
        *addr_length = this_length;
        return 0;
    }

    isize LocalDatagramSocket::getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        socklen_t peer_length = get_local_addr_length(&connected->address);
        memcpy(addr_ptr, &connected->address, klib::min(peer_length, *addr_length));
        *addr_length = peer_length;
        return 0;
    }
}
