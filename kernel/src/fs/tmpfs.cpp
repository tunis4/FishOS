#include <fs/tmpfs.hpp>
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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace tmpfs {
    void Filesystem::lookup(vfs::Entry *entry) {
        // if it didnt exist in the cache then it doesnt exist
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

        auto *node_data = new NodeData();
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
        statbuf->st_nlink = 2; // FIXME: hack for useradd
    }

    void Filesystem::statfs(struct statfs *buf) {
        buf->f_type = TMPFS_MAGIC;
        *(u64*)&buf->f_fsid = 0x21948930289035;
        buf->f_namelen = 255;
        buf->f_bsize = 0x1000;
        buf->f_frsize = 0x1000;
        buf->f_blocks = pmm::stats.total_pages_usable;
        buf->f_bfree = pmm::stats.total_free_pages;
        buf->f_bavail = pmm::stats.total_free_pages;
        buf->f_files = 1 << 20;
        buf->f_ffree = (1 << 20) - 1000;
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

    void Node::grow_to(usize length) {
        NodeData *node_data = (NodeData*)fs_data;
        node_data->size = length;
        if (node_data->capacity < node_data->size) {
            while (node_data->capacity < node_data->size)
                node_data->capacity = klib::max((usize)1, node_data->capacity * 4);
            node_data->storage = (u8*)klib::realloc(node_data->storage, node_data->capacity);
        }
    }

    isize Node::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        if (node_type == vfs::NodeType::DIRECTORY) return -EISDIR;
        NodeData *node_data = (NodeData*)fs_data;
        if (offset >= node_data->size)
            return 0;
        usize actual_count = offset + count > node_data->size ? node_data->size - offset : count;
        memcpy(buf, node_data->storage + offset, actual_count);
        return actual_count;
    }

    isize Node::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        if (node_type == vfs::NodeType::DIRECTORY) return -EISDIR;
        NodeData *node_data = (NodeData*)fs_data;
        if (count == 0) [[unlikely]] return 0;
        if (offset + count > node_data->size)
            grow_to(offset + count);
        memcpy(node_data->storage + offset, buf, count);
        return count;
    }
    
    isize Node::seek(vfs::FileDescription *fd, usize position, isize offset, int whence) {
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

    isize Node::mmap(vfs::FileDescription *fd, uptr addr, usize length, isize offset, int prot, int flags) {
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE))
            klib::printf("tmpfs/mmap: MAP_SHARED with PROT_WRITE not supported\n");

        sched::Process *process = cpu::get_current_thread()->process;
        u64 page_flags = mem::mmap_prot_to_page_flags(prot);
        process->pagemap->map_file(addr, length, page_flags, fd, offset);
        return addr;
    }

    isize Node::truncate(vfs::FileDescription *fd, usize length) {
        if (node_type == vfs::NodeType::DIRECTORY) return -EISDIR;
        NodeData *node_data = (NodeData*)fs_data;
        if (length > node_data->size) {
            grow_to(length);
            memset(node_data->storage + node_data->size, 0, length - node_data->size);
        }
        node_data->size = length;
        return 0;
    }

    vfs::Filesystem* Driver::mount(vfs::Entry *mount_entry) {
        auto *fs = new Filesystem();
        fs->last_inode_num = 2;

        fs->create(mount_entry, vfs::NodeType::DIRECTORY);
        mount_entry->vnode->fs_mounted_here = fs;
        fs->root_entry = mount_entry;

        return fs;
    }

    void Driver::unmount(vfs::Filesystem *fs) {
        delete fs;
    }
}
