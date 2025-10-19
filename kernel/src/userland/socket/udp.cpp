#include <userland/socket/udp.hpp>

namespace socket {
    static constexpr u16 PORT_ALLOC_RANGE_START = 32768;
    static constexpr u16 PORT_ALLOC_RANGE_END = 60999;

    static UdpSocket *port_table[65536] = {};
    static klib::Spinlock port_table_lock;

    isize UdpSocket::alloc_port(u16 chosen) {
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

    void UdpSocket::free_port() {
        if (has_port) {
            klib::InterruptLock interrupt_guard;
            klib::LockGuard guard(port_table_lock);
            port_table[bound_address.port] = nullptr;
            bound_address.port = 0;
            has_port = false;
        }
    }

    void udp_process(net::UdpHeader *header, net::Ipv4Header *ipv4_header) {
        if (header->length < sizeof(net::UdpHeader))
            return;
        usize data_length = header->length - sizeof(net::UdpHeader);

        if (ipv4_transport_checksum((u16*)header, header->length, ipv4_header->src_addr, ipv4_header->dst_addr, ipv4_header->protocol) != 0)
            return;

        UdpSocket *target_socket = nullptr;
        {
            klib::InterruptLock interrupt_guard;
            klib::LockGuard guard(port_table_lock);
            target_socket = port_table[header->dst_port];
        }
        if (!target_socket)
            return;

        auto *datagram = UdpSocket::Datagram::construct(data_length);
        datagram->src_addr = ipv4_header->src_addr;
        datagram->src_port = header->src_port;
        memcpy(datagram->data, (u8*)header + sizeof(net::UdpHeader), data_length);

        {
            klib::InterruptLock interrupt_guard;
            if (target_socket->ring_buffer->is_full()) {
                UdpSocket::Datagram *old_datagram;
                target_socket->ring_buffer->read(&old_datagram, 1);
                klib::free(old_datagram);
            }
            target_socket->ring_buffer->write(&datagram, 1);
        }
        target_socket->socket_event.trigger();
    }

    UdpSocket::Datagram* UdpSocket::Datagram::construct(u16 length) {
        auto *datagram = (Datagram*)klib::malloc(sizeof(Datagram) + length);
        datagram->length = length;
        return datagram;
    }

    UdpSocket::UdpSocket() {
        socket_family = AF_INET;
        socket_type = SOCK_DGRAM;
        event = &socket_event;
        ring_buffer = new RingBuffer();
    }

    UdpSocket::~UdpSocket() {
        while (!ring_buffer->is_empty()) {
            Datagram *datagram;
            ring_buffer->read(&datagram, 1);
            klib::free(datagram);
        }
        delete ring_buffer;
    }

    isize UdpSocket::poll(vfs::FileDescription *fd, isize events) {
        isize revents = 0;
        if (events & POLLIN)
            if (ring_buffer && !ring_buffer->is_empty())
                revents |= POLLIN;
        return revents;
    }

    isize UdpSocket::bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
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

    isize UdpSocket::connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) {
        auto *addr = (sockaddr_in*)addr_ptr;
        connected_address.from_sockaddr(addr);
        is_connected = true;
        return 0;
    }

    isize UdpSocket::recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) {
        if (flags & ~(MSG_DONTWAIT))
            klib::printf("UdpSocket::recvmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("UdpSocket::recvmsg: hdr->msg_control unsupported\n");
        if (hdr->msg_namelen != 0)
            klib::printf("UdpSocket::recvmsg: hdr->msg_name unsupported\n");

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
            usize count = klib::min(datagram->length - transferred, iov->iov_len);
            memcpy(iov->iov_base, datagram->data + transferred, count);
            transferred += count;
            if (count < iov->iov_len)
                break;
        }

        if (transferred < datagram->length)
            hdr->msg_flags |= MSG_TRUNC;

        return transferred;
    }

    isize UdpSocket::sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) {
        if (flags != 0)
            klib::printf("UdpSocket::sendmsg: unsupported flags %#X\n", flags);
        if (hdr->msg_controllen != 0)
            klib::printf("UdpSocket::sendmsg: hdr->msg_control unsupported\n");

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

        usize count = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            count += iov->iov_len;
        }

        usize buffer_size = count + sizeof(net::UdpHeader);
        u8 *buffer = new u8[buffer_size];
        defer { delete[] buffer; };
        auto *header = (net::UdpHeader*)buffer;
        auto *data = buffer + sizeof(net::UdpHeader);

        usize copied = 0;
        for (usize i = 0; i < hdr->msg_iovlen; i++) {
            struct iovec *iov = &hdr->msg_iov[i];
            memcpy(data + copied, iov->iov_base, iov->iov_len);
            copied += iov->iov_len;
        }

        header->src_port = bound_address.port;
        header->dst_port = target.port;
        header->length = buffer_size;
        header->checksum = 0; // computed in ipv4_send

        isize ret = klib::sync(net::ipv4_send(buffer, buffer_size, target.ip, net::Ipv4Header::PROTOCOL_UDP, true));
        if (ret < 0)
            return ret;
        return count;
    }

    isize UdpSocket::shutdown(vfs::FileDescription *fd, int how) {
        return 0;
    }

    isize UdpSocket::getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) {
        return -ENOPROTOOPT;
    }

    isize UdpSocket::setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) {
        return -ENOPROTOOPT;
    }

    isize UdpSocket::getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        sockaddr_in addr {};
        this->bound_address.to_sockaddr(&addr);
        memcpy(addr_ptr, &addr, klib::min(sizeof(sockaddr_in), *addr_length));
        *addr_length = sizeof(sockaddr_in);
        return 0;
    }

    isize UdpSocket::getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) {
        if (!is_connected)
            return -ENOTCONN;
        sockaddr_in addr {};
        this->connected_address.to_sockaddr(&addr);
        memcpy(addr_ptr, &addr, klib::min(sizeof(sockaddr_in), *addr_length));
        *addr_length = sizeof(sockaddr_in);
        return 0;
    }
}
