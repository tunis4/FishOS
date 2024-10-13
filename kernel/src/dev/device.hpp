#pragma once

#include <klib/common.hpp>

namespace dev {
    struct BlockInterface {
        virtual isize read_write_block(usize block, uptr page_phy, Direction direction) = 0;

        isize read_write_blocks(uptr buffer, usize block_count, usize first_block, Direction direction);
        isize read_write(void *buf, usize count, usize offset, Direction direction);
    };
}
