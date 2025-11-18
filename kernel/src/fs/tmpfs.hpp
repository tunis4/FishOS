#pragma once

#include <fs/vfs.hpp>

namespace tmpfs {
    struct Node final : public vfs::VNode {
        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) override;
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override;
        isize mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) override;
        isize truncate(vfs::FileDescription *fd, usize length) override;

    private:
        void grow_to(usize length);
    };

    struct NodeData {
        ino_t inode_num;
        u8 *storage = nullptr;
        usize size = 0, capacity = 0;
    };

    struct Filesystem final : public vfs::Filesystem {
        ino_t last_inode_num;

        Filesystem() { allocate_non_device_dev_id(); }
        ~Filesystem() {}

        void lookup(vfs::Entry *entry) override;
        void create(vfs::Entry *entry, vfs::NodeType new_node_type = vfs::NodeType::HARD_LINK) override;
        void remove(vfs::Entry *entry) override;
        void stat(vfs::VNode *vnode, struct stat *statbuf) override;
        void statfs(struct statfs *buf) override;
        isize readdir(vfs::Entry *entry, void *buf, usize max_size, usize *cursor) override;
    };

    struct Driver final : public vfs::FilesystemDriver {
        Driver() {}
        vfs::Filesystem* mount(vfs::Entry *mount_entry) override;
        void unmount(vfs::Filesystem *fs) override;
    };
}
