#include <dev/virtio.hpp>
#include <dev/devnode.hpp>
#include <mem/vmm.hpp>
#include <cpu/interrupts/interrupts.hpp>
#include <klib/cstdio.hpp>
#include <klib/cstring.hpp>

namespace dev::virtio {
    void Device::init() {
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
    }

    void Device::add_queue(Queue *queue, u16 index) {
        common_cfg.write<u16>(CommonCfg::QUEUE_SELECT, index);
        mmio::sync();
        queue->notify_offset = common_cfg.read<u16>(CommonCfg::QUEUE_NOTIFY_OFFSET);
        common_cfg.write<u16>(CommonCfg::QUEUE_SIZE, queue->length);
        common_cfg.write<u16>(CommonCfg::QUEUE_MSIX_VECTOR, queue->msix_vec);
        common_cfg.write<u64>(CommonCfg::QUEUE_DESC, (uptr)queue->descriptor_array - vmm::hhdm);
        common_cfg.write<u64>(CommonCfg::QUEUE_AVAIL, (uptr)queue->available_ring - vmm::hhdm);
        common_cfg.write<u64>(CommonCfg::QUEUE_USED, (uptr)queue->used_ring - vmm::hhdm);
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
        uptr addr = page->pfn * 0x1000 + vmm::hhdm;
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
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(lock);

        u16 index = free_desc_index;
        if (index == length)
            return index;
        free_desc_index = descriptor_array[index].next;
        num_free_descs--;
        return index;
    }

    void Queue::free_descriptor(u16 index) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(lock);

        descriptor_array[index].next = free_desc_index;
        free_desc_index = index;
        num_free_descs++;
    }

    void Queue::submit_descriptor(u16 index) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(lock);

        available_ring->ring[available_ring->index % length] = index;
        mmio::sync();
        available_ring->index += 1;
        mmio::sync();
    }

    void BlockDevice::init() {
        Device::init();

        common_cfg.write<u32>(CommonCfg::DEVICE_FEATURE_SELECT, 0);
        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURE_SELECT, 0);
        mmio::sync();
        common_cfg.write<u32>(CommonCfg::DRIVER_FEATURES, 0);
        common_cfg.write<u32>(CommonCfg::DEVICE_STATUS, common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) | DeviceStatus::FEATURES_OK);
        mmio::sync();
        if (!(common_cfg.read<u32>(CommonCfg::DEVICE_STATUS) & DeviceStatus::FEATURES_OK)) {
            klib::printf("VirtIO-Block: Device did not accept features\n");
            return;
        }

        queue.init();
        queue.msix_vec = 0;
        add_queue(&queue, 0);

        requests_page = pmm::alloc_page();
        usize num_requests = 0x1000 / sizeof(Request);
        Request *requests = (Request*)(requests_page->pfn * 0x1000 + vmm::hhdm);
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

        BlockDevNode::register_node_initializer(make_dev_id(8, 0), "vda", [&] -> BlockDevNode* { return new BlockDevNode(this); });
    }

    void BlockDevice::deinit() {
        queue.deinit();
    }

    void BlockDevice::irq() {
        defer { cpu::interrupts::eoi(); };

        u16 i;
        for (i = queue.last_seen_used; i != queue.used_ring->index % queue.length; i = (i + 1) % queue.length) {
            u16 desc = queue.used_ring->ring[i].id;
            Request *request = (Request*)(queue.descriptor_array[desc].address + vmm::hhdm - offsetof(Request, request_header));
            request->event.trigger();
            // klib::printf("free request %#lX\n", (uptr)request);
            free_request(request);
            queue.free_descriptor(queue.descriptor_array[queue.descriptor_array[desc].next].next);
            queue.free_descriptor(queue.descriptor_array[desc].next);
            queue.free_descriptor(desc);
        }
        queue.last_seen_used = i;
    }

    BlockDevice::Request* BlockDevice::alloc_request() {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(requests_lock);

        if (first_free_request == nullptr)
            return nullptr;
        Request *ret = first_free_request;
        first_free_request = ret->next;
        return ret;
    }

    void BlockDevice::free_request(Request *request) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(requests_lock);

        request->next = first_free_request;
        first_free_request = request;
    }

    // FIXME: the request and 3 descriptors won't be freed if any of them fail to allocate
    isize BlockDevice::read_write_block(usize block, uptr page_phy, Direction direction) {
        auto *descriptors = queue.descriptor_array;
        auto *request = alloc_request();
        // klib::printf("alloc request %#lX, block %lu, page_phy %#lX\n", (uptr)request, block, page_phy);
        if (!request) return -ENOMEM;

        request->request_header.type = direction == WRITE ? RequestHeader::TYPE_OUT : RequestHeader::TYPE_IN;
        request->request_header.sector = block * 8;

        u16 desc1 = queue.alloc_descriptor();
        if (desc1 == queue.length) return -ENOMEM;
        descriptors[desc1].address = (uptr)&request->request_header - vmm::hhdm;
        descriptors[desc1].length = sizeof(RequestHeader);
        descriptors[desc1].flags = Queue::Descriptor::FLAG_NEXT;

        u16 desc2 = queue.alloc_descriptor();
        if (desc2 == queue.length) return -ENOMEM;
        descriptors[desc2].address = page_phy;
        descriptors[desc2].length = 0x1000;
        descriptors[desc2].flags = Queue::Descriptor::FLAG_NEXT | (direction == WRITE ? 0 : Queue::Descriptor::FLAG_DEVICE_WRITE);

        u16 desc3 = queue.alloc_descriptor();
        if (desc3 == queue.length) return -ENOMEM;
        descriptors[desc3].address = (uptr)&request->request_status - vmm::hhdm;
        descriptors[desc3].length = sizeof(u8);
        descriptors[desc3].flags = Queue::Descriptor::FLAG_DEVICE_WRITE;

        descriptors[desc1].next = desc2;
        descriptors[desc2].next = desc3;

        {
            klib::InterruptLock interrupt_guard;
            queue.submit_descriptor(desc1);
            notify_queue(&queue, 0);
        }

        if (request->event.await() == -EINTR)
            return -EINTR;

        // return request->request_status == 0 ? 0 : -EIO;
        return 0;
    }
}
