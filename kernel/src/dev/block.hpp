#pragma once

#include <klib/common.hpp>
#include <klib/async.hpp>

namespace dev {
    struct BlockInterface {
        virtual klib::RootAwaitable<isize> read_write_block(usize block, uptr page_phy, Direction direction) = 0;

        klib::Awaitable<isize> read_write_blocks(uptr buffer, usize block_count, usize first_block, Direction direction);
        klib::Awaitable<isize> read_write(void *buf, usize count, usize offset, Direction direction);
    };

    struct PartitionTable {
        BlockInterface *interface;

        static PartitionTable* from_block_interface(BlockInterface *interface);
    };

    struct Partition : public BlockInterface {
        PartitionTable *table;
        BlockInterface *interface;
        usize offset_blocks;
        usize length_blocks;
    };
}
