#pragma once

#include <klib/common.hpp>
#include <klib/list.hpp>
#include <panic.hpp>
#include <limine.hpp>

namespace mem { extern uptr hhdm; }

namespace pmm {
    struct Page {
        klib::ListHead link;
        bool free : 1; // true if its in the freelist
        u64 pfn : 63; // page frame number (the physical address of the page >> 12)
        uptr mapped_addr; // virtual address that it is mapped to if this is anonymous memory

        inline uptr phy() const { return pfn * 0x1000; }

        template<typename T>
        inline T* as() const { return (T*)(phy() + mem::hhdm); }
    };
    static_assert(sizeof(Page) == 32);

    struct Region {
        klib::ListHead link;
        usize num_pages;
        usize padding;

        inline usize base_phy() const { return (uptr)this - mem::hhdm; }
        inline usize end_phy() const { return base_phy() + num_pages * 0x1000; }
        inline usize num_pages_reserved() const { return (sizeof(Region) + num_pages * sizeof(Page) + 0x1000 - 1) / 0x1000; }
        inline usize num_pages_usable() const { return num_pages - num_pages_reserved(); }
        inline Page* pages_array() const { return (Page*)((uptr)this + sizeof(Region)); }
    };
    static_assert(sizeof(Region) == 32);

    void init(uptr hhdm, limine_memmap_response *memmap_res);
    void absorb_memmap_entry(uptr hhdm, limine_memmap_entry *entry);
    void reclaim_bootloader_mem(uptr hhdm, limine_memmap_response *memmap_res);

    Page* find_page(uptr phy);

    Page* alloc_page();
    void free_page(Page *page);

    inline uptr alloc_pages(usize num_pages) {
        if (num_pages != 1) [[unlikely]]
            panic("incorrect pmm allocation\n");
        
        Page *page = alloc_page();
        return page->pfn * 0x1000;
    }

    struct Stats {
        usize total_pages_usable = 0;
        usize total_pages_reserved = 0;
        usize total_free_pages = 0;
    };

    extern Stats stats;
}
