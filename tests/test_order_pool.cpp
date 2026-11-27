#include <cstdint>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "me/order_pool.hpp"

using me::OrderPool;

TEST(order_pool_hands_out_every_slot_once_then_reuses_freed_ones) {
    OrderPool pool(4);
    std::vector<std::uint32_t> got;
    for (int i = 0; i < 4; ++i) got.push_back(pool.alloc());
    CHECK((got == std::vector<std::uint32_t>{0, 1, 2, 3}));
    CHECK_EQ(pool.live(), 4u);
    pool.free(2);
    pool.free(0);
    CHECK_EQ(pool.live(), 2u);
    CHECK_EQ(pool.alloc(), 0u);  // last freed, first reused
    CHECK_EQ(pool.alloc(), 2u);
    CHECK_EQ(pool.capacity(), 4u);  // reuse, no growth
}

TEST(order_pool_grows_when_full_and_keeps_live_orders) {
    OrderPool pool(2);
    const std::uint32_t a = pool.alloc(), b = pool.alloc();
    pool[a].id = 11;
    pool[b].id = 22;
    const std::uint32_t c = pool.alloc();  // full: doubles to 4
    CHECK_EQ(pool.capacity(), 4u);
    CHECK_EQ(c, 2u);
    CHECK_EQ(pool.alloc(), 3u);
    CHECK_EQ(pool[a].id, 11u);
    CHECK_EQ(pool[b].id, 22u);
    for (int i = 0; i < 100; ++i) CHECK(pool.alloc() != me::kNil);
    CHECK_EQ(pool.live(), 104u);

    OrderPool empty(0);  // no capacity at all still works: it starts with 1024
    CHECK_EQ(empty.alloc(), 0u);
    CHECK_EQ(empty.capacity(), 1024u);
}

TEST(order_pool_never_grows_past_a_32_bit_index) {
    constexpr std::size_t half = std::size_t{1} << 31;
    CHECK_EQ(OrderPool::grown_size(8, 8), 16u);
    CHECK_EQ(OrderPool::grown_size(0, 0), 1024u);
    // Doubling 2^31 slots would need index 2^32 - 1, which is kNil, and 2^32, which does not fit.
    CHECK_EQ(OrderPool::grown_size(half, static_cast<std::uint32_t>(half)), OrderPool::kMaxSlots);
    CHECK_EQ(OrderPool::grown_size(OrderPool::kMaxSlots - 1, 1024), OrderPool::kMaxSlots);
    bool threw = false;
    try {
        OrderPool::grown_size(OrderPool::kMaxSlots, 1024);
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);
}
