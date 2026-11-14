#include <atomic>
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

// size_approx is read while both sides are moving. It must stay within
// 0..N: reading tail before head lets the consumer catch up in between, and
// tail - head then wraps to a huge number.
TEST(spsc_ring_size_approx_stays_in_range_while_running) {
    static me::SpscRing<std::uint64_t, 8> q;
    constexpr std::uint64_t kN = 1'000'000;
    std::atomic<bool> done{false};
    std::thread prod([] {
        for (std::uint64_t i = 1; i <= kN; ++i)
            while (!q.try_push(i)) {
            }
    });
    std::thread cons([&] {
        std::uint64_t v = 0;
        for (std::uint64_t got = 0; got < kN;)
            if (q.try_pop(v)) ++got;
        done.store(true, std::memory_order_release);
    });
    std::uint64_t out_of_range = 0;
    while (!done.load(std::memory_order_acquire))
        if (q.size_approx() > 8) ++out_of_range;
    prod.join();
    cons.join();
    CHECK_EQ(out_of_range, 0u);
    CHECK_EQ(q.size_approx(), 0u);
}
