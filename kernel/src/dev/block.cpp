#include <dev/block.hpp>
#include <mem/vmm.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdio.hpp>

// FIXME: read_write and read_write_blocks are extremely hacky due to pagemap changing when coroutine suspends, need to find a better way to handle that
namespace dev {
    klib::Awaitable<isize> BlockInterface::read_write_blocks(uptr buffer, usize block_count, usize first_block, Direction direction) {
        auto *pagemap = mem::vmm->active_pagemap;
        for (usize i = 0; i < block_count; i++) {
            isize phy = pagemap->get_physical_addr(buffer + i * 0x1000);
            if (phy < 0)
                co_return phy;
            if (isize err = co_await read_write_block(first_block + i, phy, direction); err < 0)
                co_return err;
        }
        co_return 0;
    }

    klib::Awaitable<isize> BlockInterface::read_write(void *buf, usize count, usize offset, Direction direction) {
        auto *pagemap = mem::vmm->active_pagemap;
        usize done_count = 0;
        uptr buf_virt = (uptr)buf;

        // read/write partial block at the beginning
        if (buf_virt % 0x1000) {
            pmm::Page *tmp_page = pmm::alloc_page();
            defer { pmm::free_page(tmp_page); };
            uptr tmp_phy = tmp_page->phy();

            if (isize err = co_await read_write_block(offset / 0x1000, tmp_phy, direction); err < 0)
                co_return err;

            auto *old_pagemap = mem::vmm->active_pagemap;
            pagemap->activate();
            usize copy_size = klib::min(0x1000 - (buf_virt % 0x1000), count);
            if (direction == READ)
                memcpy((void*)buf_virt, (void*)(tmp_phy + mem::hhdm), copy_size);
            else
                memcpy((void*)(tmp_phy + mem::hhdm), (void*)buf_virt, copy_size);
            old_pagemap->activate();
            count -= copy_size;
            buf_virt += copy_size;
            done_count += copy_size;
            offset += copy_size;
        }

        if (count == 0)
            co_return done_count;

        // read/write full blocks in the middle
        ASSERT(buf_virt % 0x1000 == 0);
        usize num_full_blocks = count / 0x1000;
        if (num_full_blocks > 0) {
            usize read_size = num_full_blocks * 0x1000;
            if (isize err = co_await read_write_blocks(buf_virt, num_full_blocks, offset / 0x1000, direction); err < 0)
                co_return err;
            count -= read_size;
            buf_virt += read_size;
            done_count += read_size;
            offset += read_size;
        }
        if (count == 0)
            co_return done_count;

        // read/write partial block at the end
        ASSERT(buf_virt % 0x1000 == 0);
        pmm::Page *tmp_page = pmm::alloc_page();
        defer { pmm::free_page(tmp_page); };
        uptr tmp_phy = tmp_page->phy();

        if (isize err = co_await read_write_block(offset / 0x1000, tmp_phy, direction); err < 0)
            co_return err;

        auto *old_pagemap = mem::vmm->active_pagemap;
        pagemap->activate();
        usize copy_size = klib::min(count, (usize)0x1000);
        if (direction == READ)
            memcpy((void*)buf_virt, (void*)(tmp_phy + mem::hhdm), copy_size);
        else
            memcpy((void*)(tmp_phy + mem::hhdm), (void*)buf_virt, copy_size);
        old_pagemap->activate();
        done_count += copy_size;

        co_return done_count;
    }

    struct [[gnu::packed]] GPTHeader {
        char signature[8];
        u32 revision;
        u32 size;
        u32 crc32;
        u32 reserved;
        u64 header_lba;
        u64 alternate_lba;
        u64 first_usable;
        u64 last_usable;
        u64 guid[2];
        u64 entry_array_lba_start;
        u32 entry_count;
        u32 entry_byte_size;
        u32 entry_array_crc32;
    };

    struct [[gnu::packed]] GPTEntry {
        u64 type_guid[2];
        u64 guid[2];
        u64 start_lba;
        u64 end_lba;
        u64 attributes;
    };

    PartitionTable* PartitionTable::from_block_interface(BlockInterface *interface) {
        pmm::Page *header_page = pmm::alloc_page();
        defer { pmm::free_page(header_page); };

        if (klib::sync(interface->read_write_block(0, header_page->phy(), Direction::READ)) < 0) {
            klib::printf("PartitionTable: Failed to read from block interface\n");
            return nullptr;
        }

        GPTHeader *header = header_page->as<GPTHeader>() + 512;
        if (memcmp(header->signature, "EFI PART", 8) != 0) {
            klib::printf("PartitionTable: GPT signature not found\n");
            return nullptr;
        }

        PartitionTable *table = new PartitionTable(interface);
        return table;
    }
}
