#include <fs/initramfs.hpp>
#include <klib/cstdio.hpp>
#include <klib/cstring.hpp>
#include <klib/algorithm.hpp>
#include <dev/devnode.hpp>
#include <panic.hpp>

namespace initramfs {
    static usize octal_to_bin(const char *str, usize len) {
        usize value = 0;
        while (*str && len > 0) {
            value = value * 8 + (*str++ - '0');
            len--;
        }
        return value;
    }

    static void read_path(char dest[257], char name[100], char prefix[100]) {
        // if there's no prefix, use name directly
        if (prefix[0] == '\0') {
            memcpy(dest, name, 100);
            dest[100] = '\0';
            return;
        }

        // if there is a prefix, the path is: <prefix> '/' <name>
        size_t prefix_len = klib::strnlen(prefix, 155);
        memcpy(dest, prefix, prefix_len);
        dest[prefix_len] = '/';
        memcpy(&dest[prefix_len + 1], name, 100);
        dest[256] = '\0';
    }

    void load_into(vfs::Entry *dir, void *file_addr, usize file_size) {
        USTARHeader *current = (USTARHeader*)file_addr;
        while (true) {
            if (memcmp(current->magic, "ustar", 5) != 0)
                break;
            
            usize size = octal_to_bin(current->size, sizeof(current->size));
            usize dev_id = dev::make_dev_id(octal_to_bin(current->devmajor, sizeof(current->devmajor)), octal_to_bin(current->devminor, sizeof(current->devminor)));

            char current_path[257] = {};
            read_path(current_path, current->name, current->prefix);
            char current_link_path[257] = {};
            read_path(current_link_path, current->linkname, current->prefix);

            // klib::printf("Path: %s, type: %c\n", current_path, current->typeflag);
            // if (size > 1024) klib::printf("%ld KiB\n", size / 1024);
            // else klib::printf("%ld B\n", size);

            if (klib::strcmp(current_path, "./") != 0) {
                auto *entry = vfs::path_to_entry(current_path, dir);
                ASSERT(entry->vnode == nullptr);

                switch (current->typeflag) {
                case '0': // regular
                case '\0':
                    entry->create(vfs::NodeType::REGULAR);
                    entry->vnode->write(nullptr, (void*)((uptr)current + 512), size, 0);
                    break;
                case '1': // hard link
                    entry->vnode = vfs::path_to_entry(current_link_path, dir)->vnode;
                    ASSERT(entry->vnode != nullptr);
                    entry->create(vfs::NodeType::NONE);
                    break;
                case '2': // symbolic link
                    entry->create(vfs::NodeType::SYMLINK);
                    entry->vnode->write(nullptr, current_link_path, klib::strlen(current_link_path), 0);
                    break;
                case '3': // character device
                    entry->vnode = dev::CharDevNode::create_node(dev_id);
                    entry->create(vfs::NodeType::CHAR_DEVICE);
                    break;
                case '4': // block device
                    entry->vnode = dev::BlockDevNode::create_node(dev_id);
                    entry->create(vfs::NodeType::BLOCK_DEVICE);
                    break;
                case '5': // directory
                    entry->create(vfs::NodeType::DIRECTORY);
                    break;
                default:
                    klib::printf("Unimplemented node type in initramfs: %c\n", current->typeflag);
                    break;
                }
            }

            current = (USTARHeader*)((uptr)current + 512 + klib::align_up(size, 512));
            if ((uptr)current >= (uptr)file_addr + file_size)
                break;
        }
    }
}
