#pragma once

#include <klib/common.hpp>

// link: pointer to a ListHead, type: type of struct that the ListHead is in, member: the name of the ListHead in the struct
#define LIST_ENTRY(link, type, member) (type*)((uptr)link - (uptr)(&((type*)0)->member))
#define LIST_HEAD(list, type, member) LIST_ENTRY((list)->next, type, member)
#define LIST_TAIL(list, type, member) LIST_ENTRY((list)->prev, type, member)
#define LIST_NEXT(elm, member) LIST_ENTRY((elm)->member.next, typeof(*elm), member)
#define LIST_FOR_EACH(pos, list, member) for (pos = LIST_HEAD(list, typeof(*pos), member); &pos->member != (list); pos = LIST_NEXT(pos, member))
#define LIST_FOR_EACH_SAFE(pos, list, member) decltype(pos) next; for (pos = LIST_HEAD(list, typeof(*pos), member), next = LIST_NEXT(pos, member); &pos->member != (list); pos = next, next = LIST_NEXT(next, member))

#define HLIST_ENTRY(link, type, member) (type*)((uptr)link - (uptr)(&((type*)0)->member))
#define HLIST_FOR_EACH(pos, head) for (pos = (head)->first; pos; pos = pos->next)
#define HLIST_FOR_EACH_SAFE(pos, n, head) for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)

namespace klib {
    struct ListHead {
        ListHead *next = nullptr, *prev = nullptr;

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

    struct HListNode {
        HListNode *next = nullptr, **pprev = nullptr;

        // add entry after this
        inline void add(HListNode *entry) {
            entry->next = this->next;
            this->next = entry;
            entry->pprev = &this->next;
            if (entry->next)
                entry->next->pprev = &entry->next;
        }

        // add entry before this
        inline void add_before(HListNode *entry) {
            entry->pprev = next->pprev;
            entry->next = this;
            this->pprev = &entry->next;
            *entry->pprev = entry;
        }

        // remove this entry from the list
        inline void remove() {
            *pprev = next;
            if (next)
                next->pprev = pprev;
            this->next = nullptr;
            this->pprev = nullptr;
        }
    };

    struct HListHead {
        HListNode *first = nullptr;

        // add entry after this
        inline void add(HListNode *entry) {
            entry->next = first;
            if (first)
                first->pprev = &entry->next;
            first = entry;
            entry->pprev = &first;
        }

        inline bool is_empty() {
            return first == nullptr;
        }
    };
}
