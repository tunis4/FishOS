#include <fs/vfs.hpp>
#include <fs/tmpfs.hpp>
#include <klib/cstdio.hpp>
#include <klib/algorithm.hpp>
#include <dev/devnode.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <cpu/syscall/syscall.hpp>
#include <userland/pipe.hpp>
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

    VNode::~VNode() {
        if (fs_data)
            klib::free(fs_data);
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

    void Entry::create(NodeType new_node_type) {
        if (new_node_type != NodeType::NONE) {
            Filesystem *fs = parent->vnode->fs_mounted_here ? parent->vnode->fs_mounted_here : parent->vnode->fs;
            fs->create(this, new_node_type);
        }
        if (!parent->find_child(name))
            parent->add_child(this);
        if (vnode && vnode->node_type == NodeType::DIRECTORY) {
            add_child(Entry::construct(vnode, this, ".", this));
            add_child(Entry::construct(parent->vnode, this, "..", parent));
        }
        if (vnode)
            vnode->increment_ref_count();
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

    isize syscall_open(int dirfd, const char *path, int flags) {
        log_syscall("open(%d, \"%s\", %#X)\n", dirfd, path, flags);
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

            if (entry->vnode->node_type == NodeType::DIRECTORY) {
                if ((flags & O_ACCMODE) == O_RDWR || (flags & O_ACCMODE) == O_WRONLY || (flags & O_CREAT))
                    return -EISDIR;
            } else {
                if (flags & O_DIRECTORY)
                    return -ENOTDIR;
            }
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

    isize syscall_mkdir(int dirfd, const char *path) {
        log_syscall("mkdir(%d, \"%s\")\n", dirfd, path);
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
        log_syscall("read(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        isize ret = description->vnode->read(description, buf, count, description->cursor);
        if (ret < 0)
            return ret;
        description->cursor += ret;
        return ret;
    }

    isize syscall_pread(int fd, void *buf, usize count, usize offset) {
        log_syscall("pread(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->read(description, buf, count, offset);
    }

    isize syscall_write(int fd, const void *buf, usize count) {
        log_syscall("write(%d, %#lX, %ld)\n", fd, (uptr)buf, count);
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
        log_syscall("pwrite(%d, %#lX, %ld, %ld)\n", fd, (uptr)buf, count, offset);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->write(description, buf, count, offset);
    }

    isize syscall_readv(int fd, const iovec *iovs, int iovc) {
        log_syscall("readv(%d, %#lX, %d)\n", fd, (uptr)iovs, iovc);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;

        usize transferred = 0;
        for (int i = 0; i < iovc; i++) {
            const iovec *iov = &iovs[i];
            isize ret = description->vnode->read(description, iov->iov_base, iov->iov_len, description->cursor);
            if (ret < 0)
                return transferred ? transferred : ret;
            transferred += ret;
            description->cursor += ret;
            if (ret < (isize)iov->iov_len)
                break;
        }
        return transferred;
    }

    isize syscall_writev(int fd, const iovec *iovs, int iovc) {
        log_syscall("writev(%d, %#lX, %d)\n", fd, (uptr)iovs, iovc);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;

        usize transferred = 0;
        for (int i = 0; i < iovc; i++) {
            const iovec *iov = &iovs[i];
            isize ret = description->vnode->write(description, iov->iov_base, iov->iov_len, description->cursor);
            if (ret < 0)
                return transferred ? transferred : ret;
            transferred += ret;
            description->cursor += ret;
            if (ret < (isize)iov->iov_len)
                break;
        }
        return transferred;
    }

    isize syscall_seek(int fd, isize offset, int whence) {
        log_syscall("seek(%d, %ld, %d)\n", fd, offset, whence);
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
        Entry *starting_point = nullptr;
        if (path[0] != '/') // path is relative
            starting_point = process->cwd;
        Entry *entry = path_to_entry(path, starting_point);
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

    isize syscall_readdir(int fd, void *buf, usize max_size) {
        log_syscall("readdir(%d, %#lX, %ld)\n", fd, (uptr)buf, max_size);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        if (description->vnode->node_type != NodeType::DIRECTORY)
            return -ENOTDIR;
        return description->vnode->fs->readdir(description->entry->reduce(), buf, max_size, &description->cursor);
    }
    
    isize syscall_unlink(int dirfd, const char *path, int flags) {
        log_syscall("unlink(%d, \"%s\", %d)\n", dirfd, path, flags);
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->node_type == NodeType::DIRECTORY && !(flags & AT_REMOVEDIR))
            return -EISDIR;
        entry->remove();
        return 0;
    }

    constexpr usize setfl_mask = O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK;
    isize syscall_fcntl(int fd, int cmd, usize arg) {
        log_syscall("fcntl(%d, %d, %ld)\n", fd, cmd, arg);
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

    // actual dup is just fcntl(oldfd, F_DUPFD, 0); this one is dup3 but it behaves like dup2 if oldfd == newfd (this gets corrected by mlibc)
    isize syscall_dup(int oldfd, int newfd, int flags) {
        log_syscall("dup(%d, %d, %d)\n", oldfd, newfd, flags);
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
        log_syscall("stat(%d, \"%s\", %#lX, %d)\n", fd, path, (uptr)statbuf, flags);
        sched::Process *process = cpu::get_current_thread()->process;
        VNode *vnode;
        if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
            if (fd == AT_FDCWD) {
                vnode = process->cwd->vnode;
            } else {
                FileDescription *description = get_file_description(fd);
                if (!description)
                    return -EBADF;
                vnode = description->vnode;
            }
        } else {
            Entry *starting_point = nullptr;
            if (int err = get_starting_point(&starting_point, fd, path); err < 0)
                return err;
            Entry *result = path_to_entry(path, starting_point, (flags & AT_SYMLINK_NOFOLLOW) == 0);
            if (result->vnode == nullptr)
                return -ENOENT;
            vnode = result->reduce()->vnode;
        }

        memset(statbuf, 0, sizeof(struct stat));
        switch (vnode->node_type) {
        case NodeType::REGULAR:      statbuf->st_mode = S_IFREG  | 0777; break;
        case NodeType::DIRECTORY:    statbuf->st_mode = S_IFDIR  | 0755; break;
        case NodeType::BLOCK_DEVICE: statbuf->st_mode = S_IFBLK  | 0666; break;
        case NodeType::CHAR_DEVICE:  statbuf->st_mode = S_IFCHR  | 0666; break;
        case NodeType::SYMLINK:      statbuf->st_mode = S_IFLNK  | 0777; break;
        case NodeType::FIFO:         statbuf->st_mode = S_IFIFO  | 0777; break;
        case NodeType::SOCKET:       statbuf->st_mode = S_IFSOCK | 0777; break;
        default: klib::unreachable();
        }

        statbuf->st_dev = dev::make_dev_id(1, 3);
        if (is_device(vnode->node_type))
            statbuf->st_rdev = ((dev::DevNode*)vnode)->dev_id;
        statbuf->st_atim = vnode->access_time.to_posix();
        statbuf->st_mtim = vnode->modification_time.to_posix();
        statbuf->st_ctim = vnode->creation_time.to_posix();
        Filesystem *fs = vnode->fs;
        if (fs)
            fs->stat(vnode, statbuf);
        if (vnode == root->vnode)
            statbuf->st_blocks = pmm::stats.total_free_pages;
        return 0;
    }

    isize syscall_rename(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
        log_syscall("rename(%d, \"%s\", %d, \"%s\", %d)\n", old_dirfd, old_path, new_dirfd, new_path, flags);
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
            new_entry->remove();
        new_entry->vnode = old_entry->vnode;
        new_entry->create();
        old_entry->remove();
        return 0;
    }

    isize syscall_poll(struct pollfd *fds, nfds_t nfds, const klib::TimeSpec *timeout, const u64 *sigmask) {
        log_syscall("poll(%#lX, %lu, %#lX, %#lX)\n", (uptr)fds, nfds, (uptr)timeout, (uptr)sigmask);
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

        usize max_events = nfds;
        if (timeout && !timeout->is_zero())
            max_events++;

        sched::Event **events = alloca(sched::Event*, max_events); // totally safe
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

    isize syscall_readlink(int dirfd, const char *path, void *buf, usize count) {
        log_syscall("readlink(%d, \"%s\", %#lX, %lu)\n", dirfd, path, (uptr)buf, count);
        Entry *starting_point = nullptr;
        if (int err = get_starting_point(&starting_point, dirfd, path); err < 0)
            return err;
        Entry *entry = path_to_entry(path, starting_point, false);
        if (entry->vnode == nullptr)
            return -ENOENT;
        if (entry->vnode->node_type != NodeType::SYMLINK)
            return -EINVAL;
        return entry->vnode->read(nullptr, buf, count, 0);
    }

    isize syscall_ioctl(int fd, usize cmd, void *arg) {
        log_syscall("ioctl(%d, %#lX, %#lX)\n", fd, cmd, (uptr)arg);
        FileDescription *description = get_file_description(fd);
        if (!description)
            return -EBADF;
        return description->vnode->ioctl(description, cmd, arg);
    }

    isize syscall_link(int old_dirfd, const char *old_path, int new_dirfd, const char *new_path, int flags) {
        log_syscall("link(%d, \"%s\", %d, \"%s\", %d)\n", old_dirfd, old_path, new_dirfd, new_path, flags);
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
        new_entry->create(NodeType::NONE);
        return 0;
    }

    isize syscall_symlink(const char *target_path, int dirfd, const char *link_path) {
        log_syscall("symlink(\"%s\", %d, \"%s\")\n", target_path, dirfd, link_path);
        Entry *link_starting_point = nullptr;
        if (int err = get_starting_point(&link_starting_point, dirfd, link_path); err < 0)
            return err;
        Entry *entry = path_to_entry(link_path, link_starting_point);
        if (entry->parent == nullptr)
            return -ENOENT;
        if (entry->vnode != nullptr)
            return -EEXIST;
        entry->create(NodeType::SYMLINK);
        entry->vnode->write(nullptr, target_path, klib::strlen(target_path), 0);
        return 0;
    }
}
