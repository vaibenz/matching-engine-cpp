#include <thread>

#include "check.hpp"
#include "me/spsc_ring.hpp"

TEST(spsc_ring_single_thread_full_and_empty) {
    me::SpscRing<int, 4> q;
    int v = 0;
    CHECK(!q.try_pop(v));
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!q.try_pop(v));
}

// Two threads, a small ring so it is constantly full and empty, and every
// value must arrive exactly once and in order.
TEST(spsc_ring_two_threads_in_order) {
    static me::SpscRing<std::uint64_t, 64> q;
    constexpr std::uint64_t kN = 2'000'000;
    std::thread prod([] {
        for (std::uint64_t i = 1; i <= kN; ++i)
            while (!q.try_push(i)) {
            }
    });
    std::uint64_t expect = 1, v = 0, bad = 0;
    while (expect <= kN) {
        if (q.try_pop(v)) {
            if (v != expect) ++bad;
            ++expect;
        }
    }
    prod.join();
    CHECK_EQ(bad, 0u);
}
