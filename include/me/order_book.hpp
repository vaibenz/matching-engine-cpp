#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "order_pool.hpp"
#include "types.hpp"

namespace me {

// FIFO queue of resting orders at one price.
struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    Qty total = 0;
    std::uint32_t count = 0;
};

struct LevelInfo {
    Price price;
    Qty qty;
    std::uint32_t orders;
};

// Single instrument limit order book, price time priority.
//
// Each side is a flat array of levels indexed directly by price (a "price
// ladder"), so finding the level for a price is one array access instead of
// a tree walk. Valid prices are 1..max_price ticks.
template <class Listener = NullListener>
class OrderBook {
public:
    explicit OrderBook(Price max_price, Listener listener = {}, std::uint32_t expected_orders = 1 << 16)
        : max_price_(max_price),
          listener_(listener),
          bids_(static_cast<std::size_t>(max_price) + 1),
          asks_(static_cast<std::size_t>(max_price) + 1),
          pool_(expected_orders) {
        ids_.reserve(expected_orders);
    }

    // ------------------------------------------------------------- commands

    // Returns the quantity filled immediately. Price is ignored for Market.
    Qty submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (qty <= 0) return reject(id, RejectReason::InvalidQuantity);
        if (has_price(type) && !valid_price(price)) return reject(id, RejectReason::InvalidPrice);
        if (ids_.count(id)) return reject(id, RejectReason::DuplicateOrderId);
        if (type == OrderType::PostOnly && would_cross(side, price))
            return reject(id, RejectReason::PostOnlyWouldTake);

        const Price limit = has_price(type) ? price : market_limit(side);
        listener_.on_accepted(id, side, type, has_price(type) ? price : 0, qty);

        // FOK checks liquidity before touching the book, so a failed fill
        // or kill leaves it exactly as it was.
        if (type == OrderType::FOK && available_to_take(side, limit, qty) < qty) {
            listener_.on_cancelled(id, qty, CancelReason::FillOrKillFailed);
            return 0;
        }

        const Qty remaining = match(id, side, limit, qty);
        if (remaining > 0) {
            if (can_rest(type)) {
                rest(id, side, price, remaining);
            } else {
                listener_.on_cancelled(id, remaining, CancelReason::UnfilledRemainder);
            }
        }
        return qty - remaining;
    }

    bool cancel(OrderId id) {
        auto it = ids_.find(id);
        if (it == ids_.end()) {
            listener_.on_rejected(id, RejectReason::UnknownOrderId);
            return false;
        }
        const std::uint32_t idx = it->second;
        ids_.erase(it);
        const Qty cancelled = pool_[idx].remaining;
        unlink(idx);
        pool_.free(idx);
        listener_.on_cancelled(id, cancelled, CancelReason::UserRequested);
        return true;
    }

    // Reducing quantity at the same price keeps queue position. A price
    // change or a quantity increase loses priority: the order is pulled and
    // re-entered as a fresh limit order, and may trade if it now crosses.
    bool modify(OrderId id, Price new_price, Qty new_qty) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return reject(id, RejectReason::UnknownOrderId), false;
        if (new_qty <= 0) return reject(id, RejectReason::InvalidQuantity), false;
        if (!valid_price(new_price)) return reject(id, RejectReason::InvalidPrice), false;

        const std::uint32_t idx = it->second;
        Order& o = pool_[idx];
        if (new_price == o.price && new_qty <= o.remaining) {
            ladder(o.side)[o.price].total -= o.remaining - new_qty;
            o.remaining = new_qty;
            listener_.on_modified(id, new_price, new_qty);
            return true;
        }

        const Side side = o.side;
        ids_.erase(it);
        unlink(idx);
        pool_.free(idx);
        listener_.on_modified(id, new_price, new_qty);
        const Qty remaining = match(id, side, new_price, new_qty);
        if (remaining > 0) rest(id, side, new_price, remaining);
        return true;
    }

    // -------------------------------------------------------------- queries

    Price best_bid() const { return best_bid_; }                         // 0 if empty
    Price best_ask() const { return best_ask_ > max_price_ ? 0 : best_ask_; }  // 0 if empty

    Qty quantity_at(Side side, Price price) const {
        return valid_price(price) ? ladder(side)[price].total : 0;
    }

    std::vector<LevelInfo> depth(Side side, std::size_t n) const {
        std::vector<LevelInfo> out;
        const auto& lv = ladder(side);
        if (side == Side::Buy) {
            for (Price p = best_bid_; p >= 1 && out.size() < n; --p)
                if (lv[p].count) out.push_back({p, lv[p].total, lv[p].count});
        } else {
            for (Price p = best_ask_; p <= max_price_ && out.size() < n; ++p)
                if (lv[p].count) out.push_back({p, lv[p].total, lv[p].count});
        }
        return out;
    }

    // Order ids resting at one price, in queue order.
    std::vector<OrderId> queue_at(Side side, Price price) const {
        std::vector<OrderId> out;
        if (!valid_price(price)) return out;
        for (auto i = ladder(side)[price].head; i != kNil; i = pool_[i].next) out.push_back(pool_[i].id);
        return out;
    }

    const Order* find(OrderId id) const {
        auto it = ids_.find(id);
        return it == ids_.end() ? nullptr : &pool_[it->second];
    }

    std::size_t resting_orders() const { return ids_.size(); }
    Listener& listener() { return listener_; }

private:
    bool valid_price(Price p) const { return p >= 1 && p <= max_price_; }

    static Price market_limit(Side s) {
        return s == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
    }

    Qty reject(OrderId id, RejectReason r) {
        listener_.on_rejected(id, r);
        return 0;
    }

    std::vector<Level>& ladder(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const std::vector<Level>& ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    static bool crosses(Side taker, Price limit, Price resting) {
        return taker == Side::Buy ? resting <= limit : resting >= limit;
    }

    bool would_cross(Side side, Price price) const {
        return side == Side::Buy ? best_ask_ <= max_price_ && best_ask_ <= price
                                 : best_bid_ >= 1 && best_bid_ >= price;
    }

    // Liquidity an incoming order could take right now, stopping once it
    // reaches `wanted`.
    Qty available_to_take(Side side, Price limit, Qty wanted) const {
        Qty got = 0;
        const auto& opp = ladder(opposite(side));
        if (side == Side::Buy) {
            for (Price p = best_ask_; p <= max_price_ && crosses(side, limit, p) && got < wanted; ++p) got += opp[p].total;
        } else {
            for (Price p = best_bid_; p >= 1 && crosses(side, limit, p) && got < wanted; --p) got += opp[p].total;
        }
        return got;
    }

    // Walk the opposite side from the touch while it still crosses `limit`.
    // Trades print at the maker's price. Returns the unfilled quantity.
    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        auto& opp = ladder(opposite(side));
        while (qty > 0) {
            const Price p = side == Side::Buy ? best_ask_ : best_bid_;
            if (side == Side::Buy ? (p > max_price_ || p > limit) : (p < 1 || p < limit)) break;

            Level& lv = opp[p];
            std::uint32_t idx = lv.head;
            while (qty > 0 && idx != kNil) {
                Order& m = pool_[idx];
                const Qty fill = std::min(qty, m.remaining);
                const OrderId maker = m.id;
                const std::uint32_t next = m.next;
                qty -= fill;
                if (fill == m.remaining) {
                    lv.head = next;
                    if (next != kNil) pool_[next].prev = kNil; else lv.tail = kNil;
                    lv.total -= fill;
                    --lv.count;
                    ids_.erase(maker);
                    pool_.free(idx);
                } else {
                    m.remaining -= fill;
                    lv.total -= fill;
                }
                listener_.on_trade(Trade{taker, maker, side, p, fill});
                idx = next;
            }
            if (lv.count == 0) level_emptied(opposite(side), p);
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t idx = pool_.alloc();
        Order& o = pool_[idx];
        o = Order{id, price, qty, kNil, kNil, side};
        Level& lv = ladder(side)[price];
        o.prev = lv.tail;
        if (lv.tail != kNil) pool_[lv.tail].next = idx; else lv.head = idx;
        lv.tail = idx;
        lv.total += qty;
        if (lv.count++ == 0) level_filled(side, price);
        ids_.emplace(id, idx);
        listener_.on_rested(id, side, price, qty);
    }

    void unlink(std::uint32_t idx) {
        Order& o = pool_[idx];
        Level& lv = ladder(o.side)[o.price];
        if (o.prev != kNil) pool_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.total -= o.remaining;
        if (--lv.count == 0) level_emptied(o.side, o.price);
    }

    void level_filled(Side s, Price p) {
        if (s == Side::Buy) best_bid_ = std::max(best_bid_, p);
        else best_ask_ = std::min(best_ask_, p);
    }

    // Best level went empty: scan outward for the next non empty one.
    void level_emptied(Side s, Price p) {
        if (s == Side::Buy) {
            if (p != best_bid_) return;
            while (best_bid_ >= 1 && bids_[best_bid_].count == 0) --best_bid_;
            if (best_bid_ < 1) best_bid_ = 0;
        } else {
            if (p != best_ask_) return;
            while (best_ask_ <= max_price_ && asks_[best_ask_].count == 0) ++best_ask_;
        }
    }

    Price max_price_;
    Listener listener_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    OrderPool pool_;
    std::unordered_map<OrderId, std::uint32_t> ids_;
    Price best_bid_ = 0;               // 0 = no bids
    Price best_ask_ = max_price_ + 1;  // max+1 = no asks
};

}  // namespace me
