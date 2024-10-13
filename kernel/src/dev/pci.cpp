#include <dev/pci.hpp>
#include <dev/virtio.hpp>
#include <mem/vmm.hpp>
#include <klib/cstdio.hpp>

namespace dev::pci {
    void BAR::map() {
        ASSERT(is_mmio);
        address = vmm::virt_alloc(length);
        u64 page_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE | (is_prefetchable ? PAGE_WRITE_THROUGH : PAGE_CACHE_DISABLE);
        vmm::kernel_pagemap.map_range(address, length, page_flags, vmm::MappedRange::Type::DIRECT, physical);
    }

    BAR* Device::get_bar(u8 index) {
        BAR *ret = &bars[index];
        if (ret->length)
            return ret;

        u32 offset = 0x10 + index * 4;
        u32 bar = config_read<u32>(offset);
        if (bar & BAR::IO) {
            ret->is_mmio = false;
            ret->is_prefetchable = false;
            ret->address = bar & ~0b11;
            config_write<u32>(offset, -1);
            ret->length = ~(config_read<u32>(offset) & ~0b11) + 1;
            config_write<u32>(offset, bar);
        } else {
            ret->is_mmio = true;
            ret->is_prefetchable = bar & BAR::PREFETCHABLE;
            ret->physical = bar & ~0b1111;
            ret->is_64_bit = (bar & 0b110) == BAR::TYPE_64_BIT;
            if (ret->is_64_bit)
                ret->physical += (u64)config_read<u32>(offset + 4) << 32;

            config_write<u32>(offset, -1);
            ret->length = ~(config_read<u32>(offset) & ~0b1111) + 1;
            config_write<u32>(offset, bar);

            ret->map();
        }

        return ret;
    }

    void Device::set_command(u16 mask, bool value) {
        u16 command = config_read<u16>(Config::COMMAND);
        command = value ? command | mask : command & ~mask;
        config_write<u16>(Config::COMMAND, command);
    }

    void Device::msix_init() {
        if (!msix.exists)
            return;

        u16 msg_control = config_read<u16>(msix.cap_ptr + 2);
        msg_control |= 0xC000;
        config_write<u16>(msix.cap_ptr + 2, msg_control);

        msix.entry_count = (msg_control & 0x3FF) + 1;

        u32 bir = config_read<u32>(msix.cap_ptr + 4);
        msix.table_bar_index = bir & 0b111;
        msix.table_bar_offset = bir & ~0b111;

        bir = config_read<u32>(msix.cap_ptr + 8);
        msix.pending_bar_index = bir & 0b111;
        msix.pending_bar_offset = bir & ~0b111;

        set_command(Command::IRQ_DISABLE, 1);
    }

    void Device::msix_set_mask(bool mask) {
        u16 msg_control = config_read<u16>(msix.cap_ptr + 2);
        msg_control = mask ? msg_control | 0x4000 : msg_control & ~0x4000;
        config_write<u16>(msix.cap_ptr + 2, msg_control);
    }

    static u64 msi_format_message(u32 *data, u8 vector, bool edge_trigger, bool deassert, u8 lapic_id) {
        *data = (edge_trigger ? 0 : (1 << 15)) | (deassert ? 0 : (1 << 14)) | vector;
        return 0xFEE00000 | (lapic_id << 12);
    }

    void Device::msix_add(u32 msix_vec, u8 vec, bool edge_trigger, bool deassert) {
        ASSERT(msix.exists);
        ASSERT(msix_vec < msix.entry_count);

        BAR *bar = get_bar(msix.table_bar_index);
        u32 data;
        u64 address = msi_format_message(&data, vec, edge_trigger, deassert, cpu::get_current_cpu()->lapic_id);

        bar->write<u64>(msix.table_bar_offset, address);
        bar->write<u32>(msix.table_bar_offset + 8, data);
        bar->write<u32>(msix.table_bar_offset + 12, 0);
    }

    void Device::identify() {
        vendor_id = config_read<u16>(Config::VENDOR_ID);
        device_id = config_read<u16>(Config::DEVICE_ID);
        class_code = config_read<u8>(Config::CLASS);
        subclass_code = config_read<u8>(Config::SUBCLASS);
        prog_if = config_read<u8>(Config::PROG_IF);

        const char *name = "Unknown";
        switch (class_code) {
        case 0x1:
            switch (subclass_code) {
            case 0x0: name = "SCSI Bus Controller"; break;
            case 0x6: 
                switch (prog_if) {
                case 0x1: name = "AHCI Controller"; break;
                default:  name = "Unknown SATA Controller";
                } break;
            default:  name = "Unknown Mass Storage Controller";
            } break;
        case 0x2:
            switch (subclass_code) {
            case 0x0: name = "Ethernet Controller"; break;
            default:  name = "Unknown Network Controller";
            } break;
        case 0x3:
            switch (subclass_code) {
            case 0x0: name = "VGA Compatible Controller"; break;
            default:  name = "Unknown Display Controller";
            } break;
        case 0x6:
            switch (subclass_code) {
            case 0x0: name = "Host Bridge"; break;
            case 0x1: name = "ISA Bridge"; break;
            default:  name = "Unknown Bridge";
            } break;
        case 0xC:
            switch (subclass_code) {
            case 0x3: 
                switch (prog_if) {
                case 0x30: name = "XHCI (USB3) Controller"; break;
                default:   name = "Unknown USB Controller";
                } break;
            case 0x5: name = "SMBus Controller"; break;
            default:  name = "Unknown Serial Bus Controller";
            } break;
        }
        klib::printf("PCI: %02x:%02x.%x %s %04x:%04x (C: %x, SC: %x, IF: %x)\n", 
            bus, dev, function, name, vendor_id, device_id, class_code, subclass_code, prog_if);

        iterate_capabilities([this] (u8 cap_ptr) {
            u8 id = config_read<u8>(cap_ptr);
            switch (id) {
            case Cap::MSI:
                klib::printf("\t- Found MSI capability\n");
                break;
            case Cap::MSIX:
                klib::printf("\t- Found MSI-X capability\n");
                msix.exists = true;
                msix.cap_ptr = cap_ptr;
                break;
            // default:
            //     klib::printf("\t- Found unknown capability, id: %#X\n", id);
            }
        });

        if (vendor_id == 0x1af4 && device_id == 0x1001) {
            auto *virtio_blk = new virtio::BlockDevice();
            virtio_blk->pci_device = this;
            virtio_blk->init();
        }
    }

    void check_function(u8 bus, u8 dev, u8 function) {
        Device *device = new Device(bus, dev, function);
        device->identify();
    }

    void check_device(u8 bus, u8 dev) {
        u16 vendor_id = config_read<u16>(bus, dev, 0, Config::VENDOR_ID);
        if (vendor_id == 0xFFFF)
            return;

        check_function(bus, dev, 0);

        u8 header_type = config_read<u8>(bus, dev, 0, Config::HEADER_TYPE);
        if ((header_type & 0x80) != 0)
            for (u8 function = 1; function < 8; function++)
                if (config_read<u16>(bus, dev, function, Config::VENDOR_ID) != 0xFFFF)
                    check_function(bus, dev, function);
    }

    void init() {
        for (u16 bus = 0; bus < 256; bus++)
            for (u8 dev = 0; dev < 32; dev++)
                check_device(bus, dev);
    }
}
