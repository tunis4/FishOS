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

namespace vmm {
    uptr hhdm;
    static uptr hhdm_end;
    static uptr kernel_phy_base;
    static uptr kernel_virt_base;
    uptr heap_base;
    usize heap_size;
    static uptr kernel_virt_alloc_base;

    Pagemap kernel_pagemap;
    Pagemap *active_pagemap;
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
        hhdm_end = klib::align_up(hhdm_end, 0x1000);
        heap_base = hhdm_end;
        heap_size = (usize)4 * 1024 * 1024 * 1024; // 4 GiB
        kernel_virt_alloc_base = heap_base + heap_size;

        kernel_pagemap.pml4 = (u64*)(pmm::alloc_pages(1) + hhdm);
        memset(kernel_pagemap.pml4, 0, 0x1000);

        usize kernel_size = 0;

        klib::printf("VMM: Physical memory map:\n");
        for (usize e = 0; e < memmap_res->entry_count; e++) {
            auto *entry = memmap_res->entries[e];
            const char *entry_name;
            u64 flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_GLOBAL;

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

        kernel_pagemap.map_pages(kernel_phy_base, kernel_virt_base, kernel_size, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL);

        new (&kernel_hhdm_range) MappedRange(0, hhdm_base, hhdm_end - hhdm_base, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL, MappedRange::Type::DIRECT);
        new (&kernel_heap_range) MappedRange(0, heap_base, heap_size, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_GLOBAL, MappedRange::Type::ANONYMOUS);

        kernel_pagemap.range_list.init();
        kernel_pagemap.range_list.add(&kernel_hhdm_range.range_link);
        kernel_pagemap.range_list.add(&kernel_heap_range.range_link);
        kernel_pagemap.activate();
    }

    MappedRange::MappedRange(uptr phy_base, uptr base, usize length, u64 page_flags, Type type) 
        : phy_base(phy_base), base(base), length(length), page_flags(page_flags), type(type)
    {
        page_list.init();
    }

    MappedRange::~MappedRange() {
        range_link.remove();

        pmm::Page *page;
        LIST_FOR_EACH_SAFE(page, &page_list, link)
            pmm::free_page(page);
    }

    void MappedRange::print() {
        klib::printf("%#lX-%#lX ", base, base + length);
        klib::printf("r%c%c", (page_flags & PAGE_WRITABLE) ? 'w' : '-', !(page_flags & PAGE_NO_EXECUTE) ? 'x' : '-');
        if (type == Type::ANONYMOUS)
            klib::printf(" anonymous\n");
        else if (type == Type::DIRECT)
            klib::printf(" direct to %#lX\n", phy_base);
    }

    Pagemap::~Pagemap() {
        MappedRange *range;
        LIST_FOR_EACH_SAFE(range, &range_list, range_link)
            delete range;
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
        usize num_pages = klib::align_up(size, 0x1000) / 0x1000;
        // klib::printf("Mapping %ld pages phy %#lX virt %#lX end %#lX\n", num_pages, phy, virt, phy + num_pages * 0x1000);
        for (usize i = 0; i < num_pages; i++)
            map_page(phy + (i * 0x1000), virt + (i * 0x1000), flags);
    }

    void Pagemap::map_kernel() {
        klib::LockGuard guard(this->lock);
        for (usize i = 256; i < 512; i++)
            pml4[i] = kernel_pagemap.pml4[i];
    }

    void Pagemap::activate() {
        cpu::write_cr3(uptr(pml4) - hhdm);
        active_pagemap = this;
    }

    isize Pagemap::get_physical_addr(uptr virt) {
        auto get_actual = [&] -> uptr {
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
        };

        uptr phy = get_actual();
        if (phy)
            return phy;
        else
            return handle_page_fault(virt);
    }

    MappedRange* Pagemap::addr_to_range(uptr virt) {
        // first check current pagemap
        MappedRange *range;
        LIST_FOR_EACH(range, &this->range_list, range_link)
            if (virt >= range->base && virt < range->base + range->length)
                return range;

        if (this == &kernel_pagemap)
            return nullptr;

        // otherwise also check kernel pagemap
        LIST_FOR_EACH(range, &kernel_pagemap.range_list, range_link)
            if (virt >= range->base && virt < range->base + range->length)
                return range;

        return nullptr;
    }

    // returns errno if the page fault couldnt be handled
    isize Pagemap::handle_page_fault(uptr virt) {
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
                return -EFAULT;

            switch (range->type) {
            case MappedRange::Type::ANONYMOUS: {
                pmm::Page *new_page = pmm::alloc_page();
                new_page->mapped_addr = klib::align_down(virt, 0x1000);
                range->page_list.add_before(&new_page->link);

                uptr phy = new_page->pfn * 0x1000;
                memset((void*)(phy + hhdm), 0, 0x1000);
                *entry = phy | range->page_flags;
                return phy;
            }
            case MappedRange::Type::DIRECT: {
                uptr phy = virt - range->base + range->phy_base;
                *entry = (phy & 0x000FFFFFFFFFF000) | range->page_flags;
                return phy;
            }
            default:
                klib::printf("Unknown mapped range type: %#lX\n", u64(range->type));
                return -EFAULT;
            }
        }
        return -EFAULT;
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

                    for (usize i = 0; i < 512; i++)
                        if (!(pml1[i] & PAGE_PRESENT))
                            forked_pml1[i] = pml1[i];
                }
            }
        }

        forked->range_list.init();
        MappedRange *old_range;
        LIST_FOR_EACH(old_range, &range_list, range_link) {
            // old_range->print();
            MappedRange *new_range = new MappedRange(old_range->phy_base, old_range->base, old_range->length, old_range->page_flags, old_range->type);
            forked->range_list.add_before(&new_range->range_link);

            ASSERT(new_range->type == MappedRange::Type::ANONYMOUS);

            pmm::Page *old_page;
            LIST_FOR_EACH(old_page, &old_range->page_list, link) {
                pmm::Page *new_page = pmm::alloc_page();
                new_page->mapped_addr = old_page->mapped_addr;
                new_range->page_list.add_before(&new_page->link);

                uptr new_phy = new_page->pfn * 0x1000;
                uptr old_phy = old_page->pfn * 0x1000;

                // klib::printf("%#lX\n", old_page->mapped_addr);
                forked->map_page(new_phy, new_page->mapped_addr, new_range->page_flags);
                memcpy((void*)(new_phy + hhdm), (void*)(old_phy + hhdm), 0x1000);
            }
        }

        return forked;
    }

    void Pagemap::map_range(uptr base, usize length, u64 page_flags, MappedRange::Type type, uptr phy_base) {
#ifndef NDEBUG
        MappedRange *other;
        LIST_FOR_EACH(other, &this->range_list, range_link)
            if (base < other->base + other->length && base + length > other->base)
                panic("New MappedRange { base = %#lX, length = %#lX } collides with existing MappedRange { base = %#lX, length = %#lX }", base, length, other->base, other->length);
#endif
        ASSERT(page_flags & PAGE_PRESENT);

        MappedRange *range = new MappedRange(phy_base, base, length, page_flags, type);
        this->range_list.add(&range->range_link);
    }

    uptr virt_alloc(usize length) {
        uptr base = kernel_virt_alloc_base;
        kernel_virt_alloc_base += klib::align_up(length, 0x1000);
        return base;
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
        usize aligned_size = klib::align_up(length, 0x1000);
        if (flags & MAP_FIXED) {
            base = (uptr)addr;
            process->mmap_anon_base = klib::align_up(klib::max(process->mmap_anon_base, base + aligned_size), 0x1000);
        } else {
            base = process->mmap_anon_base;
            process->mmap_anon_base += aligned_size;
        }

        process->pagemap->map_range(base, aligned_size, mmap_prot_to_page_flags(prot), MappedRange::Type::ANONYMOUS);

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
        sched::Process *process = cpu::get_current_thread()->process;

        MappedRange *range = process->pagemap->addr_to_range((uptr)addr);
        if (range->length != length) {
            klib::printf("unsupported partial munmap (unmap length: %#lX, mapped length: %#lX)\n", length, range->length);
            return 0;
        }

        process->pagemap->map_pages(0, range->base, range->length, 0);
        delete range;
        return 0;
    }
}
