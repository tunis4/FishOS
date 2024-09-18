#pragma once

#include <dev/device.hpp>
#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/cstring.hpp>

namespace dev {
    struct NullDevNode final : public CharDevNode {
        NullDevNode() {}
        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) { return 0; }
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) { return count; }
        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) { return 0; }
    };

    struct ZeroDevNode final : public CharDevNode {
        ZeroDevNode() {}
        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) { memset(buf, 0, count); return count; }
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) { return count; }
        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) { return 0; }
    };

    struct FullDevNode final : public CharDevNode {
        FullDevNode() {}
        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) { memset(buf, 0, count); return count; }
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) { return -ENOSPC; }
        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) { return 0; }
    };

    struct MemDevNode final : public SeekableCharDevNode {
        MemDevNode() {}

        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
            memcpy(buf, (void*)(mem::vmm::get_hhdm() + offset), count);
            return count;
        }

        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
            memcpy((void*)(mem::vmm::get_hhdm() + offset), buf, count);
            return count;
        }
    };

    struct PortDevNode final : public SeekableCharDevNode {
        PortDevNode() {}

        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
            if (offset >= 0x10000)
                return 0;
            usize actual_count = offset + count > 0x10000 ? 0x10000 - offset : count;
            for (usize i = 0; i < actual_count; i++)
                ((u8*)buf)[i] = cpu::in<u8>(offset + i);
            return actual_count;
        }

        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
            if (offset >= 0x10000)
                return 0;
            usize actual_count = offset + count > 0x10000 ? 0x10000 - offset : count;
            for (usize i = 0; i < actual_count; i++)
                cpu::out(offset + i, ((u8*)buf)[i]);
            return actual_count;
        }
    };
}
