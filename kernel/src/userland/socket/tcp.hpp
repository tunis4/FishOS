#pragma once

#include <klib/ring_buffer.hpp>
#include <userland/socket/socket.hpp>

namespace socket {
    struct TcpSocket final : public Socket {
        // only used on receive
        struct [[gnu::packed]] Datagram {
            u16 length; // length of tcp header + data
            net::TcpHeader header;
            u8 data[];

            static Datagram* construct(u16 length);
        };

        using DatagramRingBuffer = klib::RingBuffer<Datagram*, 256>;
        using StreamRingBuffer = klib::RingBuffer<u8, 32 * 0x1000>;

        sched::Event socket_event;
        // DatagramRingBuffer *datagram_ring_buffer = nullptr;
        StreamRingBuffer *recv_ring_buffer = nullptr;
        StreamRingBuffer *send_ring_buffer = nullptr;

        InternetAddress bound_address = {};
        InternetAddress connected_address = {};
        bool has_port = false;
        bool is_connected = false;

        TcpSocket();
        ~TcpSocket();

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

    private:
        isize alloc_port(u16 chosen = 0);
        void free_port();
    };

    void tcp_process(net::TcpHeader *header, net::Ipv4Header *ipv4_header);
}
