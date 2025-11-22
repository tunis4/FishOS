#pragma once

#include <fs/vfs.hpp>
#include <klib/functional.hpp>

namespace procfs {
    struct Node final : public vfs::VNode {};

    struct InfoNode : public vfs::VNode {
        template<typename F>
        InfoNode(F f, vfs::NodeType type) : print_contents(f) { node_type = type; }
        virtual ~InfoNode() {}

        isize read(vfs::FileDescription *fd, void *buf, usize count, usize offset) override;
        isize seek(vfs::FileDescription *fd, usize position, isize offset, int whence) override;

        void print_char(char c);
        void grow_to(usize length);

    private:
        isize generate_contents();
        isize write_contents(const void *buf, usize count, usize offset);
        void flush_temp_buffer();

        static constexpr usize temp_buffer_size = 1024;
        char *temp_buffer;
        isize temp_written = 0, total_written = 0;

        bool has_contents = false;
        klib::Function<void(InfoNode *self)> print_contents;
    };

    struct NodeData {
        ino_t inode_num;
        u8 *storage;
        usize size, capacity;
    };

    struct Filesystem final : public vfs::Filesystem {
        ino_t last_inode_num;

        Filesystem();
        ~Filesystem() {}

        void lookup(vfs::Entry *entry) override;
        void create(vfs::Entry *entry, vfs::NodeType new_node_type = vfs::NodeType::HARD_LINK) override;
        void remove(vfs::Entry *entry) override;
        void stat(vfs::VNode *vnode, struct stat *statbuf) override;
        void statfs(struct statfs *buf) override;
        isize readdir(vfs::Entry *entry, void *buf, usize max_size, usize *cursor) override;
    };

    struct Driver final : public vfs::FilesystemDriver {
        Driver();
        vfs::Filesystem* mount(vfs::Entry *mount_entry) override;
        void unmount(vfs::Filesystem *fs) override;
    };

    extern Filesystem *fs_global;
    extern const char *kernel_cmdline;

    void create_process_dir(sched::Process *process);
    void create_thread_dir(sched::Thread *thread);
}
