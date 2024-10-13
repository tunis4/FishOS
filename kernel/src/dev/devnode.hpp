#pragma once

#include <dev/device.hpp>
#include <fs/vfs.hpp>
#include <klib/functional.hpp>
#include <fcntl.h>

namespace dev {
    struct DevNode : public vfs::VNode {
        dev_t dev_id;

        DevNode() {}
        virtual ~DevNode() {}
    };

    template<typename T>
    struct DevNodeInitializer {
        dev_t dev_id;
        const char *name;
        klib::Function<T*()> create;

        DevNodeInitializer(dev_t dev_id, const char *name, klib::Function<T*()> &&create)
            : dev_id(dev_id), name(name), create(klib::move(create)) {}
    };

    struct CharDevNode : public DevNode {
        using NodeInitializer = DevNodeInitializer<CharDevNode>;

        static void register_node_initializer(dev_t dev_id, const char *name, klib::Function<CharDevNode*()> &&create);
        static CharDevNode* create_node(dev_t dev_id);

        CharDevNode() {
            type = vfs::NodeType::CHAR_DEVICE;
        }

        virtual ~CharDevNode() {}
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override { return -ESPIPE; }
    };

    struct SeekableCharDevNode : public CharDevNode {
        SeekableCharDevNode() {}
        virtual ~SeekableCharDevNode() {}

        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override {
            switch (whence) {
            case SEEK_SET:
                return offset;
            case SEEK_CUR:
                return position + offset;
            default:
                return -EINVAL;
            }
        }
    };

    struct BlockDevNode : public DevNode {
        using NodeInitializer = DevNodeInitializer<BlockDevNode>;

        static void register_node_initializer(dev_t dev_id, const char *name, klib::Function<BlockDevNode*()> &&create);
        static BlockDevNode* create_node(dev_t dev_id);

        BlockInterface *block_device;

        BlockDevNode(BlockInterface *block_device) : block_device(block_device) {
            type = vfs::NodeType::BLOCK_DEVICE;
        }

        virtual ~BlockDevNode() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
    };

    void init_devices();

    inline constexpr uint major_dev_id(dev_t dev_id) {
        return ((dev_id >> 8) & 0xFFF) | ((uint)(dev_id >> 32) & ~0xFFF);
    }

    inline constexpr uint minor_dev_id(dev_t dev_id) {
        return (dev_id & 0xFF) | ((uint)(dev_id >> 12) & ~0xFF);
    }

    inline constexpr dev_t make_dev_id(uint major_id, uint minor_id) {
        return ((minor_id & 0xFF) | ((major_id & 0xFFF) << 8) | (((usize)(minor_id & ~0xFF)) << 12) | (((usize)(major_id & ~0xFFF)) << 32));
    }
}
