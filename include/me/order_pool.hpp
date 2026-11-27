#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "types.hpp"

namespace me {

inline constexpr std::uint32_t kNil = UINT32_MAX;

// A resting order. Linked into its price level's FIFO by 32 bit indices
// rather than pointers: half the size, and the pool can grow without
// invalidating anything.
struct Order {
    OrderId id;
    Price price;
    Qty remaining;
    std::uint32_t prev;
    std::uint32_t next;
    Side side;
};

// Fixed size slab of orders with an intrusive free list. After warmup the
// book never calls the allocator on the hot path.
class OrderPool {
public:
    explicit OrderPool(std::uint32_t capacity) { grow(capacity); }

    std::uint32_t alloc() {
        if (free_head_ == kNil) grow(static_cast<std::uint32_t>(slots_.size()));
        std::uint32_t idx = free_head_;
        free_head_ = slots_[idx].next;
        ++live_;
        return idx;
    }

    void free(std::uint32_t idx) {
        slots_[idx].next = free_head_;
        free_head_ = idx;
        --live_;
    }

    Order& operator[](std::uint32_t idx) { return slots_[idx]; }
    const Order& operator[](std::uint32_t idx) const { return slots_[idx]; }

    std::uint32_t live() const { return live_; }
    std::size_t capacity() const { return slots_.size(); }

    // Indices are 32 bit and kNil marks "no order", so slots 0..kNil-1 are all
    // the pool can address. Doubling past 2^31 slots used to wrap the 32 bit
    // size to 0 and leave the free list pointing past the end of the slab.
    static constexpr std::size_t kMaxSlots = kNil;

    // The size the pool grows to from `size` when asked for `extra` more
    // slots: capped at kMaxSlots, and a std::length_error once it is there.
    static std::size_t grown_size(std::size_t size, std::uint32_t extra) {
        if (extra == 0) extra = 1024;
        if (size >= kMaxSlots) throw std::length_error("order pool is full: no 32 bit index left");
        return std::min<std::size_t>(size + extra, kMaxSlots);
    }

private:
    void grow(std::uint32_t extra) {
        const std::size_t old = slots_.size(), size = grown_size(old, extra);
        slots_.resize(size);
        // thread the new slots onto the free list in index order
        for (std::size_t i = old; i < size; ++i)
            slots_[i].next = i + 1 < size ? static_cast<std::uint32_t>(i + 1) : free_head_;
        free_head_ = static_cast<std::uint32_t>(old);
    }

    std::vector<Order> slots_;
    std::uint32_t free_head_ = kNil;
    std::uint32_t live_ = 0;
};

}  // namespace me
