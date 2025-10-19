#pragma once

#include <klib/list.hpp>
#include <klib/cstdlib.hpp>

namespace klib {
    template<typename K, typename V>
    class HashTable {
        struct Entry {
            klib::ListHead hash_link;
            K key;
            V value;
        };

        klib::ListHead *table = nullptr;
        usize table_size = 0;
        usize num_stored = 0;

        klib::ListHead* get_hash_bucket(const K &key) const {
            return &table[klib::hash(key) % table_size];
        }

    public:
        HashTable() {}

        HashTable(usize table_size) : table_size(table_size) {
            table = new klib::ListHead[table_size];
            for (usize i = 0; i < table_size; i++)
                table[i].init();
        }

        ~HashTable() {
            delete[] table;
        }

        HashTable(const HashTable &) = delete;
        HashTable(const HashTable &&) = delete;

        V* operator [](const K &key) const {
            klib::ListHead *bucket = get_hash_bucket(key);
            Entry *entry;
            LIST_FOR_EACH(entry, bucket, hash_link)
                if (entry->key == key)
                    return &entry->value;
            return nullptr;
        }

        template<typename... Args>
        V* emplace(const K &key, Args&&... args) {
            klib::ListHead *bucket = get_hash_bucket(key);
            Entry *entry = new Entry();
            entry->key = key;
            new (&entry->value) V(klib::forward<Args>(args)...);
            bucket->add(&entry->hash_link);
            num_stored++;
            return &entry->value;
        }
    };

    // template<typename K, usize size>
    // struct HashTable {
    //     HListHead list_heads[size];

    //     HListHead* get_hash_bucket(const K &key) {
    //         return &list_heads[klib::hash(key) % size];
    //     }
    // };
}

// struct Entry {
//     klib::HListNode children_link;
//     klib::HashTable<const char*, 32> children;
//     const char *name;
// };

// inline void hash_test() {
//     Entry parent, child1, child2;
//     parent.name = "parent";
//     child1.name = "child1";
//     child2.name = "child2";
//     parent.children.get_hash_bucket(child1.name)->add(&child1.children_link);
//     parent.children.get_hash_bucket(child2.name)->add(&child2.children_link);
// }
