#include <fs/vfs.hpp>
#include <fs/tmpfs.hpp>
#include <fs/procfs.hpp>
#include <klib/cstdio.hpp>
#include <klib/algorithm.hpp>
#include <klib/hashtable.hpp>
#include <dev/devnode.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <cpu/syscall/syscall.hpp>
#include <userland/pipe.hpp>
#include <sys/random.h>
#include <fcntl.h>
#include <dirent.h>

#define MODE_TYPE_MASK (S_IFREG | S_IFDIR | S_IFBLK | S_IFCHR | S_IFLNK | S_IFIFO | S_IFSOCK)

namespace vfs {
    static Entry *null_entry;
    static Entry *root = nullptr;
    static klib::HashTable<const char*, FilesystemDriver*> fs_driver_hash_table(8);
    klib::ListHead mount_list;

    Entry* get_root_entry() { return root; }

    static void register_fs_driver(const char *name, FilesystemDriver *fs_driver) {
        fs_driver->name = name;
        fs_driver_hash_table.emplace(fs_driver->name, fs_driver);
    }

    FilesystemDriver* get_fs_driver(const char *name) {
        if (auto **fs_driver = fs_driver_hash_table[name])
            return *fs_driver;
        return nullptr;
    }

    static isize mount(FilesystemDriver *driver, Entry *mount_entry, const char *source, const char *target, u64 mount_flags, const void *data) {
        Filesystem *fs = driver->mount(mount_entry);
        fs->driver = driver;
        fs->mount_source = klib::strdup(source);
        fs->mount_target = klib::strdup(target);
        mount_list.add_before(&fs->mounts_link);
        return 0;
    }

    void init() {
        root = Entry::construct(nullptr, nullptr, "");
        null_entry = Entry::construct(nullptr, nullptr, "");
        mount_list.init();

        register_fs_driver("tmpfs", new tmpfs::Driver());
        register_fs_driver("proc", new procfs::Driver());

        FilesystemDriver *tmpfs = get_fs_driver("tmpfs");
        mount(tmpfs, root, "tmpfs", "/", 0, (const void*)"rw");

        root->add_child(Entry::construct(root->vnode, root, ".", root));
        root->add_child(Entry::construct(root->vnode, root, "..", root));
        root->vnode->uid = 0;
        root->vnode->gid = 0;
        root->vnode->mode = 0755;
    }

    static uint non_device_dev_id = 1;
    void Filesystem::allocate_non_device_dev_id() {
        dev_id = dev::make_dev_id(0, non_device_dev_id++);
    }

    Filesystem::~Filesystem() {
        klib::free(mount_source);
        klib::free(mount_target);
    }

    VNode::~VNode() {
        if (fs_data)
            klib::free(fs_data);
    }

    isize VNode::readv(FileDescription *fd, const iovec *iovs, int iovc, usize offset) {
        usize transferred = 0;
        for (int i = 0; i < iovc; i++) {
            const iovec *iov = &iovs[i];
            if (iov->iov_len == 0) continue;
            isize ret = read(fd, iov->iov_base, iov->iov_len, offset);
            if (ret < 0)
                return transferred ? transferred : ret;
            transferred += ret;
            offset += ret;
            if (ret < (isize)iov->iov_len)
                break;
        }
        return transferred;
    }

    isize VNode::writev(FileDescription *fd, const iovec *iovs, int iovc, usize offset) {
        usize transferred = 0;
        for (int i = 0; i < iovc; i++) {
            const iovec *iov = &iovs[i];
            if (iov->iov_len == 0) continue;
            isize ret = write(fd, iov->iov_base, iov->iov_len, offset);
            if (ret < 0)
                return transferred ? transferred : ret;
            transferred += ret;
            offset += ret;
            if (ret < (isize)iov->iov_len)
                break;
        }
        return transferred;
    }

    Entry* Entry::construct(VNode *vnode, Entry *parent, const char *name, Entry *redirect) {
        auto name_len = klib::strlen(name);
        Entry *entry = (Entry*)klib::malloc(sizeof(Entry) + name_len + 1);
        entry->vnode = vnode;
        entry->parent = parent;
        entry->redirect = redirect;
        entry->children_list.init();
        entry->sibling_link.init();
        entry->name_hash = klib::hash(name);
        memcpy(entry->name, name, name_len + 1);
        return entry;
    }

    Entry* Entry::find_child(const char *child_name) {
        u32 target_hash = klib::hash(child_name);
        Entry *child;
        LIST_FOR_EACH(child, &children_list, sibling_link)
            if (child->name_hash == target_hash && klib::strcmp(child->name, child_name) == 0)
                return child;
        return nullptr;
    }

    void Entry::lookup() {
        Filesystem *fs = parent->vnode->fs_mounted_here ? parent->vnode->fs_mounted_here : parent->vnode->fs;
        fs->lookup(this);
        if (!parent->reduce()->find_child(name))
            parent->add_child(this);
    }

    void Entry::create(NodeType new_node_type, uid_t uid, gid_t gid, mode_t mode) {
        if (new_node_type != NodeType::HARD_LINK) {
            Filesystem *fs = parent->vnode->fs_mounted_here ? parent->vnode->fs_mounted_here : parent->vnode->fs;
            fs->create(this, new_node_type);
        }
        if (!parent->find_child(name))
            parent->add_child(this);
        if (vnode && vnode->node_type == NodeType::DIRECTORY) {
            add_child(Entry::construct(vnode, this, ".", this));
            add_child(Entry::construct(parent->vnode, this, "..", parent));
        }
        if (vnode) {
            vnode->increment_ref_count();

            if (uid != (uid_t)-1) vnode->uid = uid;
            if (gid != (gid_t)-1) vnode->gid = gid;
            if (new_node_type == NodeType::SYMLINK)
                vnode->mode = 0777;
            else
                if (mode != (mode_t)-1) vnode->mode = mode;
        }
    }

    void Entry::remove() {
        vnode->fs->remove(this);
        remove_from_parent();
        vnode->decrement_ref_count();
    }

    Entry* lookup(Entry *parent, const char *filename) {
        Entry *result = parent->find_child(filename);
        if (result)
            return result;
        Entry *entry = Entry::construct(nullptr, parent, filename);
        entry->lookup();
        return entry;
    }

    // FIXME: does not return ENOTDIR errors
    Entry* path_to_entry(const char *path, Entry *starting_point, bool follow_last_symlink) {
        usize path_len = klib::strlen(path);
        if (path_len == 0) return null_entry;

        Entry *current_entry = starting_point ? starting_point->reduce() : nullptr;
        if (path[0] == '/') {
            current_entry = root;
            path++;
            path_len--;
            if (path[0] == '\0')
                return root;
        } else if (starting_point == nullptr)
            return null_entry;

        while (true) {
            if (current_entry->vnode->node_type != NodeType::DIRECTORY)
                return null_entry;

            bool last = false;
            const char *s = klib::strchr(path, '/');
            if (s == nullptr) {
                s = path + path_len;
                last = true;
            } else if (s == path + path_len - 1) {
                last = true;
            } else if (s == path) { // duplicate slash
                path++;
                continue;
            }

            usize entry_name_len = s - path;
            char *entry_name = alloca(char, entry_name_len + 1);
            entry_name[entry_name_len] = 0;
            memcpy(entry_name, path, entry_name_len);

            Entry *next_entry = lookup(current_entry, entry_name);
            if (next_entry->vnode == nullptr)
                return last ? next_entry : null_entry;

            if ((!last || follow_last_symlink) && next_entry->vnode->node_type == NodeType::SYMLINK) {
                char new_path[256] = {};
                isize ret = next_entry->vnode->read(nullptr, new_path, 128, 0);
                if (ret < 0 || ret > 127)
                    return null_entry;
                klib::strcpy(new_path + ret, s);
                return path_to_entry(new_path, current_entry, follow_last_symlink);
            }

            if (last)
                return next_entry;

            path += entry_name_len + 1;
            path_len -= entry_name_len + 1;
            current_entry = next_entry->reduce();
        }
        
        return null_entry;
    }

    Entry* create_entry(Entry *parent, const char *filename, VNode *vnode, uid_t uid, gid_t gid, mode_t mode) {
        auto *entry = lookup(parent, filename);
        ASSERT(entry->vnode == nullptr);
        entry->vnode = vnode;
        entry->create(vnode->node_type, uid, gid, mode);
        return entry;
    }

    void FileDescriptor::close(sched::Process *process, int fdnum) {
        get_description()->decrement_ref_count();
        reset();
        process->num_file_descriptors--;
        if (process->first_free_fdnum > fdnum)
            process->first_free_fdnum = fdnum;
    }

    FileDescriptor FileDescriptor::duplicate(bool keep_flags) {
        FileDescriptor new_descriptor;
        if (keep_flags) new_descriptor.data = data;
        else new_descriptor.data = data & ~flags_mask;
        get_description()->increment_ref_count();
        return new_descriptor;
    }

    void print_file_descriptors(sched::Process *process) {
        klib::printf("\e[36mProcess %d FDs: ", process->pid);
        int i = 0;
        for (auto &descriptor : process->file_descriptors) {
            auto *description = descriptor.get_description();
            if (description) {
                if (description->entry) {
                    klib::printf("%d: %s ", i, description->entry->name);
                    if (descriptor.get_flags())
                        klib::printf("(CLOEXEC) ");
                } else
                    klib::printf("%d: (null) ", i);
            }
            i++;
        }
        klib::printf("\e[0m\n");
    }

    FileDescription* get_file_description(int fd) {
        sched::Process *process = cpu::get_current_thread()->process;
        if (fd >= (int)process->file_descriptors.size() || fd < 0)
            return nullptr;
        return process->file_descriptors[fd].get_description();
    }

    static isize get_starting_point(Entry **starting_point, int dirfd, const char *path) {
        sched::Process *process = cpu::get_current_thread()->process;
        if (path[0] != '/') { // path is relative
            if (dirfd == AT_FDCWD) {
                *starting_point = process->cwd;
            } else {
                FileDescription *description = get_file_description(dirfd);
                if (!description)
                    return -EBADF;
                if (description->vnode->node_type != NodeType::DIRECTORY)
                    return -ENOTDIR;
                *starting_point = description->entry;
            }
        }
        return 0;
    }

    // for the syscalls that accept AT_EMPTY_PATH and AT_SYMLINK_NOFOLLOW
    static isize get_vnode_from_dirfd_path(VNode **vnode, int fd, const char *path, int flags) {
        sched::Process *process = cpu::get_current_thread()->process;
        if ((flags & AT_EMPTY_PATH) && (path == nullptr || path[0] == '\0')) {
            if (fd == AT_FDCWD) {
                *vnode = process->cwd->vnode;
            } else {
                FileDescription *description = get_file_description(fd);
                if (!description)
                    return -EBADF;
                *vnode = description->vnode;
            }
        } else {
            Entry *starting_point = nullptr;
            if (isize err = get_starting_point(&starting_point, fd, path); err < 0)
                return err;
            Entry *result = path_to_entry(path, starting_point, (flags & AT_SYMLINK_NOFOLLOW) == 0);
            if (result->vnode == nullptr)
                return -ENOENT;
            *vnode = result->reduce()->vnode;
        }
        return 0;
    }

    static isize openat_impl(int dirfd, const char *path, int flags, mode_t mode) {
        if (int unsupported_flags = (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK | O_TRUNC | O_NOCTTY | O_NOFOLLOW)))
            klib::printf("openat: unsupported flags %#o\n", unsupported_flags);

        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;
        Entry *starting_point = nullptr;
        if (isize err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point, !(flags & O_NOFOLLOW));

        bool should_truncate = false;

        if (entry->vnode == nullptr) {
            if (!(flags & O_CREAT))
                return -ENOENT;
            if (entry->parent == nullptr)
                return -ENOENT;
            entry->create(NodeType::REGULAR, thread->cred.uids.eid, thread->cred.gids.eid, mode & ~process->umask);
        } else {
            if (entry->vnode->node_type == NodeType::SYMLINK) {
                ASSERT(flags & O_NOFOLLOW);
                return -ELOOP;
            }

            if (flags & O_EXCL)
                return -EEXIST;

            if (entry->vnode->node_type == NodeType::DIRECTORY) {
                if ((flags & O_ACCMODE) == O_RDWR || (flags & O_ACCMODE) == O_WRONLY || (flags & O_CREAT))
                    return -EISDIR;
            } else {
                if (flags & O_DIRECTORY)
                    return -ENOTDIR;

                if ((flags & O_TRUNC) && entry->vnode->node_type == NodeType::REGULAR) {
                    if ((flags & O_ACCMODE) != O_RDWR && (flags & O_ACCMODE) != O_WRONLY) {
                        // FIXME: O_TRUNC with O_RDONLY needs to check write permission
                    }
                    should_truncate = true;
                }
            }
        }

        auto *description = new FileDescription(entry, flags & ~O_CLOEXEC);
        if (auto err = entry->vnode->open(description); err != 0) {
            delete description;
            return err;
        }

        if (should_truncate)
            entry->vnode->truncate(description, 0);

        int fd = process->allocate_fdnum();
        process->file_descriptors[fd].init(description, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
        return fd;
    }

    isize syscall_openat(int dirfd, const char *path, int flags, mode_t mode) {
        log_syscall("openat(%d, \"%s\", %#o, %#o)\n", dirfd, path, flags, mode);
        return openat_impl(dirfd, path, flags, mode);
    }

    isize syscall_open(const char *path, int flags, mode_t mode) {
        log_syscall("open(\"%s\", %#o, %#o)\n", path, flags, mode);
        return openat_impl(AT_FDCWD, path, flags, mode);
    }

    isize syscall_creat(const char *path, mode_t mode) {
        log_syscall("creat(\"%s\", %#o)\n", path, mode);
        return openat_impl(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    }

    isize syscall_close(int fd) {
        log_syscall("close(%d)\n", fd);
        sched::Process *process = cpu::get_current_thread()->process;
        if (fd >= (int)process->file_descriptors.size() || fd < 0)
            return -EBADF;
        FileDescriptor *descriptor = &process->file_descriptors[fd];
        FileDescription *description = descriptor->get_description();
        if (description == nullptr)
            return -EBADF;
        descriptor->close(process, fd);
        return 0;
    }

    isize syscall_close_range(uint first, uint last, int flags) {
        log_syscall("close_range(%u, %u, %d)\n", first, last, flags);
        sched::Process *process = cpu::get_current_thread()->process;

        if (flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE)) return -EINVAL;
        if (first > last) return -EINVAL;
        last = klib::min(last, process->file_descriptors.size() - 1);

        if (flags & CLOSE_RANGE_UNSHARE)
            klib::printf("close_range: unshare not supported\n");

        for (uint fd = first; fd <= last; fd++) {
            FileDescriptor *descriptor = &process->file_descriptors[fd];
            FileDescription *description = descriptor->get_description();
            if (description == nullptr)
                continue;

            if (flags & CLOSE_RANGE_CLOEXEC)
                descriptor->set_flags(FD_CLOEXEC);
            else
                descriptor->close(process, fd);
        }
        return 0;
    }

    static isize mkdirat_impl(int dirfd, const char *path, mode_t mode) {
        Entry *starting_point = nullptr;
        if (isize err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode != nullptr)
            return -EEXIST;
        if (entry->parent == nullptr)
            return -ENOENT;
        sched::Thread *thread = cpu::get_current_thread();
        entry->create(NodeType::DIRECTORY, thread->cred.uids.eid, thread->cred.gids.eid, mode & ~thread->process->umask & 0777);
        return 0;
    }

    isize syscall_mkdirat(int dirfd, const char *path, mode_t mode) {
        log_syscall("mkdirat(%d, \"%s\", %#o)\n", dirfd, path, mode);
        return mkdirat_impl(dirfd, path, mode);
    }

    isize syscall_mkdir(const char *path, mode_t mode) {
        log_syscall("mkdir(\"%s\", %#o)\n", path, mode);
        return mkdirat_impl(AT_FDCWD, path, mode);
    }

    isize syscall_read(int fd, void *buf, usize count) {
        log_syscall("read(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        isize ret = description->vnode->read(description, buf, count, description->cursor);
        if (ret < 0) return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_write(int fd, const void *buf, usize count) {
        log_syscall("write(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        isize ret = description->vnode->write(description, buf, count, description->cursor);
        if (ret < 0) return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_pread64(int fd, void *buf, usize count, usize offset) {
        log_syscall("pread64(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        return description->vnode->read(description, buf, count, offset);
    }

    isize syscall_pwrite64(int fd, const void *buf, usize count, usize offset) {
        log_syscall("pwrite64(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        return description->vnode->write(description, buf, count, offset);
    }

    isize syscall_readv(int fd, const iovec *iovs, int iovc) {
        log_syscall("readv(%d, %#lX, %d)\n", fd, (uptr)iovs, iovc);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        isize ret = description->vnode->readv(description, iovs, iovc, description->cursor);
        if (ret < 0) return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_writev(int fd, const iovec *iovs, int iovc) {
        log_syscall("writev(%d, %#lX, %d)\n", fd, (uptr)iovs, iovc);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        isize ret = description->vnode->writev(description, iovs, iovc, description->cursor);
        if (ret < 0) return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_preadv(int fd, const iovec *iovs, int iovc, usize offset_low, usize offset_high) {
        log_syscall("preadv(%d, %#lX, %d, %#lX, %#lX)\n", fd, (uptr)iovs, iovc, offset_low, offset_high);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        return description->vnode->readv(description, iovs, iovc, (offset_high << 32) | offset_low);
    }

    isize syscall_pwritev(int fd, const iovec *iovs, int iovc, usize offset_low, usize offset_high) {
        log_syscall("pwritev(%d, %#lX, %d, %#lX, %#lX)\n", fd, (uptr)iovs, iovc, offset_low, offset_high);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        return description->vnode->writev(description, iovs, iovc, (offset_high << 32) | offset_low);
    }

    isize syscall_lseek(int fd, isize offset, int whence) {
        log_syscall("lseek(%d, %ld, %d)\n", fd, offset, whence);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        isize ret = description->vnode->seek(description, description->cursor, offset, whence);
        if (ret >= 0)
            description->cursor = ret;
        return ret;
    }

    isize syscall_getcwd(char *buf, usize size) {
        log_syscall("getcwd(%#lX, %ld)\n", (uptr)buf, size);
        sched::Process *process = cpu::get_current_thread()->process;
        usize written = 0;
        process->cwd->print_path([&] (char c) {
            if (written + 1 < size)
                buf[written++] = c;
        });
        if (written + 1 >= size)
            return -ERANGE;
        buf[written] = '\0';
        return 0;
    }

    isize syscall_chdir(const char *path) {
        log_syscall("chdir(\"%s\")\n", path);
        sched::Process *process = cpu::get_current_thread()->process;
        Entry *entry = path_to_entry(path, process->cwd);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->node_type != NodeType::DIRECTORY)
            return -ENOTDIR;
        process->cwd = entry;
        return 0;
    }

    isize syscall_fchdir(int fd) {
        log_syscall("fchdir(%d)\n", fd);
        FileDescription *description = get_file_description(fd);
        if (!description || !description->entry)
            return -EBADF;
        if (description->vnode->node_type != NodeType::DIRECTORY)
            return -ENOTDIR;
        cpu::get_current_thread()->process->cwd = description->entry;
        return 0;
    }

    isize syscall_getdents64(int fd, void *buf, usize max_size) {
        log_syscall("getdents64(%d, %#lX, %ld)\n", fd, (uptr)buf, max_size);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->node_type != NodeType::DIRECTORY)
            return -ENOTDIR;
        return description->vnode->fs->readdir(description->entry->reduce(), buf, max_size, &description->cursor);
    }

    static isize unlinkat_impl(int dirfd, const char *path, int flags) {
        Entry *starting_point = nullptr;
        if (isize err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->node_type == NodeType::DIRECTORY && !(flags & AT_REMOVEDIR))
            return -EISDIR;
        entry->remove();
        return 0;
    }

    isize syscall_unlinkat(int dirfd, const char *path, int flags) {
        log_syscall("unlinkat(%d, \"%s\", %d)\n", dirfd, path, flags);
        return unlinkat_impl(dirfd, path, flags);
    }

    isize syscall_unlink(const char *path) {
        log_syscall("unlink(\"%s\")\n", path);
        return unlinkat_impl(AT_FDCWD, path, 0);
    }

    isize syscall_rmdir(const char *path) {
        log_syscall("rmdir(\"%s\")\n", path);
        return unlinkat_impl(AT_FDCWD, path, AT_REMOVEDIR);
    }

    constexpr usize setfl_mask = O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK;
    static isize fcntl_impl(int fd, int cmd, usize arg) {
        sched::Process *process = cpu::get_current_thread()->process;
        if (fd >= (int)process->file_descriptors.size() || fd < 0)
            return -EBADF;
        FileDescriptor *descriptor = &process->file_descriptors[fd];
        FileDescription *description = descriptor->get_description();
        if (description == nullptr)
            return -EBADF;

        switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int newfd = process->allocate_fdnum(arg);
            FileDescriptor *new_descriptor = &process->file_descriptors[newfd];
            *new_descriptor = descriptor->duplicate();
            if (cmd == F_DUPFD_CLOEXEC)
                new_descriptor->set_flags(FD_CLOEXEC);
            return newfd;
        }
        case F_GETFD:
            return descriptor->get_flags();
        case F_SETFD:
            descriptor->set_flags(arg);
            return 0;
        case F_GETFL:
            return description->flags;
        case F_SETFL:
            description->flags = (description->flags & ~setfl_mask) | (arg & setfl_mask);
            return 0;
        case F_SETLK:
        case F_SETLKW:
            klib::printf("fcntl: F_SETLK and F_SETLKW are stubs\n");
            return 0;
        case F_GETPIPE_SZ:
            return userland::Pipe::capacity;
        default:
            klib::printf("fcntl: unsupported cmd %u\n", cmd);
            return -EINVAL;
        }
    }

    isize syscall_fcntl(int fd, int cmd, usize arg) {
        log_syscall("fcntl(%d, %d, %ld)\n", fd, cmd, arg);
        return fcntl_impl(fd, cmd, arg);
    }

    // note: this behaves as dup2 if oldfd == newfd, which is corrected in syscall_dup3
    static isize dup3_impl(int oldfd, int newfd, int flags) {
        sched::Process *process = cpu::get_current_thread()->process;
        if (oldfd >= (int)process->file_descriptors.size() || oldfd < 0)
            return -EBADF;
        if (newfd > 1023 || newfd < 0)
            return -EBADF;

        FileDescriptor *old_descriptor = &process->file_descriptors[oldfd];
        FileDescription *old_description = old_descriptor->get_description();
        if (old_description == nullptr)
            return -EBADF;
        if (flags & ~FD_CLOEXEC)
            return -EINVAL;
        if (oldfd == newfd)
            return newfd;
        if (newfd >= (int)process->file_descriptors.size())
            for (int emptyfd = process->file_descriptors.size(); emptyfd <= newfd; emptyfd++)
                process->file_descriptors.push_back(FileDescriptor());

        FileDescriptor *new_descriptor = &process->file_descriptors[newfd];
        if (new_descriptor->get_description() != nullptr)
            new_descriptor->close(process, newfd);
        *new_descriptor = old_descriptor->duplicate();
        new_descriptor->set_flags(flags);
        return newfd;
    }

    isize syscall_dup(int oldfd) {
        log_syscall("dup(%d)\n", oldfd);
        return fcntl_impl(oldfd, F_DUPFD, 0);
    }

    isize syscall_dup2(int oldfd, int newfd) {
        log_syscall("dup2(%d, %d)\n", oldfd, newfd);
        return dup3_impl(oldfd, newfd, 0);
    }

    isize syscall_dup3(int oldfd, int newfd, int flags) {
        log_syscall("dup3(%d, %d, %d)\n", oldfd, newfd, flags);
        if (oldfd == newfd)
            return -EINVAL;
        return dup3_impl(oldfd, newfd, flags);
    }

    static isize fstatat_impl(int fd, const char *path, struct stat *statbuf, int flags) {
        VNode *vnode;
        if (isize err = get_vnode_from_dirfd_path(&vnode, fd, path, flags); err < 0)
            return err;

        memset(statbuf, 0, sizeof(struct stat));
        statbuf->st_mode = vnode->mode & ~MODE_TYPE_MASK;
        switch (vnode->node_type) {
        case NodeType::REGULAR:      statbuf->st_mode |= S_IFREG; break;
        case NodeType::DIRECTORY:    statbuf->st_mode |= S_IFDIR; break;
        case NodeType::BLOCK_DEVICE: statbuf->st_mode |= S_IFBLK; break;
        case NodeType::CHAR_DEVICE:  statbuf->st_mode |= S_IFCHR; break;
        case NodeType::SYMLINK:      statbuf->st_mode |= S_IFLNK; break;
        case NodeType::FIFO:         statbuf->st_mode |= S_IFIFO; break;
        case NodeType::SOCKET:       statbuf->st_mode |= S_IFSOCK; break;
        default: klib::printf("stat: unsupported node type %d\n", int(vnode->node_type));
        }

        if (vnode->fs) {
            statbuf->st_dev = vnode->fs->dev_id;
        } else {
            static dev_t pipe_dev_id = dev::make_dev_id(0, non_device_dev_id++);
            static dev_t socket_dev_id = dev::make_dev_id(0, non_device_dev_id++);

            switch (vnode->node_type) {
            case NodeType::FIFO: statbuf->st_dev = pipe_dev_id; break;
            case NodeType::SOCKET: statbuf->st_dev = socket_dev_id; break;
            default:
                klib::printf("stat: unsupported node type\n");
                statbuf->st_dev = dev::make_dev_id(0, 0);
            }
        }

        statbuf->st_uid = vnode->uid;
        statbuf->st_gid = vnode->gid;
        if (is_device(vnode->node_type))
            statbuf->st_rdev = ((dev::DevNode*)vnode)->dev_id;
        statbuf->st_atim = vnode->access_time.to_posix();
        statbuf->st_mtim = vnode->modification_time.to_posix();
        statbuf->st_ctim = vnode->creation_time.to_posix();
        Filesystem *fs = vnode->fs;
        if (fs)
            fs->stat(vnode, statbuf);
        return 0;
    }

    isize syscall_newfstatat(int fd, const char *path, struct stat *statbuf, int flags) {
        log_syscall("newfstatat(%d, \"%s\", %#lX, %d)\n", fd, path, (uptr)statbuf, flags);
        return fstatat_impl(fd, path, statbuf, flags);
    }

    isize syscall_stat(const char *path, struct stat *statbuf) {
        log_syscall("stat(\"%s\", %#lX)\n", path, (uptr)statbuf);
        return fstatat_impl(AT_FDCWD, path, statbuf, 0);
    }

    isize syscall_fstat(int fd, struct stat *statbuf) {
        log_syscall("fstat(%d, %#lX)\n", fd, (uptr)statbuf);
        return fstatat_impl(fd, "", statbuf, AT_EMPTY_PATH);
    }

    isize syscall_lstat(const char *path, struct stat *statbuf) {
        log_syscall("lstat(\"%s\", %#lX)\n", path, (uptr)statbuf);
        return fstatat_impl(AT_FDCWD, path, statbuf, AT_SYMLINK_NOFOLLOW);
    }

    static isize renameat_impl(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path) {
        Entry *old_starting_point = nullptr;
        if (isize err = get_starting_point(&old_starting_point, old_dirfd, old_path); err < 0)
            return err;

        Entry *old_entry = path_to_entry(old_path, old_starting_point);
        if (old_entry->vnode == nullptr)
            return -ENOENT;

        Entry *new_starting_point = nullptr;
        if (isize err = get_starting_point(&new_starting_point, new_dirfd, new_path); err < 0)
            return err;

        Entry *new_entry = path_to_entry(new_path, new_starting_point);
        if (new_entry->vnode != nullptr)
            new_entry->remove();
        new_entry->vnode = old_entry->vnode;
        new_entry->create(NodeType::HARD_LINK, -1, -1, -1);
        old_entry->remove();
        return 0;
    }

    isize syscall_renameat(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path) {
        log_syscall("renameat(%d, \"%s\", %d, \"%s\")\n", old_dirfd, old_path, new_dirfd, new_path);
        return renameat_impl(old_dirfd, old_path, new_dirfd, new_path);
    }

    isize syscall_rename(const char *old_path, const char *new_path) {
        log_syscall("rename(\"%s\", \"%s\")\n", old_path, new_path);
        return renameat_impl(AT_FDCWD, old_path, AT_FDCWD, new_path);
    }

    // FIXME: ppoll should modify timeout
    static isize ppoll_impl(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout, const u64 *sigmask) {
        isize ret = 0;
        if (nfds > 1024)
            return -EINVAL;

        auto *thread = cpu::get_current_thread();
        if (sigmask) {
            ASSERT(thread->has_poll_saved_signal_mask == false);
            thread->has_poll_saved_signal_mask = true;
            thread->poll_saved_signal_mask = thread->signal_mask;
            thread->signal_mask = *sigmask;
            if (thread->has_pending_signals())
                return -EINTR;
        }

        // usize max_events = nfds;
        // if (timeout && !timeout->is_zero())
        //     max_events++;

        sched::Event *events[1024 + 1] = {};
        // sched::Event **events = new sched::Event*[max_events];
        // defer { delete[] events; };
        usize allocated_events = 0;

        for (nfds_t i = 0; i < nfds; i++) {
            auto *pollfd = &fds[i];
            pollfd->revents = 0;
            if (pollfd->fd < 0 || pollfd->events == 0)
                continue;

            FileDescription *description = get_file_description(pollfd->fd);
            if (description) {
                events[allocated_events] = description->vnode->event;
                allocated_events++;
            }
        }

        sched::Timer timer;
        defer { timer.disarm(); };
        bool block = true;
        if (timeout) {
            if (timeout->is_zero()) {
                block = false;
            } else {
                events[allocated_events] = &timer.event;
                allocated_events++;
                timer.arm(*timeout);
            }
        }

        while (true) {
            for (nfds_t i = 0; i < nfds; i++) {
                auto *pollfd = &fds[i];
                if (pollfd->fd < 0 || pollfd->events == 0)
                    continue;
                FileDescription *description = get_file_description(pollfd->fd);
                if (!description) {
                    fds[i].revents = POLLNVAL;
                    ret++;
                } else {
                    isize revents = description->vnode->poll(description, pollfd->events);
                    if (revents) {
                        fds[i].revents = revents;
                        ret++;
                    }
                }
            }

            if (ret != 0)
                return ret;
            if (!block)
                return 0;
            if (sched::Event::wait({events, allocated_events}) == -EINTR)
                return -EINTR;
            if (timer.fired)
                return 0;
        }
    }

    isize syscall_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
        log_syscall("poll(%#lX, %lu, %d)\n", (uptr)fds, nfds, timeout);
        if (timeout >= 0) {
            klib::TimeSpec ts;
            ts.seconds = timeout / 1000;
            ts.nanoseconds = (timeout % 1000) * 1000000;
            return ppoll_impl(fds, nfds, &ts, nullptr);
        }
        return ppoll_impl(fds, nfds, nullptr, nullptr);
    }

    isize syscall_ppoll(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout, const u64 *sigmask) {
        log_syscall("ppoll(%#lX, %lu, %#lX, %#lX)\n", (uptr)fds, nfds, (uptr)timeout, (uptr)sigmask);
        return ppoll_impl(fds, nfds, timeout, sigmask);
    }

    static isize pselect_impl(nfds_t nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const klib::TimeSpec *timeout, const u64 *sigmask) {
        if (nfds > 1024)
            return -EINVAL;
        struct pollfd fds[1024] = {};

        for (usize i = 0; i < nfds; i++) {
            struct pollfd *fd = &fds[i];

            if (readfds && FD_ISSET(i, readfds))
                fd->events |= POLLIN;
            if (writefds && FD_ISSET(i, writefds))
                fd->events |= POLLOUT;
            if (exceptfds && FD_ISSET(i, exceptfds))
                fd->events |= POLLPRI;

            if (!fd->events) {
                fd->fd = -1;
                continue;
            }
            fd->fd = i;
        }

        isize ret = ppoll_impl(fds, nfds, timeout, sigmask);
        if (ret < 0)
            return ret;

        fd_set res_readfds, res_writefds, res_exceptfds;
        FD_ZERO(&res_readfds);
        FD_ZERO(&res_writefds);
        FD_ZERO(&res_exceptfds);

        for (usize i = 0; i < nfds; i++) {
            struct pollfd *fd = &fds[i];

            if (readfds && FD_ISSET(i, readfds) && (fd->revents & (POLLIN | POLLERR | POLLHUP)) != 0)
                FD_SET(i, &res_readfds);
            if (writefds && FD_ISSET(i, writefds) && (fd->revents & (POLLOUT | POLLERR | POLLHUP)) != 0)
                FD_SET(i, &res_writefds);
            if (exceptfds && FD_ISSET(i, exceptfds) && (fd->revents & POLLPRI) != 0) 
                FD_SET(i, &res_exceptfds);
        }

        if (readfds)
            *readfds = res_readfds;
        if (writefds)
            *writefds = res_writefds;
        if (exceptfds)
            *exceptfds = res_exceptfds;
        return ret;
    }

    isize syscall_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, timeval *timeout) {
        log_syscall("select(%d, %#lX, %#lX, %#lX, %#lX)\n", nfds, (uptr)readfds, (uptr)writefds, (uptr)exceptfds, (uptr)timeout);
        if (timeout) {
            klib::TimeSpec ts;
            ts.seconds = timeout->tv_sec;
            ts.nanoseconds = timeout->tv_usec * 1000;
            return pselect_impl(nfds, readfds, writefds, exceptfds, &ts, nullptr);
        }
        return pselect_impl(nfds, readfds, writefds, exceptfds, nullptr, nullptr);
    }

    isize syscall_pselect6(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const klib::TimeSpec *timeout, const u64 **sigmask) {
        log_syscall("pselect6(%d, %#lX, %#lX, %#lX, %#lX, %#lX)\n", nfds, (uptr)readfds, (uptr)writefds, (uptr)exceptfds, (uptr)timeout, (uptr)sigmask);
        return pselect_impl(nfds, readfds, writefds, exceptfds, timeout, sigmask ? *sigmask : nullptr);
    }

    static isize readlinkat_impl(int dirfd, const char *path, void *buf, usize count) {
        Entry *starting_point = nullptr;
        if (isize err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point, false);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->node_type != NodeType::SYMLINK)
            return -EINVAL;
        return entry->vnode->read(nullptr, buf, count, 0);
    }

    isize syscall_readlinkat(int dirfd, const char *path, void *buf, usize count) {
        log_syscall("readlinkat(%d, \"%s\", %#lX, %lu)\n", dirfd, path, (uptr)buf, count);
        return readlinkat_impl(dirfd, path, buf, count);
    }

    isize syscall_readlink(const char *path, void *buf, usize count) {
        log_syscall("readlink(\"%s\", %#lX, %lu)\n", path, (uptr)buf, count);
        return readlinkat_impl(AT_FDCWD, path, buf, count);
    }

    isize syscall_ioctl(int fd, usize cmd, void *arg) {
        log_syscall("ioctl(%d, %#lX, %#lX)\n", fd, cmd, (uptr)arg);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->ioctl(description, cmd, arg);
    }

    static isize linkat_impl(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
        Entry *old_starting_point = nullptr;
        if (isize err = get_starting_point(&old_starting_point, old_dirfd, old_path); err < 0)
            return err;

        Entry *old_entry = path_to_entry(old_path, old_starting_point);
        if (old_entry->vnode == nullptr)
            return -ENOENT;

        Entry *new_starting_point = nullptr;
        if (isize err = get_starting_point(&new_starting_point, new_dirfd, new_path); err < 0)
            return err;

        Entry *new_entry = path_to_entry(new_path, new_starting_point);
        if (new_entry->vnode != nullptr)
            return -EEXIST;
        
        new_entry->vnode = old_entry->vnode;
        new_entry->create(NodeType::HARD_LINK, -1, -1, -1);
        return 0;
    }

    isize syscall_linkat(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
        log_syscall("linkat(%d, \"%s\", %d, \"%s\", %d)\n", old_dirfd, old_path, new_dirfd, new_path, flags);
        return linkat_impl(old_dirfd, old_path, new_dirfd, new_path, flags);
    }

    isize syscall_link(const char *old_path, const char *new_path) {
        log_syscall("link(\"%s\", \"%s\")\n", old_path, new_path);
        return linkat_impl(AT_FDCWD, old_path, AT_FDCWD, new_path, 0);
    }

    static isize symlinkat_impl(const char *target_path, int dirfd, const char *link_path) {
        Entry *link_starting_point = nullptr;
        if (isize err = get_starting_point(&link_starting_point, dirfd, link_path); err < 0)
            return err;
        Entry *entry = path_to_entry(link_path, link_starting_point);
        if (entry->parent == nullptr)
            return -ENOENT;
        if (entry->vnode != nullptr)
            return -EEXIST;
        sched::Thread *thread = cpu::get_current_thread();
        entry->create(NodeType::SYMLINK, thread->cred.uids.eid, thread->cred.gids.eid, 0777);
        entry->vnode->write(nullptr, target_path, klib::strlen(target_path), 0);
        return 0;
    }

    isize syscall_symlinkat(const char *target_path, int dirfd, const char *link_path) {
        log_syscall("symlinkat(\"%s\", %d, \"%s\")\n", target_path, dirfd, link_path);
        return symlinkat_impl(target_path, dirfd, link_path);
    }

    isize syscall_symlink(const char *target_path, const char *link_path) {
        log_syscall("symlink(\"%s\", \"%s\")\n", target_path, link_path);
        return symlinkat_impl(target_path, AT_FDCWD, link_path);
    }

    isize faccessat2_impl(int dirfd, const char *pathname, int mode, int flags) {
        struct stat buf;
        return fstatat_impl(dirfd, pathname, &buf, flags & (AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
    }

    isize syscall_faccessat(int dirfd, const char *pathname, int mode) {
        log_syscall("faccessat(%d, \"%s\", %d)\n", dirfd, pathname, mode);
        return faccessat2_impl(dirfd, pathname, mode, 0);
    }

    isize syscall_faccessat2(int dirfd, const char *pathname, int mode, int flags) {
        log_syscall("faccessat2(%d, \"%s\", %d, %d)\n", dirfd, pathname, mode, flags);
        return faccessat2_impl(dirfd, pathname, mode, flags);
    }

    isize syscall_access(const char *pathname, int mode) {
        log_syscall("access(\"%s\", %d)\n", pathname, mode);
        return faccessat2_impl(AT_FDCWD, pathname, mode, 0);
    }

    static isize fchownat_impl(int fd, const char *path, uid_t owner, gid_t group, int flags) {
        VNode *vnode;
        if (isize err = get_vnode_from_dirfd_path(&vnode, fd, path, flags); err < 0)
            return err;
        if (owner != (uid_t)-1) vnode->uid = owner;
        if (group != (gid_t)-1) vnode->gid = group;
        return 0;
    }

    isize syscall_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flags) {
        log_syscall("fchownat(%d, \"%s\", %u, %u, %d)\n", fd, path, owner, group, flags);
        return fchownat_impl(fd, path, owner, group, flags);
    }

    isize syscall_chown(const char *path, uid_t owner, gid_t group) {
        log_syscall("chown(\"%s\", %u, %u)\n", path, owner, group);
        return fchownat_impl(AT_FDCWD, path, owner, group, 0);
    }

    isize syscall_fchown(int fd, uid_t owner, gid_t group) {
        log_syscall("fchown(%d, %u, %u)\n", fd, owner, group);
        return fchownat_impl(fd, "", owner, group, AT_EMPTY_PATH);
    }

    isize syscall_lchown(const char *path, uid_t owner, gid_t group) {
        log_syscall("lchown(\"%s\", %u, %u)\n", path, owner, group);
        return fchownat_impl(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW);
    }

    static isize fchmodat2_impl(int fd, const char *path, mode_t mode, int flags) {
        VNode *vnode;
        if (isize err = get_vnode_from_dirfd_path(&vnode, fd, path, flags); err < 0)
            return err;
        vnode->mode = mode;
        return 0;
    }

    isize syscall_fchmodat2(int fd, const char *path, mode_t mode, int flags) {
        log_syscall("fchmodat2(%d, \"%s\", %#o, %d)\n", fd, path, mode, flags);
        return fchmodat2_impl(fd, path, mode, flags);
    }

    isize syscall_fchmodat(int fd, const char *path, mode_t mode) {
        log_syscall("fchmodat(%d, \"%s\", %#o)\n", fd, path, mode);
        return fchmodat2_impl(fd, path, mode, 0);
    }

    isize syscall_chmod(const char *path, mode_t mode) {
        log_syscall("chmod(\"%s\", %#o)\n", path, mode);
        return fchmodat2_impl(AT_FDCWD, path, mode, 0);
    }

    isize syscall_fchmod(int fd, mode_t mode) {
        log_syscall("fchmod(%d, %#o)\n", fd, mode);
        return fchmodat2_impl(fd, "", mode, AT_EMPTY_PATH);
    }

    isize syscall_mount(const char *source, const char *target, const char *fs_type, u64 mount_flags, const void *data) {
        log_syscall("mount(\"%s\", \"%s\", \"%s\", %#lX, \"%s\")\n", source, target, fs_type, mount_flags, data ? (const char*)data : "");
        sched::Process *process = cpu::get_current_thread()->process;

        if (klib::strcmp(target, "/") == 0)
            return -ENOSYS;

        FilesystemDriver *driver = get_fs_driver(fs_type);
        if (driver == nullptr) return -ENODEV;

        Entry *target_entry = path_to_entry(target, process->cwd);
        if (target_entry->vnode == nullptr) return -ENOENT;
        if (target_entry->vnode->node_type != NodeType::DIRECTORY) return -ENOTDIR;

        return mount(driver, target_entry, source, target, mount_flags, data);
    }

    static VNode *random_device = nullptr, *urandom_device = nullptr;

    isize syscall_getrandom(void *buf, usize count, uint flags) {
        log_syscall("getrandom(%#lX, %ld, %u)\n", (uptr)buf, count, flags);
        if (!random_device) {
            Entry *entry = path_to_entry("/dev/random");
            if (entry->vnode == nullptr) return -ENOENT;
            random_device = entry->vnode;
        }
        if (!urandom_device) {
            Entry *entry = path_to_entry("/dev/urandom");
            if (entry->vnode == nullptr) return -ENOENT;
            urandom_device = entry->vnode;
        }
        VNode *device = (flags & GRND_RANDOM) ? random_device : urandom_device;
        return device->read(nullptr, buf, count, 0);
    }

    static isize statfs_vnode(VNode *vnode, struct statfs *buf) {
        Filesystem *fs = vnode->fs_mounted_here ? vnode->fs_mounted_here : vnode->fs;
        memset(buf, 0, sizeof(struct statfs));
        if (fs)
            fs->statfs(buf);
        else
            return -ENOSYS; // TODO: need to handle non-fs vnodes
        return 0;
    }

    isize syscall_statfs(const char *path, struct statfs *buf) {
        log_syscall("statfs(\"%s\", %#lX)\n", path, (uptr)buf);
        sched::Process *process = cpu::get_current_thread()->process;
        Entry *entry = path_to_entry(path, process->cwd);
        if (entry->vnode == nullptr) return -ENOENT;
        return statfs_vnode(entry->vnode, buf);
    }

    isize syscall_fstatfs(int fd, struct statfs *buf) {
        log_syscall("fstatfs(%d, %#lX)\n", fd, (uptr)buf);
        FileDescription *description = get_file_description(fd);
        if (!description) return -EBADF;
        return statfs_vnode(description->vnode, buf);
    }

    static isize mknodat_impl(int dirfd, const char *path, mode_t mode, dev_t dev) {
        Entry *starting_point = nullptr;
        if (isize err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode != nullptr)
            return -EEXIST;
        if (entry->parent == nullptr)
            return -ENOENT;
        sched::Thread *thread = cpu::get_current_thread();
        sched::Process *process = thread->process;

        mode_t requested_type = mode & MODE_TYPE_MASK;
        mode &= ~MODE_TYPE_MASK;

        NodeType node_type;
        switch (requested_type) {
        case 0:
        case S_IFREG: {
            node_type = NodeType::REGULAR;
        } break;
        case S_IFCHR: {
            node_type = NodeType::CHAR_DEVICE;
            entry->vnode = dev::CharDevNode::create_node(dev);
            if (entry->vnode == nullptr)
                return -EINVAL;
        } break;
        case S_IFBLK: {
            node_type = NodeType::BLOCK_DEVICE;
            entry->vnode = dev::BlockDevNode::create_node(dev);
            if (entry->vnode == nullptr)
                return -EINVAL;
        } break;
        case S_IFIFO: {
            node_type = NodeType::FIFO;
            entry->vnode = new userland::Pipe();
        } break;
        case S_IFSOCK: {
            node_type = NodeType::SOCKET;
            klib::printf("mknod: creating socket is unimplemented\n");
            return -EINVAL;
        } break;
        default:
            return -EINVAL;
        }

        entry->create(node_type, thread->cred.uids.eid, thread->cred.gids.eid, mode & ~process->umask & 0777);
        return 0;
    }

    isize syscall_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
        log_syscall("mknodat(%d, \"%s\", %#o, %#lX)\n", dirfd, path, mode, dev);
        return mknodat_impl(dirfd, path, mode, dev);
    }

    isize syscall_mknod(const char *path, mode_t mode, dev_t dev) {
        log_syscall("mknod(\"%s\", %#o, %#lX)\n", path, mode, dev);
        return mknodat_impl(AT_FDCWD, path, mode, dev);
    }

    isize syscall_truncate(const char *path, isize length) {
        log_syscall("truncate(\"%s\", %#lX)\n", path, length);
        if (length < 0) return -EINVAL;
        sched::Process *process = cpu::get_current_thread()->process;
        Entry *entry = path_to_entry(path, process->cwd);
        if (entry->vnode == nullptr)
            return -ENOENT;
        return entry->vnode->truncate(nullptr, length);
    }

    isize syscall_ftruncate(int fd, isize length) {
        log_syscall("ftruncate(%d, %#lX)\n", fd, length);
        if (length < 0) return -EINVAL;
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->truncate(description, length);
    }
}
