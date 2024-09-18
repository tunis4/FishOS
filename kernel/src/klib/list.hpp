#pragma once

#include <klib/common.hpp>

// link: pointer to a ListHead, type: type of struct that the ListHead is in, member: the name of the ListHead in the struct
#define LIST_ENTRY(link, type, member) (type*)((uptr)link - (uptr)(&((type*)0)->member))
#define LIST_HEAD(list, type, member) LIST_ENTRY((list)->next, type, member)
#define LIST_TAIL(list, type, member) LIST_ENTRY((list)->prev, type, member)
#define LIST_NEXT(elm, member) LIST_ENTRY((elm)->member.next, typeof(*elm), member)
#define LIST_FOR_EACH(pos, list, member) for (pos = LIST_HEAD(list, typeof(*pos), member); &pos->member != (list); pos = LIST_NEXT(pos, member))

namespace klib {
    struct ListHead {
        ListHead *next, *prev;

        // init a list
        inline void init() {
            next = this;
            prev = this;
        }

        // add entry after this
        inline void add(ListHead *entry) {
            next->prev = entry;
            entry->next = next;
            entry->prev = this;
            this->next = entry;
        }

        // add entry before this
        inline void add_before(ListHead *entry) {
            prev->next = entry;
            entry->prev = prev;
            entry->next = this;
            this->prev = entry;
        }

        // remove this entry from the list
        inline void remove() {
            next->prev = prev;
            prev->next = next;
            this->next = nullptr;
            this->prev = nullptr;
        }

        inline bool is_empty() {
            return next == this;
        }

        inline bool is_invalid() {
            return next == nullptr || prev == nullptr;
        }
    };
}
