#include <fs/tmpfs.hpp>
#include <klib/algorithm.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdlib.hpp>
#include <dev/devnode.hpp>
#include <sched/time.hpp>
#include <panic.hpp>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace tmpfs {
    void Filesystem::lookup(vfs::Entry *entry) {
        return; // if it didnt exist in the cache then it doesnt exist
    }

    void Filesystem::create(vfs::Entry *entry, vfs::NodeType new_node_type) {
        if (!entry->vnode) {
            ASSERT(new_node_type != vfs::NodeType::NONE);
            entry->vnode = new Node();
            entry->vnode->type = new_node_type;
        }
        entry->vnode->fs = this;
        entry->vnode->creation_time = sched::realtime_clock;
        entry->vnode->modification_time = sched::realtime_clock;
        entry->vnode->access_time = sched::realtime_clock;

        auto *node_data = new NodeData();
        node_data->inode_num = last_inode_num++;

        entry->vnode->fs_data = node_data;
    }
    
    void Filesystem::remove(vfs::Entry *entry) {
        delete (NodeData*)(entry->vnode->fs_data);
    }

    void Filesystem::stat(vfs::VNode *vnode, struct stat *statbuf) {
        NodeData *node_data = (NodeData*)vnode->fs_data;
        statbuf->st_ino = node_data->inode_num;
        if (vnode->type == vfs::NodeType::DIRECTORY)
            statbuf->st_size = sizeof(vfs::Entry);
        else
            statbuf->st_size = node_data->size;
        statbuf->st_blksize = 4096;
        statbuf->st_blocks = (statbuf->st_size + 4095) / 4096;
        statbuf->st_nlink = 1;
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
            usize entry_len = klib::align_up<usize, 8>(sizeof(vfs::Dirent) + name_len + 1);
            if (count + entry_len > max_size)
                break;
            auto dirent = (vfs::Dirent*)((uptr)buf + count);
            dirent->d_ino = ((NodeData*)child->vnode->fs_data)->inode_num;
            dirent->d_off = i;
            dirent->d_reclen = entry_len;
            dirent->d_type_from_node_type(child->vnode->type);
            memcpy(dirent->d_name, child->name, name_len + 1);
            count += entry_len;
            i++;
        }
        *cursor = i;
        return count;
    }

    isize Node::read(vfs::FileDescription *fd, void *buf, usize count, usize offset) {
        NodeData *node_data = (NodeData*)fs_data;
        if (offset >= node_data->size)
            return 0;
        usize actual_count = offset + count > node_data->size ? node_data->size - offset : count;
        memcpy(buf, node_data->storage + offset, actual_count);
        return actual_count;
    }

    isize Node::write(vfs::FileDescription *fd, const void *buf, usize count, usize offset) {
        NodeData *node_data = (NodeData*)fs_data;
        if (count == 0) [[unlikely]] return 0;
        if (offset + count > node_data->size) {
            node_data->size = offset + count;
            node_data->storage = (u8*)klib::realloc(node_data->storage, node_data->size);
        }
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

    vfs::Filesystem* Driver::mount(vfs::Entry *mount_entry) {
        auto *tmpfs = new Filesystem();
        tmpfs->last_inode_num = 1;

        tmpfs->create(mount_entry, vfs::NodeType::DIRECTORY);
        tmpfs->root_entry = mount_entry;

        return tmpfs;
    }

    void Driver::unmount(vfs::Filesystem *fs) {
        delete fs;
    }
}
