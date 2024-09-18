#include <userland/socket.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <sys/un.h>
#include <errno.h>

namespace userland {
    static int create_socket_fd(Socket *socket, int flags) {
        sched::Process *process = cpu::get_current_thread()->process;
        int fd = process->allocate_fdnum();
        auto *description = new vfs::FileDescription(socket, O_RDONLY | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
        socket->open(description);
        process->file_descriptors[fd].init(description, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
        return fd;
    }

    Socket::Socket() {
        type = vfs::NodeType::SOCKET;
    }

    LocalSocket::LocalSocket() {
        family = AF_LOCAL;
        event = &socket_event;
        pending_list.init();
    }

    LocalSocket::~LocalSocket() {
        if (ring_buffer)
            delete ring_buffer;
    }

    isize LocalSocket::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        if (!ring_buffer)
            return -ENOTCONN;

        while (ring_buffer->is_empty()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            socket_event.await();
        }

        count = ring_buffer->read((u8*)buf, count);
        peer->socket_event.trigger();
        return count;
    }

    isize LocalSocket::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        if (!peer->ring_buffer)
            return -ENOTCONN;

        while (peer->ring_buffer->is_full()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            socket_event.await();
        }

        count = peer->ring_buffer->write((const u8*)buf, count);
        peer->socket_event.trigger();
        return count;
    }

    isize LocalSocket::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN) {
            if (ring_buffer && !ring_buffer->is_empty())
                revents |= POLLIN;
            if (max_pending > 0 && !pending_list.is_empty())
                revents |= POLLIN;
        }
        if (events & POLLOUT)
            if (peer->ring_buffer && !peer->ring_buffer->is_full())
                revents |= POLLOUT;
        return revents;
    }

    isize LocalSocket::bind(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Process *process = cpu::get_current_thread()->process;
        auto *addr = (struct sockaddr_un*)addr_ptr;

        vfs::Entry *entry = vfs::path_to_node(addr->sun_path, process->cwd);
        if (entry->vnode != nullptr)
            return -EADDRINUSE;
        if (entry->parent == nullptr)
            return -ENOENT;
        entry->vnode = this;
        entry->create();
        return 0;
    }

    isize LocalSocket::connect(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
        sched::Process *process = cpu::get_current_thread()->process;
        auto *addr = (struct sockaddr_un*)addr_ptr;
        if (addr->sun_family != AF_LOCAL)
            return -EAFNOSUPPORT;

        vfs::Entry *entry = vfs::path_to_node(addr->sun_path, process->cwd);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->type != vfs::NodeType::SOCKET)
            return -ECONNREFUSED;

        LocalSocket *target = (LocalSocket*)entry->vnode;
        {
            klib::LockGuard guard(target->pending_lock);
            if (target->num_pending >= target->max_pending)
                return -ECONNREFUSED;
            target->pending_list.add_before(&pending_list);
        }
        target->socket_event.trigger();
        ring_buffer = new RingBuffer();
        socket_event.await();
        return 0;
    }

    isize LocalSocket::listen(vfs::FileDescription *fd, int backlog) {
        max_pending = backlog;
        return 0;
    }

    isize LocalSocket::accept(vfs::FileDescription *fd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
        klib::LockGuard guard(pending_lock);

        while (pending_list.is_empty()) {
            if (fd->flags & O_NONBLOCK)
                return -EWOULDBLOCK;
            guard.unlock();
            socket_event.await();
            guard.lock();
        }

        LocalSocket *peer = LIST_ENTRY(pending_list.next, LocalSocket, pending_list);
        peer->pending_list.remove();

        if (addr_ptr)
            addr_ptr->sa_family = AF_LOCAL;
        if (addr_length)
            *addr_length = sizeof(sa_family_t);

        auto *connected_socket = new LocalSocket();
        connected_socket->peer = peer;
        connected_socket->ring_buffer = new RingBuffer();

        peer->peer = connected_socket;
        peer->socket_event.trigger();

        return create_socket_fd(connected_socket, flags);
    }

    isize LocalSocket::recvmsg(vfs::FileDescription *fd, struct msghdr *hdr, int flags) {
        // if (flags != 0)
        //     klib::printf("LocalSocket::recvmsg: unsupported flags %#X\n", flags);
        // if (hdr->msg_flags != 0)
        //     klib::printf("LocalSocket::recvmsg: unsupported hdr->msg_flags %#X\n", hdr->msg_flags);
        // if (hdr->msg_controllen != 0)
        //     klib::printf("LocalSocket::recvmsg: hdr->msg_control unsupported\n");
        // if (hdr->msg_namelen != 0)
        //     klib::printf("LocalSocket::recvmsg: hdr->msg_name unsupported\n");

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];

            isize count = read(fd, iov->iov_base, iov->iov_len, 0);
            if (count < 0)
                return count;

            transferred += count;
            if (count < (isize)iov->iov_len)
                break;
            if (ring_buffer->is_empty())
                break;
        }
        return transferred;
    }

    isize LocalSocket::sendmsg(vfs::FileDescription *fd, const struct msghdr *hdr, int flags) {
        // if (flags != 0)
        //     klib::printf("LocalSocket::sendmsg: unsupported flags %#X\n", flags);
        // if (hdr->msg_flags != 0)
        //     klib::printf("LocalSocket::sendmsg: unsupported hdr->msg_flags %#X\n", hdr->msg_flags);
        // if (hdr->msg_controllen != 0)
        //     klib::printf("LocalSocket::sendmsg: hdr->msg_control unsupported\n");
        // if (hdr->msg_namelen != 0)
        //     klib::printf("LocalSocket::sendmsg: hdr->msg_name unsupported\n");

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];

            isize count = write(fd, iov->iov_base, iov->iov_len, 0);
            if (count < 0)
                return count;
            transferred += count;

            if (count < (isize)iov->iov_len)
                break;
            if (peer->ring_buffer->is_full())
                break;
        }
        return transferred;
    }

    isize syscall_socket(int family, int type, int protocol) {
#if SYSCALL_TRACE
        klib::printf("socket(%d, %d, %d)\n", family, type, protocol);
#endif
        if ((type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM) {
            klib::printf("unsupported socket type: %#X\n", type);
            return -EINVAL;
        }

        Socket *socket;
        switch (family) {
        case AF_LOCAL: socket = new LocalSocket(); break;
        default: return -EAFNOSUPPORT;
        }

        return create_socket_fd(socket, type);
    }

    isize syscall_socketpair(int family, int type, int protocol, int fds[2]) {
#if SYSCALL_TRACE
        klib::printf("socketpair(%d, %d, %d, %#lX)\n", family, type, protocol, (uptr)fds);
#endif
        if ((type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM) {
            klib::printf("unsupported socket type: %#X\n", type);
            return -EINVAL;
        }

        if (family != AF_LOCAL)
            return -EAFNOSUPPORT;

        LocalSocket *socket1 = new LocalSocket();
        LocalSocket *socket2 = new LocalSocket();

        socket1->ring_buffer = new LocalSocket::RingBuffer();
        socket2->ring_buffer = new LocalSocket::RingBuffer();

        socket1->peer = socket2;
        socket2->peer = socket1;

        fds[0] = create_socket_fd(socket1, type);
        fds[1] = create_socket_fd(socket2, type);
        return 0;
    }

    isize syscall_bind(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
#if SYSCALL_TRACE
        klib::printf("bind(%d, %#lX, %u)\n", fd, (uptr)addr_ptr, addr_length);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        if (addr_ptr->sa_family != socket->family)
            return -EINVAL;
        return socket->bind(description, addr_ptr, addr_length);
    }

    isize syscall_connect(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
#if SYSCALL_TRACE
        klib::printf("connect(%d, %#lX, %u)\n", fd, (uptr)addr_ptr, addr_length);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        return socket->connect(description, addr_ptr, addr_length);
    }

    isize syscall_listen(int fd, int backlog) {
#if SYSCALL_TRACE
        klib::printf("listen(%d, %d)\n", fd, backlog);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        return socket->listen(description, backlog);
    }

    isize syscall_accept(int fd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
#if SYSCALL_TRACE
        klib::printf("accept(%d, %#lX, %#lX, %d)\n", fd, (uptr)addr_ptr, (uptr)addr_length, flags);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        return socket->accept(description, addr_ptr, addr_length, flags);
    }

    isize syscall_recvmsg(int fd, struct msghdr *hdr, int flags) {
#if SYSCALL_TRACE
        klib::printf("recvmsg(%d, %#lX, %d)\n", fd, (uptr)hdr, flags);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        return socket->recvmsg(description, hdr, flags);
    }

    isize syscall_sendmsg(int fd, const struct msghdr *hdr, int flags) {
#if SYSCALL_TRACE
        klib::printf("sendmsg(%d, %#lX, %d)\n", fd, (uptr)hdr, flags);
#endif
        auto *description = vfs::get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != vfs::NodeType::SOCKET)
            return -ENOTSOCK;
        Socket *socket = (Socket*)description->vnode;
        return socket->sendmsg(description, hdr, flags);
    }

    isize syscall_shutdown(int fd, int how) {
#if SYSCALL_TRACE
        klib::printf("shutdown(%d, %d)\n", fd, how);
#endif
        return -ENOSYS;
    }
}
