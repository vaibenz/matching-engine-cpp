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
};
}  // namespace

TEST(limit_rests_when_nothing_to_match) {
    Fixture f;
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::Limit, 100, 10), 0);
    CHECK_EQ(f.book.best_bid(), 100);
    CHECK_EQ(f.book.best_ask(), 0);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 10);
    CHECK((f.log == Log{"accept 1 B LMT 100x10", "rest 1 B 100x10"}));
}

TEST(price_time_priority_within_level) {
    Fixture f;
    f.book.submit(1, Side::Sell, OrderType::Limit, 101, 5);
    f.book.submit(2, Side::Sell, OrderType::Limit, 101, 5);
    f.book.submit(3, Side::Sell, OrderType::Limit, 100, 5);
    f.log.clear();
    CHECK_EQ(f.book.submit(9, Side::Buy, OrderType::Limit, 101, 12), 12);
    CHECK((f.log == Log{"accept 9 B LMT 101x12", "trade 9<-3 B 100x5", "trade 9<-1 B 101x5", "trade 9<-2 B 101x2"}));
    CHECK_EQ(f.book.quantity_at(Side::Sell, 101), 3);
    CHECK((f.book.queue_at(Side::Sell, 101) == std::vector<OrderId>{2}));
}

TEST(trades_print_at_maker_price) {
    Fixture f;
    f.book.submit(1, Side::Buy, OrderType::Limit, 105, 4);
    f.log.clear();
    f.book.submit(2, Side::Sell, OrderType::Limit, 100, 4);
    CHECK((f.log == Log{"accept 2 S LMT 100x4", "trade 2<-1 S 105x4"}));
    CHECK_EQ(f.book.resting_orders(), 0u);
}

TEST(partial_fill_rests_remainder) {
    Fixture f;
    f.book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    CHECK_EQ(f.book.submit(2, Side::Buy, OrderType::Limit, 100, 10), 3);
    CHECK_EQ(f.book.best_bid(), 100);
    CHECK_EQ(f.book.best_ask(), 0);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 7);
}

TEST(sweep_stops_at_limit) {
    Fixture f;
    for (int i = 0; i < 5; ++i) f.book.submit(10 + i, Side::Sell, OrderType::Limit, 100 + i, 1);
    CHECK_EQ(f.book.submit(1, Side::Buy, OrderType::Limit, 102, 10), 3);
    CHECK_EQ(f.book.best_ask(), 103);
    CHECK_EQ(f.book.best_bid(), 102);
}

TEST(market_order_takes_everything_then_cancels_rest) {
    Fixture f;
    f.book.submit(1, Side::Buy, OrderType::Limit, 99, 2);
    f.book.submit(2, Side::Buy, OrderType::Limit, 50, 2);
    f.log.clear();
    CHECK_EQ(f.book.submit(3, Side::Sell, OrderType::Market, 0, 10), 4);
    CHECK((f.log == Log{"accept 3 S MKT 0x10", "trade 3<-1 S 99x2", "trade 3<-2 S 50x2", "cancel 3 6 remainder"}));
    CHECK_EQ(f.book.best_bid(), 0);
}

TEST(cancel_from_middle_of_queue) {
    Fixture f;
    for (OrderId id = 1; id <= 3; ++id) f.book.submit(id, Side::Buy, OrderType::Limit, 100, 1);
    CHECK(f.book.cancel(2));
    CHECK((f.book.queue_at(Side::Buy, 100) == std::vector<OrderId>{1, 3}));
    CHECK(f.book.cancel(1));
    CHECK(f.book.cancel(3));
    CHECK_EQ(f.book.best_bid(), 0);
    CHECK(!f.book.cancel(3));
    CHECK(f.log.back() == "reject 3 unknown_id");
}

TEST(best_price_moves_when_level_empties) {
    Fixture f;
    f.book.submit(1, Side::Sell, OrderType::Limit, 100, 1);
    f.book.submit(2, Side::Sell, OrderType::Limit, 140, 1);
    f.book.cancel(1);
    CHECK_EQ(f.book.best_ask(), 140);
    f.book.cancel(2);
    CHECK_EQ(f.book.best_ask(), 0);
}

TEST(input_validation) {
    Fixture f;
    f.book.submit(1, Side::Buy, OrderType::Limit, 100, 0);
    f.book.submit(2, Side::Buy, OrderType::Limit, 0, 5);
    f.book.submit(3, Side::Buy, OrderType::Limit, 1001, 5);  // above max_price
    f.book.submit(4, Side::Buy, OrderType::Limit, 100, 5);
    f.book.submit(4, Side::Buy, OrderType::Limit, 100, 5);
    CHECK((f.log == Log{"reject 1 bad_qty", "reject 2 bad_px", "reject 3 bad_px", "accept 4 B LMT 100x5",
                        "rest 4 B 100x5", "reject 4 dup_id"}));
}
