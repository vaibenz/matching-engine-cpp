// ITCH 5.0: message framing and fields, hand written scenarios, and a
// differential test where the matching engine is the ground truth. Many
// engines (one per stock) produce an interleaved ITCH feed; the book builder,
// seeing only that feed, must reconstruct every engine's book exactly: the
// same price levels, the same quantities, the same order counts, and the same
// queue order within each level.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "me/itch/book_builder.hpp"
#include "me/itch/engine_feed.hpp"
#include "me/itch/messages.hpp"
#include "me/itch/writer.hpp"

using namespace me;
using namespace me::itch;

namespace {

std::vector<Message> parse_all(const std::vector<std::uint8_t>& buf) {
    std::vector<Message> out;
    std::size_t used = Framer::each(buf.data(), buf.size(), [&](const Message& m) { out.push_back(m); });
    CHECK_EQ(used, buf.size());
    return out;
}

void feed(BookBuilder<>& b, Writer& w) {
    Framer::each(w.buffer().data(), w.buffer().size(), [&](const Message& m) { b.on(m); });
    w.clear();
}

// Every level on both sides, and the queue at the top few levels.
bool same_book(SyntheticSession::Book& engine, BookBuilder<>& builder, std::uint16_t locate, int queue_levels,
               std::string* why) {
    StockBook& got = builder.book(locate);
    for (Side s : {Side::Buy, Side::Sell}) {
        const char side = s == Side::Buy ? 'B' : 'S';
        auto want = engine.depth(s, 1'000'000);
        const LevelSide& have = got.side(side);
        if (have.size() != want.size()) {
            *why = std::string("level count on ") + side + ": " + std::to_string(have.size()) + " vs " +
                   std::to_string(want.size());
            return false;
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            const PxLevel& lv = have.at(i);
            if (lv.price != EngineToItch::px(want[i].price) || lv.total != static_cast<std::uint64_t>(want[i].qty) ||
                lv.count != want[i].orders) {
                *why = std::string("level ") + std::to_string(i) + " on " + side + " differs";
                return false;
            }
            if (static_cast<int>(i) < queue_levels) {
                auto ids = engine.queue_at(s, want[i].price);
                auto refs = builder.queue(locate, side, lv.price);
                if (ids.size() != refs.size()) {
                    *why = "queue length differs";
                    return false;
                }
                for (std::size_t k = 0; k < ids.size(); ++k)
                    if (engine.listener().ref_of(ids[k]) != refs[k]) {
                        *why = "queue order differs at level " + std::to_string(i);
                        return false;
                    }
            }
        }
    }
    return true;
}

}  // namespace

TEST(itch_round_trips_every_message_type_we_write) {
    Writer w;
    w.system_event(1, 'O');
    w.stock_directory(2, 7, "AAPL");
    w.add(3, 7, 1001, 'B', 300, "AAPL", 1'234'500);
    w.add(4, 7, 1002, 'S', 200, "AAPL", 1'235'000, true);
    w.executed(5, 7, 1001, 100, 9);
    w.executed_with_price(6, 7, 1002, 50, 10, 1'234'900);
    w.cancel(7, 7, 1001, 20);
    w.replace(8, 7, 1002, 1003, 400, 1'236'000);
    w.remove(9, 7, 1001);
    w.hidden_trade(10, 7, 'S', 77, "AAPL", 1'234'700, 11);
    auto msgs = parse_all(w.buffer());
    CHECK_EQ(msgs.size(), 10u);
    CHECK(msgs[1].directory_stock() == "AAPL    ");
    CHECK_EQ(msgs[2].add_ref(), 1001u);
    CHECK(msgs[2].add_side() == 'B');
    CHECK_EQ(msgs[2].add_shares(), 300u);
    CHECK_EQ(msgs[2].add_price(), 1'234'500u);
    CHECK_EQ(msgs[2].locate(), 7);
    CHECK_EQ(msgs[2].timestamp(), 3u);
    CHECK(msgs[3].type() == 'F');
    CHECK_EQ(msgs[4].exec_shares(), 100u);
    CHECK_EQ(msgs[4].match_number(), 9u);
    CHECK_EQ(msgs[5].exec_price(), 1'234'900u);
    CHECK_EQ(msgs[6].cancel_shares(), 20u);
    CHECK_EQ(msgs[7].replace_old(), 1002u);
    CHECK_EQ(msgs[7].replace_new(), 1003u);
    CHECK_EQ(msgs[7].replace_shares(), 400u);
    CHECK_EQ(msgs[7].replace_price(), 1'236'000u);
    CHECK_EQ(msgs[9].trade_shares(), 77u);
}

TEST(itch_framer_handles_any_split) {
    Writer w;
    for (int i = 0; i < 50; ++i) w.add(static_cast<std::uint64_t>(i), 1, 100 + static_cast<std::uint64_t>(i), 'B', 100, "X", 10'000);
    const auto& all = w.buffer();
    for (std::size_t cut = 0; cut <= all.size(); cut += 7) {  // feed in two arbitrary pieces
        std::vector<std::uint8_t> carry(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(cut));
        int n = 0;
        std::size_t used = Framer::each(carry.data(), carry.size(), [&](const Message&) { ++n; });
        carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(used));
        carry.insert(carry.end(), all.begin() + static_cast<std::ptrdiff_t>(cut), all.end());
        Framer::each(carry.data(), carry.size(), [&](const Message&) { ++n; });
        CHECK_EQ(n, 50);
    }
}

TEST(itch_rejects_wrong_length_for_type) {
    std::vector<std::uint8_t> bad = {0, 5, 'A', 0, 1, 0, 0};
    bool threw = false;
    try {
        Framer::each(bad.data(), bad.size(), [](const Message&) {});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(itch_builder_applies_partial_cancel_execute_replace_delete) {
    Writer w;
    BookBuilder<> b;
    w.stock_directory(1, 3, "MSFT");
    w.add(2, 3, 1, 'B', 500, "MSFT", 4'000'000);
    w.add(3, 3, 2, 'B', 300, "MSFT", 4'000'000);
    w.add(4, 3, 3, 'B', 100, "MSFT", 3'999'900);
    w.add(5, 3, 4, 'S', 200, "MSFT", 4'000'100);
    feed(b, w);
    StockBook& s = b.book(3);
    CHECK(s.name() == "MSFT");
    CHECK_EQ(s.best_bid(), 4'000'000u);
    CHECK_EQ(s.bids.best().total, 800u);
    CHECK_EQ(s.bids.size(), 2u);

    w.executed(6, 3, 1, 500, 1);  // the first order at the touch is filled completely
    w.cancel(7, 3, 2, 100);       // the second keeps its place with 200 left
    feed(b, w);
    CHECK_EQ(s.bids.best().total, 200u);
    CHECK((b.queue(3, 'B', 4'000'000) == std::vector<std::uint64_t>{2}));

    w.replace(8, 3, 3, 30, 150, 4'000'000);  // moves up and joins the back of the queue
    feed(b, w);
    CHECK_EQ(s.bids.size(), 1u);
    CHECK((b.queue(3, 'B', 4'000'000) == std::vector<std::uint64_t>{2, 30}));
    CHECK_EQ(s.bids.best().total, 350u);

    w.remove(9, 3, 4);
    feed(b, w);
    CHECK(s.asks.empty());
    CHECK_EQ(b.stats().unknown_ref, 0u);
    CHECK_EQ(b.live_orders(), 2u);
}

TEST(itch_builder_counts_feed_anomalies) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'S', 100, "X", 100);
    w.executed(2, 1, 99, 10, 1);  // unknown reference
    w.cancel(3, 1, 5, 150);       // more than is resting
    w.add(4, 1, 6, 'S', 100, "X", 100);
    w.add(5, 1, 6, 'S', 100, "X", 100);  // duplicate reference
    feed(b, w);
    CHECK_EQ(b.stats().unknown_ref, 1u);
    CHECK_EQ(b.stats().overfill, 1u);
    CHECK_EQ(b.stats().duplicate_ref, 1u);
}

// ITCH sides are 'B' or 'S'. Anything else used to be read as an ask, so a
// corrupt byte put a phantom order on the wrong side of the book, where it
// could set the best ask and lock or cross it.
TEST(itch_builder_skips_an_add_with_a_bad_side) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'B', 100, "X", 10'000);
    w.add(2, 1, 6, 'X', 100, "X", 9'900);
    w.add(3, 1, 7, 'b', 100, "X", 10'100);
    feed(b, w);
    CHECK_EQ(b.stats().bad_side, 2u);
    CHECK_EQ(b.live_orders(), 1u);
    CHECK(b.book(1).asks.empty());
    CHECK(b.order(6) == nullptr);
    CHECK(b.order(7) == nullptr);

    // the references were never live, so a later delete for one is an unknown reference
    w.remove(4, 1, 6);
    feed(b, w);
    CHECK_EQ(b.stats().unknown_ref, 1u);
}

// An add for zero shares used to create a level holding one order and no
// shares. It still counted as the touch, so best_bid() reported a price
// nobody was bidding at and the backtest would join it with nothing ahead.
// A replace for zero shares is refused the same way and keeps the old order.
TEST(itch_builder_skips_an_add_or_replace_for_zero_shares) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'B', 100, "X", 10'000);
    w.add(2, 1, 6, 'B', 0, "X", 10'100);
    feed(b, w);
    CHECK_EQ(b.stats().zero_shares, 1u);
    CHECK_EQ(b.live_orders(), 1u);
    CHECK_EQ(b.book(1).best_bid(), 10'000u);
    CHECK(b.book(1).bids.find(10'100) == nullptr);

    w.replace(3, 1, 5, 7, 0, 10'200);
    feed(b, w);
    CHECK_EQ(b.stats().zero_shares, 2u);
    CHECK_EQ(b.live_orders(), 1u);
    CHECK((b.queue(1, 'B', 10'000) == std::vector<std::uint64_t>{5}));
    CHECK_EQ(b.book(1).bids.best().total, 100u);
    CHECK(b.book(1).bids.find(10'200) == nullptr);
}

// Price 0 is what best_bid() and best_ask() return for an empty side. An ask
// added at 0 used to become the best ask, so best_ask() said "no asks" while
// the side held an order, and a bid at 0 behind real bids was a level nobody
// could ever trade. A replace to price 0 is refused the same way and keeps the
// old order.
TEST(itch_builder_skips_an_add_or_replace_at_price_zero) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'S', 100, "X", 10'100);
    w.add(2, 1, 6, 'S', 100, "X", 0);
    w.add(3, 1, 7, 'B', 100, "X", 0);
    feed(b, w);
    CHECK_EQ(b.stats().zero_price, 2u);
    CHECK_EQ(b.live_orders(), 1u);
    CHECK_EQ(b.book(1).best_ask(), 10'100u);
    CHECK_EQ(b.book(1).asks.size(), 1u);
    CHECK(b.book(1).bids.empty());

    w.replace(4, 1, 5, 8, 100, 0);
    feed(b, w);
    CHECK_EQ(b.stats().zero_price, 3u);
    CHECK_EQ(b.live_orders(), 1u);
    CHECK((b.queue(1, 'S', 10'100) == std::vector<std::uint64_t>{5}));
    CHECK(b.order(8) == nullptr);
}

// A reference the index cannot hold is a corrupt feed and throws, but it must
// throw before anything changes: a replay that catches the error (or a test
// that inspects the book afterwards) must not find an order linked into a
// level that the index has no record of, or a replaced order already gone.
TEST(itch_builder_rejects_out_of_range_reference_without_side_effects) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'B', 100, "X", 10'000);
    feed(b, w);
    const std::uint64_t too_big = RefIndex::kMaxRef;

    auto throws = [&](Writer& wr) {
        bool threw = false;
        try {
            feed(b, wr);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        wr.clear();
        return threw;
    };

    w.add(2, 1, too_big, 'B', 300, "X", 10'100);
    CHECK(throws(w));
    CHECK_EQ(b.live_orders(), 1u);
    CHECK(b.book(1).bids.find(10'100) == nullptr);
    CHECK_EQ(b.book(1).bids.size(), 1u);

    w.replace(3, 1, 5, too_big, 200, 10'200);
    CHECK(throws(w));
    CHECK_EQ(b.live_orders(), 1u);
    CHECK((b.queue(1, 'B', 10'000) == std::vector<std::uint64_t>{5}));
    CHECK_EQ(b.book(1).bids.best().total, 100u);
    CHECK(b.book(1).bids.find(10'200) == nullptr);
}

// U whose new reference is already a live order is a corrupt feed. It used
// to delete the old order and then drop the add as a duplicate, so one bad
// message silently cost a resting order. Now it is counted and changes
// nothing, like an out of range reference.
TEST(itch_builder_replace_onto_live_reference_keeps_old_order) {
    Writer w;
    BookBuilder<> b;
    w.add(1, 1, 5, 'B', 100, "X", 10'000);
    w.add(2, 1, 6, 'B', 300, "X", 9'900);
    w.replace(3, 1, 5, 6, 200, 10'100);
    feed(b, w);
    CHECK_EQ(b.stats().duplicate_ref, 1u);
    CHECK_EQ(b.live_orders(), 2u);
    CHECK((b.queue(1, 'B', 10'000) == std::vector<std::uint64_t>{5}));
    CHECK_EQ(b.order(6)->shares, 300u);
    CHECK_EQ(b.order(6)->price, 9'900u);
    CHECK(b.book(1).bids.find(10'100) == nullptr);

    // replacing an order with its own reference is still a plain replace
    w.clear();
    w.replace(4, 1, 5, 5, 50, 10'050);
    feed(b, w);
    CHECK_EQ(b.stats().duplicate_ref, 1u);
    const BookOrder* o5 = b.order(5);
    CHECK(o5 != nullptr);
    if (o5) CHECK_EQ(o5->price, 10'050u);
    CHECK(b.book(1).bids.find(10'000) == nullptr);
}

TEST(itch_differential_against_the_matching_engine) {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        SyntheticSession session(40, seed);
        BookBuilder<> builder;
        feed(builder, session.writer());
        bool ok = true;
        std::string why;
        for (int step = 0; step < 60'000 && ok; ++step) {
            std::size_t i = session.step();
            feed(builder, session.writer());
            ok = same_book(session.book(i), builder, session.locate(i), step % 16 == 0 ? 1000 : 2, &why);
            if (!ok) std::printf("  seed %llu step %d stock %zu: %s\n", static_cast<unsigned long long>(seed), step, i,
                                 why.c_str());
        }
        for (std::size_t i = 0; i < session.stocks() && ok; ++i) ok = same_book(session.book(i), builder, session.locate(i), 1000, &why);
        CHECK(ok);
        CHECK_EQ(builder.stats().unknown_ref, 0u);
        CHECK_EQ(builder.stats().overfill, 0u);
        CHECK_EQ(builder.stats().duplicate_ref, 0u);
        CHECK(builder.stats().by_type['U'] > 1000);
        CHECK(builder.stats().by_type['X'] > 1000);
        CHECK(builder.stats().by_type['C'] > 1000);
    }
}

// on_batch is what itch_replay and mm_backtest use by default: the same
// messages as on(), but with the index slot and the order for messages further
// ahead prefetched while the current one is applied. A prefetch may only ever
// be a hint, so feeding a day through on_batch, in batches of any size, must
// leave exactly the books, queues and counters that on() leaves.
TEST(itch_batched_replay_matches_message_by_message) {
    SyntheticSession session(30, 7);
    for (int step = 0; step < 40'000; ++step) session.step();
    const auto& feed_bytes = session.writer().buffer();
    std::vector<Message> msgs;
    Framer::each(feed_bytes.data(), feed_bytes.size(), [&](const Message& m) { msgs.push_back(m); });

    BookBuilder<> one;
    for (const Message& m : msgs) one.on(m);

    BookBuilder<> batched;
    std::mt19937_64 rng(11);
    for (std::size_t i = 0; i < msgs.size();) {
        std::size_t k = std::min<std::size_t>(msgs.size() - i, 1 + rng() % (rng() % 4 == 0 ? 3 : 4000));
        batched.on_batch(msgs.data() + i, k);
        i += k;
    }

    CHECK_EQ(batched.stats().messages, one.stats().messages);
    CHECK_EQ(batched.stats().unknown_ref, one.stats().unknown_ref);
    CHECK_EQ(batched.stats().overfill, one.stats().overfill);
    CHECK_EQ(batched.stats().duplicate_ref, one.stats().duplicate_ref);
    CHECK_EQ(batched.live_orders(), one.live_orders());
    CHECK(batched.live_orders() > 1000);
    bool same = batched.books().size() == one.books().size();
    for (std::size_t b = 0; same && b < one.books().size(); ++b) {
        const StockBook &want = one.books()[b], &got = batched.books()[b];
        same = want.messages == got.messages && want.executed_shares == got.executed_shares &&
               want.executed_notional == got.executed_notional;
        for (char side : {'B', 'S'}) {
            const LevelSide &w = want.side(side), &g = got.side(side);
            if (!same || w.size() != g.size()) {
                same = false;
                break;
            }
            for (std::size_t i = 0; same && i < w.size(); ++i) {
                same = w.at(i).price == g.at(i).price && w.at(i).total == g.at(i).total && w.at(i).count == g.at(i).count;
                const auto loc = static_cast<std::uint16_t>(b);
                same = same && one.queue(loc, side, w.at(i).price) == batched.queue(loc, side, g.at(i).price);
            }
        }
    }
    CHECK(same);
}
