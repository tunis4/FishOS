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

    void load_into(vfs::Entry *dir, void *file_addr, usize file_size) {
        USTARHeader *current = (USTARHeader*)file_addr;
        while (true) {
            if (memcmp(current->magic, "ustar", 5) != 0)
                break;
            
            usize size = octal_to_bin(current->size, sizeof(current->size));
            usize dev_id = dev::make_dev_id(octal_to_bin(current->devmajor, sizeof(current->devmajor)), octal_to_bin(current->devminor, sizeof(current->devminor)));

            // klib::printf("Name: %s, size: ", current->name);
            // if (size > 1024) klib::printf("%ld KiB\n", size / 1024);
            // else klib::printf("%ld B\n", size);

            if (klib::strcmp(current->name, "./") != 0) {
                auto *entry = vfs::path_to_entry(current->name, dir);
                ASSERT(entry->vnode == nullptr);

                switch (current->typeflag) {
                case '0':
                case '\0':
                    entry->create(vfs::NodeType::REGULAR);
                    entry->vnode->write(nullptr, (void*)((uptr)current + 512), size, 0);
                    break;
                case '5':
                    entry->create(vfs::NodeType::DIRECTORY);
                    break;
                case '2':
                    entry->create(vfs::NodeType::SYMLINK);
                    entry->vnode->write(nullptr, &current->linkname, klib::strlen(current->linkname), 0);
                    break;
                case '3':
                    entry->vnode = dev::CharDevNode::create_node(dev_id);
                    entry->create();
                    break;
                case '4':
                    entry->vnode = dev::BlockDevNode::create_node(dev_id);
                    entry->create();
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
