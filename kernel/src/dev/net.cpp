// This file uses code from the Astral project
// See NOTICE.md for the license of Astral

#include <dev/net.hpp>
#include <mem/pmm.hpp>
#include <klib/cstdlib.hpp>
#include <klib/cstdio.hpp>
#include <klib/vector.hpp>
#include <userland/socket/udp.hpp>
#include <userland/socket/tcp.hpp>
#include <errno.h>

namespace net {
    static klib::Vector<Route> routing_table;
    static klib::Spinlock routing_table_lock;

    Route lookup_route(Ipv4 ip) {
        klib::SpinlockGuard guard(routing_table_lock);

        Route chosen;
        for (Route &route : routing_table) {
            if ((ip & route.netmask) != (route.destination & route.netmask))
                continue;
            if (route.metric < chosen.metric)
                chosen = route;
        }
        return chosen;
    }

    void add_route(Route route) {
        klib::SpinlockGuard guard(routing_table_lock);
        routing_table.push_back(route);
    }

    Interface::Interface() {}

    klib::Awaitable<void> Interface::eth_process(EthHeader *header) {
        if (header->destination != this->mac && header->destination != BROADCAST_MAC)
            co_return;

        switch (header->type) {
        case EthHeader::PROTOCOL_ARP:
            co_return co_await arp_process((ArpHeader*)((uptr)header + sizeof(EthHeader)));
        case EthHeader::PROTOCOL_IPV4:
            co_return co_await ipv4_process((Ipv4Header*)((uptr)header + sizeof(EthHeader)));
        }
    }

    klib::Awaitable<void> Interface::arp_process(ArpHeader *header) {
        if (header->hardware_type != ArpHeader::HARDWARE_ETHERNET
         || header->protocol_type != ArpHeader::PROTOCOL_IPV4
         || header->hardware_len != sizeof(Mac)
         || header->protocol_len != sizeof(Ipv4)
         || (header->opcode == ArpHeader::OPCODE_REPLY && header->dst_hardware != this->mac)
         || (header->opcode == ArpHeader::OPCODE_REQUEST && header->dst_protocol != this->ipv4))
            co_return;

        switch (header->opcode) {
        case ArpHeader::OPCODE_REPLY:
            // klib::printf("received arp reply\n");
            arp_cache.emplace(header->src_protocol, header->src_hardware);
            arp_request_callback.invoke(0);
            break;
        case ArpHeader::OPCODE_REQUEST:
            // klib::printf("received arp request\n");
            co_await arp_send(ArpHeader::OPCODE_REPLY, this->mac, this->ipv4, header->src_hardware, header->src_protocol);
        }
    }

    klib::Awaitable<isize> Interface::arp_send(u16 opcode, Mac src_hardware, Ipv4 src_protocol, Mac dst_hardware, Ipv4 dst_protocol) {
        Packet packet = alloc_packet(sizeof(ArpHeader));
        defer { free_packet(packet); };
        ArpHeader *header = (ArpHeader*)(packet.addr + packet.offset);
        *header = {
            .hardware_type = ArpHeader::HARDWARE_ETHERNET,
            .protocol_type = ArpHeader::PROTOCOL_IPV4,
            .hardware_len = sizeof(Mac),
            .protocol_len = sizeof(Ipv4),
            .opcode = opcode,
            .src_hardware = src_hardware,
            .src_protocol = src_protocol,
            .dst_hardware = dst_hardware,
            .dst_protocol = dst_protocol
        };
        Mac target_mac = BROADCAST_MAC;
        if (opcode == ArpHeader::OPCODE_REPLY)
            target_mac = dst_hardware;
        co_return co_await send_packet(packet, target_mac, EthHeader::PROTOCOL_ARP);
    }

    klib::Awaitable<isize> Interface::arp_lookup(Ipv4 target_ip, Mac *result_mac) {
        if (Mac *cached = arp_cache[target_ip]) {
            *result_mac = *cached;
            co_return 0;
        }
        co_await arp_send(ArpHeader::OPCODE_REQUEST, this->mac, this->ipv4, {}, target_ip);
        co_await klib::RootAwaitable<isize>(&this->arp_request_callback);
        if (Mac *cached = arp_cache[target_ip]) {
            *result_mac = *cached;
            co_return 0;
        } else {
            co_return -ENETUNREACH;
        }
    }

    static int partial_checksum(u16 *addr, usize count) {
        int sum = 0;
        while (count > 1) {
            sum += *addr;
            addr++;
            count -= 2;
        }
        if (count > 0)
            sum += *(u8*)addr;
        return sum;
    }

    u16 Ipv4Header::compute_header_checksum() {
        int sum = partial_checksum((u16*)this, sizeof(Ipv4Header));
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        return klib::bswap((u16)~sum);
    }

    u16 ipv4_transport_checksum(u16 *addr, usize count, Ipv4 src_addr, Ipv4 dst_addr, u8 protocol) {
        Ipv4PseudoHeader pseudo_header = {
            .src_addr = src_addr,
            .dst_addr = dst_addr,
            .zero = 0,
            .protocol = protocol,
            .length = count
        };
        int sum = partial_checksum((u16*)&pseudo_header, sizeof(Ipv4PseudoHeader));
        sum += partial_checksum(addr, count);
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        return klib::bswap((u16)~sum);
    }

    klib::Awaitable<void> Interface::ipv4_process(Ipv4Header *header) {
        if (header->version_and_header_length != Ipv4Header::VERSION_AND_HEADER_LENGTH
         || (header->dst_addr != BROADCAST_IPV4 && header->dst_addr != this->ipv4)
         || header->compute_header_checksum() != 0)
            co_return;

        if ((header->get_flags() & Ipv4Header::FLAG_MORE_FRAGMENTS) || header->get_frag_offset() > 0) {
            klib::printf("ipv4: reassembly unsupported\n");
            co_return;
        }

        switch (header->protocol) {
        case Ipv4Header::PROTOCOL_TCP:
            socket::tcp_process((TcpHeader*)((uptr)header + sizeof(Ipv4Header)), header);
            break;
        case Ipv4Header::PROTOCOL_UDP:
            socket::udp_process((UdpHeader*)((uptr)header + sizeof(Ipv4Header)), header);
            break;
        default:
            klib::printf("ipv4: received packet with unrecognized protocol %u\n", header->protocol);
        }
    }

    klib::Awaitable<isize> Interface::ipv4_send_fragment(void *data, u16 fragment_length, u16 fragment_offset, bool is_last_fragment, Ipv4 target_ip, Mac target_mac, u16 packet_id, u8 protocol) {
        Packet packet = alloc_packet(fragment_length + sizeof(Ipv4Header));
        defer { free_packet(packet); };
        Ipv4Header *header = (Ipv4Header*)(packet.addr + packet.offset);
        *header = {
            .version_and_header_length = Ipv4Header::VERSION_AND_HEADER_LENGTH,
            .dscp_and_ecn = 0,
            .packet_length = fragment_length + sizeof(Ipv4Header),
            .id = packet_id,
            .time_to_live = 0xFF,
            .protocol = protocol,
            .header_checksum = 0,
            .src_addr = this->ipv4,
            .dst_addr = target_ip
        };
        header->set_flags_and_frag_offset(is_last_fragment ? 0 : Ipv4Header::FLAG_MORE_FRAGMENTS, fragment_offset);
        header->header_checksum = header->compute_header_checksum();

        memcpy((u8*)header + sizeof(Ipv4Header), data, fragment_length);
        co_return co_await send_packet(packet, target_mac, EthHeader::PROTOCOL_IPV4);
    }

    klib::Awaitable<isize> ipv4_send(void *data, usize length, Ipv4 target_ip, u8 protocol, bool compute_checksum) {
        Interface *interface = nullptr;
        Mac target_mac;

        if (target_ip != BROADCAST_IPV4) {
            Route route = lookup_route(target_ip);
            interface = route.interface;
            if (interface) {
                isize err = co_await interface->arp_lookup(route.gateway ? route.gateway : target_ip, &target_mac);
                if (err < 0)
                    co_return err;
            }
        } else {
            klib::printf("no broadcast device\n");
        }

        if (!interface)
            co_return -EHOSTUNREACH;

        if (compute_checksum) {
            if (protocol == Ipv4Header::PROTOCOL_UDP)
                ((UdpHeader*)data)->checksum = ipv4_transport_checksum((u16*)data, length, interface->ipv4, target_ip, protocol);
            else if (protocol == Ipv4Header::PROTOCOL_TCP)
                ((TcpHeader*)data)->checksum = ipv4_transport_checksum((u16*)data, length, interface->ipv4, target_ip, protocol);
        }

        usize packet_id = interface->ipv4_packet_id++;
        usize max_fragment_length = klib::align_down(interface->mtu - sizeof(Ipv4Header), 8);
        usize num_fragments = length / max_fragment_length + 1;

        usize fragment_offset = 0;
        for (usize i = 0; i < num_fragments; i++) {
            bool is_last_fragment = i == num_fragments - 1;
            uptr fragment_data = (uptr)data + fragment_offset;
            usize fragment_length = klib::min(max_fragment_length, length - fragment_offset);

            isize err = co_await interface->ipv4_send_fragment((void*)fragment_data, fragment_length, fragment_offset, is_last_fragment, target_ip, target_mac, packet_id, protocol);
            if (err < 0)
                co_return err;

            fragment_offset += max_fragment_length;
        }
        co_return 0;
    }

    LoopbackInterface::LoopbackInterface() {
        mtu = 0x1000 - sizeof(EthHeader);

        this->ipv4 = Ipv4(127, 0, 0, 1);
        add_route(Route {
            .interface = this,
            .destination = {},
            .netmask = Ipv4(255, 0, 0, 0),
            .gateway = {},
            .metric = 1
        });
    }

    Packet LoopbackInterface::alloc_packet(usize requested_size) {
        ASSERT(requested_size <= mtu);
        pmm::Page *page = pmm::alloc_page();
        if (page == nullptr)
            return {};
        return Packet {
            .addr = mem::hhdm + page->phy(),
            .size = sizeof(EthHeader) + requested_size,
            .offset = sizeof(EthHeader)
        };
    }

    void LoopbackInterface::free_packet(Packet packet) {
        pmm::free_page(pmm::find_page(packet.addr - mem::hhdm));
    }

    klib::RootAwaitable<isize> LoopbackInterface::send_packet(Packet packet, Mac target_mac, u16 proto) {
        klib::sync(eth_process((EthHeader*)packet.addr));
        return (isize)0;
    }
}
