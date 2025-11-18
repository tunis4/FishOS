#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <klib/timespec.hpp>
#include <klib/cstdio.hpp>
#include <sched/event.hpp>
#include <asm/ioctls.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

namespace sched { struct Process; }

namespace vfs {
    enum class NodeType : u8 {
        HARD_LINK, // not an actual node type
        REGULAR,
        DIRECTORY,
        CHAR_DEVICE,
        BLOCK_DEVICE,
        SYMLINK,
        FIFO,
        SOCKET,
        EPOLL,
        INOTIFY,
        EVENTFD
    };
    static constexpr inline bool is_special(NodeType type) { return type != NodeType::REGULAR && type != NodeType::DIRECTORY; }
    static constexpr inline bool is_device(NodeType type) { return type == NodeType::CHAR_DEVICE && type != NodeType::BLOCK_DEVICE; }

    struct Filesystem;
    struct FilesystemDriver;
    struct FileDescription;

    struct VNode {
        NodeType node_type;
        Filesystem *fs;
        Filesystem *fs_mounted_here;
        void *fs_data = nullptr; // must be allocated with malloc
        sched::Event *event = nullptr;

        klib::TimeSpec creation_time, modification_time, access_time;
        uid_t uid = -1;
        gid_t gid = -1;
        mode_t mode = 0;

        void increment_ref_count() { ref_count++; }
        void decrement_ref_count() { ref_count--; } // if (ref_count == 0) delete this; }

        virtual ~VNode();
        virtual isize     open(FileDescription *fd) { return 0; }
        virtual  void    close(FileDescription *fd) {}
        virtual isize     read(FileDescription *fd, void *buf, usize count, usize offset) { return -ENOTSUP; }
        virtual isize    write(FileDescription *fd, const void *buf, usize count, usize offset) { return -ENOTSUP; }
        virtual isize    readv(FileDescription *fd, const iovec *iovs, int iovc, usize offset);
        virtual isize   writev(FileDescription *fd, const iovec *iovs, int iovc, usize offset);
        virtual isize     seek(FileDescription *fd, usize position, isize offset, int whence) { return -ENOTSUP; }
        virtual isize     poll(FileDescription *fd, isize events) { return POLLIN | POLLOUT; }
        virtual isize     mmap(FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) { return -ENODEV; }
        virtual isize truncate(FileDescription *fd, usize length) { return -EINVAL; }
        virtual isize    ioctl(FileDescription *fd, usize cmd, void *arg) {
            return cmd == TCGETS || cmd == TCSETS || cmd == TIOCSCTTY || cmd == TIOCGWINSZ ? -ENOTTY : -EINVAL;
        }

    private:
        i32 ref_count = 0;
    };

    struct Entry {
        VNode *vnode;
        Entry *parent, *redirect;
        klib::ListHead children_list;
        klib::ListHead sibling_link;
        u32 name_hash;
        char name[];

        static Entry* construct(VNode *vnode, Entry *parent, const char *name, Entry *redirect = nullptr);
        inline void add_child(Entry *entry) { children_list.add(&entry->sibling_link); }
        inline void remove_from_parent() { sibling_link.remove(); }
        inline Entry* reduce() { return redirect ? redirect->reduce() : this; }
        Entry* find_child(const char *child_name);

        void lookup();
        void create(NodeType new_node_type, uid_t uid, gid_t gid, mode_t mode);
        void remove();

        template<klib::Putchar Put>
        void print_path(Put put) {
            if (parent && parent->parent)
                parent->print_path(put);
            klib::printf_template(put, "/%s", name);
        }
    };

    struct Filesystem {
        FilesystemDriver *driver;
        klib::ListHead mounts_link;
        char *mount_source, *mount_target;

        Entry *root_entry;
        dev_t dev_id;

        Filesystem() {}
        virtual ~Filesystem();

        virtual void lookup(Entry *entry) = 0;
        virtual void create(Entry *entry, NodeType new_node_type) = 0;
        virtual void remove(Entry *entry) = 0;
        virtual void stat(VNode *vnode, struct stat *statbuf) = 0;
        virtual void statfs(struct statfs *buf) = 0;
        virtual isize readdir(Entry *entry, void *buf, usize max_size, usize *cursor) = 0;

    protected:
        void allocate_non_device_dev_id();
    };

    struct FilesystemDriver {
        const char *name;

        FilesystemDriver() {}
        virtual ~FilesystemDriver() {}

        virtual Filesystem* mount(Entry *mount_entry) = 0;
        virtual void unmount(Filesystem *fs) = 0;
    };

    extern klib::ListHead mount_list;

    void init();
    FilesystemDriver* get_fs_driver(const char *name);
    Entry* get_root_entry();

    Entry* lookup(Entry *parent, const char *filename);
    Entry* path_to_entry(const char *path, Entry *starting_point = nullptr, bool follow_last_symlink = true);
    Entry* create_entry(Entry *parent, const char *filename, VNode *vnode, uid_t uid, gid_t gid, mode_t mode);

    struct FileDescription {
        VNode *vnode;
        Entry *entry;
        void  *priv;
        usize cursor;
        usize flags;

        FileDescription(Entry *entry, usize flags) : vnode(entry->vnode), entry(entry), cursor(0), flags(flags), ref_count(1) {
            vnode->increment_ref_count();
        }

        FileDescription(VNode *vnode, usize flags) : vnode(vnode), cursor(0), flags(flags), ref_count(1) {
            vnode->increment_ref_count();
        }

        ~FileDescription() {
            vnode->close(this);
            vnode->decrement_ref_count();
        }

        bool can_read() { return ((flags & O_ACCMODE) == O_RDONLY) || ((flags & O_ACCMODE) == O_RDWR); }
        bool can_write() { return ((flags & O_ACCMODE) == O_WRONLY) || ((flags & O_ACCMODE) == O_RDWR); }

        void increment_ref_count() { ref_count++; }
        void decrement_ref_count() { ref_count--; if (ref_count == 0) delete this; }

    private:
        int ref_count;
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
        inline void set_flags(usize flags) { data = (usize)get_description() | (flags & flags_mask); }

        void close(sched::Process *process, int fdnum);
        FileDescriptor duplicate(bool keep_flags = false);
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

    isize syscall_openat(int dirfd, const char *path, int flags, mode_t mode);
    isize syscall_open(const char *path, int flags, mode_t mode);
    isize syscall_creat(const char *path, mode_t mode);
    isize syscall_close(int fd);
    isize syscall_close_range(uint first, uint last, int flags);

    isize syscall_mkdirat(int dirfd, const char *path, mode_t mode);
    isize syscall_mkdir(const char *path, mode_t mode);

    isize syscall_read(int fd, void *buf, usize count);
    isize syscall_write(int fd, const void *buf, usize count);
    isize syscall_pread64(int fd, void *buf, usize count, usize offset);
    isize syscall_pwrite64(int fd, const void *buf, usize count, usize offset);
    isize syscall_readv(int fd, const iovec *iovs, int iovc);
    isize syscall_writev(int fd, const iovec *iovs, int iovc);
    isize syscall_preadv(int fd, const iovec *iovs, int iovc, usize offset_low, usize offset_high);
    isize syscall_pwritev(int fd, const iovec *iovs, int iovc, usize offset_low, usize offset_high);

    isize syscall_lseek(int fd, isize offset, int whence);
    isize syscall_getcwd(char *buf, usize size);

    isize syscall_chdir(const char *path);
    isize syscall_fchdir(int fd);

    isize syscall_getdents64(int fd, void *buf, usize max_size);

    isize syscall_unlinkat(int dirfd, const char *path, int flags);
    isize syscall_unlink(const char *path);
    isize syscall_rmdir(const char *path);

    isize syscall_fcntl(int fd, int cmd, usize arg);

    isize syscall_dup(int oldfd);
    isize syscall_dup2(int oldfd, int newfd);
    isize syscall_dup3(int oldfd, int newfd, int flags);

    isize syscall_newfstatat(int fd, const char *path, struct stat *statbuf, int flags);
    isize syscall_stat(const char *path, struct stat *statbuf);
    isize syscall_fstat(int fd, struct stat *statbuf);
    isize syscall_lstat(const char *path, struct stat *statbuf);

    isize syscall_renameat(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path);
    isize syscall_rename(const char *old_path, const char *new_path);

    isize syscall_poll(struct pollfd *fds, nfds_t nfds, int timeout);
    isize syscall_ppoll(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout, const u64 *sigmask);
    isize syscall_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, timeval *timeout);
    isize syscall_pselect6(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const klib::TimeSpec *timeout, const u64 **sigmask);

    isize syscall_readlinkat(int dirfd, const char *path, void *buf, usize count);
    isize syscall_readlink(const char *path, void *buf, usize count);

    isize syscall_ioctl(int fd, usize cmd, void *arg);

    isize syscall_linkat(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags);
    isize syscall_link(const char *old_path, const char *new_path);

    isize syscall_symlinkat(const char *target_path, int dirfd, const char *link_path);
    isize syscall_symlink(const char *target_path, const char *link_path);

    isize syscall_faccessat(int dirfd, const char *pathname, int mode);
    isize syscall_faccessat2(int dirfd, const char *pathname, int mode, int flags);
    isize syscall_access(const char *pathname, int mode);

    isize syscall_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flags);
    isize syscall_chown(const char *path, uid_t owner, gid_t group);
    isize syscall_fchown(int fd, uid_t owner, gid_t group);
    isize syscall_lchown(const char *path, uid_t owner, gid_t group);

    isize syscall_fchmodat2(int fd, const char *path, mode_t mode, int flags);
    isize syscall_fchmodat(int fd, const char *path, mode_t mode);
    isize syscall_chmod(const char *path, mode_t mode);
    isize syscall_fchmod(int fd, mode_t mode);

    isize syscall_mount(const char *source, const char *target, const char *fs_type, u64 mount_flags, const void *data);

    isize syscall_getrandom(void *buf, usize count, uint flags);

    isize syscall_statfs(const char *path, struct statfs *buf);
    isize syscall_fstatfs(int fd, struct statfs *buf);

    isize syscall_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
    isize syscall_mknod(const char *path, mode_t mode, dev_t dev);

    isize syscall_truncate(const char *path, isize length);
    isize syscall_ftruncate(int fd, isize length);
}
