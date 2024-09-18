#pragma once

#include <klib/common.hpp>
#include <fs/vfs.hpp>
#include <fcntl.h>

namespace dev {
    struct DevNode : public vfs::VNode {
        dev_t dev_id;

        DevNode() {}
        virtual ~DevNode() {}
    };

    struct CharDevNode : public DevNode {
        struct NodeInitializer {
            dev_t dev_id;
            const char *name;
            CharDevNode* (*create)();
        };

        static void register_node_initializer(dev_t dev_id, const char *name, CharDevNode* (*create)());
        static CharDevNode* create_node(dev_t dev_id);

        CharDevNode() {
            type = vfs::NodeType::CHAR_DEVICE;
        }

        virtual ~CharDevNode() {}
        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) { return -ESPIPE; }
    };

    struct SeekableCharDevNode : public CharDevNode {
        SeekableCharDevNode() {}
        virtual ~SeekableCharDevNode() {}

        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) {
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
        struct NodeInitializer {
            dev_t dev_id;
            const char *name;
            BlockDevNode* (*create)();
        };

        static void register_node_initializer(dev_t dev_id, const char *name, BlockDevNode* (*create)());
        static BlockDevNode* create_node(dev_t dev_id);

        BlockDevNode() {
            type = vfs::NodeType::BLOCK_DEVICE;
        }

        virtual ~BlockDevNode() {}
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
