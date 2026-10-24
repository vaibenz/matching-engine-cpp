#pragma once
// Order flow generator, a line by line port of Benchmark.generate() in
// vaibenz/order-book-engine (Java). Same RNG, same scratch book semantics,
// so the same seed gives the same command stream.
//
// Mix: ~55% limit orders near the touch, 25% cancels of live orders,
// 10% modifies, 7% IOC, 3% market, around a random walk mid price.
#include <cstdint>
#include <vector>

#include "java_random.hpp"
#include "me/order_book.hpp"

namespace me::bench {

enum class Kind : std::uint8_t { Submit, Cancel, Modify };

struct Command {
    Kind kind;
    Side side;
    OrderType type;
    OrderId id;
    Price price;
    Qty qty;
};

inline constexpr Price kMaxPrice = 1 << 15;

inline Price passive_price(Side s, Price mid, JavaRandom& rnd) {
    Price off = 1 + rnd.next_int(20);
    return s == Side::Buy ? mid - off : mid + off;
}

inline Price aggressive_price(Side s, Price mid, JavaRandom& rnd) {
    Price off = rnd.next_int(5);
    return s == Side::Buy ? mid + off : mid - off;
}

template <class Book>
inline OrderId pick_live(const Book& book, std::vector<OrderId>& ids, JavaRandom& rnd) {
    for (int tries = 0; tries < 8 && !ids.empty(); ++tries) {
        auto idx = static_cast<std::size_t>(rnd.next_int(static_cast<std::int32_t>(ids.size())));
        OrderId c = ids[idx];
        if (book.find(c)) return c;
        ids[idx] = ids.back();
        ids.pop_back();
    }
    return 0;  // java returns null here; ids start at 1 so 0 is free
}

inline std::vector<Command> generate(std::size_t n, std::int64_t seed) {
    JavaRandom rnd(seed);
    OrderBook<> scratch(kMaxPrice, {}, 1 << 18);
    std::vector<OrderId> submitted;
    std::vector<Command> out(n);
    Price mid = 10'000;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < n; ++i) {
        if (rnd.next_int(100) < 5) mid = std::max<Price>(100, mid + rnd.next_int(3) - 1);
        const int roll = rnd.next_int(100);
        const OrderId target = roll >= 55 && roll < 90 ? pick_live(scratch, submitted, rnd) : 0;
        Command& c = out[i];

        if (target != 0 && roll < 80) {
            c = {Kind::Cancel, Side::Buy, OrderType::Limit, target, 0, 0};
            scratch.cancel(target);
        } else if (target != 0) {
            const Order* o = scratch.find(target);
            Price np = rnd.next_bool() ? o->price : passive_price(o->side, mid, rnd);
            Qty nq = 1 + rnd.next_int(100);
            c = {Kind::Modify, Side::Buy, OrderType::Limit, target, np, nq};
            scratch.modify(target, np, nq);
        } else {
            Side s = rnd.next_bool() ? Side::Buy : Side::Sell;
            OrderType t;
            Price p;
            if (roll >= 97) {
                t = OrderType::Market;
                p = 0;
            } else if (roll >= 90) {
                t = OrderType::IOC;
                p = aggressive_price(s, mid, rnd);
            } else {
                t = OrderType::Limit;
                p = rnd.next_int(10) == 0 ? aggressive_price(s, mid, rnd) : passive_price(s, mid, rnd);
            }
            Qty q = 1 + rnd.next_int(100);
            OrderId oid = next_id++;
            c = {Kind::Submit, s, t, oid, p, q};
            scratch.submit(oid, s, t, p, q);
            if (t == OrderType::Limit) submitted.push_back(oid);
        }
    }
    return out;
}

template <class Book>
inline void apply(Book& book, const Command& c) {
    switch (c.kind) {
        case Kind::Submit: book.submit(c.id, c.side, c.type, c.price, c.qty); break;
        case Kind::Cancel: book.cancel(c.id); break;
        case Kind::Modify: book.modify(c.id, c.price, c.qty); break;
    }
}

}  // namespace me::bench
