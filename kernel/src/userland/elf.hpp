#pragma once

#include <klib/common.hpp>
#include <mem/vmm.hpp>
#include <fs/vfs.hpp>
#include <elf.h>

namespace elf {
    struct [[gnu::packed]] Header {
        u8  identifier[16]; // magic number and other info
        u16 type;
        u16 arch;
        u32 version;
        u64 entry_addr;
        u64 ph_table_offset; // offset in file
        u64 sh_table_offset; // offset in file
        u32 flags; // arch specific flags
        u16 eh_size;
        u16 ph_entry_size;
        u16 ph_count;
        u16 sh_entry_size;
        u16 sh_count;
        u16 sh_str_table_index;
    };

    struct [[gnu::packed]] ProgramHeader {
        u32 type;
        u32 flags;
        u64 offset; // file offset
        u64 virt_addr;
        u64 phy_addr;
        u64 file_size; // segment size in file
        u64 mem_size; // segment size in memory
        u64 alignment;
    };

    struct Auxval {
        u64 at_entry;
        u64 at_phdr;
        u64 at_phent;
        u64 at_phnum;
    };

    // first_free_virt will hold the first virtual address that is free (used for anon mmap)
    isize load(mem::Pagemap *pagemap, vfs::VNode *file, uptr load_base, char **ld_path, Auxval *auxv, uptr *first_free_virt);
}
