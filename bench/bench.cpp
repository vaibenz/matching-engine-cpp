// Throughput and per command latency on the same replayable flow as the
// Java engine's benchmark.
//
//   ./build/bench [commands] [seed]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clock.hpp"
#include "flow.hpp"
#include "me/order_book.hpp"

using namespace me;
using namespace me::bench;

namespace {

struct CountingListener {
    std::uint64_t* trades;
    std::int64_t* volume;
    void on_accepted(OrderId, Side, OrderType, Price, Qty) {}
    void on_trade(const Trade& t) {
        ++*trades;
        *volume += t.qty;
    }
    void on_rested(OrderId, Side, Price, Qty) {}
    void on_cancelled(OrderId, Qty, CancelReason) {}
    void on_modified(OrderId, Price, Qty) {}
    void on_rejected(OrderId, RejectReason) {}
};

template <class Book>
double replay_ns(const std::vector<Command>& cmds, std::uint64_t& trades, std::int64_t& volume) {
    trades = 0;
    volume = 0;
    Book book(kMaxPrice, CountingListener{&trades, &volume}, 1 << 18);
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : cmds) apply(book, c);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

template <class Book>
std::vector<std::uint64_t> replay_timed(const std::vector<Command>& cmds, const Clock& clk) {
    std::uint64_t trades = 0;
    std::int64_t volume = 0;
    Book book(kMaxPrice, CountingListener{&trades, &volume}, 1 << 18);
    std::vector<std::uint64_t> lat(cmds.size());
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        std::uint64_t a = clk.raw();
        apply(book, cmds[i]);
        lat[i] = clk.raw() - a;
    }
    return lat;
}

double pct(const std::vector<std::uint64_t>& sorted, double p, const Clock& clk) {
    auto idx = static_cast<std::size_t>(std::max(0.0, p / 100.0 * static_cast<double>(sorted.size()) - 1));
    return clk.to_ns(sorted[std::min(idx, sorted.size() - 1)]);
}

template <class Book>
void run(const char* name, const std::vector<Command>& cmds, const Clock& clk) {
    std::uint64_t trades = 0;
    std::int64_t volume = 0;
    for (int w = 0; w < 3; ++w) replay_ns<Book>(cmds, trades, volume);

    constexpr int kRuns = 7;
    std::vector<double> mops;
    for (int r = 0; r < kRuns; ++r) mops.push_back(static_cast<double>(cmds.size()) / replay_ns<Book>(cmds, trades, volume) * 1e3);
    std::sort(mops.begin(), mops.end());

    replay_timed<Book>(cmds, clk);
    auto lat = replay_timed<Book>(cmds, clk);
    std::sort(lat.begin(), lat.end());

    // overhead of the two timestamps themselves
    std::vector<std::uint64_t> empty(200'000);
    for (auto& e : empty) {
        std::uint64_t a = clk.raw();
        e = clk.raw() - a;
    }
    std::sort(empty.begin(), empty.end());

    std::printf("%-8s trades %llu (volume %lld)  throughput median %.2f M cmd/s (min %.2f, max %.2f)\n", name,
                (unsigned long long)trades, (long long)volume, mops[kRuns / 2], mops.front(), mops.back());
    std::printf("%-8s latency p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  max %.0f ns  (timer overhead p50 %.0f ns, not subtracted)\n",
                name, pct(lat, 50, clk), pct(lat, 99, clk), pct(lat, 99.9, clk), clk.to_ns(lat.back()),
                pct(empty, 50, clk));
}

// Worst case for finding the next level: asks spaced 1,000 ticks apart.
// Each market buy takes the whole best level and a new one is posted at the
// back, so every command empties the touch and has to find the next level.
std::vector<Command> sparse_flow(std::size_t n) {
    std::vector<Command> out;
    OrderId id = 1;
    constexpr Price kGap = 1000, kLevels = 16;
    Price back = 1000;
    for (Price i = 0; i < kLevels; ++i, back += kGap) out.push_back({Kind::Submit, Side::Sell, OrderType::Limit, id++, back, 10});
    while (out.size() < n) {
        out.push_back({Kind::Submit, Side::Buy, OrderType::Market, id++, 0, 10});
        out.push_back({Kind::Submit, Side::Sell, OrderType::Limit, id++, back, 10});
        back += kGap;
        if (back > kMaxPrice - kGap) back = 1000 + (back % kGap) + 1;  // wrap, keep spacing roughly even
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const std::int64_t seed = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : 42;

    std::printf("generating %zu commands (seed %lld)...\n", n, (long long)seed);
    auto cmds = generate(n, seed);
    std::size_t s = 0, c = 0, m = 0;
    for (auto& x : cmds) (x.kind == Kind::Submit ? s : x.kind == Kind::Cancel ? c : m)++;
    std::printf("mix: %zu submits, %zu cancels, %zu modifies\n", s, c, m);

    Clock clk;
    run<OrderBook<CountingListener>>("ladder", cmds, clk);

    std::printf("\nsparse book (levels 1,000 ticks apart, every command moves the touch):\n");
    auto sparse = sparse_flow(n);
    run<OrderBook<CountingListener>>("ladder", sparse, clk);
    return 0;
}
