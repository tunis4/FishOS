#include <mem/vmem.hpp>
#include <klib/lock.hpp>
#include <klib/cstring.hpp>
#include <klib/cstdio.hpp>
#include <panic.hpp>

namespace mem::vmem {
    constexpr usize num_static_tags = 128;
    static BoundaryTag static_tags[num_static_tags];
    static klib::ListHead free_tags_list_head;
    static klib::Spinlock free_tags_lock;
    static usize num_free_tags = 0;

    void early_init() {
        free_tags_list_head.init();
        for (usize i = 0; i < num_static_tags; i++)
            free_tags_list_head.add_before(&static_tags[i].tag_list);
        num_free_tags += num_static_tags;
    }

    BoundaryTag* allocate_tag() {
        klib::LockGuard guard(free_tags_lock);
        ASSERT(!free_tags_list_head.is_empty());
        BoundaryTag *tag = LIST_ENTRY(free_tags_list_head.next, BoundaryTag, tag_list);
        tag->tag_list.remove();
        num_free_tags--;
        return tag;
    }

    void free_tag(BoundaryTag *tag) {
        klib::LockGuard guard(free_tags_lock);
        free_tags_list_head.add_before(&tag->tag_list);
        num_free_tags++;
    }

    // TODO: function to refill free tags list

    void Arena::init(const char *name, usize quantum) {
        klib::strcpy(this->name, name);
        this->quantum = quantum;

        tag_list_head.init();
        span_list_head.init();
        for (usize i = 0; i < num_freelists; i++)
            freelist_heads[i].init();
        for (usize i = 0; i < num_hash_buckets; i++)
            hash_table[i].init();
    }
        
    void Arena::add(uptr base, usize size) {
        BoundaryTag *previous_span = nullptr;
        LIST_FOR_EACH(previous_span, &span_list_head, list)
            if (previous_span->base >= base)
                break;

        BoundaryTag *new_span = allocate_tag();
        new_span->type = BoundaryTag::Type::SPAN;
        new_span->base = base;
        new_span->size = size;

        BoundaryTag *new_segment = allocate_tag();
        new_segment->type = BoundaryTag::Type::FREE;
        new_segment->base = base;
        new_segment->size = size;

        if (previous_span) {
            klib::ListHead *next = previous_span->list.next;
            previous_span->list.add(&new_span->list);
            if (next != &span_list_head) {
                BoundaryTag *next_span = LIST_ENTRY(next, BoundaryTag, list);
                next_span->tag_list.add_before(&new_span->tag_list);
            } else {
                tag_list_head.add_before(&new_span->tag_list);
            }
        } else { // this is the first span
            span_list_head.add(&new_span->list);
            tag_list_head.add(&new_span->tag_list);
        }

        new_span->tag_list.add(&new_segment->tag_list);
        freelist_insert(new_segment);
    }

    uptr Arena::xalloc(usize size) {
        ASSERT(size != 0);
        ASSERT(size % quantum == 0);

        usize freelist_index = freelist_index_for_size(size);
        if (size & (size - 1)) // if size is not a power of 2
            freelist_index++;
        
        for (usize i = freelist_index; i < num_freelists; i++) {
            klib::ListHead *freelist = &freelist_heads[i];
            if (freelist->is_empty())
                continue;
            
            BoundaryTag *segment = LIST_ENTRY(freelist->next, BoundaryTag, list);
            segment->list.remove();

            if (segment->size == size) { // perfect match
                segment->type = BoundaryTag::Type::ALLOCATED;
                hash_table_insert(segment);
                return segment->base;
            }

            // not a perfect match, need to split the segment
            BoundaryTag *new_segment = allocate_tag();
            new_segment->base = segment->base;
            new_segment->size = size;
            new_segment->type = BoundaryTag::Type::ALLOCATED;
            segment->base += size;
            segment->size -= size;
            segment->tag_list.add_before(&new_segment->tag_list);
            freelist_insert(segment); // reinsert to the correct size freelist
            hash_table_insert(new_segment);

            return new_segment->base;
        }

        return 0;
    }

    void Arena::xfree(uptr addr, usize size) {
        klib::ListHead *bucket = hash_bucket_for_addr(addr);
        BoundaryTag *segment = nullptr;
        BoundaryTag *tag;
        LIST_FOR_EACH(tag, bucket, list) {
            if (tag->base == addr) {
                segment = tag;
                break;
            }
        }
        if (!segment)
            panic("No VMem segment at address %#lX", addr);
        
        if (size != 0 && size != segment->size)
            panic("Mismatched size in VMem free (%#lX given, %#lX actual)", size, segment->size);
        
        segment->list.remove(); // remove from hash table

        bool coalesced = false, coalesced_left = false;

        BoundaryTag *left = LIST_ENTRY(segment->tag_list.prev, BoundaryTag, list);
        if (left->type == BoundaryTag::Type::FREE) {
            expand_free_segment(left, left->base, left->size + segment->size);
            segment->tag_list.remove();
            free_tag(segment);
            segment = left;
            coalesced = true;
            coalesced_left = true;
        }

        if (segment->tag_list.next != &tag_list_head) {
            BoundaryTag *right = LIST_ENTRY(segment->tag_list.next, BoundaryTag, list);
            if (right->type == BoundaryTag::Type::FREE) {
                expand_free_segment(right, segment->base, segment->size + right->size);
                segment->tag_list.remove();
                if (coalesced_left) // since the segment is now the left segment, its on the freelist so it must be removed
                    segment->list.remove();
                free_tag(segment);
                segment = right;
                coalesced = true;
            }
        }

        if (!coalesced) {
            segment->type = BoundaryTag::Type::FREE;
            freelist_insert(segment);
        }
    }

    usize Arena::freelist_index_for_size(usize size) {
        return num_freelists - 1 - __builtin_clzl(size);
    }

    void Arena::freelist_insert(BoundaryTag *segment) {
        freelist_heads[freelist_index_for_size(segment->size)].add_before(&segment->list);
    }
    
    void Arena::expand_free_segment(BoundaryTag *segment, uptr new_base, usize new_size) {
        usize old_freelist_index = freelist_index_for_size(segment->size);
        usize new_freelist_index = freelist_index_for_size(new_size);
        segment->base = new_base;
        segment->size = new_size;
        if (new_freelist_index != old_freelist_index) { // change the freelist
            segment->list.remove();
            freelist_heads[new_freelist_index].add_before(&segment->list);
        }
    }

    static u64 murmur64(u64 h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccd;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53;
        h ^= h >> 33;
        return h;
    }

    klib::ListHead* Arena::hash_bucket_for_addr(uptr addr) {
        return &hash_table[murmur64(addr) % num_hash_buckets];
    }

    void Arena::hash_table_insert(BoundaryTag *segment) {
        hash_bucket_for_addr(segment->base)->add(&segment->list);
    }
}
