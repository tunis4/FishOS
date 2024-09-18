#include <mem/vmm.hpp>
#include <mem/pmm.hpp>
#include <panic.hpp>
#include <klib/cstdio.hpp>
#include <klib/cstring.hpp>
#include <klib/algorithm.hpp>
#include <cpu/cpu.hpp>
#include <cpu/syscall/syscall.hpp>
#include <sched/sched.hpp>
#include <limine.hpp>
#include <errno.h>
#include <sys/mman.h>

namespace mem::vmm {
    static uptr hhdm;
    static uptr hhdm_end;
    static uptr kernel_phy_base;
    static uptr kernel_virt_base;
    static uptr heap_base;
    static usize heap_size;

    static Pagemap kernel_pagemap;
    static Pagemap *active_pagemap;
    static MappedRange kernel_hhdm_range;
    static MappedRange kernel_heap_range;

    void init(uptr hhdm_base, limine_memmap_response *memmap_res, limine_kernel_address_response *kernel_addr_res) {
        hhdm = hhdm_base;
        hhdm_end = hhdm_base;
        kernel_phy_base = kernel_addr_res->physical_base;
        kernel_virt_base = kernel_addr_res->virtual_base;
        for (usize e = 0; e < memmap_res->entry_count; e++) {
            auto *entry = memmap_res->entries[e];
            if (hhdm_base + entry->base + entry->length > hhdm_end)
                hhdm_end = hhdm_base + entry->base + entry->length;
        }
        hhdm_end = klib::align_up<uptr, 0x1000>(hhdm_end);
        heap_base = hhdm_end;
        heap_size = (usize)4 * 1024 * 1024 * 1024; // 4 GiB

        kernel_pagemap.pml4 = (u64*)(pmm::alloc_pages(1) + hhdm);
        memset(kernel_pagemap.pml4, 0, 0x1000);

        usize kernel_size = 0;

        klib::printf("VMM: Physical memory map:\n");
        for (usize e = 0; e < memmap_res->entry_count; e++) {
            auto *entry = memmap_res->entries[e];
            const char *entry_name;
            u64 flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE;

            switch (entry->type) {
            case LIMINE_MEMMAP_USABLE: entry_name = "Usable"; break;
            case LIMINE_MEMMAP_RESERVED: entry_name = "Reserved"; break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE: entry_name = "ACPI Reclaimable"; break;
            case LIMINE_MEMMAP_ACPI_NVS: entry_name = "ACPI NVS"; break;
            case LIMINE_MEMMAP_BAD_MEMORY: entry_name = "Bad Memory"; break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: entry_name = "Bootloader Reclaimable"; break;
            case LIMINE_MEMMAP_KERNEL_AND_MODULES: entry_name = "Kernel and Modules"; kernel_size += entry->length; break;
            case LIMINE_MEMMAP_FRAMEBUFFER: entry_name = "Framebuffer"; flags |= PAGE_WRITE_COMBINING; break;
            default: entry_name = "Unknown";
            }

            klib::printf("    %s | base: %#lX, size: %ld KiB\n", entry_name, entry->base, entry->length / 1024);

            if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE || entry->type == LIMINE_MEMMAP_FRAMEBUFFER || entry->type == LIMINE_MEMMAP_USABLE || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
                kernel_pagemap.map_pages(entry->base, entry->base + hhdm, entry->length, flags);
        }

        klib::printf("VMM: Kernel base addresses | phy: %#lX, virt: %#lX\n", kernel_phy_base, kernel_virt_base);
        
/*
        klib::printf("VMM: Kernel PMRs:\n");
        for (usize i = 0; i < tag_pmrs->entries; i++) {
            auto pmr = tag_pmrs->pmrs[i];
            u64 flags = PAGE_PRESENT;
            if (pmr.permissions & STIVALE2_PMR_WRITABLE) flags |= PAGE_WRITABLE;
            if (!(pmr.permissions & STIVALE2_PMR_EXECUTABLE)) flags |= PAGE_NO_EXECUTE;

            map_pages((pmr.base - kernel_virt_base) + kernel_phy_base, pmr.base, pmr.length, flags);
            
            klib::printf("       base %#lX, size: %ld KiB, permissions: %ld\n", pmr.base, pmr.length / 1024, pmr.permissions);
        }
*/

        kernel_pagemap.map_pages(kernel_phy_base, kernel_virt_base, kernel_size, PAGE_PRESENT | PAGE_WRITABLE);
        kernel_pagemap.range_list_head.init();
        kernel_hhdm_range = {
            .base = hhdm_base,
            .length = hhdm_end - hhdm_base,
            .page_flags = PAGE_PRESENT | PAGE_WRITABLE,
            .type = MappedRange::Type::DIRECT
        };
        kernel_pagemap.range_list_head.add(&kernel_hhdm_range.range_list);
        kernel_heap_range = { 
            .base = heap_base,
            .length = heap_size,
            .page_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE,
            .type = MappedRange::Type::ANONYMOUS
        };
        kernel_pagemap.range_list_head.add(&kernel_heap_range.range_list);
        kernel_pagemap.activate();
    }

    uptr get_hhdm() { return hhdm; }
    uptr get_heap_base() { return heap_base; }
    uptr get_heap_size() { return heap_size; }

    Pagemap* get_kernel_pagemap() {
        return &kernel_pagemap;
    }

    Pagemap* get_active_pagemap() {
        return active_pagemap;
    }

    static u64* create_next_page_table(u64 *current_entry) {
        uptr new_page = pmm::alloc_pages(1);
        memset((void*)(new_page + hhdm), 0, 0x1000);
        *current_entry = new_page | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        return (u64*)(new_page + hhdm);
    }

    void Pagemap::map_page(uptr phy, uptr virt, u64 flags) {
        klib::LockGuard guard(this->lock);
        u64 *current_table = this->pml4;

        // klib::printf("Phy: %#lX, Virt: %#lX, Flags: %#lX\n", phy, virt, flags);

        if (u64 *current_entry = &current_table[(virt >> 39) & 0x1FF]; *current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);
        if (u64 *current_entry = &current_table[(virt >> 30) & 0x1FF]; *current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);
        if (u64 *current_entry = &current_table[(virt >> 21) & 0x1FF]; *current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);

        u64 *entry = &current_table[virt >> 12 & 0x1FF];
        bool replaced = *entry != 0;
        *entry = (phy & 0x000FFFFFFFFFF000) | flags;
        if (replaced) cpu::invlpg((void*)virt); // TODO: find a better way to do this crap
    }

    void Pagemap::map_pages(uptr phy, uptr virt, usize size, u64 flags) {
        usize num_pages = klib::align_up<usize, 0x1000>(size) / 0x1000;
        // klib::printf("Mapping %ld pages phy %#lX virt %#lX end %#lX\n", num_pages, phy, virt, phy + num_pages * 0x1000);
        for (usize i = 0; i < num_pages; i++)
            map_page(phy + (i * 0x1000), virt + (i * 0x1000), flags);
    }

    void Pagemap::map_kernel() {
        klib::LockGuard guard(this->lock);
        auto kernel_pagemap = get_kernel_pagemap();
        for (usize i = 256; i < 512; i++) {
            pml4[i] = kernel_pagemap->pml4[i];
        }
    }

    void Pagemap::activate() {
        cpu::write_cr3(uptr(pml4) - hhdm);
        active_pagemap = this;
    }

    uptr Pagemap::physical_addr(uptr virt) {
        u64 *current_table = pml4;

        if (u64 current_entry = current_table[(virt >> 39) & 0x1FF]; current_entry & PAGE_PRESENT) 
            current_table = (u64*)((current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else return 0;
        if (u64 current_entry = current_table[(virt >> 30) & 0x1FF]; current_entry & PAGE_PRESENT) 
            current_table = (u64*)((current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else return 0;
        if (u64 current_entry = current_table[(virt >> 21) & 0x1FF]; current_entry & PAGE_PRESENT) 
            current_table = (u64*)((current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else return 0;

        u64 *entry = &current_table[virt >> 12 & 0x1FF];
        return *entry & 0x000FFFFFFFFFF000;
    }

    MappedRange* Pagemap::addr_to_range(uptr virt) {
        // first check current pagemap
        MappedRange *range;
        LIST_FOR_EACH(range, &this->range_list_head, range_list)
            if (virt >= range->base && virt < range->base + range->length)
                return range;

        if (this == &kernel_pagemap)
            return nullptr;

        // otherwise also check kernel pagemap
        LIST_FOR_EACH(range, &kernel_pagemap.range_list_head, range_list)
            if (virt >= range->base && virt < range->base + range->length)
                return range;

        return nullptr;
    }

    // returns true if the page fault couldnt be handled
    bool Pagemap::handle_page_fault(uptr virt) {
        klib::LockGuard guard(this->lock);

        u64 *current_table = this->pml4;
        
        u64 *current_entry = &current_table[(virt >> 39) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);
        current_entry = &current_table[(virt >> 30) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);
        current_entry = &current_table[(virt >> 21) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else current_table = create_next_page_table(current_entry);

        u64 *entry = &current_table[virt >> 12 & 0x1FF];

        if (!(*entry & PAGE_PRESENT)) {
            MappedRange *range = addr_to_range(virt);
            if (range == nullptr)
                return true;
            
            switch (range->type) {
            case MappedRange::Type::ANONYMOUS: {
                // allocate a new page
                uptr new_page = pmm::alloc_pages(1);
                memset((void*)(new_page + hhdm), 0, 0x1000);
                *entry = new_page | range->page_flags;
                return false;
            }
            case MappedRange::Type::DIRECT:
                *entry = ((virt - hhdm) & 0x000FFFFFFFFFF000) | range->page_flags;
                return false;
            default:
                klib::printf("Unknown mapped range type: %#lX\n", u64(range->type));
                return true;
            }
        } else if (*entry & PAGE_COPY_ON_WRITE) {
            klib::printf("cow\n");
            return true;
        } else return true;
    }
    
    Pagemap* Pagemap::fork() {
        forked = new Pagemap();
        forked->forked = forked;

        forked->pml4 = (u64*)(pmm::alloc_pages(1) + hhdm);
        memcpy(forked->pml4, this->pml4, 0x1000);
        for (usize i = 256; i < 512; i++) // higher half
            forked->pml4[i] = this->pml4[i];
        for (usize i = 0; i < 256; i++) { // lower half
            // if (this->pml4[i] & PAGE_WRITABLE)
            //     this->pml4[i] = (this->pml4[i] & ~u64(PAGE_WRITABLE)) | PAGE_COPY_ON_WRITE;

            if (!(this->pml4[i] & PAGE_PRESENT)) {
                forked->pml4[i] = this->pml4[i];
                continue;
            }

            u64 *pml3 = (u64*)((this->pml4[i] & 0x000FFFFFFFFFF000) + hhdm);

            uptr new_page = pmm::alloc_pages(1);
            memset((void*)(new_page + hhdm), 0, 0x1000);
            forked->pml4[i] = new_page | (pml4[i] & ~0x000FFFFFFFFFF000);
            u64 *forked_pml3 = (u64*)(new_page + hhdm);

            for (usize i = 0; i < 512; i++) {
                if (!(pml3[i] & PAGE_PRESENT)) {
                    forked_pml3[i] = pml3[i];
                    continue;
                }

                u64 *pml2 = (u64*)((pml3[i] & 0x000FFFFFFFFFF000) + hhdm);

                uptr new_page = pmm::alloc_pages(1);
                memset((void*)(new_page + hhdm), 0, 0x1000);
                forked_pml3[i] = new_page | (pml3[i] & ~0x000FFFFFFFFFF000);
                u64 *forked_pml2 = (u64*)(new_page + hhdm);

                for (usize i = 0; i < 512; i++) {
                    if (!(pml2[i] & PAGE_PRESENT)) {
                        forked_pml2[i] = pml2[i];
                        continue;
                    }

                    u64 *pml1 = (u64*)((pml2[i] & 0x000FFFFFFFFFF000) + hhdm);

                    uptr new_page = pmm::alloc_pages(1);
                    memset((void*)(new_page + hhdm), 0, 0x1000);
                    forked_pml2[i] = new_page | (pml2[i] & ~0x000FFFFFFFFFF000);
                    u64 *forked_pml1 = (u64*)(new_page + hhdm);

                    for (usize i = 0; i < 512; i++) {
                        if (!(pml1[i] & PAGE_PRESENT)) {
                            forked_pml1[i] = pml1[i];
                            continue;
                        }

                        u64 *page = (u64*)((pml1[i] & 0x000FFFFFFFFFF000) + hhdm);

                        uptr new_page = pmm::alloc_pages(1);
                        memset((void*)(new_page + hhdm), 0, 0x1000);
                        forked_pml1[i] = new_page | (pml1[i] & ~0x000FFFFFFFFFF000);
                        u64 *forked_page = (u64*)(new_page + hhdm);

                        memcpy(forked_page, page, 0x1000);
                    }
                }
            }
        }

        forked->range_list_head.init();
        klib::ListHead *current_head = &this->range_list_head;
        while (current_head->next != &this->range_list_head) {
            current_head = current_head->next;
            MappedRange *range = LIST_ENTRY(current_head, MappedRange, range_list);
            MappedRange *new_range = new MappedRange;
            memcpy(new_range, range, sizeof(MappedRange));
            forked->range_list_head.add_before(&new_range->range_list);
        }

        return forked;
    }

    void Pagemap::anon_map(uptr base, usize length, u64 page_flags) {
#ifndef NDEBUG
        MappedRange *other;
        LIST_FOR_EACH(other, &this->range_list_head, range_list)
            if (base < other->base + other->length && base + length > other->base)
                panic("New MappedRange { base = %#lX, length = %#lX } collides with existing MappedRange { base = %#lX, length = %#lX }", base, length, other->base, other->length);
#endif

        MappedRange *range = new MappedRange();
        range->base = base;
        range->length = length;
        range->page_flags = page_flags;
        range->type = MappedRange::Type::ANONYMOUS;
        this->range_list_head.add(&range->range_list);
    }
    
    u64 mmap_prot_to_page_flags(int prot) {
        u64 page_flags = PAGE_PRESENT | PAGE_USER;
        if (prot & PROT_WRITE)
            page_flags |= PAGE_WRITABLE;
        if (!(prot & PROT_EXEC))
            page_flags |= PAGE_NO_EXECUTE;
        return page_flags;
    }
    
    isize syscall_mmap(void *addr, usize length, int prot, int flags, int fd, isize offset) {
#if SYSCALL_TRACE
        klib::printf("mmap(%#lX, %#lX, %d, %d, %d, %ld)", (uptr)addr, length, prot, flags, fd, offset);
        defer { klib::putchar('\n'); };
#endif
        sched::Process *process = cpu::get_current_thread()->process;

        uptr base;
        usize aligned_size = klib::align_up<usize, 0x1000>(length);
        if (flags & MAP_FIXED) {
            base = (uptr)addr;
            process->mmap_anon_base = klib::align_up<uptr, 0x1000>(klib::max(process->mmap_anon_base, base + aligned_size));
        } else {
            base = process->mmap_anon_base;
            process->mmap_anon_base += aligned_size;
        }

        process->pagemap->anon_map(base, aligned_size, mmap_prot_to_page_flags(prot));

        if (!(flags & MAP_ANONYMOUS)) {
            vfs::FileDescription *description = vfs::get_file_description(fd);
            if (!description)
                return -EBADF;
            return description->vnode->mmap(description, base, length, offset, prot, flags);
        }

#if SYSCALL_TRACE
        klib::printf(" = %#lX", base);
#endif
        return base;
    }
    
    isize syscall_munmap(void *addr, usize length) {
#if SYSCALL_TRACE
        klib::printf("munmap(%#lX, %ld)\n", (uptr)addr, length);
#endif
        // klib::printf("munmap() is a stub\n");
        return 0;
    }
}
