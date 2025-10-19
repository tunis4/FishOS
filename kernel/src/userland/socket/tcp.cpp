#include <userland/socket/tcp.hpp>

namespace socket {
    static constexpr u16 PORT_ALLOC_RANGE_START = 32768;
    static constexpr u16 PORT_ALLOC_RANGE_END = 60999;

    static TcpSocket *port_table[65536] = {};
    static klib::Spinlock port_table_lock;

    isize TcpSocket::alloc_port(u16 chosen) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(port_table_lock);
        if (chosen == 0) {
            for (u16 i = PORT_ALLOC_RANGE_START; i < PORT_ALLOC_RANGE_END; i++) {
                if (port_table[i] == nullptr) {
                    chosen = i;
                    break;
                }
            }
        }
        if (port_table[chosen] != nullptr)
            return -EADDRINUSE;
        port_table[chosen] = this;
        return chosen;
    }

    void TcpSocket::free_port() {
        if (has_port) {
            klib::InterruptLock interrupt_guard;
            klib::LockGuard guard(port_table_lock);
            port_table[bound_address.port] = nullptr;
            bound_address.port = 0;
            has_port = false;
        }
    }

    void tcp_process(net::TcpHeader *header, net::Ipv4Header *ipv4_header) {
        usize packet_length = ipv4_header->packet_length - sizeof(net::Ipv4Header);
        if (packet_length < sizeof(net::TcpHeader))
            return;

        if (ipv4_transport_checksum((u16*)header, packet_length, ipv4_header->src_addr, ipv4_header->dst_addr, ipv4_header->protocol) != 0)
            return;

        TcpSocket *target_socket = nullptr;
        {
            klib::InterruptLock interrupt_guard;
            klib::LockGuard guard(port_table_lock);
            target_socket = port_table[header->dst_port];
        }
        if (!target_socket)
            return;

        auto *datagram = TcpSocket::Datagram::construct(packet_length);
        memcpy(&datagram->header, header, packet_length);

        // {
        //     klib::InterruptLock interrupt_guard;
        //     if (target_socket->datagram_ring_buffer->is_full()) {
        //         TcpSocket::Datagram *old_datagram;
        //         target_socket->datagram_ring_buffer->read(&old_datagram, 1);
        //         klib::free(old_datagram);
        //     }
        //     target_socket->datagram_ring_buffer->write(&datagram, 1);
        // }
        target_socket->socket_event.trigger();
    }

    TcpSocket::Datagram* TcpSocket::Datagram::construct(u16 length) {
        auto *datagram = (Datagram*)klib::malloc(sizeof(Datagram) + length);
        datagram->length = length;
        return datagram;
    }

    TcpSocket::TcpSocket() {
        socket_family = AF_INET;
        socket_type = SOCK_DGRAM;
        event = &socket_event;
        recv_ring_buffer = new StreamRingBuffer();
        send_ring_buffer = new StreamRingBuffer();
    }

    TcpSocket::~TcpSocket() {
        delete recv_ring_buffer;
        delete send_ring_buffer;
    }

    isize TcpSocket::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (!recv_ring_buffer->is_empty())
                revents |= POLLIN;
        if (events & POLLOUT)
            if (!send_ring_buffer->is_full())
                revents |= POLLOUT;
        return revents;
    }

    isize TcpSocket::bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        auto *addr = (sockaddr_in*)addr_ptr;
        u16 port = klib::bswap((u16)addr->sin_port);
        isize ret = alloc_port(port);
        if (ret < 0)
            return ret;
        bound_address.from_sockaddr(addr);
        free_port();
        bound_address.port = ret;
        has_port = true;
        return 0;
    }

    isize TcpSocket::connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        auto *addr = (sockaddr_in*)addr_ptr;
        connected_address.from_sockaddr(addr);
        is_connected = true;
        return 0;
    }

    isize TcpSocket::recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) {
        if (flags & ~(MSG_DONTWAIT))
            klib::printf("TcpSocket::recvmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("TcpSocket::recvmsg: hdr->msg_control unsupported\n");
        if (hdr->msg_namelen != 0)
            klib::printf("TcpSocket::recvmsg: hdr->msg_name unsupported\n");

        hdr->msg_controllen = 0;
        hdr->msg_flags = 0;

        while (recv_ring_buffer->is_empty()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            transferred += recv_ring_buffer->read((u8*)iov->iov_base, iov->iov_len);
            if (recv_ring_buffer->is_empty())
                break;
        }
        return transferred;
    }

    isize TcpSocket::sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) {
        if (flags & ~(MSG_DONTWAIT))
            klib::printf("TcpSocket::sendmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("TcpSocket::sendmsg: hdr->msg_control unsupported\n");

        if (!has_port) {
            isize ret = alloc_port();
            if (ret < 0)
                return ret;
            bound_address.port = ret;
            has_port = true;
        }

        InternetAddress target;
        if (hdr->msg_name) {
            target.from_sockaddr((sockaddr_in*)hdr->msg_name);
        } else {
            if (is_connected)
                target = connected_address;
            else
                return -EDESTADDRREQ;
        }

        while (send_ring_buffer->is_full()) {
            if ((fd->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT))
                return -EWOULDBLOCK;
            if (socket_event.wait() == -EINTR)
                return -EINTR;
        }

        usize transferred = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            transferred += send_ring_buffer->write((u8*)iov->iov_base, iov->iov_len);
            if (send_ring_buffer->is_full())
                break;
        }
        return transferred;
    }

    isize TcpSocket::shutdown(vfs::FileDescription *fd, int how) {
        return -ENOSYS;
    }

    isize TcpSocket::getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) {
        return -ENOPROTOOPT;
    }

    isize TcpSocket::setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) {
        return -ENOPROTOOPT;
    }

    isize TcpSocket::getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        return -ENOSYS;
    }

    isize TcpSocket::getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        return -ENOSYS;
    }
}
