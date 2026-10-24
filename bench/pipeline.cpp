// Two thread pipeline: a gateway thread encodes the benchmark flow into
// 32 byte wire commands and pushes them through an SPSC ring to the engine
// thread, which decodes and applies them to the book.
//
//   flood mode : gateway pushes as fast as the ring allows -> end to end throughput
//   paced mode : one command every ~1us, so the ring is nearly always empty
//                -> one way latency, gateway push to command applied
//
//   ./build/pipeline [commands] [seed]
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "flow.hpp"
#include "me/order_book.hpp"
#include "me/spsc_ring.hpp"
#include "me/wire.hpp"

using namespace me;
using namespace me::bench;

namespace {

void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

wire::Command encode(const Command& c) {
    wire::Command w{};
    w.msg = c.kind == Kind::Submit ? wire::Msg::Submit : c.kind == Kind::Cancel ? wire::Msg::Cancel : wire::Msg::Modify;
    w.side = c.side;
    w.type = c.type;
    w.qty = static_cast<std::int32_t>(c.qty);
    w.id = c.id;
    w.price = c.price;
    return w;
}

using Ring = SpscRing<wire::Command, 1 << 14>;

struct Result {
    double seconds;
    std::vector<std::uint64_t> lat;  // ticks, only in paced mode
    std::uint64_t trades;
};

struct Counter {
    std::uint64_t* trades;
    void on_accepted(OrderId, Side, OrderType, Price, Qty) {}
    void on_trade(const Trade&) { ++*trades; }
    void on_rested(OrderId, Side, Price, Qty) {}
    void on_cancelled(OrderId, Qty, CancelReason) {}
    void on_modified(OrderId, Price, Qty) {}
    void on_rejected(OrderId, RejectReason) {}
};

Result run(const std::vector<Command>& cmds, const Clock& clk, bool paced) {
    static Ring ring;
    Result r{0, {}, 0};
    if (paced) r.lat.resize(cmds.size());
    std::atomic<bool> go{false};

    std::thread engine([&] {
        pin(1);
        OrderBook<Counter> book(kMaxPrice, Counter{&r.trades}, 1 << 18);
        go.store(true);
        wire::Command w;
        for (std::size_t i = 0; i < cmds.size();) {
            if (!ring.try_pop(w)) continue;
            wire::apply(book, w);
            if (paced) r.lat[i] = clk.raw() - w.sent_tsc;
            ++i;
        }
    });

    pin(0);
    while (!go.load()) {
    }
    auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t gap = paced ? static_cast<std::uint64_t>(1000.0 / clk.to_ns(1)) : 0;  // ~1us in ticks
    std::uint64_t next_send = clk.raw();
    for (const auto& c : cmds) {
        wire::Command w = encode(c);
        if (paced) {
            while (clk.raw() < next_send) {
            }
            next_send += gap;
        }
        w.sent_tsc = clk.raw();
        while (!ring.try_push(w)) {
        }
    }
    engine.join();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

double pct(std::vector<std::uint64_t>& v, double p, const Clock& clk) {
    auto idx = static_cast<std::size_t>(std::max(0.0, p / 100.0 * static_cast<double>(v.size()) - 1));
    return clk.to_ns(v[std::min(idx, v.size() - 1)]);
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const std::int64_t seed = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : 42;
    if (std::thread::hardware_concurrency() < 2) {
        std::printf("needs at least 2 cpus\n");
        return 1;
    }
    auto cmds = generate(n, seed);
    Clock clk;

    run(cmds, clk, false);  // warmup
    std::vector<double> mops;
    std::uint64_t trades = 0;
    for (int i = 0; i < 5; ++i) {
        auto r = run(cmds, clk, false);
        mops.push_back(static_cast<double>(n) / r.seconds / 1e6);
        trades = r.trades;
    }
    std::sort(mops.begin(), mops.end());
    std::printf("flood: %zu commands, %llu trades, end to end median %.2f M cmd/s (min %.2f, max %.2f)\n", n,
                (unsigned long long)trades, mops[2], mops[0], mops[4]);

    const std::size_t m = std::min<std::size_t>(n, 500'000);
    std::vector<Command> head(cmds.begin(), cmds.begin() + static_cast<long>(m));
    auto r = run(head, clk, true);
    std::sort(r.lat.begin(), r.lat.end());
    std::printf("paced (1 cmd/us, %zu cmds): gateway -> applied p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  max %.0f ns\n", m,
                pct(r.lat, 50, clk), pct(r.lat, 99, clk), pct(r.lat, 99.9, clk), clk.to_ns(r.lat.back()));
    return 0;
}
