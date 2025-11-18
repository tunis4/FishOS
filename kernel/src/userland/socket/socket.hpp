#pragma once

#include <klib/lock.hpp>
#include <klib/cstring.hpp>
#include <fs/vfs.hpp>
#include <dev/net.hpp>
#define _SYS_SOCKET_H
#include <bits/socket.h>
#include <sys/un.h>
#include <netinet/in.h>

namespace socket {
    struct Socket : public vfs::VNode {
        int socket_family = 0;
        int socket_type = 0;

        Socket();
        virtual ~Socket() {}

        virtual isize bind(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) = 0;
        virtual isize connect(vfs::FileDescription *fd, const sockaddr *addr_ptr, socklen_t addr_length) = 0;
        virtual isize listen(vfs::FileDescription *fd, int backlog) = 0;
        virtual isize accept(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags) = 0;
        virtual isize recvmsg(vfs::FileDescription *fd, msghdr *hdr, int flags) = 0;
        virtual isize sendmsg(vfs::FileDescription *fd, const msghdr *hdr, int flags) = 0;
        virtual isize shutdown(vfs::FileDescription *fd, int how) = 0;
        virtual isize getsockopt(vfs::FileDescription *fd, int layer, int number, void *buffer, socklen_t *size) = 0;
        virtual isize setsockopt(vfs::FileDescription *fd, int layer, int number, const void *buffer, socklen_t size) = 0;
        virtual isize getsockname(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) = 0;
        virtual isize getpeername(vfs::FileDescription *fd, sockaddr *addr_ptr, socklen_t *addr_length) = 0;

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize readv(vfs::FileDescription *fd, const iovec *iovs, int iovc, usize offset) override;
        isize writev(vfs::FileDescription *fd, const iovec *iovs, int iovc, usize offset) override;

        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return -ESPIPE; }
        isize mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) override { return -EACCES; }

        int create_socket_fd(int flags);
    };

    struct InternetAddress {
        net::Ipv4 ip;
        be16 port;

        inline void from_sockaddr(const sockaddr_in *addr) {
            ip.address.from_big_endian(addr->sin_addr.s_addr);
            port.from_big_endian(addr->sin_port);
        }

        inline void to_sockaddr(sockaddr_in *addr) {
            addr->sin_family = AF_INET;
            addr->sin_addr.s_addr = ip.address.as_big_endian();
            addr->sin_port = port.as_big_endian();
        }
    };

    inline socklen_t get_local_addr_length(sockaddr_un *addr) {
        return klib::min(offsetof(sockaddr_un, sun_path) + klib::strlen(addr->sun_path), sizeof(sockaddr_un));
    }

    isize syscall_socket(int family, int type, int protocol);
    isize syscall_socketpair(int family, int type, int protocol, int fds[2]);
    isize syscall_bind(int fd, const sockaddr *addr_ptr, socklen_t addr_length);
    isize syscall_connect(int fd, const sockaddr *addr_ptr, socklen_t addr_length);
    isize syscall_listen(int fd, int backlog);
    isize syscall_accept4(int fd, sockaddr *addr_ptr, socklen_t *addr_length, int flags);
    isize syscall_accept(int fd, sockaddr *addr_ptr, socklen_t *addr_length);
    isize syscall_recvfrom(int fd, void *buf, usize size, int flags, sockaddr *src_addr, socklen_t *addrlen);
    isize syscall_recvmsg(int fd, msghdr *hdr, int flags);
    isize syscall_sendto(int fd, void *buf, usize size, int flags, const sockaddr *dest_addr, socklen_t addrlen);
    isize syscall_sendmsg(int fd, const msghdr *hdr, int flags);
    isize syscall_shutdown(int fd, int how);
    isize syscall_getsockopt(int fd, int layer, int number, void *buffer, socklen_t *size);
    isize syscall_setsockopt(int fd, int layer, int number, const void *buffer, socklen_t size);
    isize syscall_getsockname(int fd, sockaddr *addr_ptr, socklen_t *addr_length);
    isize syscall_getpeername(int fd, sockaddr *addr_ptr, socklen_t *addr_length);
}
