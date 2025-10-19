#pragma once

#include <dev/block.hpp>
#include <dev/net.hpp>
#include <dev/pci.hpp>
#include <mem/pmm.hpp>
#include <sched/sched.hpp>
#include <klib/lock.hpp>
#include <klib/coroutine.hpp>

namespace dev::virtio {
    struct Queue {
        struct [[gnu::packed]] Descriptor {
            static constexpr u16 FLAG_NEXT = 1 << 0;
            static constexpr u16 FLAG_DEVICE_WRITE = 1 << 1;

            u64 address;
            u32 length;
            u16 flags;
            u16 next; // if flags & FLAG_NEXT
        };

        struct [[gnu::packed]] AvailableRing {
            u16 flags;
            u16 index;
            u16 ring[];
        };

        struct [[gnu::packed]] Used {
            u32 id;
            u32 length;
        };

        struct [[gnu::packed]] UsedRing {
            u16 flags;
            u16 index;
            Used ring[];
        };

        u16 length;
        u16 free_desc_index;
        u16 num_free_descs;
        u16 last_seen_used;

        klib::Spinlock lock;

        pmm::Page *page; // page for the 3 arrays
        volatile Descriptor *descriptor_array;
        volatile AvailableRing *available_ring;
        volatile UsedRing *used_ring;

        u16 msix_vec;
        u16 notify_offset;

        void init();
        void deinit();

        u16 alloc_descriptor();
        void free_descriptor(u16 index);
        void submit_descriptor(u16 index);
    };

    struct Device {
        pci::Device *pci_device;

        struct Config {
            pci::BAR *bar;
            u32 bar_offset;
            u32 length;

            template<klib::Integral T> inline T read(u32 offset) { return bar->read<T>(bar_offset + offset); }
            template<klib::Integral T> inline void write(u32 offset, T value) { bar->write<T>(bar_offset + offset, value); }
        };

        struct NotifyConfig : public Config {
            u32 notify_offset_multiplier;
        };

        Config common_cfg, device_cfg;
        NotifyConfig notify_cfg;

        u64 init(u64 features);

        void add_queue(Queue *queue, u16 index);
        void notify_queue(Queue *queue, u16 index);
    };

    struct BlockDevice final : public virtio::Device, public dev::BlockInterface {
        void init();
        void deinit();

        klib::RootAwaitable<isize> read_write_block(usize block, uptr page_phy, Direction direction) override;
    
    private:
        struct [[gnu::packed]] RequestHeader {
            static constexpr u32 TYPE_IN = 0;
            static constexpr u32 TYPE_OUT = 1;

            u32 type;
            u32 reserved;
            u64 sector;
        };

        struct Request {
            using Callback = void(std::coroutine_handle<>);

            Request *next;
            klib::RequestCallback<isize> callback;
            volatile RequestHeader request_header;
            volatile u8 request_status;
        };

        pmm::Page *requests_page;
        Request *first_free_request;
        klib::Spinlock requests_lock;

        Queue queue;

        Request* alloc_request();
        void free_request(Request *request);

        void irq();
    };

    struct NetDevice final : public virtio::Device, public net::Interface {
        void init();
        void deinit();

        net::Packet alloc_packet(usize requested_size) override;
        void free_packet(net::Packet packet) override;
        klib::RootAwaitable<isize> send_packet(net::Packet packet, net::Mac target_mac, u16 proto) override;

    private:
        struct [[gnu::packed]] PacketHeader {
            u8 flags;
            u8 gso_type;
            u16 header_len;
            u16 gso_size;
            u16 checksum_start;
            u16 checksum_offset;
            u16 num_buffers;
        };

        Queue rx_queue, tx_queue;

        klib::RequestCallback<isize> tx_callbacks[128] = {};

        void rx_irq();
        void tx_irq();
    };

    namespace Cap {
        namespace Type {
            constexpr u8 COMMON_CFG = 1;
            constexpr u8 NOTIFY_CFG = 2;
            constexpr u8 DEVICE_CFG = 4;
        }

        constexpr u8 TYPE = 3;
        constexpr u8 BAR = 4;
        constexpr u8 BAR_OFFSET = 8;
        constexpr u8 BAR_LENGTH = 12;
        constexpr u8 NOTIFY_OFFSET_MULTIPLIER = 16;
    }

    namespace DeviceStatus {
        constexpr u32 ACKNOWLEDGE = 1;
        constexpr u32 DRIVER = 2;
        constexpr u32 DRIVER_OK = 4;
        constexpr u32 FEATURES_OK = 8;
        constexpr u32 DEVICE_NEEDS_RESET = 64;
        constexpr u32 FAILED = 128;
    }

    namespace CommonCfg {
        constexpr u32 DEVICE_FEATURE_SELECT = 0;
        constexpr u32 DEVICE_FEATURES = 4;
        constexpr u32 DRIVER_FEATURE_SELECT = 8;
        constexpr u32 DRIVER_FEATURES = 12;
        constexpr u32 CONFIG_MSIX_VECTOR = 16;
        constexpr u32 NUM_QUEUES = 18;
        constexpr u32 DEVICE_STATUS = 20;
        constexpr u32 CONFIG_GENERATION = 21;

        constexpr u32 QUEUE_SELECT = 22;
        constexpr u32 QUEUE_SIZE = 24;
        constexpr u32 QUEUE_MSIX_VECTOR = 26;
        constexpr u32 QUEUE_ENABLE = 28;
        constexpr u32 QUEUE_NOTIFY_OFFSET = 30;
        constexpr u32 QUEUE_DESC = 32;
        constexpr u32 QUEUE_AVAIL = 40;
        constexpr u32 QUEUE_USED = 48;
    }

    namespace Feature {
        constexpr u64 VERSION_1 = (u64)1 << 32;
        constexpr u64 NET_MAC = (u64)1 << 5;
    }
}
