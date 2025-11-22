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

namespace mem {
    VMM *vmm;
    uptr hhdm;

    void VMM::init(uptr hhdm_base, limine_memmap_response *memmap_res, limine_kernel_address_response *kernel_addr_res) {
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
        heap_size = (usize)12 * 1024 * 1024 * 1024;
        kernel_virt_alloc_base = heap_base + heap_size;

        new (&kernel_pagemap) Pagemap();
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

            klib::printf("%12lx - %12lx    (%10ld KiB)  %s\n", entry->base, entry->base + entry->length, entry->length / 1024, entry_name);

            if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE || entry->type == LIMINE_MEMMAP_FRAMEBUFFER || entry->type == LIMINE_MEMMAP_USABLE || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
                kernel_pagemap.map_pages(entry->base, entry->base + hhdm, entry->length, flags);
        }

        klib::printf("VMM: Kernel base addresses | phy: %#lX, virt: %#lX\n", kernel_phy_base, kernel_virt_base);

        kernel_pagemap.map_pages(kernel_phy_base, kernel_virt_base, kernel_size, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL);

        new (&kernel_hhdm_range) MappedRange(hhdm_base, hhdm_end - hhdm_base, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL, MappedRange::Type::DIRECT);
        kernel_hhdm_range.phy_base = 0;
        new (&kernel_heap_range) MappedRange(heap_base, heap_size, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_GLOBAL, MappedRange::Type::ANONYMOUS);

        kernel_pagemap.range_list.add_before(&kernel_hhdm_range.range_link);
        kernel_pagemap.range_list.add_before(&kernel_heap_range.range_link);
        kernel_pagemap.activate();
    }

    MappedRange::MappedRange(uptr base, usize length, u64 page_flags, Type type) 
        : base(base), length(length), page_flags(page_flags), type(type)
    {
        page_list.init();
    }

    MappedRange::~MappedRange() {
        if (!range_link.is_invalid())
            range_link.remove();

        pmm::Page *page;
        LIST_FOR_EACH_SAFE(page, &page_list, link)
            pmm::free_page(page);

        if (file)
            file->decrement_ref_count();
    }

    Pagemap::Pagemap() {
        range_list.init();
        page_table_pages_list.init();
        pml4 = (u64*)(alloc_page_for_page_table() + hhdm);
    }

    Pagemap::~Pagemap() {
        {
            MappedRange *range;
            LIST_FOR_EACH_SAFE(range, &range_list, range_link)
                delete range;
        }
        {
            pmm::Page *page;
            LIST_FOR_EACH_SAFE(page, &page_table_pages_list, link)
                pmm::free_page(page);
        }
    }

    // returns physical address
    uptr Pagemap::alloc_page_for_page_table() {
        pmm::Page *new_page = pmm::alloc_page();
        page_table_pages_list.add_before(&new_page->link);
        return new_page->pfn * 0x1000;
    }

    u64* Pagemap::create_next_page_table(u64 *current_entry) {
        uptr new_page = alloc_page_for_page_table();
        memset((void*)(new_page + hhdm), 0, 0x1000);
        *current_entry = new_page | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        return (u64*)(new_page + hhdm);
    }

    void Pagemap::map_page(uptr phy, uptr virt, u64 flags) {
        klib::SpinlockGuard guard(this->lock);
        u64 *entry = find_page_table_entry(virt, true);
        bool replaced = *entry != 0;
        *entry = (phy & 0x000FFFFFFFFFF000) | flags;
        if (replaced) cpu::invlpg((void*)virt);
    }

    void Pagemap::map_pages(uptr phy, uptr virt, usize size, u64 flags) {
        usize num_pages = klib::align_up(size, 0x1000) / 0x1000;
        for (usize i = 0; i < num_pages; i++)
            map_page(phy + (i * 0x1000), virt + (i * 0x1000), flags);
    }

    void Pagemap::map_kernel() {
        klib::SpinlockGuard guard(this->lock);
        for (usize i = 256; i < 512; i++)
            pml4[i] = vmm->kernel_pagemap.pml4[i];
    }

    void Pagemap::activate() {
        if (this != vmm->active_pagemap || this == &vmm->kernel_pagemap) {
            cpu::write_cr3(uptr(pml4) - hhdm);
            vmm->active_pagemap = this;
        }
    }

    u64* Pagemap::find_page_table_entry(uptr virt, bool create_missing) {
        u64 *current_table = this->pml4;

        u64 *current_entry = &current_table[(virt >> 39) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else if (create_missing) current_table = create_next_page_table(current_entry);
        else return nullptr;
        current_entry = &current_table[(virt >> 30) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else if (create_missing) current_table = create_next_page_table(current_entry);
        else return nullptr;
        current_entry = &current_table[(virt >> 21) & 0x1FF];
        if (*current_entry & PAGE_PRESENT) 
            current_table = (u64*)((*current_entry & 0x000FFFFFFFFFF000) + hhdm);
        else if (create_missing) current_table = create_next_page_table(current_entry);
        else return nullptr;

        return &current_table[virt >> 12 & 0x1FF];
    }

    isize Pagemap::get_physical_addr(uptr virt) {
        u64 *entry = find_page_table_entry(virt);
        if (entry && (*entry & 0x000FFFFFFFFFF000))
            return *entry & 0x000FFFFFFFFFF000;
        else
            return handle_page_fault(virt);
    }

    isize Pagemap::access_memory(uptr virt, void *target, usize count, bool write) {
        uptr start = virt, end = virt + count;
        uptr start_page_virt = klib::align_down(start, 0x1000);
        uptr end_page_virt = klib::align_down(end, 0x1000);

        usize transferred = 0;
        for (uptr page_virt = start_page_virt; page_virt <= end_page_virt; page_virt += 0x1000) {
            uptr page_ptr = hhdm;
            usize bytes_in_page = 0x1000;
            if (page_virt < start) {
                usize offset = start - page_virt;
                page_ptr += offset;
                bytes_in_page -= offset;
            }
            if (page_virt + 0x1000 >= end) {
                bytes_in_page -= page_virt + 0x1000 - end;
            }

            if (bytes_in_page == 0)
                return transferred;

            isize page_phy = get_physical_addr(page_virt);
            if (page_phy == -EFAULT)
                return transferred ? transferred : -EFAULT;
            page_ptr += page_phy;

            if (write)
                memcpy((void*)page_ptr, (u8*)target + transferred, bytes_in_page);
            else
                memcpy((u8*)target + transferred, (void*)page_ptr, bytes_in_page);

            transferred += bytes_in_page;
        }
        return transferred;
    }

    MappedRange* Pagemap::addr_to_range(uptr virt) {
        if (cached_range_lookup && virt >= cached_range_lookup->base && virt < cached_range_lookup->end())
            return cached_range_lookup;

        MappedRange *range;
        LIST_FOR_EACH(range, &this->range_list, range_link)
            if (virt >= range->base && virt < range->end())
                return cached_range_lookup = range;

        if (this == &vmm->kernel_pagemap)
            return nullptr;

        LIST_FOR_EACH(range, &vmm->kernel_pagemap.range_list, range_link)
            if (virt >= range->base && virt < range->end())
                return cached_range_lookup = range;

        return nullptr;
    }

    // returns EFAULT if the page fault couldnt be handled
    isize Pagemap::handle_page_fault(uptr virt) {
        // klib::SpinlockGuard guard(this->lock);

        uptr page_virt = klib::align_down(virt, 0x1000);
        u64 *entry = find_page_table_entry(virt, true);

        if (!(*entry & PAGE_PRESENT)) {
            MappedRange *range = addr_to_range(virt);
            if (range == nullptr)
                return -EFAULT;

            switch (range->type) {
            case MappedRange::Type::ANONYMOUS: {
                pmm::Page *new_page = pmm::alloc_page();
                new_page->mapped_addr = page_virt;
                range->page_list.add_before(&new_page->link);

                uptr phy = new_page->pfn * 0x1000;
                memset((void*)(phy + hhdm), 0, 0x1000);
                *entry = phy | range->page_flags;
                return phy;
            }
            case MappedRange::Type::DIRECT: {
                uptr phy = page_virt - range->base + range->phy_base;
                *entry = (phy & 0x000FFFFFFFFFF000) | range->page_flags;
                return phy;
            }
            case MappedRange::Type::FILE: {
                pmm::Page *new_page = pmm::alloc_page();
                new_page->mapped_addr = page_virt;
                range->page_list.add_before(&new_page->link);

                uptr phy = new_page->pfn * 0x1000;
                void *ptr = (void*)(phy + hhdm);
                usize offset = page_virt - range->base + range->file_offset;

                memset(ptr, 0, 0x1000);
                range->file->vnode->read(nullptr, ptr, 0x1000, offset);

                *entry = phy | range->page_flags;
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
        Pagemap *forked = new Pagemap();

        memcpy(forked->pml4, this->pml4, 0x1000);
        for (usize i = 256; i < 512; i++) // higher half
            forked->pml4[i] = this->pml4[i];

        for (usize i = 0; i < 256; i++) { // lower half
            if (!(this->pml4[i] & PAGE_PRESENT)) {
                forked->pml4[i] = this->pml4[i];
                continue;
            }

            u64 *pml3 = (u64*)((this->pml4[i] & 0x000FFFFFFFFFF000) + hhdm);

            uptr new_page = alloc_page_for_page_table();
            memset((void*)(new_page + hhdm), 0, 0x1000);
            forked->pml4[i] = new_page | (pml4[i] & ~0x000FFFFFFFFFF000);
            u64 *forked_pml3 = (u64*)(new_page + hhdm);

            for (usize i = 0; i < 512; i++) {
                if (!(pml3[i] & PAGE_PRESENT)) {
                    forked_pml3[i] = pml3[i];
                    continue;
                }

                u64 *pml2 = (u64*)((pml3[i] & 0x000FFFFFFFFFF000) + hhdm);

                uptr new_page = alloc_page_for_page_table();
                memset((void*)(new_page + hhdm), 0, 0x1000);
                forked_pml3[i] = new_page | (pml3[i] & ~0x000FFFFFFFFFF000);
                u64 *forked_pml2 = (u64*)(new_page + hhdm);

                for (usize i = 0; i < 512; i++) {
                    if (!(pml2[i] & PAGE_PRESENT)) {
                        forked_pml2[i] = pml2[i];
                        continue;
                    }

                    u64 *pml1 = (u64*)((pml2[i] & 0x000FFFFFFFFFF000) + hhdm);

                    uptr new_page = alloc_page_for_page_table();
                    memset((void*)(new_page + hhdm), 0, 0x1000);
                    forked_pml2[i] = new_page | (pml2[i] & ~0x000FFFFFFFFFF000);
                    u64 *forked_pml1 = (u64*)(new_page + hhdm);

                    for (usize i = 0; i < 512; i++)
                        if (!(pml1[i] & PAGE_PRESENT))
                            forked_pml1[i] = pml1[i];
                }
            }
        }

        MappedRange *old_range;
        LIST_FOR_EACH(old_range, &range_list, range_link) {
            auto *new_range = forked->add_range(old_range->base, old_range->length, old_range->page_flags, old_range->type, old_range->phy_base, old_range->file, old_range->file_offset, false, false);

            if (new_range->type == MappedRange::Type::ANONYMOUS || new_range->type == MappedRange::Type::FILE) {
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
            } else {
                ASSERT(new_range->type == MappedRange::Type::DIRECT);
            }
        }

        return forked;
    }

    void Pagemap::assert_consistency() {
        MappedRange *previous = nullptr;
        LIST_FOR_EACH_SAFE(previous, &this->range_list, range_link) {
            if (&next->range_link == &this->range_list)
                break;
            if (next->base < previous->end())
                panic("Pagemap ranges not in order or overlapping");
        }
    }

    MappedRange* Pagemap::add_range(uptr base, usize length, u64 page_flags, MappedRange::Type type, uptr phy_base, vfs::FileDescription *file,
        usize file_offset, bool merge, bool resolve_overlap, bool keep_pages)
    {
        klib::InterruptLock interrupt_guard;
        uptr end = base + length;

        cached_range_lookup = nullptr;

        bool had_overlap = false;
        bool need_to_resolve_overlap = resolve_overlap;
        while (need_to_resolve_overlap) { // loop until all overlaps are resolved
            need_to_resolve_overlap = false;
            MappedRange *existing;
            LIST_FOR_EACH_SAFE(existing, &this->range_list, range_link) {
                if (base >= existing->end() || end <= existing->base)
                    continue; // no overlap

                if (base <= existing->base) {
                    if (end >= existing->end()) {
                        if (keep_pages && base == existing->base && end == existing->end() && type == existing->type) {
                            if (existing->page_flags != page_flags) {
                                existing->page_flags = page_flags;
                                invalidate_pages(existing);
                            }
                            break;
                        } else {
                            delete existing;
                        }
                    } else {
                        usize overlap = end - existing->base;
                        existing->base += overlap;
                        existing->length -= overlap;
                    }
                } else {
                    if (end >= existing->end()) {
                        existing->length -= existing->end() - base;
                    } else {
                        // need to split existing range
                        usize base2 = end;
                        usize length2 = existing->end() - end;

                        existing->length = base - existing->base;

                        auto *split = add_range(base2, length2, existing->page_flags, existing->type, existing->phy_base, existing->file, existing->file_offset, false, false);
                        if (split)
                            invalidate_pages(split);
                    }
                }
                had_overlap = true;
                need_to_resolve_overlap = true;
            }
        }

        if (merge && type == MappedRange::Type::ANONYMOUS) {
            MappedRange *existing;
            LIST_FOR_EACH_SAFE(existing, &this->range_list, range_link) {
                if (type == existing->type && page_flags == existing->page_flags) {
                    if (base == existing->end()) {
                        existing->length += length;
                    } else if (end == existing->base) {
                        existing->base -= length;
                        existing->length += length;
                    } else {
                        continue;
                    }
                    // invalidate_pages(existing); // FIXME: unsure if this is necessary
                    return nullptr;
                }
            }
        }

#if 0
        {
            MappedRange *other;
            LIST_FOR_EACH(other, &this->range_list, range_link)
                if (base < other->base + other->length && base + length > other->base)
                    panic("New MappedRange { base = %#lX, length = %#lX } overlaps with existing MappedRange { base = %#lX, length = %#lX }", base, length, other->base, other->length);
        }
        assert_consistency();
#endif

        if (type == MappedRange::Type::NONE) {
            invalidate_pages(base, length);
            return nullptr;
        }

        MappedRange *new_range = new MappedRange(base, length, page_flags, type);
        new_range->phy_base = phy_base;
        new_range->file = file;
        new_range->file_offset = file_offset;
        if (new_range->file)
            new_range->file->increment_ref_count();

        MappedRange *previous = nullptr;
        LIST_FOR_EACH_SAFE(previous, &this->range_list, range_link) {
            if (&next->range_link == &this->range_list)
                break;
            if (next->base > base)
                break;
        }
        previous->range_link.add(&new_range->range_link);

        if (had_overlap)
            invalidate_pages(new_range);
        return new_range;
    }

    void Pagemap::map_anonymous(uptr base, usize length, u64 page_flags) {
        ASSERT(page_flags & PAGE_PRESENT);
        add_range(base, length, page_flags, MappedRange::Type::ANONYMOUS, 0, nullptr, 0, true, true, true);
    }

    void Pagemap::map_direct(uptr base, usize length, u64 page_flags, uptr phy_base) {
        ASSERT(page_flags & PAGE_PRESENT);
        add_range(base, length, page_flags, MappedRange::Type::DIRECT, phy_base, nullptr, 0);
    }

    void Pagemap::map_file(uptr base, usize length, u64 page_flags, vfs::FileDescription *file, usize file_offset) {
        ASSERT(page_flags & PAGE_PRESENT);
        add_range(base, length, page_flags, MappedRange::Type::FILE, 0, file, file_offset);
    }

    void Pagemap::invalidate_page(uptr virt, MappedRange *range) {
        u64 *entry = find_page_table_entry(virt);
        if (!entry || !(*entry & PAGE_PRESENT)) return;

        u64 phy = (*entry & 0x000FFFFFFFFFF000);
        pmm::Page *page = pmm::find_page(phy);
        if (page->free) // FIXME
            page = nullptr;
        if (page)
            page->link.remove();

        if (range) {
            ASSERT(range->page_flags & PAGE_PRESENT);
            if (!page)
                page = pmm::alloc_page();
            page->mapped_addr = virt;
            range->page_list.add_before(&page->link);
            *entry = phy | range->page_flags;
        } else {
            *entry = 0;
            if (page)
                pmm::free_page(page);
        }

        cpu::invlpg((void*)virt);
    }

    void Pagemap::invalidate_pages(uptr base, usize length, MappedRange *range) {
        usize num_pages = klib::align_up(length, 0x1000) / 0x1000;
        for (usize i = 0; i < num_pages; i++) {
            uptr virt = base + (i * 0x1000);
            invalidate_page(virt, range);
        }
    }

    uptr VMM::virt_alloc(usize length) {
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
        log_syscall("mmap(%#lX, %#lX, %d, %d, %d, %#lX)\n", (uptr)addr, length, prot, flags, fd, offset);
        if (length == 0) return -EINVAL;
        sched::Process *process = cpu::get_current_thread()->process;

        klib::InterruptLock interrupt_guard;

        uptr base;
        usize aligned_size = klib::align_up(length, 0x1000);
        if (flags & MAP_FIXED) {
            base = (uptr)addr;
            process->mmap_anon_base = klib::align_up(klib::max(process->mmap_anon_base, base + aligned_size), 0x1000);
        } else {
            base = process->mmap_anon_base;
            process->mmap_anon_base += aligned_size;
        }

        if (flags & MAP_ANONYMOUS) {
            if (flags & MAP_SHARED)
                klib::printf("mmap: shared anonymous mapping not supported\n");
            process->pagemap->map_anonymous(base, aligned_size, mmap_prot_to_page_flags(prot));
        } else {
            vfs::FileDescription *description = vfs::get_file_description(fd);
            if (!description)
                return -EBADF;
            if (!description->can_read())
                return -EACCES;
            if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !description->can_write())
                return -EACCES;
            return description->vnode->mmap(description, base, aligned_size, offset, prot, flags);
        }

        return base;
    }

    isize syscall_munmap(void *addr, usize length) {
        log_syscall("munmap(%#lX, %#lX)\n", (uptr)addr, length);
        sched::Process *process = cpu::get_current_thread()->process;

        if ((uptr)addr % 0x1000 != 0)
            return -EINVAL;
        length = klib::align_up(length, 0x1000);

        klib::InterruptLock interrupt_guard;

        process->pagemap->add_range((uptr)addr, length, 0, MappedRange::Type::NONE, 0, nullptr, 0, false, true, false);

        if (process->mmap_anon_base == (uptr)addr + length)
            process->mmap_anon_base = (uptr)addr;
        return 0;
    }

    isize syscall_mprotect(void *addr, usize length, int prot) {
        log_syscall("mprotect(%#lX, %#lX, %d)\n", (uptr)addr, length, prot);
        sched::Process *process = cpu::get_current_thread()->process;

        if ((uptr)addr % 0x1000 != 0)
            return -EINVAL;
        length = klib::align_up(length, 0x1000);

        klib::InterruptLock interrupt_guard;

        MappedRange *existing = process->pagemap->addr_to_range((uptr)addr);
        process->pagemap->add_range((uptr)addr, length, mmap_prot_to_page_flags(prot), existing->type, existing->phy_base, existing->file, existing->file_offset, true, true, true);
        return 0;
    }

    isize syscall_mincore(void *addr, usize length, u8 *vec) {
        log_syscall("mincore(%#lX, %#lX, %#lX)\n", (uptr)addr, length, (uptr)vec);
        sched::Process *process = cpu::get_current_thread()->process;

        if ((uptr)addr % 0x1000 != 0)
            return -EINVAL;
        length = klib::align_up(length, 0x1000);
        usize num_pages = length / 0x1000;

        klib::InterruptLock interrupt_guard;

        for (usize i = 0; i < num_pages; i++) {
            uptr page = (uptr)addr + i * 0x1000;
            MappedRange *range = process->pagemap->addr_to_range(page);
            if (range == nullptr)
                return -ENOMEM;
            vec[i] = 1; // FIXME: need to actually check residency
        }
        return 0;
    }

    isize syscall_madvise(void *addr, usize length, int advice) {
        log_syscall("madvise(%#lX, %#lX, %d)\n", (uptr)addr, length, advice);
        sched::Process *process = cpu::get_current_thread()->process;

        if ((uptr)addr % 0x1000 != 0)
            return -EINVAL;
        length = klib::align_up(length, 0x1000);

        klib::InterruptLock interrupt_guard;

        switch (advice) {
        case MADV_NORMAL:
        case MADV_RANDOM:
        case MADV_SEQUENTIAL:
        case MADV_WILLNEED:
        case MADV_DONTDUMP:
        case MADV_DODUMP:
            return 0; // these operations are safe to ignore
        case MADV_FREE: // FIXME: not actually equivalent
        case MADV_DONTNEED: {
            usize num_pages = length / 0x1000;
            for (usize i = 0; i < num_pages; i++) {
                uptr page = (uptr)addr + i * 0x1000;
                process->pagemap->invalidate_page(page);
            }
        } return 0;
        default:
            klib::printf("madvise: unsupported advice %d\n", advice);
            return -EINVAL;
        }
    }
}
