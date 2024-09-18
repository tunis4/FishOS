#pragma once

#include <fs/vfs.hpp>

namespace tmpfs {
    struct Node final : public vfs::VNode {
        virtual isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset);
        virtual isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset);
        virtual isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence);
    };

    struct NodeData {
        ino_t inode_num;
        u8 *storage;
        usize size;
    };

    struct Filesystem final : public vfs::Filesystem {
        ino_t last_inode_num;

        Filesystem() {}
        ~Filesystem() {}

        virtual void lookup(vfs::Entry *entry);
        virtual void create(vfs::Entry *entry, vfs::NodeType new_node_type = vfs::NodeType::NONE);
        virtual void remove(vfs::Entry *entry);
        virtual void stat(vfs::Entry *entry, struct stat *statbuf);
        virtual isize readdir(vfs::Entry *entry, void *buf, usize max_size, usize *cursor);
    };

    struct Driver final : public vfs::FilesystemDriver {
        Driver() : vfs::FilesystemDriver("tmpfs") {}
        virtual vfs::Filesystem* mount(vfs::Entry *mount_entry) override;
        virtual void unmount(vfs::Filesystem *fs) override;
    };
}
