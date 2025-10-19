#pragma once

#include <mem/pmm.hpp>
#include <mem/vmem.hpp>
#include <fs/vfs.hpp>
#include <klib/common.hpp>
#include <klib/lock.hpp>
#include <klib/list.hpp>
#include <klib/cstdio.hpp>
#include <limine.hpp>

#define PAGE_PRESENT (1 << 0)
#define PAGE_WRITABLE (1 << 1)
#define PAGE_USER (1 << 2)
#define PAGE_WRITE_THROUGH (1 << 3)
#define PAGE_CACHE_DISABLE (1 << 4)
#define PAGE_ACCESSED (1 << 5)
#define PAGE_DIRTY (1 << 6)
#define PAGE_ATTRIBUTE_TABLE (1 << 7)
#define PAGE_GLOBAL (1 << 8)
#define PAGE_WRITE_COMBINING (PAGE_ATTRIBUTE_TABLE | PAGE_CACHE_DISABLE)
#define PAGE_NO_EXECUTE ((u64)1 << 63)

namespace mem {
    struct Pagemap;

    struct MappedRange {
        enum class Type {
            NONE,
            DIRECT,
            ANONYMOUS,
            FILE
        };

        klib::ListHead range_link;
        klib::ListHead page_list;

        uptr base;
        usize length;
        u64 page_flags;
        Type type;

        // used for direct mapping
        uptr phy_base = 0;

        // used for file mapping
        vfs::FileDescription *file = nullptr;
        usize file_offset = 0;

        MappedRange() {}
        MappedRange(uptr base, usize length, u64 page_flags, Type type);
        ~MappedRange();

        MappedRange(const MappedRange &) = delete;
        MappedRange(const MappedRange &&) = delete;

        MappedRange* copy();

        void invalidate_pages(Pagemap *pagemap);
        inline uptr end() { return base + length; }

        template<klib::Putchar Put>
        void print(Put put) {
            int written = 0;
            written += klib::printf_template(put,
                "%08lx-%08lx r%c%cp %08lx %02x:%02x %u",
                base, end(),
                (page_flags & PAGE_WRITABLE) ? 'w' : '-', !(page_flags & PAGE_NO_EXECUTE) ? 'x' : '-',
                file_offset,
                0, 0,
                0);
            for (; written < 72; written++)
                put(' ');
            if (type == Type::DIRECT) klib::printf_template(put, "direct to %#lX", phy_base);
            else if (type == Type::FILE) file->entry->print_path(put);
            put('\n');
        }
    };

    struct Pagemap {
        u64 *pml4;
        klib::Spinlock lock;
        klib::ListHead range_list;

        // if this pagemap hasnt been forked, then this will be set to nullptr.
        // if this pagemap has been forked, then this will point to the fork.
        // if this pagemap is the fork, then this will point to itself.
        Pagemap *forked;

        Pagemap();
        ~Pagemap();

        void activate();
        isize get_physical_addr(uptr virt);

        void map_page(uptr phy, uptr virt, u64 flags);
        void map_pages(uptr phy, uptr virt, usize size, u64 flags);
        void map_kernel(); // for user pagemaps

        void map_anonymous(uptr base, usize length, u64 page_flags);
        void map_direct(uptr base, usize length, u64 page_flags, uptr phy_base);
        void map_file(uptr base, usize length, u64 page_flags, vfs::FileDescription *file, usize file_offset);

        void add_range(MappedRange *new_range, bool merge = true, bool resolve_overlap = true);

        MappedRange* addr_to_range(uptr virt);
        isize handle_page_fault(uptr virt);

        Pagemap* fork();

        template<klib::Putchar Put>
        void print(Put put) {
            MappedRange *range;
            LIST_FOR_EACH(range, &this->range_list, range_link)
                range->print(put);
        }
    };

    struct VMM {
        uptr heap_base;
        usize heap_size;

        Pagemap kernel_pagemap;
        Pagemap *active_pagemap = nullptr;

        void init(uptr hhdm_base, limine_memmap_response *memmap_res, limine_kernel_address_response *kernel_addr_res);

        uptr virt_alloc(usize length);

    private:
        uptr hhdm_end;
        uptr kernel_phy_base;
        uptr kernel_virt_base;
        uptr kernel_virt_alloc_base;
        MappedRange kernel_hhdm_range;
        MappedRange kernel_heap_range;
    };

    extern VMM *vmm;
    extern uptr hhdm;

    u64 mmap_prot_to_page_flags(int prot);

    isize syscall_mmap(void *addr, usize length, int prot, int flags, int fd, isize offset);
    isize syscall_munmap(void *addr, usize length);
    isize syscall_mprotect(void *addr, usize length, int prot);
}
