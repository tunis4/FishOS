#pragma once

#include <klib/common.hpp>
#include <klib/async.hpp>
#include <klib/hashtable.hpp>

namespace net {
    struct [[gnu::packed]] Mac {
        u8 address[6];

        constexpr inline bool operator==(const Mac &other) const {
            for (int i = 0; i < 6; i++)
                if (this->address[i] != other.address[i])
                    return false;
            return true;
        }
    };
    static constexpr Mac BROADCAST_MAC = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    struct [[gnu::packed]] Ipv4 {
        be32 address;

        constexpr Ipv4() { address = 0; }
        constexpr Ipv4(u32 a0, u32 a1, u32 a2, u32 a3) { address = (a0 << 24) | (a1 << 16) | (a2 << 8) | a3; }
        constexpr inline bool operator==(const Ipv4 &other) const { return this->address == other.address; }
        constexpr inline u32 operator&(const Ipv4 &other) const { return this->address & other.address; }
        constexpr inline operator bool() const { return this->address != 0; }
    };
    static constexpr Ipv4 BROADCAST_IPV4 = Ipv4(0xff, 0xff, 0xff, 0xff);

    // only used for sending
    struct Packet {
        uptr addr; // virtual addr
        usize size; // full size including device/ethernet headers
        usize offset; // offset at which device/ethernet headers end
    };

    struct [[gnu::packed]] EthHeader {
        Mac destination;
        Mac source;
        be16 type;

        static constexpr u16 PROTOCOL_IPV4 = 0x800;
        static constexpr u16 PROTOCOL_ARP = 0x806;
    };

    struct [[gnu::packed]] ArpHeader {
        be16 hardware_type;
        be16 protocol_type;
        u8 hardware_len;
        u8 protocol_len;
        be16 opcode;
        Mac src_hardware;
        Ipv4 src_protocol;
        Mac dst_hardware;
        Ipv4 dst_protocol;

        static constexpr u16 HARDWARE_ETHERNET = 1;

        static constexpr u16 PROTOCOL_IPV4 = 0x0800;

        static constexpr u16 OPCODE_REQUEST = 1;
        static constexpr u16 OPCODE_REPLY = 2;
    };

    struct [[gnu::packed]] Ipv4Header {
        u8 version_and_header_length;
        u8 dscp_and_ecn;
        be16 packet_length;
        be16 id;
        be16 flags_and_frag_offset;
        u8 time_to_live;
        u8 protocol;
        be16 header_checksum;
        Ipv4 src_addr;
        Ipv4 dst_addr;

        u16 get_flags() const { return (flags_and_frag_offset >> 13) & 0x7; }
        u16 get_frag_offset() const { return (flags_and_frag_offset & 0x1fff) << 3; }
        void set_flags_and_frag_offset(u16 flags, u16 frag_offset) {
            flags_and_frag_offset = (flags << 13) | (frag_offset >> 3);
        }

        u16 compute_header_checksum();

        static constexpr u8 VERSION_AND_HEADER_LENGTH = (4 << 4) | 5;

        static constexpr u16 FLAG_MORE_FRAGMENTS = 1 << 0;
        static constexpr u16 FLAG_DONT_FRAGMENT = 1 << 1;

        static constexpr u16 PROTOCOL_TCP = 6;
        static constexpr u16 PROTOCOL_UDP = 17;
    };

    struct [[gnu::packed]] UdpHeader {
        be16 src_port;
        be16 dst_port;
        be16 length;
        be16 checksum;
    };

    struct [[gnu::packed]] TcpHeader {
        be16 src_port;
        be16 dst_port;
        be32 seq;
        be32 ack;
        u8 data_offset;
        u8 control_flags;
        be16 window;
        be16 checksum;
        be16 urgent_ptr;

        static constexpr u8 CONTROL_FIN = 1 << 0;
        static constexpr u8 CONTROL_SYN = 1 << 1;
        static constexpr u8 CONTROL_RST = 1 << 2;
        static constexpr u8 CONTROL_PSH = 1 << 3;
        static constexpr u8 CONTROL_ACK = 1 << 4;
        static constexpr u8 CONTROL_URG = 1 << 5;
    };

    struct Interface {
        Mac mac = {};
        usize mtu;
        Ipv4 ipv4;
        int ipv4_packet_id;
        klib::HashTable<Ipv4, Mac> arp_cache { 16 };
        klib::RequestCallback<isize> arp_request_callback;

        Interface();

        virtual Packet alloc_packet(usize requested_size) = 0;
        virtual void free_packet(Packet packet) = 0;
        virtual klib::RootAwaitable<isize> send_packet(Packet packet, Mac target_mac, u16 proto) = 0;

        klib::Awaitable<void> eth_process(EthHeader *header);
        klib::Awaitable<void> arp_process(ArpHeader *header);
        klib::Awaitable<void> ipv4_process(Ipv4Header *header);

        klib::Awaitable<isize> arp_send(u16 opcode, Mac src_hardware, Ipv4 src_protocol, Mac dst_hardware, Ipv4 dst_protocol);
        klib::Awaitable<isize> ipv4_send_fragment(void *data, u16 fragment_length, u16 fragment_offset, bool is_last_fragment, Ipv4 target_ip, Mac target_mac, u16 packet_id, u8 protocol);

        klib::Awaitable<isize> arp_lookup(Ipv4 target_ip, Mac *result_mac);
    };

    struct LoopbackInterface final : public Interface {
        LoopbackInterface();

        Packet alloc_packet(usize requested_size) override;
        void free_packet(Packet packet) override;
        klib::RootAwaitable<isize> send_packet(Packet packet, Mac target_mac, u16 proto) override;
    };

    struct Route {
        Interface *interface = nullptr;
        Ipv4 destination, netmask, gateway;
        int metric = klib::NumericLimits<int>::max;
    };

    Route lookup_route(Ipv4 ip);
    void add_route(Route route);

    klib::Awaitable<isize> ipv4_send(void *data, usize length, Ipv4 target_ip, u8 protocol, bool compute_checksum);

    // used for udp/tcp checksum calculation
    struct [[gnu::packed]] Ipv4PseudoHeader {
        Ipv4 src_addr;
        Ipv4 dst_addr;
        u8 zero;
        u8 protocol;
        be16 length;
    };

    // addr points to the udp/tcp header and count includes the header + data
    u16 ipv4_transport_checksum(u16 *addr, usize count, Ipv4 src_addr, Ipv4 dst_addr, u8 protocol);
}
