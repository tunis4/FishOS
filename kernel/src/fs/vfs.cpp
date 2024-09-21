#include <fs/vfs.hpp>
#include <fs/tmpfs.hpp>
#include <fs/pipe.hpp>
#include <klib/cstdio.hpp>
#include <klib/algorithm.hpp>
#include <dev/device.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <cpu/syscall/syscall.hpp>
#include <fcntl.h>
#include <dirent.h>

namespace vfs {
    static Entry *null_entry;
    static Entry *root = nullptr;

    Entry* get_root_entry() { return root; }

    void register_filesystem(FilesystemDriver *fs_driver) {
        // fs_drivers().insert(fs_driver->name, fs_driver);
    }

    void init() {
        null_entry = Entry::construct(nullptr, nullptr, "");

        auto *tmpfs_driver = new tmpfs::Driver();
        register_filesystem(tmpfs_driver);

        Filesystem *root_fs = tmpfs_driver->mount(Entry::construct(nullptr, nullptr, "(root)"));
        root_fs->root_entry->add_child(Entry::construct(root_fs->root_entry->vnode, root_fs->root_entry, ".", root_fs->root_entry));
        root_fs->root_entry->add_child(Entry::construct(root_fs->root_entry->vnode, root_fs->root_entry, "..", root_fs->root_entry));
        root = root_fs->root_entry;
    }

    Entry* Entry::construct(VNode *vnode, Entry *parent, const char *name, Entry *redirect) {
        auto name_len = klib::strlen(name);
        Entry *entry = (Entry*)klib::malloc(sizeof(Entry) + name_len + 1);
        entry->vnode = vnode;
        entry->parent = parent;
        entry->redirect = redirect;
        entry->children_list.init();
        entry->sibling_link.init();
        entry->ref_count = 1;
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

    void Entry::create(NodeType new_node_type) {
        Filesystem *fs = parent->vnode->fs_mounted_here ? parent->vnode->fs_mounted_here : parent->vnode->fs;
        fs->create(this, new_node_type);
        if (!parent->find_child(name))
            parent->add_child(this);
        if (vnode && vnode->type == vfs::NodeType::DIRECTORY) {
            add_child(Entry::construct(vnode, this, ".", this));
            add_child(Entry::construct(parent->vnode, this, "..", parent));
        }
    }

    void Entry::remove() {
        vnode->fs->remove(this);
        remove_from_parent();
        delete vnode;
    }

    Entry* lookup(Entry *parent, const char *filename) {
        Entry *result = parent->find_child(filename);
        if (result)
            return result;
        Entry *entry = Entry::construct(nullptr, parent, filename);
        entry->lookup();
        return entry;
    }

    Entry* path_to_entry(const char *path, Entry *starting_point, bool follow_last_symlink) {
        usize path_len = klib::strlen(path);
        if (path_len == 0) return null_entry;
        
        Entry *current_entry = starting_point;
        if (path[0] == '/') {
            current_entry = root;
            path++;
            path_len--;
            if (path[0] == '\0')
                return root;
        } else if (starting_point == nullptr)
            return null_entry;
        
        while (true) {
            if (current_entry->vnode->type != NodeType::DIRECTORY)
                return null_entry;

            bool last = false;
            const char *s = klib::strchr(path, '/');
            if (s == nullptr) {
                s = path + path_len;
                last = true;
            } else if (s == path + path_len - 1) {
                last = true;
            }

            usize entry_name_len = s - path;
            char *entry_name = alloca(char, entry_name_len + 1);
            entry_name[entry_name_len] = 0;
            memcpy(entry_name, path, entry_name_len);
            
            Entry *next_entry = lookup(current_entry, entry_name);
            if (next_entry->vnode == nullptr)
                return next_entry;

            if ((!last || follow_last_symlink) && next_entry->vnode->type == NodeType::SYMLINK) {
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

    void FileDescriptor::close(sched::Process *process, int fdnum) {
        auto *description = get_description();
        description->ref_count--;
        if (description->ref_count == 0) {
            description->vnode->close(description);
            delete description;
        }
        reset();
        process->num_file_descriptors--;
        if (process->first_free_fdnum > fdnum)
            process->first_free_fdnum = fdnum;
    }

    FileDescriptor FileDescriptor::duplicate() {
        FileDescriptor new_descriptor;
        new_descriptor.data = data & ~flags_mask;
        get_description()->ref_count++;
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
                if (description->vnode->type != NodeType::DIRECTORY)
                    return -ENOTDIR;
                *starting_point = description->entry;
            }
        }
        return 0;
    }

    isize syscall_open(int dirfd, const char *path, int flags) {
#if SYSCALL_TRACE
        klib::printf("open(%d, \"%s\", %d)\n", dirfd, path, flags);
#endif
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);

        if (entry->vnode == nullptr) {
            if (!(flags & O_CREAT))
                return -ENOENT;
            if (entry->parent == nullptr)
                return -ENOENT;
            entry->create(NodeType::REGULAR);
        } else {
            if (flags & O_EXCL)
                return -EEXIST;
            if ((flags & O_DIRECTORY) && entry->vnode->type != NodeType::DIRECTORY)
                return -ENOTDIR;
        }

        auto *description = new FileDescription(entry, flags & ~O_CLOEXEC);
        if (auto err = entry->vnode->open(description); err != 0) {
            delete description;
            return err;
        }

        sched::Process *process = cpu::get_current_thread()->process;
        int fd = process->allocate_fdnum();
        process->file_descriptors[fd].init(description, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
        return fd;
    }

    isize syscall_close(int fd) {
#if SYSCALL_TRACE
        klib::printf("close(%d)\n", fd);
#endif
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

    isize syscall_mkdir(int dirfd, const char *path) {
#if SYSCALL_TRACE
        klib::printf("mkdir(%d, \"%s\")\n", dirfd, path);
#endif
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode != nullptr)
            return -EEXIST;
        if (entry->parent == nullptr)
            return -ENOENT;
        entry->create(NodeType::DIRECTORY);
        return 0;
    }

    isize syscall_read(int fd, void *buf, usize count) {
#if SYSCALL_TRACE
        klib::printf("read(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        isize ret = description->vnode->read(description, buf, count, description->cursor);
        if (ret < 0) // error
            return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_pread(int fd, void *buf, usize count, usize offset) {
#if SYSCALL_TRACE
        klib::printf("pread(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->read(description, buf, count, description->cursor);
    }

    isize syscall_write(int fd, const void *buf, usize count) {
#if SYSCALL_TRACE
        klib::printf("write(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        isize ret = description->vnode->write(description, buf, count, description->cursor);
        if (ret < 0)
            return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_pwrite(int fd, const void *buf, usize count, usize offset) {
#if SYSCALL_TRACE
        klib::printf("pwrite(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->write(description, buf, count, offset);
    }

    isize syscall_seek(int fd, isize offset, int whence) {
#if SYSCALL_TRACE
        klib::printf("seek(%d, %ld, %d)\n", fd, offset, whence);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        isize ret = description->vnode->seek(description, description->cursor, offset, whence);
        if (ret >= 0)
            description->cursor = ret;
        return ret;
    }

    isize syscall_getcwd(char *buf, usize size) {
#if SYSCALL_TRACE
        klib::printf("getcwd(%#lX, %ld)\n", (uptr)buf, size);
#endif
        sched::Process *process = cpu::get_current_thread()->process;
        klib::Vector<const char*> parent_names;
        Entry *current = process->cwd;
        while (true) {
            parent_names.push_back(current->name);
            if (current->parent == nullptr || current->parent->parent == nullptr) // exclude root directory to prevent double slash
                break;
            current = current->parent;
        }
        usize buf_index = 0;
        for (int n = parent_names.size() - 1; n >= 0; n--) {
            buf[buf_index++] = '/';
            if (buf_index >= size)
                return -ERANGE;
            const char *name = parent_names[n];
            while (*name) {
                buf[buf_index++] = *name++;
                if (buf_index >= size)
                    return -ERANGE;
            }
        }
        return 0;
    }

    isize syscall_chdir(const char *path) {
#if SYSCALL_TRACE
        klib::printf("chdir(\"%s\")\n", path);
#endif
        sched::Process *process = cpu::get_current_thread()->process;
        Entry *starting_point = nullptr;
        if (path[0] != '/') // path is relative
            starting_point = process->cwd;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->type != NodeType::DIRECTORY)
            return -ENOTDIR;
        process->cwd = entry;
        return 0;
    }

    isize syscall_readdir(int fd, void *buf, usize max_size) {
#if SYSCALL_TRACE
        klib::printf("readdir(%d, %#lX, %ld)\n", fd, (uptr)buf, max_size);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->type != NodeType::DIRECTORY)
            return -ENOTDIR;
        return description->vnode->fs->readdir(description->entry->reduce(), buf, max_size, &description->cursor);
    }
    
    isize syscall_unlink(int dirfd, const char *path, int flags) {
#if SYSCALL_TRACE
        klib::printf("unlink(%d, \"%s\", %d)\n", dirfd, path, flags);
#endif
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->type == NodeType::DIRECTORY && !(flags & AT_REMOVEDIR))
            return -EISDIR;
        entry->remove();
        return 0;
    }

    constexpr usize setfl_mask = O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK;
    isize syscall_fcntl(int fd, int cmd, usize arg) {
#if SYSCALL_TRACE
        klib::printf("fcntl(%d, %d, %ld)\n", fd, cmd, arg);
#endif
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
        default:
            klib::printf("unsupported fcntl cmd %u\n", cmd);
            return -EINVAL;
        }
    }

    // actual dup is just fcntl(oldfd, F_DUPFD, 0); this one is dup3 but it behaves like dup2 if oldfd == newfd (this gets corrected by mlibc)
    isize syscall_dup(int oldfd, int newfd, int flags) {
#if SYSCALL_TRACE
        klib::printf("dup(%d, %d, %d)\n", oldfd, newfd, flags);
#endif
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

    isize syscall_stat(int fd, const char *path, struct stat *statbuf, int flags) {
#if SYSCALL_TRACE
        klib::printf("stat(%d, \"%s\", %#lX, %d)\n", fd, path, (uptr)statbuf, flags);
#endif
        sched::Process *process = cpu::get_current_thread()->process;
        Entry *entry;
        if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
            if (fd == AT_FDCWD) {
                entry = process->cwd;
            } else {
                FileDescription *description = get_file_description(fd);
                if (!description)
                    return -EBADF;
                entry = description->entry;
            }
        } else {
            Entry *starting_point = nullptr;
            if (int err = get_starting_point(&starting_point, fd, path); err < 0)
                return err;
            Entry *result = path_to_entry(path, starting_point, (flags & AT_SYMLINK_NOFOLLOW) == 0);
            if (result->vnode == nullptr)
                return -ENOENT;
            entry = result;
        }

        VNode *vnode = entry->reduce()->vnode;
        memset(statbuf, 0, sizeof(struct stat));
        switch (vnode->type) {
        case NodeType::REGULAR:      statbuf->st_mode = S_IFREG  | 0644; break;
        case NodeType::DIRECTORY:    statbuf->st_mode = S_IFDIR  | 0755; break;
        case NodeType::BLOCK_DEVICE: statbuf->st_mode = S_IFBLK  | 0666; break;
        case NodeType::CHAR_DEVICE:  statbuf->st_mode = S_IFCHR  | 0666; break;
        case NodeType::SYMLINK:      statbuf->st_mode = S_IFLNK  | 0777; break;
        case NodeType::FIFO:         statbuf->st_mode = S_IFIFO  | 0777; break;
        case NodeType::SOCKET:       statbuf->st_mode = S_IFSOCK | 0777; break;
        default: klib::unreachable();
        }

        statbuf->st_dev = dev::make_dev_id(1, 3);
        if (vfs::is_device(entry->vnode->type))
            statbuf->st_rdev = ((dev::DevNode*)entry->vnode)->dev_id;
        statbuf->st_atim = vnode->access_time.to_posix();
        statbuf->st_mtim = vnode->modification_time.to_posix();
        statbuf->st_ctim = vnode->creation_time.to_posix();
        Filesystem *fs = vnode->fs;
        if (fs)
            fs->stat(entry->reduce(), statbuf);
        if (entry == root)
            statbuf->st_blocks = mem::pmm::stats.total_free_pages;
        return 0;
    }

    isize syscall_rename(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
#if SYSCALL_TRACE
        klib::printf("rename(%d, \"%s\", %d, \"%s\", %d)\n", old_dirfd, old_path, new_dirfd, new_path, flags);
#endif
        return -ENOSYS;

        Entry *old_starting_point = nullptr;
        if (int err = get_starting_point(&old_starting_point, old_dirfd, old_path); err < 0)
            return err;

        Entry *old_entry = path_to_entry(old_path, old_starting_point);
        if (old_entry->vnode == nullptr)
            return -ENOENT;

        Entry *new_starting_point = nullptr;
        if (int err = get_starting_point(&new_starting_point, new_dirfd, new_path); err < 0)
            return err;

        Entry *new_entry = path_to_entry(new_path, new_starting_point);
        if (new_entry->vnode == nullptr) {
            return -ENOSYS;
            // if (!(flags & O_CREAT))
            //     return -ENOENT;
            // if (result.target_parent == nullptr)
            //     return -ENOENT;
            // result.target = result.target_parent->fs->create(result.target_parent->fs, result.target_parent, result.basename, Node::Type::FILE);
        } else {
            
            return 0;
        }
    }

    isize syscall_poll(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout, const u64 *sigmask) {
#if SYSCALL_TRACE
        klib::printf("poll(%#lX, %lu, %#lX, %#lX)\n", (uptr)fds, nfds, (uptr)timeout, (uptr)sigmask);
#endif
        isize ret = 0;
        if (nfds > 1024)
            return -EINVAL;

        auto *thread = cpu::get_current_thread();
        u64 saved_signal_mask = thread->signal_mask;
        if (sigmask)
            thread->signal_mask = *sigmask;
        defer {
            if (sigmask)
                thread->signal_mask = saved_signal_mask;
        };

        usize max_events = nfds;
        if (timeout && !timeout->is_zero())
            max_events++;

        sched::Event **events = alloca(sched::Event*, max_events);
        usize allocated_events = 0;

        for (nfds_t i = 0; i < nfds; i++) {
            auto *pollfd = &fds[i];
            pollfd->revents = 0;
            if (pollfd->fd < 0 || pollfd->events == 0)
                continue;

            FileDescription *description = get_file_description(pollfd->fd);
            if (!description) {
                pollfd->revents = POLLNVAL;
                ret++;
            } else {
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
                timer.remaining = *timeout;
                events[allocated_events] = &timer.event;
                allocated_events++;
                timer.arm();
            }
        }

        while (true) {
            for (nfds_t i = 0; i < nfds; i++) {
                auto *pollfd = &fds[i];
                if (pollfd->fd < 0 || pollfd->events == 0)
                    continue;
                FileDescription *description = get_file_description(pollfd->fd);
                isize revents = description->vnode->poll(description, pollfd->events);
                if (revents) {
                    fds[i].revents = revents;
                    ret++;
                }
            }

            if (ret != 0)
                return ret;
            if (!block)
                return 0;
            if (sched::Event::await({events, allocated_events}) == -EINTR)
                return -EINTR;
        }
    }

    isize syscall_readlink(int dirfd, const char *path, void *buf, usize count) {
#if SYSCALL_TRACE
        klib::printf("readlink(%d, \"%s\", %#lX, %lu)\n", dirfd, path, (uptr)buf, count);
#endif
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point, false);
        if (entry->vnode == nullptr)
            return -ENOENT;
        return entry->vnode->read(nullptr, buf, count, 0);
    }

    isize syscall_ioctl(int fd, usize cmd, void *arg) {
#if SYSCALL_TRACE
        klib::printf("ioctl(%d, %#lX, %#lX)\n", fd, cmd, (uptr)arg);
#endif
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->ioctl(description, cmd, arg);
    }

    isize syscall_link(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
#if SYSCALL_TRACE
        klib::printf("link(%d, \"%s\", %d, \"%s\", %d)\n", old_dirfd, old_path, new_dirfd, new_path, flags);
#endif
        Entry *old_starting_point = nullptr;
        if (int err = get_starting_point(&old_starting_point, old_dirfd, old_path); err < 0)
            return err;

        Entry *old_entry = path_to_entry(old_path, old_starting_point);
        if (old_entry->vnode == nullptr)
            return -ENOENT;

        Entry *new_starting_point = nullptr;
        if (int err = get_starting_point(&new_starting_point, new_dirfd, new_path); err < 0)
            return err;

        Entry *new_entry = path_to_entry(new_path, new_starting_point);
        if (new_entry->vnode != nullptr)
            return -EEXIST;
        
        new_entry->vnode = old_entry->vnode;
        new_entry->create();
        return 0;
    }

    isize syscall_symlink(const char *target_path, int dirfd, const char *link_path) {
#if SYSCALL_TRACE
        klib::printf("symlink(\"%s\", %d, \"%s\")\n", target_path, dirfd, link_path);
#endif
        return -ENOSYS;
    }
}
