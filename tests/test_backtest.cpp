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

// Only orders that were resting before us count as ahead. A cancel of an order
// behind us must not move us up, and a replace of an order ahead of us must: the
// replaced order goes to the back of the queue, behind us.
TEST(backtest_only_orders_ahead_of_us_move_us_up_the_queue) {
    Harness h(config());
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 200, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'S', 100, "TEST", 100'500);           // two sided: we decide to quote
    h.w.add(t + 100 * us, 1, 3, 'B', 100, "TEST", 100'000);    // our bid is live by now: this one is behind us
    h.run();
    CHECK(h.st().bid.live);
    CHECK_EQ(h.st().bid.ahead, 200u);

    h.w.cancel(t + 200 * us, 1, 3, 50);  // part of the order behind us goes away
    h.run();
    CHECK_EQ(h.st().bid.ahead, 200u);    // still 200 ahead of us

    h.w.replace(t + 300 * us, 1, 1, 4, 200, 100'000);  // order 1 is ahead of us, and loses its priority
    h.run();
    CHECK_EQ(h.st().bid.ahead, 0u);
    CHECK_EQ(h.st().inventory, 0);

    h.w.executed(t + 400 * us, 1, 3, 30, 1);  // the aggressor reaches order 3, behind us: it hits us first
    h.run();
    CHECK_EQ(h.st().inventory, 30);
    CHECK_EQ(h.st().bid.remaining, 70u);
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

// An execution for zero shares is a feed anomaly, not a trade. Reported at a
// price worse than our bid it used to read as a sweep through our level and
// fill the whole quote, though nothing traded at all.
TEST(backtest_zero_share_execution_is_not_a_trade_through) {
    Harness h(config());
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 500, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'B', 300, "TEST", 99'900);
    h.w.add(t + 2, 1, 3, 'S', 100, "TEST", 100'500);
    h.run();
    h.w.executed(t + 100 * us, 1, 2, 0, 1);                       // below our bid, for nothing
    h.w.executed_with_price(t + 101 * us, 1, 2, 0, 2, 99'900);    // the same with a price
    h.run();
    CHECK_EQ(h.st().inventory, 0);
    CHECK(h.st().fills.empty());
    CHECK(h.st().bid.live);
    CHECK_EQ(h.b.stats().zero_shares, 2u);
    CHECK_EQ(h.b.order(2)->shares, 300u);
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

// A latency so long the change never reaches the exchange must mean exactly
// that. ts + latency used to wrap past 2^64 to a time in the past, so the
// quote went live on the next message, as if there were no latency at all.
TEST(backtest_huge_latency_never_quotes) {
    MmConfig c = config();
    c.latency_ns = UINT64_MAX - 1000;
    Harness h(c);
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'S', 100, "TEST", 100'500);
    h.w.add(t + 1, 1, 2, 'B', 100, "TEST", 100'000);  // two sided: we decide to quote
    h.w.add(t + 100 * us, 1, 3, 'B', 100, "TEST", 100'000);
    h.w.executed(t + 200 * us, 1, 2, 100, 1);
    h.w.executed(t + 300 * us, 1, 3, 100, 2);  // the whole bid level trades
    h.run();
    CHECK(!h.st().bid.live);
    CHECK(h.st().bid.pending);
    CHECK_EQ(h.st().inventory, 0);
    CHECK(h.st().fills.empty());
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

// At stop_ns the strategy pulls its quotes, so nothing after the stop can fill
// it, and the mid used to mark the inventory is the last one seen before the stop.
TEST(backtest_pulls_quotes_at_the_stop_time) {
    MmConfig c = config();
    c.stop_ns = kOpen + 1'000'000 * us;  // one second of trading
    Harness h(c);
    std::uint64_t t = kOpen;
    h.w.add(t, 1, 1, 'B', 100, "TEST", 100'000);
    h.w.add(t + 1, 1, 2, 'S', 100, "TEST", 100'500);
    h.w.add(t + 100 * us, 1, 3, 'B', 100, "TEST", 100'000);  // behind us
    h.run();
    CHECK(h.st().bid.live);
    CHECK(h.st().ask.live);

    const std::uint64_t stop = c.stop_ns;
    h.w.add(stop + 1 * us, 1, 4, 'S', 100, "TEST", 100'400);  // the touch moves after the stop
    h.w.remove(stop + 100 * us, 1, 1);                        // the order ahead of us goes...
    h.w.executed(stop + 101 * us, 1, 3, 30, 1);               // ...and one behind us trades
    h.run();
    CHECK(!h.st().bid.live);
    CHECK(!h.st().ask.live);
    CHECK_EQ(h.st().inventory, 0);
    CHECK(h.st().fills.empty());
    CHECK_EQ(h.st().last_mid2, std::uint64_t{100'000 + 100'500});  // not the 10.04 ask seen after the stop
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

// Prices are 4 implied decimals in 32 bits, so a stock near $300,000 (Berkshire A
// traded there in 2019) has bid + ask past 2^32. The mid used for markouts and for
// marking the inventory must not be summed in 32 bits.
TEST(backtest_mid_of_a_high_priced_stock_does_not_overflow) {
    Harness h(config());
    std::uint64_t t = kOpen;
    const std::uint32_t bid = 2'999'900'000u, ask = 3'000'100'000u;  // $299,990 and $300,010
    h.w.add(t, 1, 1, 'B', 100, "TEST", bid);
    h.w.add(t + 1, 1, 2, 'S', 100, "TEST", ask);
    h.w.add(t + 50 * us, 1, 3, 'B', 100, "TEST", bid);  // behind us
    h.run();
    h.w.executed(t + 100 * us, 1, 1, 100, 1);
    h.w.executed(t + 101 * us, 1, 3, 100, 2);  // we bought 100 at $299,990
    h.w.add(t + 200 * us, 1, 5, 'B', 100, "TEST", bid);  // the bid refills, so the mid stays $300,000
    h.w.add(t + 2'000'000 * us, 1, 4, 'S', 100, "TEST", ask + 10'000);  // 2 s later: triggers the 1 s mark
    h.run();
    const auto& f = h.st().fills.at(0);
    CHECK_EQ(f.price, bid);
    CHECK(f.marked >= 2);
    CHECK(std::fabs(f.markout[1] - 10.0) < 1e-6);  // mid $300,000: $10 a share in our favour
    const auto s = h.b.observer().summary(h.st());
    CHECK_EQ(s.inventory, 100);
    CHECK(std::fabs(s.spread_pnl - 1000.0) < 1e-6);  // 100 shares marked $10 above cost
}

// A requested symbol with no order activity used to vanish from the report
// without a word. Both a name the feed never lists and a listed stock that
// never gets an order come back from not_seen(); a quoted stock does not.
TEST(backtest_names_requested_symbols_that_never_traded) {
    Writer w;
    BookBuilder<MarketMaker> b(MarketMaker(config(), {"TEST", "QUIET", "aapl"}));
    w.stock_directory(1, 1, "TEST");
    w.stock_directory(2, 2, "QUIET");
    w.add(kOpen, 1, 1, 'B', 100, "TEST", 100'000);
    Framer::each(w.buffer().data(), w.buffer().size(), [&](const Message& m) { b.on(m); });
    CHECK_EQ(b.observer().states().size(), std::size_t{1});
    CHECK((b.observer().not_seen() == std::vector<std::string>{"QUIET", "aapl"}));
}
