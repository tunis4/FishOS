#pragma once

#include <klib/ring_buffer.hpp>
#include <userland/socket/socket.hpp>

namespace socket {
    struct LocalDatagramSocket final : public Socket {
        struct Datagram {
            LocalDatagramSocket *from;
            u32 length; // length of data, not whole datagram
            u8 data[];

            static Datagram* construct(LocalDatagramSocket *from, u32 length);
        };

        using RingBuffer = klib::RingBuffer<Datagram*, 256>;

        sched::Event socket_event;
        RingBuffer *ring_buffer = nullptr;
        LocalDatagramSocket *connected = nullptr;
        sockaddr_un address = {};

        LocalDatagramSocket();
        ~LocalDatagramSocket();

        isize bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) override;
        isize connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) override;
        isize listen(vfs::FileDescription *fd, int backlog) override { return -EOPNOTSUPP; }
        isize accept(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags) override { return -EOPNOTSUPP; }
        isize recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) override;
        isize sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) override;
        isize shutdown(vfs::FileDescription *fd, int how) override;
        isize getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) override;
        isize setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) override;
        isize getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) override;
        isize getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) override;

        isize poll(vfs::FileDescription *fd, isize events) override;
    };
}
