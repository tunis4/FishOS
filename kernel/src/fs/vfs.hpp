#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <klib/timespec.hpp>
#include <sched/event.hpp>
#include <asm/ioctls.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

namespace sched { struct Process; }

namespace vfs {
    enum class NodeType : u8 {
        NONE,
        REGULAR,
        DIRECTORY,
        CHAR_DEVICE,
        BLOCK_DEVICE,
        SYMLINK,
        FIFO,
        SOCKET
    };
    static constexpr inline bool is_special(NodeType type) { return type != NodeType::REGULAR && type != NodeType::DIRECTORY; }
    static constexpr inline bool is_device(NodeType type) { return type == NodeType::CHAR_DEVICE && type != NodeType::BLOCK_DEVICE; }

    struct Filesystem;
    struct FileDescription;

    struct VNode {
        NodeType type;
        Filesystem *fs;
        Filesystem *fs_mounted_here;
        void *fs_data;
        sched::Event *event = nullptr;
        klib::TimeSpec creation_time, modification_time, access_time;

        virtual ~VNode() {}
        virtual isize  open(FileDescription *fd) { return 0; }
        virtual  void close(FileDescription *fd) {}
        virtual isize  read(FileDescription *fd, void *buf, usize count, usize offset) { return -ENOTSUP; }
        virtual isize write(FileDescription *fd, const void *buf, usize count, usize offset) { return -ENOTSUP; }
        virtual isize  seek(FileDescription *fd, usize position, isize offset, int whence) { return -ENOTSUP; }
        virtual isize  poll(FileDescription *fd, isize events) { return -ENOTSUP; }
        virtual isize  mmap(FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) { return -EACCES; }
        virtual isize ioctl(FileDescription *fd, usize cmd, void *arg) {
            return cmd == TCGETS || cmd == TCSETS || cmd == TIOCSCTTY || cmd == TIOCGWINSZ ? -ENOTTY : -EINVAL;
        }
    };

    struct Entry {
        VNode *vnode;
        Entry *parent, *redirect;
        klib::ListHead children_list;
        klib::ListHead sibling_link;
        u32 ref_count;
        u32 name_hash;
        char name[];

        static Entry* construct(VNode *vnode, Entry *parent, const char *name, Entry *redirect = nullptr);
        inline void add_child(Entry *entry) { children_list.add(&entry->sibling_link); }
        inline void remove_from_parent() { sibling_link.remove(); }
        inline Entry* reduce() { return redirect ? redirect->reduce() : this; }
        Entry* find_child(const char *child_name);

        void lookup();
        void create(NodeType new_node_type = NodeType::NONE);
        void remove();
    };

    struct Filesystem {
        Entry *root_entry;

        Filesystem() {}
        virtual ~Filesystem() {}

        virtual void lookup(Entry *entry) = 0;
        virtual void create(Entry *entry, NodeType new_node_type = NodeType::NONE) = 0;
        virtual void remove(Entry *entry) = 0;
        virtual void stat(Entry *entry, struct stat *statbuf) = 0;
        virtual isize readdir(Entry *entry, void *buf, usize max_size, usize *cursor) = 0;
    };

    struct FilesystemDriver {
        const char *name;

        FilesystemDriver(const char *name) : name(name) {}
        virtual ~FilesystemDriver() {}

        virtual Filesystem* mount(Entry *mount_entry) = 0;
        virtual void unmount(Filesystem *fs) = 0;
    };

    void init();
    void register_filesystem(FilesystemDriver *fs_driver);
    Entry* get_root_entry();

    Entry* lookup(Entry *parent, const char *filename);
    Entry* path_to_entry(const char *path, Entry *starting_point = nullptr, bool follow_last_symlink = true);

    struct FileDescription {
        VNode *vnode;
        Entry *entry;
        void  *priv;
        usize cursor;
        usize flags;
        usize ref_count;

        FileDescription(Entry *entry, usize flags) : vnode(entry->vnode), entry(entry), cursor(0), flags(flags), ref_count(1) {}
        FileDescription(VNode *vnode, usize flags) : vnode(vnode), cursor(0), flags(flags), ref_count(1) {}
        bool can_read() { return ((flags & O_ACCMODE) == O_RDONLY) || ((flags & O_ACCMODE) == O_RDWR); }
        bool can_write() { return ((flags & O_ACCMODE) == O_WRONLY) || ((flags & O_ACCMODE) == O_RDWR); }
    };

    FileDescription* get_file_description(int fd);

    class FileDescriptor {
        static constexpr usize flags_mask = 0b1;
        usize data = 0;

    public:
        inline FileDescriptor() {}
        inline FileDescriptor(FileDescription *description, usize flags) { init(description, flags); }
        inline void init(FileDescription *description, usize flags) { data = (usize)description | flags; }
        inline void reset() { data = 0; }
        inline FileDescription* get_description() { return (FileDescription*)(data & ~flags_mask); }
        inline usize get_flags() { return data & flags_mask; }
        inline void set_flags(usize flags) { data |= flags & flags_mask; }

        void close(sched::Process *process, int fdnum);
        FileDescriptor duplicate();
    };

    struct Dirent {
        u64  d_ino;
        i64  d_off;
        u16  d_reclen;
        u8   d_type;
        char d_name[];

        void d_type_from_node_type(NodeType type) {
            switch (type) {
            case NodeType::REGULAR: d_type = DT_REG; break;
            case NodeType::DIRECTORY: d_type = DT_DIR; break;
            case NodeType::BLOCK_DEVICE: d_type = DT_BLK; break;
            case NodeType::CHAR_DEVICE: d_type = DT_CHR; break;
            case NodeType::SYMLINK: d_type = DT_LNK; break;
            default: d_type = DT_UNKNOWN;
            }
        }
    };

    isize syscall_open(int dirfd, const char *path, int flags);
    isize syscall_mkdir(int dirfd, const char *path);
    isize syscall_close(int fd);
    isize syscall_read(int fd, void *buf, usize count);
    isize syscall_pread(int fd, void *buf, usize count, usize offset);
    isize syscall_write(int fd, const void *buf, usize count);
    isize syscall_pwrite(int fd, const void *buf, usize count, usize offset);
    isize syscall_seek(int fd, isize offset, int whence);
    isize syscall_getcwd(char *buf, usize size);
    isize syscall_chdir(const char *path);
    isize syscall_readdir(int fd, void *buf, usize max_size);
    isize syscall_unlink(int dirfd, const char *path, int flags);
    isize syscall_fcntl(int fd, int cmd, usize arg);
    isize syscall_dup(int oldfd, int newfd, int flags);
    isize syscall_stat(int fd, const char *path, struct stat *statbuf, int flags);
    isize syscall_rename(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags);
    isize syscall_poll(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout_ts, const u64 *sigmask);
    isize syscall_readlink(int dirfd, const char *path, void *buf, usize count);
    isize syscall_ioctl(int fd, usize cmd, void *arg);
    isize syscall_link(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags);
    isize syscall_symlink(const char *target_path, int dirfd, const char *link_path);
}
