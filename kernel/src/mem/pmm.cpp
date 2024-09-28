#include <mem/pmm.hpp>
#include <klib/lock.hpp>
#include <klib/cstdio.hpp>
#include <klib/cstring.hpp>
#include <klib/algorithm.hpp>
#include <panic.hpp>
#include <limine.hpp>

namespace mem::pmm {
    Stats stats;

    static klib::Spinlock pmm_lock;
    static klib::ListHead freelist;

    void init(uptr hhdm, limine_memmap_response *memmap_res) {
        freelist.init();
        for (usize e = 0; e < memmap_res->entry_count; e++) {
            auto *entry = memmap_res->entries[e];
            if (entry->type == LIMINE_MEMMAP_USABLE)
                absorb_memmap_entry(hhdm, entry);
        }

        klib::printf("PMM: %ld page structs created, representing %ld KiB (%ld MiB) of usable memory\n", stats.total_free_pages, stats.total_free_pages * 0x1000 / 1024, stats.total_free_pages * 0x1000 / 1024 / 1024);
        klib::printf("PMM: %ld pages (%ld KiB) reserved for storing page structs\n", stats.total_pages_reserved, stats.total_pages_reserved * 0x1000 / 1024);
    }

    void absorb_memmap_entry(uptr hhdm, limine_memmap_entry *entry) {            
        usize pfn_start = entry->base / 0x1000;
        usize pfn_end = entry->base / 0x1000 + entry->length / 0x1000;
        if (entry->base % 0x1000)
            pfn_start++;
        if (entry->length % 0x1000)
            pfn_end--;
        usize num_pages = pfn_end - pfn_start;

        constexpr usize page_structs_per_page = 0x1000 / sizeof(Page);
        usize num_pages_reserved = (num_pages + page_structs_per_page - 1) / page_structs_per_page;
        stats.total_pages_reserved += num_pages_reserved;
        usize num_page_structs = num_pages - num_pages_reserved;
        
        usize current_pfn = pfn_start;
        Page *page = nullptr;
        for (usize i = 0; i < num_page_structs; i++) {
            if (i % page_structs_per_page == 0) {
                page = (Page*)(current_pfn * 0x1000 + hhdm);
                current_pfn++;
            }
            
            usize pfn = pfn_start + num_pages_reserved + i;
            page->pfn = pfn;
            page->free = true;
            freelist.add_before(&page->list);

            stats.total_free_pages++;
            page++;
        }
    }

    // broken
    void reclaim_bootloader_mem(uptr hhdm, limine_memmap_response *memmap_res) {
        usize previous_free_pages = stats.total_free_pages;
        Page *page = alloc_page();
        pmm_lock.lock();

        auto *entry = (limine_memmap_entry*)(page->pfn * 0x1000);
        usize num_entries = 0;
        for (usize e = 0; e < memmap_res->entry_count; e++) {
            if (memmap_res->entries[e]->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
                continue;
            
            memcpy(&entry[num_entries], memmap_res->entries[e], sizeof(limine_memmap_entry));
            num_entries++;
            if (num_entries >= 0x1000 / sizeof(limine_memmap_entry))
                break;
        }

        for (usize i = 0; i < num_entries; i++)
            absorb_memmap_entry(hhdm, &entry[i]);

        pmm_lock.unlock();
        free_page(page);

        usize num_reclaimed_pages = stats.total_free_pages - previous_free_pages;
        klib::printf("PMM: Reclaimed %ld pages (%ld KiB) of bootloader memory\n", num_reclaimed_pages, num_reclaimed_pages * 0x1000 / 1024);
    }

    Page* alloc_page() {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(pmm_lock);

        if (freelist.is_empty()) [[unlikely]]
            panic("Out of physical memory");

        Page *page = LIST_ENTRY(freelist.next, Page, list);
        page->free = false;
        freelist.next->remove();
        stats.total_free_pages--;
        return page;
    }

    void free_page(Page *page) {
        klib::InterruptLock interrupt_guard;
        klib::LockGuard guard(pmm_lock);

        page->free = true;
        freelist.add_before(&page->list);
        stats.total_free_pages++;
    }
}
