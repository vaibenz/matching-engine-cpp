#pragma once
// Deliberately naive order book: one flat vector of resting orders, every
// operation is a linear scan. Slow, but simple enough to be obviously
// correct, which makes it a good oracle for the real book.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "me/types.hpp"

namespace me::test {

template <class Listener>
class ReferenceBook {
public:
    ReferenceBook(Price max_price, Listener l) : max_price_(max_price), l_(l) {}

    Qty submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (qty <= 0) return rej(id, RejectReason::InvalidQuantity);
        if (has_price(type) && (price < 1 || price > max_price_)) return rej(id, RejectReason::InvalidPrice);
        if (find(id)) return rej(id, RejectReason::DuplicateOrderId);
        if (type == OrderType::PostOnly) {
            auto b = best(opposite(side));
            if (b && crosses(side, price, b->price)) return rej(id, RejectReason::PostOnlyWouldTake);
        }
        Price limit = has_price(type) ? price
                                      : (side == Side::Buy ? std::numeric_limits<Price>::max()
                                                           : std::numeric_limits<Price>::min());
        l_.on_accepted(id, side, type, has_price(type) ? price : 0, qty);
        if (type == OrderType::FOK) {
            Qty avail = 0;
            for (auto& o : orders_)
                if (o.side != side && crosses(side, limit, o.price)) avail += o.qty;
            if (avail < qty) {
                l_.on_cancelled(id, qty, CancelReason::FillOrKillFailed);
                return 0;
            }
        }
        Qty rem = match(id, side, limit, qty);
        if (rem > 0) {
            if (can_rest(type)) rest(id, side, price, rem);
            else l_.on_cancelled(id, rem, CancelReason::UnfilledRemainder);
        }
        return qty - rem;
    }

    bool cancel(OrderId id) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](auto& o) { return o.id == id; });
        if (it == orders_.end()) return rej(id, RejectReason::UnknownOrderId), false;
        Qty q = it->qty;
        orders_.erase(it);
        l_.on_cancelled(id, q, CancelReason::UserRequested);
        return true;
    }

    bool modify(OrderId id, Price np, Qty nq) {
        auto* o = find(id);
        if (!o) return rej(id, RejectReason::UnknownOrderId), false;
        if (nq <= 0) return rej(id, RejectReason::InvalidQuantity), false;
        if (np < 1 || np > max_price_) return rej(id, RejectReason::InvalidPrice), false;
        if (np == o->price && nq <= o->qty) {
            o->qty = nq;
            l_.on_modified(id, np, nq);
            return true;
        }
        Side side = o->side;
        cancel_silently(id);
        l_.on_modified(id, np, nq);
        Qty rem = match(id, side, np, nq);
        if (rem > 0) rest(id, side, np, rem);
        return true;
    }

    // (price, total qty, order count) for every level on a side, best first
    std::vector<std::tuple<Price, Qty, std::uint32_t>> levels(Side side) const {
        std::vector<std::tuple<Price, Qty, std::uint32_t>> out;
        for (auto& o : orders_) {
            if (o.side != side) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](auto& t) { return std::get<0>(t) == o.price; });
            if (it == out.end()) out.emplace_back(o.price, o.qty, 1);
            else std::get<1>(*it) += o.qty, ++std::get<2>(*it);
        }
        std::sort(out.begin(), out.end(), [&](auto& a, auto& b) {
            return side == Side::Buy ? std::get<0>(a) > std::get<0>(b) : std::get<0>(a) < std::get<0>(b);
        });
        return out;
    }

    std::size_t resting_orders() const { return orders_.size(); }

private:
    struct O {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    static bool crosses(Side taker, Price limit, Price resting) {
        return taker == Side::Buy ? resting <= limit : resting >= limit;
    }

    Qty rej(OrderId id, RejectReason r) {
        l_.on_rejected(id, r);
        return 0;
    }

    O* find(OrderId id) {
        for (auto& o : orders_)
            if (o.id == id) return &o;
        return nullptr;
    }

    // best resting order on a side: best price, then lowest sequence
    std::optional<O> best(Side side) const {
        const O* b = nullptr;
        for (auto& o : orders_) {
            if (o.side != side) continue;
            bool better = !b || (side == Side::Buy ? o.price > b->price : o.price < b->price) ||
                          (o.price == b->price && o.seq < b->seq);
            if (better) b = &o;
        }
        return b ? std::optional<O>(*b) : std::nullopt;
    }

    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        while (qty > 0) {
            auto b = best(opposite(side));
            if (!b || !crosses(side, limit, b->price)) break;
            O* m = find(b->id);
            Qty fill = std::min(qty, m->qty);
            qty -= fill;
            m->qty -= fill;
            OrderId maker = m->id;
            Price px = m->price;
            if (m->qty == 0) cancel_silently(maker);
            l_.on_trade(Trade{taker, maker, side, px, fill});
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        orders_.push_back({id, side, price, qty, seq_++});
        l_.on_rested(id, side, price, qty);
    }

    void cancel_silently(OrderId id) {
        orders_.erase(std::find_if(orders_.begin(), orders_.end(), [&](auto& o) { return o.id == id; }));
    }

    Price max_price_;
    Listener l_;
    std::vector<O> orders_;
    std::uint64_t seq_ = 0;
};

}  // namespace me::test
