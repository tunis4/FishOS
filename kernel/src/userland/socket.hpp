#pragma once

#include <klib/ring_buffer.hpp>
#include <klib/lock.hpp>
#include <fs/vfs.hpp>
#include <sys/socket.h>

namespace userland {
    struct Socket : public vfs::VNode {
        int family = 0;

        Socket();
        virtual ~Socket() {}

        virtual isize bind(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) = 0;
        virtual isize connect(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) = 0;
        virtual isize listen(vfs::FileDescription *fd, int backlog) = 0;
        virtual isize accept(vfs::FileDescription *fd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) = 0;
        virtual isize recvmsg(vfs::FileDescription *fd, struct msghdr *hdr, int flags) = 0;
        virtual isize sendmsg(vfs::FileDescription *fd, const struct msghdr *hdr, int flags) = 0;

        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return -ESPIPE; }
    };

    struct LocalSocket final : public Socket {
        using RingBuffer = klib::RingBuffer<u8, 52 * 0x1000>;

        LocalSocket *peer;
        sched::Event socket_event;
        RingBuffer *ring_buffer = nullptr;

        klib::Spinlock pending_lock;
        klib::ListHead pending_list;
        usize num_pending = 0, max_pending = 0;

        LocalSocket();
        ~LocalSocket();

        isize bind(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) override;
        isize connect(vfs::FileDescription *fd, const struct sockaddr *addr_ptr, socklen_t addr_length) override;
        isize listen(vfs::FileDescription *fd, int backlog) override;
        isize accept(vfs::FileDescription *fd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) override;
        isize recvmsg(vfs::FileDescription *fd, struct msghdr *hdr, int flags) override;
        isize sendmsg(vfs::FileDescription *fd, const struct msghdr *hdr, int flags) override;

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize poll(vfs::FileDescription *fd, isize events) override;
    };

    isize syscall_socket(int family, int type, int protocol);
    isize syscall_socketpair(int family, int type, int protocol, int fds[2]);
    isize syscall_bind(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length);
    isize syscall_connect(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length);
    isize syscall_listen(int fd, int backlog);
    isize syscall_accept(int fd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags);
    isize syscall_recvmsg(int fd, struct msghdr *hdr, int flags);
    isize syscall_sendmsg(int fd, const struct msghdr *hdr, int flags);
    isize syscall_shutdown(int fd, int how);
}
