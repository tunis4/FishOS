#include <dev/device.hpp>
#include <mem/vmm.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstring.hpp>

namespace dev {
    isize BlockInterface::read_write_blocks(uptr buffer, usize block_count, usize first_block, Direction direction) {
        for (usize i = 0; i < block_count; i++) {
            isize phy = vmm::active_pagemap->get_physical_addr(buffer + i * 0x1000);
            if (phy < 0)
                return phy;
            if (isize err = read_write_block(first_block + i, phy, direction); err < 0)
                return err;
        }
        return 0;
    }

    isize BlockInterface::read_write(void *buf, usize count, usize offset, Direction direction) {
        usize done_count = 0;
        uptr buf_virt = (uptr)buf;

        // read/write partial block at the beginning
        if (buf_virt % 0x1000) {
            pmm::Page *tmp_page = pmm::alloc_page();
            defer { pmm::free_page(tmp_page); };
            uptr tmp_phy = tmp_page->pfn * 0x1000;

            if (isize err = read_write_block(offset / 0x1000, tmp_phy, direction); err < 0)
                return err;

            usize copy_size = klib::min(0x1000 - (buf_virt % 0x1000), count);
            if (direction == READ)
                memcpy((void*)buf_virt, (void*)(tmp_phy + vmm::hhdm), copy_size);
            else
                memcpy((void*)(tmp_phy + vmm::hhdm), (void*)buf_virt, copy_size);
            count -= copy_size;
            buf_virt += copy_size;
            done_count += copy_size;
            offset += copy_size;
        }

        if (count == 0)
            return done_count;

        // read/write full blocks in the middle
        ASSERT(buf_virt % 0x1000 == 0);
        usize num_full_blocks = count / 0x1000;
        if (num_full_blocks > 0) {
            usize read_size = num_full_blocks * 0x1000;
            if (isize err = read_write_blocks(buf_virt, num_full_blocks, offset / 0x1000, direction); err < 0)
                return err;
            count -= read_size;
            buf_virt += read_size;
            done_count += read_size;
            offset += read_size;
        }
        if (count == 0)
            return done_count;

        // read/write partial block at the end
        ASSERT(buf_virt % 0x1000 == 0);
        pmm::Page *tmp_page = pmm::alloc_page();
        defer { pmm::free_page(tmp_page); };
        uptr tmp_phy = tmp_page->pfn * 0x1000;

        if (isize err = read_write_block(offset / 0x1000, tmp_phy, direction); err < 0)
            return err;

        usize copy_size = klib::min(count, (usize)0x1000);
        if (direction == READ)
            memcpy((void*)buf_virt, (void*)(tmp_phy + vmm::hhdm), copy_size);
        else
            memcpy((void*)(tmp_phy + vmm::hhdm), (void*)buf_virt, copy_size);
        done_count += copy_size;

        return done_count;
    }
}
