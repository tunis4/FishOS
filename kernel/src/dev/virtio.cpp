#include <dev/virtio.hpp>
#include <dev/devnode.hpp>
#include <dev/io_service.hpp>
#include <mem/vmm.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <klib/cstdio.hpp>
#include <klib/cstring.hpp>

namespace dev::virtio {
    u64 Device::init(u64 features) {
        pci_device->iterate_capabilities([this] (u8 cap_ptr) {
            u8 pci_cap_type = pci_device->config_read<u8>(cap_ptr);
            if (pci_cap_type != pci::Cap::VENDOR_SPECIFIC)
                return;

            u8 type = pci_device->config_read<u8>(cap_ptr + Cap::TYPE);
            u8 bar = pci_device->config_read<u8>(cap_ptr + Cap::BAR);
            u32 offset = pci_device->config_read<u32>(cap_ptr + Cap::BAR_OFFSET);
            u32 length = pci_device->config_read<u32>(cap_ptr + Cap::BAR_LENGTH);

            Config *config;
            switch (type) {
            case Cap::Type::COMMON_CFG: config = &common_cfg; break;
            case Cap::Type::DEVICE_CFG: config = &device_cfg; break;
            case Cap::Type::NOTIFY_CFG:
                config = &notify_cfg;
                notify_cfg.notify_offset_multiplier = pci_device->config_read<u32>(cap_ptr + Cap::NOTIFY_OFFSET_MULTIPLIER);
                break;
            default: return;
            }

            config->bar = pci_device->get_bar(bar);
            config->bar_offset = offset;
            config->length = length;
        });

        // reset
        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, 0);
        mmio::sync();
        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::ACKNOWLEDGE);
        mmio::sync();
        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::DRIVER);
        mmio::sync();

        // feature negotiation
        common_cfg.write<u32>(CommonCfg::DEVICE_FEATURE_SELECT, 0); mmio::sync();
        u64 offered = common_cfg.read<u32>(CommonCfg::DEVICE_FEATURES); mmio::sync();
        common_cfg.write<u32>(CommonCfg::DEVICE_FEATURE_SELECT, 1); mmio::sync();
        offered |= (u64)common_cfg.read<u32>(CommonCfg::DEVICE_FEATURES) << 32; mmio::sync();

        u64 negotiated = offered & features;

        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURE_SELECT, 0); mmio::sync();
        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURES, negotiated & 0xFFFF'FFFF); mmio::sync();
        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURE_SELECT, 1); mmio::sync();
        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURES, (negotiated >> 32) & 0xFFFF'FFFF); mmio::sync();

        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::FEATURES_OK); mmio::sync();
        if (!(common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) & DeviceStatus::FEATURES_OK)) {
            klib::printf("VirtIO: Device did not accept features (requested: %#lX, negotiated: %#lX)\n", features, negotiated);
            return 0;
        }
        return negotiated;
    }

    void Device::add_queue(Queue *queue, u16 index) {
        common_cfg.write<u16>(CommonCfg::QUEUE_SELECT, index);
        mmio::sync();
        queue->notify_offset = common_cfg.read<u16>(CommonCfg::QUEUE_NOTIFY_OFFSET);
        common_cfg.write<u16>(CommonCfg::QUEUE_SIZE, queue->length);
        common_cfg.write<u16>(CommonCfg::QUEUE_MSIX_VECTOR, queue->msix_vec);
        common_cfg.write<u64>(CommonCfg::QUEUE_DESC, (uptr)queue->descriptor_array - mem::hhdm);
        common_cfg.write<u64>(CommonCfg::QUEUE_AVAIL, (uptr)queue->available_ring - mem::hhdm);
        common_cfg.write<u64>(CommonCfg::QUEUE_USED, (uptr)queue->used_ring - mem::hhdm);
        mmio::sync();
        common_cfg.write<u16>(CommonCfg::QUEUE_ENABLE, 1);
        mmio::sync();
    }

    void Device::notify_queue(Queue *queue, u16 index) {
        notify_cfg.write<u32>(queue->notify_offset * notify_cfg.notify_offset_multiplier, index);
        mmio::sync();
    }

    void Queue::init() {
        length = 128;
        last_seen_used = 0;

        page = pmm::alloc_page();
        uptr addr = page->phy() + mem::hhdm;
        descriptor_array = (Descriptor*)addr;
        available_ring = (AvailableRing*)((uptr)descriptor_array + length * sizeof(Descriptor));
        used_ring = (UsedRing*)((uptr)available_ring + sizeof(AvailableRing) + sizeof(u16) * length);
        memset((void*)addr, 0, 0x1000);

        for (usize i = 0; i < length; i++)
            descriptor_array[i].next = i + 1;
        free_desc_index = 0;
        num_free_descs = length;
    }

    void Queue::deinit() {
        pmm::free_page(page);
    }

    u16 Queue::alloc_descriptor() {
        klib::SpinlockGuard guard(lock);

        u16 index = free_desc_index;
        if (index == length)
            return index;
        free_desc_index = descriptor_array[index].next;
        num_free_descs--;
        return index;
    }

    void Queue::free_descriptor(u16 index) {
        klib::SpinlockGuard guard(lock);

        descriptor_array[index].next = free_desc_index;
        free_desc_index = index;
        num_free_descs++;
    }

    void Queue::submit_descriptor(u16 index) {
        klib::SpinlockGuard guard(lock);

        available_ring->ring[available_ring->index % length] = index;
        mmio::sync();
        available_ring->index += 1;
        mmio::sync();
    }

    void BlockDevice::init() {
        Device::init(0);

        queue.init();
        queue.msix_vec = 0;
        add_queue(&queue, 0);

        requests_page = pmm::alloc_page();
        usize num_requests = 0x1000 / sizeof(Request);
        Request *requests = (Request*)(requests_page->phy() + mem::hhdm);
        first_free_request = &requests[0];
        for (usize i = 0; i < num_requests; i++) {
            Request *request = new (&requests[i]) Request();
            if (i != num_requests - 1)
                request->next = &requests[i + 1];
        }

        pci_device->msix_init();
        if (!pci_device->msix.exists) {
            klib::printf("VirtIO-Block: Device does not support MSI-X\n");
            return;
        }

        u8 vec = cpu::interrupts::allocate_vector();
        cpu::interrupts::set_isr(vec, [] (void *priv, cpu::InterruptState *) {
            ((BlockDevice*)priv)->irq();
        }, this);
        pci_device->msix_add(0, vec, false, false);
        pci_device->msix_set_mask(false);

        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::DRIVER_OK);
        mmio::sync();

        BlockDevNode::register_node_initializer(8, 0, "vda", [&] -> BlockDevNode* { return new BlockDevNode(this); });

        PartitionTable::from_block_interface(this);
    }

    void BlockDevice::deinit() {
        queue.deinit();
    }

    void BlockDevice::irq() {
        defer { cpu::interrupts::eoi(); };

        u16 i;
        for (i = queue.last_seen_used; i != queue.used_ring->index % queue.length; i = (i + 1) % queue.length) {
            u16 desc = queue.used_ring->ring[i].id;
            Request *request = (Request*)(queue.descriptor_array[desc].address + mem::hhdm - offsetof(Request, request_header));
            request->callback.invoke(0); // FIXME: should return actual result
            free_request(request);
            queue.free_descriptor(queue.descriptor_array[queue.descriptor_array[desc].next].next);
            queue.free_descriptor(queue.descriptor_array[desc].next);
            queue.free_descriptor(desc);
        }
        queue.last_seen_used = i;
    }

    BlockDevice::Request* BlockDevice::alloc_request() {
        klib::SpinlockGuard guard(requests_lock);

        if (first_free_request == nullptr)
            return nullptr;
        Request *ret = first_free_request;
        first_free_request = ret->next;
        return ret;
    }

    void BlockDevice::free_request(Request *request) {
        klib::SpinlockGuard guard(requests_lock);

        request->next = first_free_request;
        first_free_request = request;
    }

    klib::RootAwaitable<isize> BlockDevice::read_write_block(usize block, uptr page_phy, Direction direction) {
        isize err = 0;
        auto *descriptors = queue.descriptor_array;

        auto *request = alloc_request();
        defer { if (err < 0) free_request(request); };
        if (!request) return err = -ENOMEM;

        request->request_header.type = direction == WRITE ? RequestHeader::TYPE_OUT : RequestHeader::TYPE_IN;
        request->request_header.sector = block * 8;

        u16 desc1 = queue.alloc_descriptor();
        if (desc1 == queue.length) return err = -ENOMEM;
        defer { if (err < 0) queue.free_descriptor(desc1); };
        descriptors[desc1].address = (uptr)&request->request_header - mem::hhdm;
        descriptors[desc1].length = sizeof(RequestHeader);
        descriptors[desc1].flags = Queue::Descriptor::FLAG_NEXT;

        u16 desc2 = queue.alloc_descriptor();
        if (desc2 == queue.length) return err = -ENOMEM;
        defer { if (err < 0) queue.free_descriptor(desc2); };
        descriptors[desc2].address = page_phy;
        descriptors[desc2].length = 0x1000;
        descriptors[desc2].flags = Queue::Descriptor::FLAG_NEXT | (direction == WRITE ? 0 : Queue::Descriptor::FLAG_DEVICE_WRITE);

        u16 desc3 = queue.alloc_descriptor();
        if (desc3 == queue.length) return err = -ENOMEM;
        defer { if (err < 0) queue.free_descriptor(desc3); };
        descriptors[desc3].address = (uptr)&request->request_status - mem::hhdm;
        descriptors[desc3].length = sizeof(u8);
        descriptors[desc3].flags = Queue::Descriptor::FLAG_DEVICE_WRITE;

        descriptors[desc1].next = desc2;
        descriptors[desc2].next = desc3;

        auto awaitable = klib::RootAwaitable<isize>(&request->callback);
        {
            klib::InterruptLock interrupt_guard;
            queue.submit_descriptor(desc1);
            notify_queue(&queue, 0);
        }
        return awaitable;
    }

    void NetDevice::init() {
        u64 required_features = Feature::VERSION_1 | Feature::NET_MAC;
        u64 negotiated = Device::init(required_features);
        if (negotiated != required_features) {
            klib::printf("VirtIO-Net: Device does not support required features\n");
            return;
        }

        rx_queue.init();
        rx_queue.msix_vec = 0;
        add_queue(&rx_queue, 0);
        tx_queue.init();
        tx_queue.msix_vec = 1;
        add_queue(&tx_queue, 1);

        pci_device->msix_init();
        if (!pci_device->msix.exists) {
            klib::printf("VirtIO-Net: Device does not support MSI-X\n");
            return;
        }

        u8 rx_vec = cpu::interrupts::allocate_vector();
        u8 tx_vec = cpu::interrupts::allocate_vector();
        cpu::interrupts::set_isr(rx_vec, [] (void *priv, cpu::InterruptState *) {
            ((NetDevice*)priv)->rx_irq();
        }, this);
        cpu::interrupts::set_isr(tx_vec, [] (void *priv, cpu::InterruptState *) {
            ((NetDevice*)priv)->tx_irq();
        }, this);
        pci_device->msix_add(0, rx_vec, true, false);
        pci_device->msix_add(1, tx_vec, true, false);
        pci_device->msix_set_mask(false);

        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::DRIVER_OK);
        mmio::sync();

        constexpr usize buffer_size = 0x1000;
        while (true) {
            u16 desc = rx_queue.alloc_descriptor();
            if (desc == rx_queue.length)
                break;

            pmm::Page *page = pmm::alloc_page();
            if (!page) {
                klib::printf("VirtIO-Net: Failed to allocate enough pages for receive queue\n");
                return;
            }

            rx_queue.descriptor_array[desc].address = page->phy();
            rx_queue.descriptor_array[desc].length = buffer_size;
            rx_queue.descriptor_array[desc].flags = Queue::Descriptor::FLAG_DEVICE_WRITE;
            rx_queue.submit_descriptor(desc);
            notify_queue(&rx_queue, 0);
        }

        mtu = 1500;
        for (int i = 0; i < 6; i++)
            mac.address[i] = device_cfg.read<u8>(i);
        klib::printf("VirtIO-Net: MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac.address[0], mac.address[1], mac.address[2], mac.address[3], mac.address[4], mac.address[5]);

        this->ipv4 = net::Ipv4(10, 0, 2, 16);
        net::add_route(net::Route {
            .interface = this,
            .destination = {},
            .netmask = {},
            .gateway = net::Ipv4(10, 0, 2, 2),
            .metric = 100
        });
    }

    net::Packet NetDevice::alloc_packet(usize requested_size) {
        ASSERT(requested_size <= mtu);
        pmm::Page *page = pmm::alloc_page();
        if (page == nullptr)
            return {};
        return net::Packet {
            .addr = mem::hhdm + page->phy(),
            .size = sizeof(PacketHeader) + sizeof(net::EthHeader) + requested_size,
            .offset = sizeof(PacketHeader) + sizeof(net::EthHeader)
        };
    }

    void NetDevice::free_packet(net::Packet packet) {
        pmm::free_page(pmm::find_page(packet.addr - mem::hhdm));
    }

    klib::RootAwaitable<isize> NetDevice::send_packet(net::Packet packet, net::Mac target_mac, u16 proto) {
        u16 desc = tx_queue.alloc_descriptor();
        if (desc == tx_queue.length) return -ENOMEM;

        PacketHeader *vio_header = (PacketHeader*)packet.addr;
        net::EthHeader *eth_header = (net::EthHeader*)(packet.addr + sizeof(PacketHeader));

        memset(vio_header, 0, sizeof(PacketHeader));
        vio_header->header_len = sizeof(PacketHeader) + sizeof(net::EthHeader);

        eth_header->destination = target_mac;
        eth_header->source = this->mac;
        eth_header->type = proto;

        tx_queue.descriptor_array[desc].address = packet.addr - mem::hhdm;
        tx_queue.descriptor_array[desc].length = packet.size;
        tx_queue.descriptor_array[desc].flags = 0;

        {
            klib::InterruptLock interrupt_guard;
            tx_queue.submit_descriptor(desc);
            notify_queue(&tx_queue, 1);
        }

        return &tx_callbacks[desc];
    }

    void NetDevice::rx_irq() {
        defer { cpu::interrupts::eoi(); };

        for (; rx_queue.last_seen_used != rx_queue.used_ring->index % rx_queue.length; rx_queue.last_seen_used = (rx_queue.last_seen_used + 1) % rx_queue.length) {
            u16 desc = rx_queue.used_ring->ring[rx_queue.last_seen_used].id;

            io_service.push([] (void *priv1, void *priv2) -> klib::Awaitable<void> {
                auto *self = (NetDevice*)priv1;
                u16 desc = (usize)priv2;
                uptr packet_addr = self->rx_queue.descriptor_array[desc].address + mem::hhdm;

                co_await self->eth_process((net::EthHeader*)((uptr)packet_addr + sizeof(PacketHeader)));
                self->rx_queue.submit_descriptor(desc);
                self->notify_queue(&self->rx_queue, 0);
            }, this, (void*)(usize)desc);
        }
    }

    void NetDevice::tx_irq() {
        defer { cpu::interrupts::eoi(); };

        u16 i;
        for (i = tx_queue.last_seen_used; i != tx_queue.used_ring->index % tx_queue.length; i = (i + 1) % tx_queue.length) {
            u16 desc = tx_queue.used_ring->ring[i].id;
            tx_callbacks[desc].invoke(0);
            tx_queue.free_descriptor(desc);
        }
        tx_queue.last_seen_used = i;
    }
}
