#include <userland/elf.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdio.hpp>
#include <klib/algorithm.hpp>
#include <mem/vmm.hpp>
#include <mem/pmm.hpp>
#include <errno.h>

namespace elf {
    isize load(vmm::Pagemap *pagemap, vfs::VNode *file, uptr load_base, char **ld_path, Auxval *auxv, uptr *first_free_virt) {
        ASSERT(auxv);
        ASSERT(first_free_virt);

        Header header {};
        file->read(nullptr, &header, sizeof(Header), 0);

        if (memcmp(header.identifier, "\177ELF", 4))
            return -ENOEXEC;

        if (header.identifier[EI_CLASS] != ELFCLASS64 || header.identifier[EI_DATA] != ELFDATA2LSB || header.identifier[EI_OSABI] != ELFOSABI_SYSV || header.arch != EM_X86_64)
            return -ENOEXEC;

        for (usize i = 0; i < header.ph_count; i++) {
            ProgramHeader ph {};
            file->read(nullptr, &ph, sizeof(ProgramHeader), header.ph_table_offset + i * header.ph_entry_size);

            switch (ph.type) {
            case PT_LOAD: {
                u64 page_flags = PAGE_PRESENT | PAGE_USER;

                // if (ph.flags & PF_W)
                    page_flags |= PAGE_WRITABLE;
                // if (!(ph.flags & PF_X))
                //     page_flags |= PAGE_NO_EXECUTE;

                usize misalign = ph.virt_addr & 0xFFF;
                usize mem_page_count = (ph.mem_size + misalign + 0x1000 - 1) / 0x1000;
                usize file_page_count = (ph.file_size + misalign + 0x1000 - 1) / 0x1000;

                usize total_read_from_file = 0;
                
                for (usize i = 0; i < mem_page_count; i++) {
                    uptr page_phy = pmm::alloc_pages(1);
                    uptr page_virt = load_base + ph.virt_addr + (i * 0x1000);
                    // klib::printf("mapping %#lX to %#lX\n", page_phy, page_virt);
                    pagemap->map_page(page_phy, page_virt, page_flags);
                    uptr dst = page_phy + vmm::hhdm;
                    memset((void*)dst, 0, 0x1000);
                    if (i < file_page_count) {
                        uptr offset = ph.offset + (i * 0x1000);
                        uptr size = 0x1000;
                        if (i == 0) {
                            dst += misalign;
                            size -= misalign;
                        } else {
                            offset -= misalign;
                        }
                        if (i == file_page_count - 1)
                            size = ph.file_size - total_read_from_file;
                        // klib::printf("reading %#lX bytes from %#lX to %#lX\n", size, offset, dst);
                        file->read(nullptr, (void*)dst, size, offset);
                        total_read_from_file += size;
                    }
                    if (page_virt >= *first_free_virt)
                        *first_free_virt = page_virt + 0x1000;
                }

                ASSERT(total_read_from_file == ph.file_size);
                
                break;
            }
            case PT_PHDR:
                auxv->at_phdr = load_base + ph.virt_addr;
                break;
            case PT_INTERP:
                if (ld_path) {
                    char *path = new char[ph.file_size + 1];
                    file->read(nullptr, (void*)path, ph.file_size, ph.offset);
                    *ld_path = path;
                }
                break;
            }
        }

        *first_free_virt = klib::align_up<uptr, 0x1000>(*first_free_virt);

        auxv->at_entry = load_base + header.entry_addr;
        auxv->at_phent = header.ph_entry_size;
        auxv->at_phnum = header.ph_count;
        return 0;
    }
}
