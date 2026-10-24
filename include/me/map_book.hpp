#pragma once
// The textbook C++ order book: std::map of price levels, a std::list per
// level, and an unordered_map from id to list iterator. Same API and same
// semantics as OrderBook (it passes the same differential test), kept as a
// baseline so the benchmark shows what the ladder, pool and flat id map buy.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "order_book.hpp"  // LevelInfo
#include "types.hpp"

namespace me {

template <class Listener = NullListener>
class MapBook {
public:
    explicit MapBook(Price max_price, Listener l = {}, std::uint32_t expected_orders = 1 << 16)
        : max_price_(max_price), l_(l) {
        ids_.reserve(expected_orders);
    }

    Qty submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (qty <= 0) return rej(id, RejectReason::InvalidQuantity);
        if (has_price(type) && (price < 1 || price > max_price_)) return rej(id, RejectReason::InvalidPrice);
        if (ids_.count(id)) return rej(id, RejectReason::DuplicateOrderId);
        if (type == OrderType::PostOnly && would_cross(side, price)) return rej(id, RejectReason::PostOnlyWouldTake);
        const Price limit = has_price(type) ? price
                                            : (side == Side::Buy ? std::numeric_limits<Price>::max()
                                                                 : std::numeric_limits<Price>::min());
        l_.on_accepted(id, side, type, has_price(type) ? price : 0, qty);
        if (type == OrderType::FOK && available(side, limit, qty) < qty) {
            l_.on_cancelled(id, qty, CancelReason::FillOrKillFailed);
            return 0;
        }
        Qty rem = match(id, side, limit, qty);
        if (rem > 0) {
            if (can_rest(type)) rest(id, side, price, rem);
            else l_.on_cancelled(id, rem, CancelReason::UnfilledRemainder);
        }
        return qty - rem;
    }

    bool cancel(OrderId id) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return rej(id, RejectReason::UnknownOrderId), false;
        Qty q = it->second.node->qty;
        erase(it->second);
        ids_.erase(it);
        l_.on_cancelled(id, q, CancelReason::UserRequested);
        return true;
    }

    bool modify(OrderId id, Price np, Qty nq) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return rej(id, RejectReason::UnknownOrderId), false;
        if (nq <= 0) return rej(id, RejectReason::InvalidQuantity), false;
        if (np < 1 || np > max_price_) return rej(id, RejectReason::InvalidPrice), false;
        Ref r = it->second;
        if (np == r.price && nq <= r.node->qty) {
            level_total(r) -= r.node->qty - nq;
            r.node->qty = nq;
            l_.on_modified(id, np, nq);
            return true;
        }
        erase(r);
        ids_.erase(it);
        l_.on_modified(id, np, nq);
        Qty rem = match(id, r.side, np, nq);
        if (rem > 0) rest(id, r.side, np, rem);
        return true;
    }

    std::vector<LevelInfo> depth(Side side, std::size_t n) const {
        std::vector<LevelInfo> out;
        auto emit = [&](auto& book) {
            for (auto& [p, lv] : book) {
                if (out.size() == n) break;
                out.push_back({p, lv.total, static_cast<std::uint32_t>(lv.q.size())});
            }
        };
        if (side == Side::Buy) emit(bids_); else emit(asks_);
        return out;
    }

    std::size_t resting_orders() const { return ids_.size(); }

private:
    struct Node {
        OrderId id;
        Qty qty;
    };
    struct Lvl {
        std::list<Node> q;
        Qty total = 0;
    };
    struct Ref {
        Side side;
        Price price;
        typename std::list<Node>::iterator node;
    };

    Qty rej(OrderId id, RejectReason r) {
        l_.on_rejected(id, r);
        return 0;
    }

    static bool crosses(Side taker, Price limit, Price resting) {
        return taker == Side::Buy ? resting <= limit : resting >= limit;
    }

    bool would_cross(Side side, Price price) const {
        return side == Side::Buy ? !asks_.empty() && asks_.begin()->first <= price
                                 : !bids_.empty() && bids_.begin()->first >= price;
    }

    Qty available(Side side, Price limit, Qty wanted) const {
        Qty got = 0;
        auto walk = [&](auto& book) {
            for (auto& [p, lv] : book) {
                if (!crosses(side, limit, p) || got >= wanted) break;
                got += lv.total;
            }
        };
        if (side == Side::Buy) walk(asks_); else walk(bids_);
        return got;
    }

    template <class Book>
    Qty match_side(Book& book, OrderId taker, Side side, Price limit, Qty qty) {
        while (qty > 0 && !book.empty()) {
            auto lit = book.begin();
            if (!crosses(side, limit, lit->first)) break;
            Lvl& lv = lit->second;
            while (qty > 0 && !lv.q.empty()) {
                Node& m = lv.q.front();
                Qty fill = std::min(qty, m.qty);
                qty -= fill;
                lv.total -= fill;
                OrderId maker = m.id;
                if (fill == m.qty) {
                    lv.q.pop_front();
                    ids_.erase(maker);
                } else {
                    m.qty -= fill;
                }
                l_.on_trade(Trade{taker, maker, side, lit->first, fill});
            }
            if (lv.q.empty()) book.erase(lit);
        }
        return qty;
    }

    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        return side == Side::Buy ? match_side(asks_, taker, side, limit, qty) : match_side(bids_, taker, side, limit, qty);
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Lvl& lv = side == Side::Buy ? bids_[price] : asks_[price];
        lv.q.push_back({id, qty});
        lv.total += qty;
        ids_.emplace(id, Ref{side, price, std::prev(lv.q.end())});
        l_.on_rested(id, side, price, qty);
    }

    Qty& level_total(const Ref& r) { return (r.side == Side::Buy ? bids_.at(r.price) : asks_.at(r.price)).total; }

    void erase(const Ref& r) {
        if (r.side == Side::Buy) erase_in(bids_, r); else erase_in(asks_, r);
    }

    template <class Book>
    void erase_in(Book& book, const Ref& r) {
        auto lit = book.find(r.price);
        lit->second.total -= r.node->qty;
        lit->second.q.erase(r.node);
        if (lit->second.q.empty()) book.erase(lit);
    }

    Price max_price_;
    Listener l_;
    std::map<Price, Lvl, std::greater<>> bids_;
    std::map<Price, Lvl> asks_;
    std::unordered_map<OrderId, Ref> ids_;
};

}  // namespace me
