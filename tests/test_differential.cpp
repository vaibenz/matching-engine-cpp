// Replays random command streams through the real book and the naive
// reference book. After every command both must have produced exactly the
// same events, and every 16 commands the same depth on both sides.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "me/map_book.hpp"
#include "me/order_book.hpp"
#include "recording_listener.hpp"
#include "reference_book.hpp"

using namespace me;
using Log = std::vector<std::string>;

namespace {

constexpr Price kMaxPrice = 200;

template <class Book>
bool same_depth(const Book& book, const test::ReferenceBook<test::RecordingListener>& ref) {
    for (Side s : {Side::Buy, Side::Sell}) {
        auto want = ref.levels(s);
        auto got = book.depth(s, 1'000'000);
        if (got.size() != want.size()) return false;
        for (std::size_t i = 0; i < got.size(); ++i) {
            auto [p, q, n] = want[i];
            if (got[i].price != p || got[i].qty != q || got[i].orders != n) return false;
        }
    }
    return book.resting_orders() == ref.resting_orders();
}

// Returns the index of the first diverging command, or -1.
template <template <class> class BookT>
long run_stream(std::uint64_t seed, int commands) {
    std::mt19937_64 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };
    Log a, b;
    BookT<test::RecordingListener> book(kMaxPrice, test::RecordingListener{&a});
    test::ReferenceBook<test::RecordingListener> ref(kMaxPrice, test::RecordingListener{&b});
    OrderId next_id = 1;
    Price mid = 100;
    std::vector<std::pair<OrderId, Price>> recent;  // last known price per submitted id

    for (int i = 0; i < commands; ++i) {
        if (pick(20) == 0) mid = std::clamp<Price>(mid + pick(5) - 2, 20, kMaxPrice - 20);
        int roll = pick(100);
        // mostly recent ids, so cancels and modifies usually hit live orders,
        // plus some uniform picks that hit filled and never seen ids
        OrderId target = 1 + static_cast<OrderId>(pick(static_cast<int>(next_id) + 3));
        Price target_px = 0;
        if (!recent.empty() && pick(10) < 7) {
            std::size_t k = recent.size() - 1 - static_cast<std::size_t>(pick(std::min<int>(64, static_cast<int>(recent.size()))));
            target = recent[k].first;
            target_px = recent[k].second;
        }
        if (roll < 55) {
            Side s = pick(2) ? Side::Buy : Side::Sell;
            auto t = static_cast<OrderType>(pick(5));
            Price p = mid + pick(21) - 10;
            if (pick(50) == 0) p = pick(3) == 0 ? 0 : kMaxPrice + 1 + pick(5);  // bad prices
            Qty q = pick(40) == 0 ? -pick(3) : 1 + pick(30);
            OrderId id = pick(25) == 0 ? target : next_id++;  // occasional duplicate
            recent.emplace_back(id, p);
            book.submit(id, s, t, p, q);
            ref.submit(id, s, t, p, q);
        } else if (roll < 80) {
            book.cancel(target);
            ref.cancel(target);
        } else {
            Price p = pick(30) == 0 ? -1 : mid + pick(21) - 10;
            if (target_px > 0 && pick(5) < 2) p = target_px;  // same price: the keep priority path
            Qty q = pick(30) == 0 ? 0 : 1 + pick(30);
            if (p > 0) recent.emplace_back(target, p);
            book.modify(target, p, q);
            ref.modify(target, p, q);
        }
        if (a != b) return i;
        // events alone miss bookkeeping bugs (a wrong level total only
        // shows up in depth or a later FOK), so compare depth as we go
        if (i % 16 == 0 && !same_depth(book, ref)) return i;
    }
    return same_depth(book, ref) ? -1 : commands;
}

}  // namespace

TEST(differential_ladder_book_matches_reference) {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        long bad = run_stream<OrderBook>(seed, 2000);
        if (bad >= 0) std::printf("  seed %llu diverged at command %ld\n", (unsigned long long)seed, bad);
        CHECK(bad < 0);
        if (bad >= 0) break;
    }
}

TEST(differential_map_book_matches_reference) {
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        long bad = run_stream<MapBook>(seed, 2000);
        if (bad >= 0) std::printf("  seed %llu diverged at command %ld\n", (unsigned long long)seed, bad);
        CHECK(bad < 0);
        if (bad >= 0) break;
    }
}
