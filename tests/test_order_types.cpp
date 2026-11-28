#include <string>
#include <vector>

#include "check.hpp"
#include "me/order_book.hpp"
#include "recording_listener.hpp"

using namespace me;
using Log = std::vector<std::string>;
using Book = OrderBook<test::RecordingListener>;

namespace {
struct Fixture {
    Log log;
    Book book{1000, test::RecordingListener{&log}};
    void asks(std::initializer_list<std::pair<Price, Qty>> lv, OrderId first = 100) {
        for (auto [p, q] : lv) book.submit(first++, Side::Sell, OrderType::Limit, p, q);
        log.clear();
    }
};
}  // namespace

TEST(ioc_takes_what_it_can_and_cancels_rest) {
    Fixture f;
    f.asks({{100, 3}, {102, 3}});
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::IOC, 101, 5), 3);
    CHECK((f.log == Log{"accept 1 B IOC 101x5", "trade 1<-100 B 100x3", "cancel 1 2 remainder"}));
    CHECK_EQ(f.book.best_bid(), 0);
    CHECK_EQ(f.book.best_ask(), 102);
}

TEST(fok_fills_completely_across_levels) {
    Fixture f;
    f.asks({{100, 3}, {101, 3}});
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::FOK, 101, 5), 5);
    CHECK_EQ(f.book.quantity_at(Side::Sell, 101), 1);
}

TEST(fok_that_cannot_fill_leaves_book_untouched) {
    Fixture f;
    f.asks({{100, 3}, {101, 3}, {105, 50}});
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::FOK, 101, 7), 0);
    CHECK((f.log == Log{"accept 1 B FOK 101x7", "cancel 1 7 fok_failed"}));
    CHECK_EQ(f.book.quantity_at(Side::Sell, 100), 3);
    CHECK_EQ(f.book.quantity_at(Side::Sell, 101), 3);
}

TEST(post_only_rests_or_is_rejected) {
    Fixture f;
    f.asks({{100, 3}});
    f.book.submit(1, Side::Buy, OrderType::PostOnly, 100, 1);
    f.book.submit(2, Side::Buy, OrderType::PostOnly, 99, 1);
    CHECK((f.log == Log{"reject 1 po_would_take", "accept 2 B PO 99x1", "rest 2 B 99x1"}));
}

// A modify used to re-enter a post only order as a plain limit, so moving it
// to the ask took liquidity: a maker only order paying the taker fee.
TEST(modify_never_makes_a_post_only_order_take) {
    Fixture f;
    f.asks({{100, 5}});
    f.book.submit(1, Side::Buy, OrderType::PostOnly, 99, 2);
    f.book.submit(2, Side::Buy, OrderType::Limit, 99, 2);
    f.log.clear();
    CHECK(!f.book.modify(1, 100, 2));  // would cross: rejected, order untouched
    CHECK((f.log == Log{"reject 1 po_would_take"}));
    CHECK((f.book.queue_at(Side::Buy, 99) == std::vector<OrderId>{1, 2}));
    CHECK_EQ(f.book.quantity_at(Side::Sell, 100), 5);
    CHECK(f.book.modify(1, 98, 3));    // a reprice that does not cross is fine
    CHECK(!f.book.modify(1, 101, 3));  // and it is still post only afterwards
    CHECK(f.book.find(1) && f.book.find(1)->price == 98);
    CHECK(f.book.modify(2, 100, 2));   // a plain limit order may still trade on a modify
    CHECK_EQ(f.book.quantity_at(Side::Sell, 100), 3);
    CHECK(f.book.check_invariants());
}

TEST(market_order_price_is_ignored) {
    Fixture f;
    f.asks({{900, 1}});
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::Market, -5, 1), 1);
}

TEST(modify_down_keeps_priority) {
    Fixture f;
    for (OrderId id = 1; id <= 3; ++id) f.book.submit(id, Side::Buy, OrderType::Limit, 100, 10);
    CHECK(f.book.modify(1, 100, 4));
    CHECK((f.book.queue_at(Side::Buy, 100) == std::vector<OrderId>{1, 2, 3}));
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 24);
}

TEST(modify_up_or_reprice_loses_priority) {
    Fixture f;
    for (OrderId id = 1; id <= 3; ++id) f.book.submit(id, Side::Buy, OrderType::Limit, 100, 10);
    CHECK(f.book.modify(1, 100, 11));
    CHECK((f.book.queue_at(Side::Buy, 100) == std::vector<OrderId>{2, 3, 1}));
    CHECK(f.book.modify(2, 101, 10));
    CHECK_EQ(f.book.best_bid(), 101);
    CHECK((f.book.queue_at(Side::Buy, 100) == std::vector<OrderId>{3, 1}));
}

TEST(modify_that_crosses_trades_immediately) {
    Fixture f;
    f.asks({{105, 4}});
    f.book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
    f.log.clear();
    CHECK(f.book.modify(1, 105, 10));
    CHECK((f.log == Log{"modify 1 105x10", "trade 1<-100 B 105x4", "rest 1 B 105x6"}));
}

TEST(modify_rejects_bad_input) {
    Fixture f;
    f.book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
    f.log.clear();
    CHECK(!f.book.modify(2, 100, 1));
    CHECK(!f.book.modify(1, 100, 0));
    CHECK(!f.book.modify(1, 0, 1));
    CHECK((f.log == Log{"reject 2 unknown_id", "reject 1 bad_qty", "reject 1 bad_px"}));
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 10);
}

TEST(filled_order_can_not_be_cancelled) {
    Fixture f;
    f.book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
    f.book.submit(2, Side::Buy, OrderType::Limit, 100, 2);
    CHECK(!f.book.cancel(1));
    CHECK(!f.book.cancel(2));
}
