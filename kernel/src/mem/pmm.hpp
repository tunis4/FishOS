#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <panic.hpp>
#include <limine.hpp>

namespace mem::vmm { struct AnonPage; }

namespace mem::pmm {
    struct Page {
        klib::ListHead list;
        u64 pfn : 40; // page frame number (basically the physical address of the page >> 12)
        bool free : 1; // true if its in the freelist
        vmm::AnonPage *anon; // if the page is used to store anonymous memory
    };

    void init(uptr hhdm, limine_memmap_response *memmap_res);
    void absorb_memmap_entry(uptr hhdm, limine_memmap_entry *entry);
    void reclaim_bootloader_mem(uptr hhdm, limine_memmap_response *memmap_res);

    Page* alloc_page();
    void free_page(Page *page);

    inline uptr alloc_pages(usize num_pages) {
        if (num_pages != 1) [[unlikely]]
            panic("incorrect pmm allocation\n");
        
        Page *page = alloc_page();
        return page->pfn * 0x1000;
    }

    struct Stats {
        usize total_pages_reserved = 0;
        usize total_free_pages = 0;
    };

    extern Stats stats;
}
