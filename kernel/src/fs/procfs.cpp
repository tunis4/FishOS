#include <fs/procfs.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdlib.hpp>
#include <klib/cstdio.hpp>
#include <dev/devnode.hpp>
#include <sched/sched.hpp>
#include <sched/time.hpp>
#include <cpu/cpu.hpp>
#include <panic.hpp>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define info_node_put [self] (char c) { self->print_char(c); }

namespace procfs {
    Filesystem *fs_global = nullptr;
    const char *kernel_cmdline = nullptr;

    void Filesystem::lookup(vfs::Entry *entry) {
    }

    void Filesystem::create(vfs::Entry *entry, vfs::NodeType new_node_type) {
        if (!entry->vnode) {
            ASSERT(new_node_type != vfs::NodeType::HARD_LINK);
            entry->vnode = new Node();
            entry->vnode->node_type = new_node_type;
        }
        entry->vnode->fs = this;

        auto realtime_clock = sched::get_clock(CLOCK_REALTIME);
        entry->vnode->creation_time = realtime_clock;
        entry->vnode->modification_time = realtime_clock;
        entry->vnode->access_time = realtime_clock;

        auto *node_data = (NodeData*)klib::calloc(sizeof(NodeData));
        node_data->inode_num = last_inode_num++;

        entry->vnode->fs_data = node_data;
    }
    
    void Filesystem::remove(vfs::Entry *entry) {
    }

    void Filesystem::stat(vfs::VNode *vnode, struct stat *statbuf) {
        NodeData *node_data = (NodeData*)vnode->fs_data;
        statbuf->st_ino = node_data->inode_num;
        if (vnode->node_type == vfs::NodeType::DIRECTORY)
            statbuf->st_size = sizeof(vfs::Entry);
        else
            statbuf->st_size = node_data->size;
        statbuf->st_blksize = 4096;
        statbuf->st_blocks = (statbuf->st_size + 4095) / 4096;
        statbuf->st_nlink = 1;
    }

    void Filesystem::statfs(struct statfs *buf) {
        buf->f_type = PROC_SUPER_MAGIC;
        *(u64*)&buf->f_fsid = 0x1700000000;
        buf->f_namelen = 255;
        buf->f_bsize = 0x1000;
        buf->f_frsize = 0x1000;
        buf->f_flags = ST_NOSUID | ST_NODEV | ST_NOEXEC | ST_RELATIME;
    }

    isize Filesystem::readdir(vfs::Entry *entry, void *buf, usize max_size, usize *cursor) {
        usize i = 0, count = 0;
        vfs::Entry *child;
        LIST_FOR_EACH(child, &entry->children_list, sibling_link) {
            if (i < *cursor || child->vnode == nullptr) {
                i++;
                continue;
            }
            usize name_len = klib::strlen(child->name);
            usize entry_len = klib::align_up(sizeof(vfs::Dirent) + name_len + 1, 8);
            if (count + entry_len > max_size)
                break;
            auto dirent = (vfs::Dirent*)((uptr)buf + count);
            dirent->d_ino = ((NodeData*)child->vnode->fs_data)->inode_num;
            dirent->d_off = i;
            dirent->d_reclen = entry_len;
            dirent->d_type_from_node_type(child->vnode->node_type);
            memcpy(dirent->d_name, child->name, name_len + 1);
            count += entry_len;
            i++;
        }
        *cursor = i;
        return count;
    }

    isize InfoNode::write_contents(const void *buf, usize count, usize offset) {
        NodeData *node_data = (NodeData*)fs_data;
        if (count == 0) [[unlikely]] return 0;
        if (offset + count > node_data->size) {
            node_data->size = offset + count;
            while (node_data->capacity < node_data->size)
                node_data->capacity = klib::max((usize)1, node_data->capacity * 2);
            node_data->storage = (u8*)klib::realloc(node_data->storage, node_data->capacity);
        }
        memcpy(node_data->storage + offset, buf, count);
        return count;
    }

    isize InfoNode::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        if (!has_contents || offset == 0) {
            generate_contents();
            has_contents = true;
        }

        NodeData *node_data = (NodeData*)fs_data;
        if (offset >= node_data->size)
            return 0;
        usize actual_count = offset + count > node_data->size ? node_data->size - offset : count;
        memcpy(buf, node_data->storage + offset, actual_count);
        return actual_count;
    }

    isize InfoNode::seek(vfs::FileDescription *fd, usize position, isize offset, int whence) {
        NodeData *node_data = (NodeData*)fs_data;
        switch (whence) {
        case SEEK_SET:
            return offset;
        case SEEK_CUR:
            return position + offset;
        case SEEK_END:
            return node_data->size + offset;
        case SEEK_DATA:
            if ((usize)offset >= node_data->size)
                return -ENXIO;
            return offset;
        case SEEK_HOLE:
            if ((usize)offset >= node_data->size)
                return -ENXIO;
            return node_data->size;
        default:
            return -EINVAL;
        }
    }

    void InfoNode::flush_temp_buffer() {
        write_contents(temp_buffer, temp_written, total_written);
        total_written += temp_written;
        temp_written = 0;
    }

    void InfoNode::print_char(char c) {
        temp_buffer[temp_written] = c;
        temp_written++;
        if (temp_written == temp_buffer_size)
            flush_temp_buffer();
    }

    isize InfoNode::generate_contents() {
        char temp[temp_buffer_size];
        temp_buffer = temp;
        temp_written = 0;
        total_written = 0;
        print_contents(this);
        flush_temp_buffer();
        temp_buffer = nullptr;
        return total_written;
    }

    Driver::Driver() {
        fs_global = new Filesystem();
    }

    vfs::Filesystem* Driver::mount(vfs::Entry *mount_entry) {
        mount_entry->redirect = fs_global->root_entry;
        return fs_global;
    }

    void Driver::unmount(vfs::Filesystem *fs) {
    }

    Filesystem::Filesystem() {
        allocate_non_device_dev_id();
        last_inode_num = 2;

        root_entry = vfs::Entry::construct(nullptr, nullptr, "");
        create(root_entry, vfs::NodeType::DIRECTORY);
        root_entry->vnode->fs_mounted_here = fs_global;
        root_entry->vnode->uid = 0;
        root_entry->vnode->gid = 0;
        root_entry->vnode->mode = 0555;

        vfs::create_entry(root_entry, "self", new InfoNode([] (InfoNode *self) {
            klib::printf_template(info_node_put, "%d", cpu::get_current_thread()->process->pid);
        }, vfs::NodeType::SYMLINK), 0, 0, 0777);

        vfs::create_entry(root_entry, "thread-self", new InfoNode([] (InfoNode *self) {
            klib::printf_template(info_node_put, "%d/task/%d", cpu::get_current_thread()->process->pid, cpu::get_current_thread()->tid);
        }, vfs::NodeType::SYMLINK), 0, 0, 0777);

        vfs::create_entry(root_entry, "mounts", new InfoNode([] (InfoNode *self) {
            klib::printf_template(info_node_put, "self/mounts");
        }, vfs::NodeType::SYMLINK), 0, 0, 0777);

        vfs::create_entry(root_entry, "cmdline", new InfoNode([] (InfoNode *self) {
            klib::printf_template(info_node_put, "%s\n", kernel_cmdline);
        }, vfs::NodeType::REGULAR), 0, 0, 0444);

        vfs::create_entry(root_entry, "uptime", new InfoNode([] (InfoNode *self) {
            auto time = sched::get_clock(CLOCK_BOOTTIME);
            klib::printf_template(info_node_put, "%lu.%lu 0.00\n", time.seconds, time.nanoseconds / 10'000'000);
        }, vfs::NodeType::REGULAR), 0, 0, 0444);

        vfs::create_entry(root_entry, "meminfo", new InfoNode([] (InfoNode *self) {
            auto print_value = [&] (const char *name, usize value) {
                klib::printf_template(info_node_put, "%s%8lu kB\n", name, value / 1024);
            };
            print_value("MemTotal:       ", pmm::stats.total_pages_usable * 0x1000);
            print_value("MemFree:        ", pmm::stats.total_free_pages * 0x1000);
            print_value("MemAvailable:   ", pmm::stats.total_free_pages * 0x1000);
            print_value("Buffers:        ", 0);
            print_value("Cached:         ", 0);
            print_value("SwapCached:     ", 0);
            print_value("Slab:           ", 0);
        }, vfs::NodeType::REGULAR), 0, 0, 0444);

        vfs::create_entry(root_entry, "stat", new InfoNode([] (InfoNode *self) {
            auto boottime = sched::get_clock(CLOCK_BOOTTIME);
            auto realtime = sched::get_clock(CLOCK_REALTIME);

            klib::printf_template(info_node_put, "cpu  0 0 0 0 0 0 0 0 0 0\n");
            klib::printf_template(info_node_put, "cpu0 0 0 0 0 0 0 0 0 0 0\n");
            klib::printf_template(info_node_put, "intr 0 0 0 0 0 0 0 0 0 0\n");
            klib::printf_template(info_node_put, "ctxt 30000\n");
            klib::printf_template(info_node_put, "btime %lu\n", realtime.seconds - boottime.seconds); // FIXME
            klib::printf_template(info_node_put, "processes 100\n");
            klib::printf_template(info_node_put, "procs_running 5\n");
            klib::printf_template(info_node_put, "procs_blocked 0\n");
            klib::printf_template(info_node_put, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
        }, vfs::NodeType::REGULAR), 0, 0, 0444);
    }

    void create_process_dir(sched::Process *process) {
        uid_t uid = process->get_main_thread()->cred.uids.rid;
        uid_t gid = process->get_main_thread()->cred.gids.rid;

        char pid_str[11] = {};
        klib::snprintf(pid_str, sizeof(pid_str), "%d", process->pid);
        auto *process_dir = vfs::lookup(fs_global->root_entry, pid_str);
        ASSERT(process_dir->vnode == nullptr);
        process_dir->create(vfs::NodeType::DIRECTORY, uid, gid, 0555);
        process->procfs_dir = process_dir;

        vfs::create_entry(process_dir, "maps", new InfoNode([process] (InfoNode *self) {
            process->pagemap->print(info_node_put);
        }, vfs::NodeType::REGULAR), uid, gid, 0444);

        vfs::create_entry(process_dir, "cwd", new InfoNode([process] (InfoNode *self) {
            process->cwd->print_path(info_node_put);
        }, vfs::NodeType::SYMLINK), uid, gid, 077);

        vfs::create_entry(process_dir, "exe", new InfoNode([process] (InfoNode *self) {
            process->exe->print_path(info_node_put);
        }, vfs::NodeType::SYMLINK), uid, gid, 077);

        vfs::create_entry(process_dir, "root", new InfoNode([] (InfoNode *self) {
            info_node_put('/');
        }, vfs::NodeType::SYMLINK), uid, gid, 077);

        vfs::create_entry(process_dir, "mounts", new InfoNode([] (InfoNode *self) {
            Filesystem *fs;
            LIST_FOR_EACH(fs, &vfs::mount_list, mounts_link) {
                klib::printf_template(info_node_put, "%s %s %s rw 0 0\n", fs->mount_source, fs->mount_target, fs->driver->name);
            }
        }, vfs::NodeType::REGULAR), uid, gid, 0444);

        vfs::create_entry(process_dir, "stat", new InfoNode([process] (InfoNode *self) {
            sched::Thread *thread = process->get_main_thread();
            char state;
            switch (thread->state) {
            case sched::Thread::READY:
            case sched::Thread::RUNNING: state = 'R'; break;
            case sched::Thread::BLOCKED: state = 'S'; break;
            case sched::Thread::STOPPED: state = 'T'; break;
            case sched::Thread::ZOMBIE:  state = 'Z'; break;
            }

            klib::printf_template(info_node_put, "%d (%s) %c %d %d %d",
                process->pid, thread->name, state, process->parent->pid, process->group->leader_process->pid, process->session_leader()->pid);
            for (int i = 0; i < 52 - 6; i++) {
                info_node_put(' ');
                info_node_put('0');
            }
            info_node_put('\n');
        }, vfs::NodeType::REGULAR), uid, gid, 0444);

        vfs::create_entry(process_dir, "status", new InfoNode([process] (InfoNode *self) {
            sched::Thread *thread = process->get_main_thread();
            auto &cred = thread->cred;
            const char *state;
            switch (thread->state) {
            case sched::Thread::READY:
            case sched::Thread::RUNNING: state = "R (running)"; break;
            case sched::Thread::BLOCKED: state = "S (sleeping)"; break;
            case sched::Thread::STOPPED: state = "T (stopped)"; break;
            case sched::Thread::ZOMBIE:  state = "Z (zombie)"; break;
            }

            klib::printf_template(info_node_put, "Name:\t%s\n", thread->name);
            klib::printf_template(info_node_put, "Umask:\t%#03o\n", process->umask);
            klib::printf_template(info_node_put, "State:\t%s\n", state);
            klib::printf_template(info_node_put, "Tgid:\t%d\n", process->pid);
            klib::printf_template(info_node_put, "Ngid:\t%d\n", 0);
            klib::printf_template(info_node_put, "Pid:\t%d\n", thread->tid);
            klib::printf_template(info_node_put, "PPid:\t%d\n", process->parent->pid);
            klib::printf_template(info_node_put, "TracerPid:\t%d\n", 0);
            klib::printf_template(info_node_put, "Uid:\t%d\t%d\t%d\t%d\n", cred.uids.rid, cred.uids.eid, cred.uids.sid, cred.uids.fsid);
            klib::printf_template(info_node_put, "Gid:\t%d\t%d\t%d\t%d\n", cred.gids.rid, cred.gids.eid, cred.gids.sid, cred.gids.fsid);
            klib::printf_template(info_node_put, "FDSize:\t%d\n", (int)process->file_descriptors.capacity());
            klib::printf_template(info_node_put, "Groups:\t");
            for (usize i = 0; i < cred.groups.size(); i++)
                klib::printf_template(info_node_put, "%d ", cred.groups[i]);
            info_node_put('\n');
        }, vfs::NodeType::REGULAR), uid, gid, 0444);
    }
}
