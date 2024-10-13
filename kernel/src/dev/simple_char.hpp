#pragma once

#include <dev/devnode.hpp>
#include <cpu/cpu.hpp>
#include <mem/vmm.hpp>
#include <klib/cstring.hpp>

namespace dev {
    struct NullDevNode final : public CharDevNode {
        NullDevNode() {}
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override { return 0; }
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override { return count; }
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return 0; }
    };

    struct ZeroDevNode final : public CharDevNode {
        ZeroDevNode() {}
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override { memset(buf, 0, count); return count; }
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override { return count; }
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return 0; }
    };

    struct FullDevNode final : public CharDevNode {
        FullDevNode() {}
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override { memset(buf, 0, count); return count; }
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override { return -ENOSPC; }
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return 0; }
    };

    struct MemDevNode final : public SeekableCharDevNode {
        MemDevNode() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override {
            memcpy(buf, (void*)(vmm::hhdm + offset), count);
            return count;
        }

        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override {
            memcpy((void*)(vmm::hhdm + offset), buf, count);
            return count;
        }
    };

    struct PortDevNode final : public SeekableCharDevNode {
        PortDevNode() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override {
            if (offset >= 0x10000)
                return 0;
            usize actual_count = offset + count > 0x10000 ? 0x10000 - offset : count;
            for (usize i = 0; i < actual_count; i++)
                ((u8*)buf)[i] = cpu::in<u8>(offset + i);
            return actual_count;
        }

        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override {
            if (offset >= 0x10000)
                return 0;
            usize actual_count = offset + count > 0x10000 ? 0x10000 - offset : count;
            for (usize i = 0; i < actual_count; i++)
                cpu::out(offset + i, ((u8*)buf)[i]);
            return actual_count;
        }
    };
}
