#pragma once

#include <klib/list.hpp>

// incomplete and simplified implementation of the VMem resource allocator
// https://www.usenix.org/legacy/publications/library/proceedings/usenix01/full_papers/bonwick/bonwick.pdf
namespace mem::vmem {
    struct BoundaryTag {
        enum class Type {
            FREE, ALLOCATED, SPAN
        };

        uptr base;
        usize size;
        Type type;

        klib::ListHead tag_list; // either in an arena's tag_list_head or in the internal free_tags_list if its not a part of an arena yet
        klib::ListHead list; // freelist if its free, hash bucket if its allocated, span_list if its a span
    };

    struct Arena {
        static constexpr usize num_freelists = klib::NumericLimits<uptr>::bits;
        static constexpr usize num_hash_buckets = 16;

        char name[64]; // identifier for debugging
        usize quantum; // minimum allocation size

        klib::ListHead tag_list_head; // lists every tag
        klib::ListHead span_list_head; // lists every span tag
        klib::ListHead freelist_heads[num_freelists]; // free segments are put into a freelist depending on their size
        klib::ListHead hash_table[num_hash_buckets]; // allocated segments are put into this hash table

        // no initial span created
        void init(const char *name, usize quantum);

        // adds a span
        void add(uptr base, usize size);

        uptr xalloc(usize size);

        // size is actually not necessary, but its used for a sanity check if you provide it
        void xfree(uptr addr, usize size = 0);

        static usize freelist_index_for_size(usize size);

    private:
        void freelist_insert(BoundaryTag *segment);
        void expand_free_segment(BoundaryTag *segment, uptr new_base, usize new_size);

        klib::ListHead* hash_bucket_for_addr(uptr addr);
        void hash_table_insert(BoundaryTag *segment);
    };

    void early_init();
}
