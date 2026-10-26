#pragma once

// Rebuilds the full depth, order by order book of every stock from an ITCH
// 5.0 stream.
//
// The feed never sends a book. It sends events: an order was added, part of
// it executed, part of it cancelled, it was deleted, it was replaced by a new
// order. Applying them in order to every stock at once means tracking every
// live order on Nasdaq (a few million at peak) and keeping each price level's
// queue in arrival order, which is what the market making backtest needs to
// know how much stands ahead of an order.
//
// Layout:
//   orders  : one slab of BookOrder linked into per level FIFOs by 32 bit
//             indices, plus a paged direct map from order reference to slab
//             index (RefIndex), since Nasdaq's references only go up
//   levels  : per stock and side, a vector of levels sorted so the best price
//             is at the back. Almost all activity is near the touch, so the
//             level for a price is found by scanning a few entries from the
//             back, and inserting or removing a level near the top moves
//             only a handful of elements.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <memory>
#include <stdexcept>

#include "messages.hpp"

namespace me::itch {

inline constexpr std::uint32_t kNone = UINT32_MAX;

// Order reference -> slab index. Nasdaq hands out order references in
// increasing order through the day, so instead of hashing them, this maps them
// directly: a page table of 64K entry pages, allocated when the first order in
// a page arrives and freed when its last order is gone. A lookup is two array
// reads with no hashing and no probing, and orders that arrive close together
// sit close together in memory. References that are not sequential still work;
// they just use more pages.
class RefIndex {
public:
    static constexpr int kBits = 16;
    static constexpr std::uint64_t kPage = 1ULL << kBits;
    static constexpr std::uint64_t kMaxRef = 1ULL << 40;

    std::uint32_t find(std::uint64_t ref) const {
        const std::uint64_t pg = ref >> kBits;
        if (pg >= pages_.size() || !pages_[pg]) return kNone;
        return pages_[pg]->slot[ref & (kPage - 1)];
    }

    bool contains(std::uint64_t ref) const { return find(ref) != kNone; }

    // Starts loading the slot for ref into cache without waiting for it.
    void prefetch(std::uint64_t ref) const {
        const std::uint64_t pg = ref >> kBits;
        if (pg < pages_.size() && pages_[pg]) __builtin_prefetch(&pages_[pg]->slot[ref & (kPage - 1)]);
    }

    void insert(std::uint64_t ref, std::uint32_t idx) {
        if (ref >= kMaxRef) throw std::runtime_error("order reference out of range");
        const std::uint64_t pg = ref >> kBits;
        if (pg >= pages_.size()) pages_.resize(pg + 1);
        if (!pages_[pg]) {
            pages_[pg] = std::make_unique<Page>();
            ++pages_live_;
        }
        pages_[pg]->slot[ref & (kPage - 1)] = idx;
        ++pages_[pg]->live;
        ++size_;
    }

    void erase(std::uint64_t ref) {
        const std::uint64_t pg = ref >> kBits;
        Page& p = *pages_[pg];
        p.slot[ref & (kPage - 1)] = kNone;
        --size_;
        if (--p.live == 0) {  // every order in this page is gone
            pages_[pg].reset();
            --pages_live_;
        }
    }

    std::size_t size() const { return size_; }
    std::size_t pages() const { return pages_live_; }

private:
    struct Page {
        Page() { slot.fill(kNone); }
        std::array<std::uint32_t, kPage> slot;
        std::uint32_t live = 0;
    };
    std::vector<std::unique_ptr<Page>> pages_;
    std::size_t size_ = 0, pages_live_ = 0;
};

struct BookOrder {
    std::uint64_t ref;
    std::uint64_t seq;       // arrival order across the whole feed: smaller is earlier in its queue
    std::uint32_t price;
    std::uint32_t shares;
    std::uint32_t prev = kNone;
    std::uint32_t next = kNone;
    std::uint16_t locate;
    char side;               // 'B' or 'S'
};

struct PxLevel {
    std::uint32_t price;
    std::uint32_t count = 0;
    std::uint64_t total = 0;
    std::uint32_t head = kNone;
    std::uint32_t tail = kNone;
};

// One side of one stock. Sorted so the best price is at the back: ascending
// for bids, descending for asks.
class LevelSide {
public:
    explicit LevelSide(bool bid = true) : bid_(bid) {}

    bool empty() const { return levels_.empty(); }
    const PxLevel& best() const { return levels_.back(); }
    std::size_t size() const { return levels_.size(); }
    // i = 0 is the best level
    const PxLevel& at(std::size_t i) const { return levels_[levels_.size() - 1 - i]; }

    PxLevel* find(std::uint32_t price) {
        std::size_t i = position(price);
        return i < levels_.size() && levels_[i].price == price ? &levels_[i] : nullptr;
    }

    const PxLevel* find(std::uint32_t price) const { return const_cast<LevelSide*>(this)->find(price); }

    PxLevel& find_or_insert(std::uint32_t price) {
        std::size_t i = position(price);
        if (i < levels_.size() && levels_[i].price == price) return levels_[i];
        PxLevel lv;
        lv.price = price;
        return *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), lv);
    }

    void erase(std::uint32_t price) {
        std::size_t i = position(price);
        if (i < levels_.size() && levels_[i].price == price) levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
    }

private:
    // true if a is strictly closer to the top of the book than b
    bool better(std::uint32_t a, std::uint32_t b) const { return bid_ ? a > b : a < b; }

    // Index of the level with this price, or where it would be inserted. Scans
    // a few levels from the top first, then falls back to binary search.
    std::size_t position(std::uint32_t price) const {
        std::size_t n = levels_.size();
        for (std::size_t k = 0; k < 8 && k < n; ++k) {
            const std::uint32_t p = levels_[n - 1 - k].price;
            if (p == price) return n - 1 - k;
            if (better(price, p)) return n - k;
        }
        auto it = std::lower_bound(levels_.begin(), levels_.end(), price,
                                   [this](const PxLevel& lv, std::uint32_t px) { return better(px, lv.price); });
        return static_cast<std::size_t>(it - levels_.begin());
    }

    std::vector<PxLevel> levels_;
    bool bid_;
};

struct StockBook {
    char symbol[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::uint16_t locate = 0;
    LevelSide bids{true};
    LevelSide asks{false};
    std::uint64_t messages = 0;
    std::uint64_t executed_shares = 0;
    std::uint64_t executed_notional = 0;  // in price units x shares

    std::string name() const {
        std::string s(symbol, 8);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    LevelSide& side(char s) { return s == 'B' ? bids : asks; }
    const LevelSide& side(char s) const { return s == 'B' ? bids : asks; }
    std::uint32_t best_bid() const { return bids.empty() ? 0 : bids.best().price; }
    std::uint32_t best_ask() const { return asks.empty() ? 0 : asks.best().price; }
};

// Event hooks, called after the book has been updated. The default does nothing
// and compiles away.
struct NullObserver {
    // before a message for this stock is applied (the book is still as it was)
    void before(std::uint64_t /*ts*/, const StockBook&) {}
    void on_add(std::uint64_t /*ts*/, const StockBook&, const BookOrder&) {}
    // executed: shares taken from this order at price; remaining is what is left
    void on_execute(std::uint64_t /*ts*/, const StockBook&, const BookOrder&, std::uint32_t /*shares*/,
                    std::uint32_t /*price*/, std::uint32_t /*remaining*/) {}
    // cancelled or deleted (remaining == 0 means the order is gone)
    void on_reduce(std::uint64_t /*ts*/, const StockBook&, const BookOrder&, std::uint32_t /*shares*/,
                   std::uint32_t /*remaining*/) {}
    // a trade against a hidden order (P): no effect on the visible book
    void on_hidden_trade(std::uint64_t /*ts*/, const StockBook&, char /*side*/, std::uint32_t /*shares*/,
                         std::uint32_t /*price*/) {}
};

struct BuilderStats {
    std::array<std::uint64_t, 128> by_type{};
    std::uint64_t messages = 0;
    std::uint64_t unknown_ref = 0;      // E, C, X, D or U for an order we never saw
    std::uint64_t overfill = 0;         // executed or cancelled more than was resting
    std::uint64_t duplicate_ref = 0;    // add for a reference that is already live
    std::uint64_t peak_orders = 0;
};

template <class Observer = NullObserver>
class BookBuilder {
public:
    explicit BookBuilder(Observer obs = {}, std::size_t expected_orders = 1 << 20) : obs_(obs) {
        orders_.reserve(expected_orders);
    }

    void on(const Message& m) {
        ++stats_.messages;
        const auto t = static_cast<std::uint8_t>(m.p[0]);
        if (t < 128) ++stats_.by_type[t];
        if constexpr (!std::is_same_v<Observer, NullObserver>) {
            if (affects_book(m.type())) obs_.before(m.timestamp(), book(m.locate()));
        }
        switch (m.type()) {
            case 'R': directory(m); break;
            case 'A':
            case 'F': add(m.timestamp(), m.locate(), m.add_ref(), m.add_side(), m.add_price(), m.add_shares()); break;
            case 'E': execute(m.timestamp(), m.ref(), m.exec_shares(), 0, false); break;
            case 'C': execute(m.timestamp(), m.ref(), m.exec_shares(), m.exec_price(), true); break;
            case 'X': reduce(m.timestamp(), m.ref(), m.cancel_shares()); break;
            case 'D': reduce(m.timestamp(), m.ref(), UINT32_MAX); break;
            case 'U': replace(m); break;
            case 'P': {
                StockBook& b = book(m.locate());
                ++b.messages;
                obs_.on_hidden_trade(m.timestamp(), b, m.add_side(), m.trade_shares(), m.trade_price());
                break;
            }
            default: break;  // system, trading status, auction and other messages do not change the book
        }
    }

    // Applies a run of messages with software prefetching. Most messages touch
    // an existing order, and with millions of live orders that order is almost
    // never in cache. So while message i is applied, the index slot for
    // message i + 16 and the order for message i + 8 are already being loaded.
    // (Prefetching the order's queue neighbours as a third stage was measured
    // and made no difference, so it is not here.)
    // Prefetches are only hints: a message that changes what a later one will
    // touch makes a prefetch useless, never wrong.
    void on_batch(const Message* msgs, std::size_t n) {
        constexpr std::size_t kSlotAhead = 16, kOrderAhead = 8;
        for (std::size_t i = 0; i < n; ++i) {
            if (i + kSlotAhead < n) {
                const Message& m = msgs[i + kSlotAhead];
                if (touches_order(m.type())) ids_.prefetch(m.ref());
            }
            if (i + kOrderAhead < n) {
                const Message& m = msgs[i + kOrderAhead];
                if (touches_order(m.type())) {
                    const std::uint32_t idx = ids_.find(m.ref());
                    if (idx != kNone) __builtin_prefetch(&orders_[idx]);
                }
            }

            on(msgs[i]);
        }
    }

    StockBook& book(std::uint16_t locate) {
        if (locate >= books_.size()) {
            std::size_t old = books_.size();
            books_.resize(static_cast<std::size_t>(locate) + 1);
            for (std::size_t i = old; i < books_.size(); ++i) books_[i].locate = static_cast<std::uint16_t>(i);
        }
        return books_[locate];
    }
    const std::vector<StockBook>& books() const { return books_; }
    const BuilderStats& stats() const { return stats_; }
    std::size_t live_orders() const { return ids_.size(); }
    Observer& observer() { return obs_; }

    const BookOrder* order(std::uint64_t ref) const {
        std::uint32_t i = ids_.find(ref);
        return i == kNone ? nullptr : &orders_[i];
    }

    // Order references resting at one price, in queue order (oldest first).
    std::vector<std::uint64_t> queue(std::uint16_t locate, char side, std::uint32_t price) {
        std::vector<std::uint64_t> out;
        PxLevel* lv = book(locate).side(side).find(price);
        if (lv)
            for (std::uint32_t i = lv->head; i != kNone; i = orders_[i].next) out.push_back(orders_[i].ref);
        return out;
    }

private:
    static bool touches_order(char t) { return t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U'; }
    static bool affects_book(char t) { return touches_order(t) || t == 'A' || t == 'F' || t == 'P'; }

    void directory(const Message& m) {
        StockBook& b = book(m.locate());
        std::memcpy(b.symbol, m.directory_stock().data(), 8);
    }

    std::uint32_t alloc() {
        if (free_ != kNone) {
            std::uint32_t i = free_;
            free_ = orders_[i].next;
            return i;
        }
        orders_.emplace_back();
        return static_cast<std::uint32_t>(orders_.size() - 1);
    }

    void release(std::uint32_t i) {
        orders_[i].next = free_;
        free_ = i;
    }

    void add(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t price,
             std::uint32_t shares) {
        if (ids_.contains(ref)) {
            ++stats_.duplicate_ref;
            return;
        }
        StockBook& b = book(locate);
        ++b.messages;
        const std::uint32_t i = alloc();
        BookOrder& o = orders_[i];
        o = BookOrder{ref, ++seq_, price, shares, kNone, kNone, locate, side};
        PxLevel& lv = b.side(side).find_or_insert(price);
        o.prev = lv.tail;
        if (lv.tail != kNone) orders_[lv.tail].next = i; else lv.head = i;
        lv.tail = i;
        lv.total += shares;
        ++lv.count;
        ids_.insert(ref, i);
        stats_.peak_orders = std::max<std::uint64_t>(stats_.peak_orders, ids_.size());
        obs_.on_add(ts, b, o);
    }

    // Removes `shares` from an order (all of it if shares >= remaining).
    // Returns the shares actually removed.
    std::uint32_t take(std::uint32_t i, std::uint32_t shares, StockBook& b) {
        BookOrder& o = orders_[i];
        const std::uint32_t removed = std::min(shares, o.shares);
        LevelSide& side = b.side(o.side);
        PxLevel* lv = side.find(o.price);
        lv->total -= removed;
        o.shares -= removed;
        if (o.shares == 0) {
            if (o.prev != kNone) orders_[o.prev].next = o.next; else lv->head = o.next;
            if (o.next != kNone) orders_[o.next].prev = o.prev; else lv->tail = o.prev;
            if (--lv->count == 0) side.erase(o.price);
        }
        return removed;
    }

    void finish(std::uint32_t i) {
        if (orders_[i].shares != 0) return;
        ids_.erase(orders_[i].ref);
        release(i);
    }

    void execute(std::uint64_t ts, std::uint64_t ref, std::uint32_t shares, std::uint32_t price, bool with_price) {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNone) {
            ++stats_.unknown_ref;
            return;
        }
        StockBook& b = book(orders_[i].locate);
        ++b.messages;
        if (shares > orders_[i].shares) ++stats_.overfill;
        const std::uint32_t px = with_price ? price : orders_[i].price;
        const std::uint32_t done = take(i, shares, b);
        b.executed_shares += done;
        b.executed_notional += static_cast<std::uint64_t>(done) * px;
        const BookOrder snapshot = orders_[i];
        finish(i);
        obs_.on_execute(ts, b, snapshot, done, px, snapshot.shares);
    }

    void reduce(std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        const std::uint32_t i = ids_.find(ref);
        if (i == kNone) {
            ++stats_.unknown_ref;
            return;
        }
        StockBook& b = book(orders_[i].locate);
        ++b.messages;
        if (shares != UINT32_MAX && shares > orders_[i].shares) ++stats_.overfill;
        const std::uint32_t done = take(i, shares, b);
        const BookOrder snapshot = orders_[i];
        finish(i);
        obs_.on_reduce(ts, b, snapshot, done, snapshot.shares);
    }

    // U: the original order is removed and a new one (new reference, same
    // stock and side) joins the back of the queue at the new price.
    void replace(const Message& m) {
        const std::uint32_t i = ids_.find(m.replace_old());
        if (i == kNone) {
            ++stats_.unknown_ref;
            return;
        }
        const std::uint16_t locate = orders_[i].locate;
        const char side = orders_[i].side;
        reduce(m.timestamp(), m.replace_old(), UINT32_MAX);
        add(m.timestamp(), locate, m.replace_new(), side, m.replace_price(), m.replace_shares());
    }

    Observer obs_;
    RefIndex ids_;
    std::vector<BookOrder> orders_;
    std::uint32_t free_ = kNone;
    std::vector<StockBook> books_;
    std::uint64_t seq_ = 0;
    BuilderStats stats_;
};

}  // namespace me::itch
