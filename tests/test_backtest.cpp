// The market making backtest's fill model, on hand written feeds where the
// right answer can be worked out on paper: queue position, fills from behind,
// trade throughs, latency, inventory limits and markouts.
#include <cmath>
#include <string>
#include <vector>

#include "check.hpp"
#include "me/itch/backtest.hpp"
#include "me/itch/book_builder.hpp"
#include "me/itch/writer.hpp"

using namespace me::itch;

namespace {

constexpr std::uint64_t kOpen = 34'200'000'000'000ULL;  // 09:30:00
constexpr std::uint64_t us = 1'000;

struct Harness {
    Writer w;
    BookBuilder<MarketMaker> b;
    explicit Harness(MmConfig cfg) : b(MarketMaker(cfg, {"TEST"})) { w.stock_directory(1, 1, "TEST"); }

    void run() {
        Framer::each(w.buffer().data(), w.buffer().size(), [&](const Message& m) { b.on(m); });
        w.clear();
    }
    const MarketMaker::State& st() { return b.observer().states().at(0); }
};

MmConfig config() {
    MmConfig c;
    c.lot = 100;
    c.max_inventory = 1000;
    c.latency_ns = 10 * us;
    c.rebate_per_share = 0;
    return c;
}

}  // namespace

// Bid 10.00 has 300 shares from two orders when we join. We only fill after both
// are gone and the aggressor reaches an order that arrived after us.
TEST(backtest_respects_queue_position) {
    Harness h(config());
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 100, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'B', 200, "TEST", 100'000);
    h.w.add(t + 2, 1, 3, 'S', 100, "TEST", 100'500);  // two sided: we decide to quote
    h.w.add(t + 100 * us, 1, 4, 'B', 100, "TEST", 100'000);  // arrives after our quote went live: behind us
    h.run();
    auto& bid = h.st().bid;
    CHECK(bid.live);
    CHECK_EQ(bid.price, 100'000u);
    CHECK_EQ(bid.ahead, 300u);

    h.w.executed(t + 200 * us, 1, 1, 100, 1);  // ahead goes 300 -> 200
    h.w.cancel(t + 210 * us, 1, 2, 50);        // 200 -> 150
    h.run();
    CHECK_EQ(h.st().bid.ahead, 150u);
    CHECK_EQ(h.st().inventory, 0);

    h.w.remove(t + 220 * us, 1, 2);            // 150 -> 0: we are at the front
    h.w.executed(t + 230 * us, 1, 4, 60, 2);   // aggressor reaches order 4, behind us: it would have hit us first
    h.run();
    CHECK_EQ(h.st().inventory, 60);
    CHECK_EQ(h.st().fills.size(), 1u);
    CHECK_EQ(h.st().fills[0].price, 100'000u);
}

TEST(backtest_trade_through_fills_completely) {
    Harness h(config());
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 500, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'B', 300, "TEST", 99'900);
    h.w.add(t + 2, 1, 3, 'S', 100, "TEST", 100'500);
    h.run();
    h.w.executed(t + 100 * us, 1, 1, 500, 1);  // the whole level goes...
    h.w.executed(t + 101 * us, 1, 2, 10, 2);   // ...and the next one trades: the aggressor went through our price
    h.run();
    CHECK_EQ(h.st().inventory, 100);
}

TEST(backtest_latency_means_early_trades_miss_us) {
    MmConfig c = config();
    c.latency_ns = 500 * us;
    Harness h(c);
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'S', 100, "TEST", 100'500);
    h.w.add(t + 1, 1, 2, 'B', 100, "TEST", 100'000);  // we decide to quote now, live only 500 us later
    h.w.add(t + 2, 1, 3, 'B', 100, "TEST", 100'000);
    h.w.executed(t + 100 * us, 1, 2, 100, 1);
    h.w.executed(t + 200 * us, 1, 3, 100, 2);  // all before our quote reaches the exchange
    h.run();
    CHECK_EQ(h.st().inventory, 0);
    CHECK(!h.st().bid.live);
}

TEST(backtest_stops_adding_at_the_inventory_limit) {
    MmConfig c = config();
    c.max_inventory = 200;
    Harness h(c);
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'S', 100, "TEST", 100'500);
    std::uint64_t ref = 10;
    for (int round = 0; round < 6; ++round) {  // buyers keep getting hit at the bid
        t += 1'000 * us;
        h.w.add(t, 1, ++ref, 'B', 100, "TEST", 100'000);
        h.run();
        t += 1'000 * us;
        h.w.add(t, 1, ++ref, 'B', 100, "TEST", 100'000);  // behind us
        h.w.executed(t + 1, 1, ref - 1, 100, static_cast<std::uint64_t>(round) * 2 + 1);
        h.w.executed(t + 2, 1, ref, 100, static_cast<std::uint64_t>(round) * 2 + 2);
        h.run();
    }
    CHECK_EQ(h.st().inventory, 200);
    CHECK_EQ(h.st().max_abs_inventory, 200);
}

TEST(backtest_markout_measures_adverse_selection) {
    Harness h(config());
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 100, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'S', 100, "TEST", 100'200);
    h.w.add(t + 50 * us, 1, 3, 'B', 100, "TEST", 100'000);
    h.run();
    h.w.executed(t + 100 * us, 1, 1, 100, 1);
    h.w.executed(t + 101 * us, 1, 3, 100, 2);  // we bought 100 at 10.0000
    // the market drops: the bid moves to 9.95, the ask to 9.97
    h.w.add(t + 200 * us, 1, 4, 'B', 100, "TEST", 99'500);
    h.w.remove(t + 300 * us, 1, 2);
    h.w.add(t + 301 * us, 1, 5, 'S', 100, "TEST", 99'700);
    h.w.add(t + 2'000'000 * us, 1, 6, 'B', 100, "TEST", 99'400);  // 2 s later, to trigger the 1 s mark
    h.run();
    const auto& f = h.st().fills.at(0);
    CHECK(f.marked >= 2);
    CHECK(std::fabs(f.markout[1] - (-0.04)) < 1e-9);  // mid 9.96 vs 10.00: lost 4 cents a share
}
