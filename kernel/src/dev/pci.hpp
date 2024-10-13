#pragma once

#include <cpu/cpu.hpp>

namespace dev::pci {
    constexpr u16 CONFIG_ADDR = 0xCF8;
    constexpr u16 CONFIG_DATA = 0xCFC;

    namespace Config {
        constexpr u32 VENDOR_ID = 0x0;
        constexpr u32 DEVICE_ID = 0x2;
        constexpr u32 COMMAND = 0x4;
        constexpr u32 STATUS = 0x6;
        constexpr u32 CLASS = 0xB;
        constexpr u32 SUBCLASS = 0xA;
        constexpr u32 PROG_IF = 0x9;
        constexpr u32 HEADER_TYPE = 0xE;
        constexpr u32 CAP_PTR = 0x34;
    }

    namespace Command {
        constexpr u16 IO = 1 << 0;
        constexpr u16 MMIO = 1 << 1;
        constexpr u16 BUS_MASTER = 1 << 2;
        constexpr u16 IRQ_DISABLE = 1 << 10;
    }

    namespace Cap {
        constexpr u32 VENDOR_SPECIFIC = 0x9;
        constexpr u32 MSI = 0x5;
        constexpr u32 MSIX = 0x11;
    }

    template<klib::Integral T>
    inline T config_read(u8 bus, u8 device, u8 function, u32 offset) {
        static_assert(false, "T must be one of u8, u16, u32");
    }

    template<>
    inline u32 config_read<u32>(u8 bus, u8 device, u8 function, u32 offset) {
        u32 addr = 0x80000000 | (offset & ~0x3) | (function << 8) | (device << 11) | (bus << 16);
        cpu::out<u32>(CONFIG_ADDR, addr);
        return cpu::in<u32>(CONFIG_DATA);
    }

    template<>
    inline u16 config_read<u16>(u8 bus, u8 device, u8 function, u32 offset) {
        return (config_read<u32>(bus, device, function, offset) >> ((offset % 4) * 8)) & 0xFFFF;
    }

    template<>
    inline u8 config_read<u8>(u8 bus, u8 device, u8 function, u32 offset) {
        return (config_read<u32>(bus, device, function, offset) >> ((offset % 4) * 8)) & 0xFF;
    }

    template<klib::Integral T>
    inline void config_write(u8 bus, u8 device, u8 function, u32 offset, T value) {
        static_assert(false, "T must be one of u8, u16, u32");
    }

    template<>
    inline void config_write<u32>(u8 bus, u8 device, u8 function, u32 offset, u32 value) {
        u32 addr = 0x80000000 | (offset & ~0x3) | (function << 8) | (device << 11) | (bus << 16);
        cpu::out<u32>(CONFIG_ADDR, addr);
        cpu::out<u32>(CONFIG_DATA, value);
    }

    template<>
    inline void config_write<u16>(u8 bus, u8 device, u8 function, u32 offset, u16 value) {
        u32 old = config_read<u32>(bus, device, function, offset);
        int bit_offset = 8 * (offset & 0b11);
        old &= ~(0xFFFF << bit_offset);
        old |= value << bit_offset;
        config_write<u32>(bus, device, function, offset, old);
    }

    template<>
    inline void config_write<u8>(u8 bus, u8 device, u8 function, u32 offset, u8 value) {
        u32 old = config_read<u32>(bus, device, function, offset);
        int bit_offset = 8 * (offset & 0b11);
        old &= ~(0xFF << bit_offset);
        old |= value << bit_offset;
        config_write<u32>(bus, device, function, offset, old);
    }

    struct BAR {
        static constexpr u32 IO = 1 << 0;
        static constexpr u32 TYPE_64_BIT = 1 << 2;
        static constexpr u32 PREFETCHABLE = 1 << 3;

        uptr address;
        uptr physical;
        usize length;
        bool is_mmio;
        bool is_64_bit;
        bool is_prefetchable;

        void map();

        template<klib::Integral T>
        inline T read(u32 offset) const {
            if (is_mmio)
                return mmio::read<T>(address + offset);
            else
                return cpu::in<T>(address + offset);
        }

        template<klib::Integral T>
        inline void write(u32 offset, T value) const {
            if (is_mmio)
                mmio::write<T>(address + offset, value);
            else
                cpu::out<T>(address + offset, value);
        }
    };

    struct MSIX {
        bool exists;
        u32 cap_ptr;
        u32 table_bar_index;
        u32 table_bar_offset;
        u32 pending_bar_index;
        u32 pending_bar_offset;
        u32 entry_count;
    };

    struct Device {
        u8 bus, dev, function;
        u16 vendor_id, device_id;
        u8 class_code, subclass_code, prog_if;
        BAR bars[6];
        MSIX msix;

        template<klib::Integral T> inline T config_read(u32 offset) { return pci::config_read<T>(bus, dev, function, offset); }
        template<klib::Integral T> inline void config_write(u32 offset, T value) { pci::config_write<T>(bus, dev, function, offset, value); }

        Device(u8 bus, u8 device, u8 function) : bus(bus), dev(device), function(function) {}

        void identify();

        template<typename F>
        void iterate_capabilities(F func) {
            u16 status = config_read<u16>(Config::STATUS);
            if (status >> 4 & 1) { // capability bit
                u8 cap_ptr = config_read<u8>(Config::CAP_PTR) & ~0b11;
                while (cap_ptr != 0) {
                    func(cap_ptr);
                    cap_ptr = config_read<u8>(cap_ptr + 1) & ~0b11;
                }
            }
        }

        BAR* get_bar(u8 index);
        void set_command(u16 mask, bool value);

        void msix_init();
        void msix_set_mask(bool mask);
        void msix_add(u32 msix_vec, u8 vec, bool edge_trigger, bool deassert);
    };

    void init();
}
