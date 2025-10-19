#include <userland/socket/socket.hpp>
#include <userland/socket/local/stream.hpp>
#include <userland/socket/local/datagram.hpp>
#include <userland/socket/udp.hpp>
#include <userland/socket/tcp.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpu.hpp>
#include <sched/sched.hpp>
#include <klib/cstdio.hpp>
#include <errno.h>

namespace socket {
    Socket::Socket() {
        node_type = vfs::NodeType::SOCKET;
    }

    int Socket::create_socket_fd(int flags) {
        sched::Process *process = cpu::get_current_thread()->process;
        int fd = process->allocate_fdnum();
        auto *description = new vfs::FileDescription(this, O_RDONLY | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
        this->open(description);
        process->file_descriptors[fd].init(description, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
        return fd;
    }

    isize Socket::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        iovec iov = { .iov_base = buf, .iov_len = count };
        msghdr hdr = { .msg_iov = &iov, .msg_iovlen = 1 };
        return this->recvmsg(fd, &hdr, 0);
    }

    isize Socket::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        iovec iov = { .iov_base = (void*)buf, .iov_len = count };
        msghdr hdr = { .msg_iov = &iov, .msg_iovlen = 1 };
        return this->sendmsg(fd, &hdr, 0);
    }

    isize syscall_socket(int family, int type, int protocol) {
        log_syscall("socket(%d, %d, %d)\n", family, type, protocol);
        int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
        type &= ~flags;

        Socket *socket;
        if (family == AF_LOCAL && type == SOCK_STREAM) {
            socket = new LocalStreamSocket();
        } else if (family == AF_LOCAL && type == SOCK_DGRAM) {
            socket = new LocalDatagramSocket();
        } else if (family == AF_INET && type == SOCK_STREAM) {
            return -EAFNOSUPPORT; // socket = new TcpSocket();
        } else if (family == AF_INET && type == SOCK_DGRAM) {
            socket = new UdpSocket();
        } else {
            return -EAFNOSUPPORT;
        }

        return socket->create_socket_fd(flags);
    }

    isize syscall_socketpair(int family, int type, int protocol, int fds[2]) {
        log_syscall("socketpair(%d, %d, %d, %#lX)\n", family, type, protocol, (uptr)fds);
        int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
        type &= ~flags;

        if (family != AF_LOCAL)
            return -EAFNOSUPPORT;

        sched::Process *process = cpu::get_current_thread()->process;
        ucred credentials;
        credentials.pid = process->pid;
        credentials.uid = 0;
        credentials.gid = 0;

        if (type == SOCK_STREAM) {
            auto *socket1 = new LocalStreamSocket();
            auto *socket2 = new LocalStreamSocket();

            socket1->ring_buffer = new LocalStreamSocket::RingBuffer();
            socket2->ring_buffer = new LocalStreamSocket::RingBuffer();
            socket1->peer = socket2;
            socket2->peer = socket1;
            socket1->credentials = credentials;
            socket2->credentials = credentials;

            fds[0] = socket1->create_socket_fd(flags);
            fds[1] = socket2->create_socket_fd(flags);
        } else if (type == SOCK_DGRAM) {
            auto *socket1 = new LocalDatagramSocket();
            auto *socket2 = new LocalDatagramSocket();

            socket1->connected = socket2;
            socket2->connected = socket1;

            fds[0] = socket1->create_socket_fd(flags);
            fds[1] = socket2->create_socket_fd(flags);
        } else {
            return -EINVAL;
        }
        return 0;
    }

#define get_socket(fd) \
        auto *description = vfs::get_file_description(fd); \
        if (!description) \
            return -EBADF; \
        if (description->vnode->node_type != vfs::NodeType::SOCKET) \
            return -ENOTSOCK; \
        Socket *socket = (Socket*)description->vnode

    isize syscall_bind(int fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        log_syscall("bind(%d, %#lX, %u)\n", fd, (uptr)addr_ptr, addr_length);
        get_socket(fd);
        if (addr_ptr->sa_family != socket->socket_family)
            return -EINVAL;
        return socket->bind(description, addr_ptr, addr_length);
    }

    isize syscall_connect(int fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        log_syscall("connect(%d, %#lX, %u)\n", fd, (uptr)addr_ptr, addr_length);
        get_socket(fd);
        if (addr_ptr->sa_family != socket->socket_family)
            return -EAFNOSUPPORT;
        return socket->connect(description, addr_ptr, addr_length);
    }

    isize syscall_listen(int fd, int backlog) {
        log_syscall("listen(%d, %d)\n", fd, backlog);
        get_socket(fd);
        return socket->listen(description, backlog);
    }

    isize syscall_accept(int fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
        log_syscall("accept(%d, %#lX, %#lX, %d)\n", fd, (uptr)addr_ptr, (uptr)addr_length, flags);
        get_socket(fd);
        if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
            return -EINVAL;
        return socket->accept(description, addr_ptr, addr_length, flags);
    }

    isize syscall_recvmsg(int fd, msghdr *hdr, int flags) {
        log_syscall("recvmsg(%d, %#lX, %d)\n", fd, (uptr)hdr, flags);
        get_socket(fd);
        return socket->recvmsg(description, hdr, flags);
    }

    isize syscall_sendmsg(int fd, const msghdr *hdr, int flags) {
        log_syscall("sendmsg(%d, %#lX, %d)\n", fd, (uptr)hdr, flags);
        get_socket(fd);
        return socket->sendmsg(description, hdr, flags);
    }

    isize syscall_shutdown(int fd, int how) {
        log_syscall("shutdown(%d, %d)\n", fd, how);
        if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
            return -EINVAL;
        get_socket(fd);
        return socket->shutdown(description, how);
    }

    isize syscall_getsockopt(int fd, int layer, int number, void *buffer, socklen_t *size) {
        log_syscall("getsockopt(%d, %d, %d, %#lX, %#lX)\n", fd, layer, number, (uptr)buffer, (uptr)size);
        get_socket(fd);
        return socket->getsockopt(description, layer, number, buffer, size);
    }

    isize syscall_setsockopt(int fd, int layer, int number, const void *buffer, socklen_t size) {
        log_syscall("setsockopt(%d, %d, %d, %#lX, %#X)\n", fd, layer, number, (uptr)buffer, size);
        get_socket(fd);
        return socket->setsockopt(description, layer, number, buffer, size);
    }

    isize syscall_getsockname(int fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        log_syscall("getsockname(%d, %#lX, %#lX)\n", fd, (uptr)addr_ptr, (uptr)addr_length);
        get_socket(fd);
        return socket->getsockname(description, addr_ptr, addr_length);
    }

    isize syscall_getpeername(int fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        log_syscall("getpeername(%d, %#lX, %#lX)\n", fd, (uptr)addr_ptr, (uptr)addr_length);
        get_socket(fd);
        return socket->getpeername(description, addr_ptr, addr_length);
    }
}
