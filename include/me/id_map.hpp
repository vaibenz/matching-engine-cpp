#pragma once

#include <bit>
#include <cstdint>
#include <vector>

#include "order_pool.hpp"

namespace me {

// Order id -> pool index. Flat open addressing with linear probing:
// one contiguous array, no per entry allocation, and a lookup is usually
// a single cache line. Deletes use backward shift instead of tombstones,
// so heavy cancel traffic never degrades probe lengths.
class IdMap {
public:
    explicit IdMap(std::size_t expected = 1 << 16) { rehash(std::bit_ceil(expected * 2)); }

    std::uint32_t find(OrderId key) const {
        for (std::size_t i = slot(key);; i = (i + 1) & mask_) {
            const Entry& e = table_[i];
            if (e.val == kNil) return kNil;
            if (e.key == key) return e.val;
        }
    }

    bool contains(OrderId key) const { return find(key) != kNil; }

    // key must not already be present
    void insert(OrderId key, std::uint32_t val) {
        if ((size_ + 1) * 2 > table_.size()) rehash(table_.size() * 2);
        std::size_t i = slot(key);
        while (table_[i].val != kNil) i = (i + 1) & mask_;
        table_[i] = {key, val};
        ++size_;
    }

    bool erase(OrderId key) {
        std::size_t i = slot(key);
        for (;; i = (i + 1) & mask_) {
            if (table_[i].val == kNil) return false;
            if (table_[i].key == key) break;
        }
        // backward shift: pull later entries of the cluster into the hole
        // if their home slot is at or before it
        for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (table_[j].val == kNil) break;
            std::size_t home = slot(table_[j].key);
            if (((j - home) & mask_) >= ((j - i) & mask_)) {
                table_[i] = table_[j];
                i = j;
            }
        }
        table_[i].val = kNil;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }

private:
    struct Entry {
        OrderId key = 0;
        std::uint32_t val = kNil;
    };

    // Fibonacci hashing: ids are often sequential, this spreads them out
    std::size_t slot(OrderId key) const { return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_); }

    void rehash(std::size_t cap) {
        std::vector<Entry> old;
        old.swap(table_);
        table_.assign(cap, Entry{});
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
        size_ = 0;
        for (const Entry& e : old)
            if (e.val != kNil) insert(e.key, e.val);
    }

    std::vector<Entry> table_;
    std::size_t mask_ = 0;
    int shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace me
