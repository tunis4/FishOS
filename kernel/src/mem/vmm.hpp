#pragma once

#include <mem/pmm.hpp>
#include <mem/vmem.hpp>
#include <klib/common.hpp>
#include <klib/lock.hpp>
#include <klib/list.hpp>
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

namespace vmm {
    struct MappedRange {
        enum class Type {
            DIRECT,
            ANONYMOUS
        };

        klib::ListHead range_link;
        klib::ListHead page_list;
        uptr phy_base; // used if direct
        uptr base;
        usize length;
        u64 page_flags;
        Type type;

        MappedRange() {}
        MappedRange(uptr phy_base, uptr base, usize length, u64 page_flags, Type type);
        ~MappedRange();

        void print();
    };

    struct Pagemap {
        u64 *pml4;
        klib::Spinlock lock;
        klib::ListHead range_list;

        // if this pagemap hasnt been forked, then this will be set to nullptr.
        // if this pagemap has been forked, then this will point to the fork.
        // if this pagemap is the fork, then this will point to itself.
        Pagemap *forked;

        Pagemap() {}
        ~Pagemap();

        void activate();
        isize get_physical_addr(uptr virt);

        void map_page(uptr phy, uptr virt, u64 flags);
        void map_pages(uptr phy, uptr virt, usize size, u64 flags);
        void map_kernel(); // for user pagemaps

        void map_range(uptr base, usize length, u64 page_flags, MappedRange::Type type, uptr phy_base = 0);

        MappedRange* addr_to_range(uptr virt);
        isize handle_page_fault(uptr virt);

        Pagemap* fork();
    };

    void init(uptr hhdm_base, limine_memmap_response *memmap_res, limine_kernel_address_response *kernel_addr_res);

    uptr virt_alloc(usize length);

    extern uptr hhdm;
    extern uptr heap_base;
    extern usize heap_size;
    extern Pagemap kernel_pagemap;
    extern Pagemap *active_pagemap;

    u64 mmap_prot_to_page_flags(int prot);

    isize syscall_mmap(void *addr, usize length, int prot, int flags, int fd, isize offset);
    isize syscall_munmap(void *addr, usize length);
}
