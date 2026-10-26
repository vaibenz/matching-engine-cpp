#pragma once

// Turns the matching engine's events into an ITCH 5.0 feed, the way an
// exchange's market data publisher does, so the book builder can be checked
// against books whose true state is known after every message.
//
// The mapping follows how Nasdaq reports activity:
//   order rests                  -> A (or F) with the resting quantity only
//   resting order is hit         -> E or C on the resting order; the aggressor gets no message
//   cancel                       -> D
//   quantity cut, same price     -> X for the difference (keeps queue position)
//   price change, does not trade -> U with a new reference (back of the queue)
//   price change that trades     -> D for the old order, E for each maker, A for any remainder

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../order_book.hpp"
#include "writer.hpp"

namespace me::itch {

class EngineToItch {
public:
    EngineToItch(Writer* out, std::uint16_t locate, std::string symbol, std::uint64_t* clock,
                 std::uint64_t* next_ref, std::uint64_t* next_match, std::mt19937_64* rng)
        : out_(out), locate_(locate), symbol_(std::move(symbol)), clock_(clock), next_ref_(next_ref),
          next_match_(next_match), rng_(rng) {}

    // ITCH price for an engine tick: one cent ticks, four implied decimals.
    static std::uint32_t px(Price p) { return static_cast<std::uint32_t>(p) * 100; }

    // The ITCH reference an engine order currently has (it changes on replace).
    std::uint64_t ref_of(OrderId id) const {
        auto it = live_.find(id);
        return it == live_.end() ? 0 : it->second.ref;
    }

    // --- listener interface
    void on_accepted(OrderId, Side, OrderType, Price, Qty) {}

    void on_trade(const Trade& t) {
        if (pending_ && t.taker == pending_id_ && !pending_deleted_) {
            out_->remove(tick(), locate_, pending_ref_);  // the old order is gone before the new one trades
            pending_deleted_ = true;
        }
        auto it = live_.find(t.maker);
        if (it == live_.end()) return;
        const auto shares = static_cast<std::uint32_t>(t.qty);
        if ((*rng_)() % 4 == 0)
            out_->executed_with_price(tick(), locate_, it->second.ref, shares, ++*next_match_, px(t.price));
        else
            out_->executed(tick(), locate_, it->second.ref, shares, ++*next_match_);
        it->second.qty -= t.qty;
        if (it->second.qty == 0) live_.erase(it);
    }

    void on_rested(OrderId id, Side s, Price p, Qty q) {
        const char side = s == Side::Buy ? 'B' : 'S';
        const std::uint64_t ref = ++*next_ref_;
        if (pending_ && id == pending_id_ && !pending_deleted_) {
            out_->replace(tick(), locate_, pending_ref_, ref, static_cast<std::uint32_t>(q), px(p));
        } else {
            out_->add(tick(), locate_, ref, side, static_cast<std::uint32_t>(q), symbol_, px(p), (*rng_)() % 5 == 0);
        }
        pending_ = false;
        live_[id] = Live{ref, q, p};
    }

    void on_cancelled(OrderId id, Qty, CancelReason r) {
        if (r != CancelReason::UserRequested) return;  // aggressor leftovers never rested
        auto it = live_.find(id);
        if (it == live_.end()) return;
        out_->remove(tick(), locate_, it->second.ref);
        live_.erase(it);
    }

    void on_modified(OrderId id, Price new_price, Qty new_qty) {
        auto it = live_.find(id);
        if (it == live_.end()) return;
        if (new_price == it->second.price && new_qty <= it->second.qty) {
            if (new_qty < it->second.qty)
                out_->cancel(tick(), locate_, it->second.ref, static_cast<std::uint32_t>(it->second.qty - new_qty));
            it->second.qty = new_qty;
            return;
        }
        // Loses priority: decide between U and D + A once we see whether it trades.
        pending_ = true;
        pending_id_ = id;
        pending_ref_ = it->second.ref;
        pending_deleted_ = false;
        live_.erase(it);
    }

    void on_rejected(OrderId, RejectReason) {}

    // Called after each engine command: a priority losing modify that traded
    // away completely never reaches on_rested, so its delete is emitted here.
    void end_command() {
        if (pending_ && !pending_deleted_) out_->remove(tick(), locate_, pending_ref_);
        pending_ = false;
    }

private:
    struct Live {
        std::uint64_t ref;
        Qty qty;
        Price price;
    };

    std::uint64_t tick() { return ++*clock_; }

    Writer* out_;
    std::uint16_t locate_;
    std::string symbol_;
    std::uint64_t* clock_;
    std::uint64_t* next_ref_;
    std::uint64_t* next_match_;
    std::mt19937_64* rng_;
    std::unordered_map<OrderId, Live> live_;
    bool pending_ = false;
    bool pending_deleted_ = false;
    OrderId pending_id_ = 0;
    std::uint64_t pending_ref_ = 0;
};

// A synthetic multi stock session: many engines, one interleaved feed. Each
// stock has its own random walk mid and flow (limits near the touch, cancels,
// modifies, marketable orders), and the stocks are chosen with a skewed
// distribution so a few are very active, as on a real exchange.
class SyntheticSession {
public:
    using Book = OrderBook<EngineToItch>;

    // Every stock gets its own engine with a price ladder of max_ticks cents, so
    // memory grows with stocks x max_ticks.
    SyntheticSession(int stocks, std::uint64_t seed, Price max_ticks = 8'000) : rng_(seed), max_ticks_(max_ticks) {
        writer_.system_event(++clock_, 'O');
        for (int s = 0; s < stocks; ++s) {
            const auto locate = static_cast<std::uint16_t>(s + 1);
            const std::string sym = "SYM" + std::to_string(s);
            writer_.stock_directory(++clock_, locate, sym);
            books_.push_back(std::make_unique<Book>(
                max_ticks_, EngineToItch(&writer_, locate, sym, &clock_, &next_ref_, &next_match_, &rng_), 1 << 10));
            const Price lo = 100, span = std::max<Price>(1, max_ticks_ - 200);
            state_.push_back(Stock{locate, sym, lo + static_cast<Price>(rng_() % static_cast<std::uint64_t>(span)), {}, {}});
        }
        writer_.system_event(++clock_, 'Q');
    }

    Writer& writer() { return writer_; }
    std::size_t stocks() const { return books_.size(); }
    Book& book(std::size_t i) { return *books_[i]; }
    std::uint16_t locate(std::size_t i) const { return state_[i].locate; }

    // Runs one engine command on a skewed random stock. Returns its index.
    std::size_t step() {
        // Skew: half the traffic goes to the first eighth of the stocks.
        std::size_t n = books_.size();
        std::size_t i = rng_() % 2 == 0 ? rng_() % std::max<std::size_t>(1, n / 8) : rng_() % n;
        Stock& st = state_[i];
        Book& b = *books_[i];
        if (rng_() % 20 == 0) st.mid = std::clamp<Price>(st.mid + static_cast<Price>(rng_() % 3) - 1, 50, max_ticks_ - 50);
        const int roll = static_cast<int>(rng_() % 100);
        OrderId target = 0;
        if (roll >= 55 && roll < 90) target = pick_live(b, st.ids);
        if (target != 0 && roll < 78) {
            b.cancel(target);
        } else if (target != 0) {
            const Order* o = b.find(target);
            Price np = rng_() % 2 ? o->price : passive(o->side, st.mid);
            if (rng_() % 10 == 0) np = aggressive(o->side, st.mid);
            Qty nq = rng_() % 3 == 0 ? std::max<Qty>(1, o->remaining - static_cast<Qty>(rng_() % (o->remaining + 1)))
                                     : static_cast<Qty>(1 + rng_() % 500);
            b.modify(target, np, nq);
        } else {
            Side s = rng_() % 2 ? Side::Buy : Side::Sell;
            const OrderId id = ++next_id_;
            const Qty q = static_cast<Qty>(100 * (1 + rng_() % 10));
            if (roll >= 96) {
                b.submit(id, s, OrderType::IOC, aggressive(s, st.mid), q);
            } else if (roll >= 90) {
                b.submit(id, s, OrderType::Limit, aggressive(s, st.mid), q);
                st.ids.push_back(id);
            } else {
                b.submit(id, s, OrderType::Limit, passive(s, st.mid), q);
                st.ids.push_back(id);
            }
        }
        b.listener().end_command();
        if (rng_() % 50 == 0)  // occasional trade against hidden liquidity
            writer_.hidden_trade(++clock_, st.locate, 'B', 100, st.sym, EngineToItch::px(st.mid), ++next_match_);
        return i;
    }

private:
    struct Stock {
        std::uint16_t locate;
        std::string sym;
        Price mid;
        std::vector<OrderId> ids;
        std::vector<OrderId> unused;
    };

    Price passive(Side s, Price mid) {
        Price off = 1 + static_cast<Price>(rng_() % 15);
        return std::clamp<Price>(s == Side::Buy ? mid - off : mid + off, 1, max_ticks_);
    }

    Price aggressive(Side s, Price mid) {
        Price off = static_cast<Price>(rng_() % 4);
        return std::clamp<Price>(s == Side::Buy ? mid + off : mid - off, 1, max_ticks_);
    }

    OrderId pick_live(const Book& b, std::vector<OrderId>& ids) {
        for (int tries = 0; tries < 8 && !ids.empty(); ++tries) {
            std::size_t k = rng_() % ids.size();
            OrderId c = ids[k];
            if (b.find(c)) return c;
            ids[k] = ids.back();
            ids.pop_back();
        }
        return 0;
    }

    std::mt19937_64 rng_;
    Price max_ticks_;
    Writer writer_;
    std::vector<std::unique_ptr<Book>> books_;
    std::vector<Stock> state_;
    std::uint64_t clock_ = 34'200'000'000'000ULL;  // 09:30:00 in nanoseconds since midnight
    std::uint64_t next_ref_ = 0, next_match_ = 0;
    OrderId next_id_ = 0;
};

}  // namespace me::itch
