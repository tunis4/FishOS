#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <panic.hpp>
#include <limine.hpp>

namespace pmm {
    struct Page {
        klib::ListHead link;
        bool free : 1; // true if its in the freelist
        u64 pfn : 63; // page frame number (basically the physical address of the page >> 12)
        uptr mapped_addr; // virtual address that it is mapped to if this is anonymous memory
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
