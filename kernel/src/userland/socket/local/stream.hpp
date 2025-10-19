#pragma once

#include <klib/ring_buffer.hpp>
#include <userland/socket/socket.hpp>

namespace socket {
    struct LocalStreamSocket final : public Socket {
        using RingBuffer = klib::RingBuffer<u8, 52 * 0x1000>;

        sched::Event socket_event;
        RingBuffer *ring_buffer = nullptr;
        klib::RingBuffer<cmsghdr*, 8> ancillary_ring_buffer;
        LocalStreamSocket *peer = nullptr;
        sockaddr_un address = {};

        klib::Spinlock pending_lock;
        klib::ListHead pending_list;
        usize num_pending = 0, max_pending = 0;

        ucred credentials;
        bool passcred = false;

        LocalStreamSocket();
        ~LocalStreamSocket();

        void close(vfs::FileDescription *fd) override;
        isize poll(vfs::FileDescription *fd, isize events) override;

        isize bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) override;
        isize connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) override;
        isize listen(vfs::FileDescription *fd, int backlog) override;
        isize accept(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags) override;
        isize recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) override;
        isize sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) override;
        isize shutdown(vfs::FileDescription *fd, int how) override;
        isize getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) override;
        isize setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) override;
        isize getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) override;
        isize getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) override;
    };
}
