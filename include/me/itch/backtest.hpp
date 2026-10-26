#pragma once

// A passive market making backtest that runs on the order by order book.
//
// The strategy is deliberately simple: post one lot at the best bid and one at
// the best ask, follow the touch, and stop adding to a side once inventory hits
// a limit. What matters is how fills are simulated, because that is where most
// backtests of passive strategies go wrong.
//
// Queue position. Our order is not in the real book. When it joins a level,
// every share already resting there is ahead of it. Ahead shares only go away
// when those specific orders (identified by arrival sequence) are executed or
// cancelled. Once nothing is ahead, an execution against an order that arrived
// after ours means the aggressor would have reached us first, so we fill
// instead. An execution at a worse price on our side means the aggressor swept
// through our level, so we fill completely.
//
// Latency. Every quote, cancel and replace takes effect `latency_ns` after the
// decision, and the old quote stays live until then, so it can still be hit
// while we are trying to move it.
//
// Markouts. For each fill, the mid price 100 ms, 1 s and 10 s later shows how
// much the fill was adverse selection (we bought just before the price fell).
//
// Assumptions, stated rather than hidden: our orders are small enough not to
// change anyone else's behaviour, queue position at a level follows strict
// price time priority (no hidden or midpoint orders jump ahead), and the maker
// rebate is a flat amount per share.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book_builder.hpp"

namespace me::itch {

struct MmConfig {
    std::uint32_t lot = 100;              // shares per quote
    std::int64_t max_inventory = 500;     // stop adding to a side beyond this many shares
    std::uint64_t latency_ns = 50'000;    // decision to effect, both ways
    double rebate_per_share = 0.0020;     // dollars per share filled passively
    std::uint64_t start_ns = 34'200'000'000'000ULL;  // 09:30:00
    std::uint64_t stop_ns = 57'540'000'000'000ULL;   // 15:59:00, then stop quoting
};

struct Fill {
    std::uint64_t ts;
    char side;  // 'B' we bought, 'S' we sold
    std::uint32_t price;
    std::uint32_t shares;
    std::array<double, 3> markout{};  // dollars per share at +100 ms, +1 s, +10 s (positive is good for us)
    int marked = 0;
};

class MarketMaker {
public:
    static constexpr std::array<std::uint64_t, 3> kHorizons = {100'000'000ULL, 1'000'000'000ULL, 10'000'000'000ULL};

    struct Quote {
        bool live = false;
        std::uint32_t price = 0;
        std::uint32_t remaining = 0;
        std::uint64_t ahead = 0;     // shares resting ahead of us at our price
        std::uint64_t join_seq = 0;  // orders with seq <= this were there before us
        // a pending change: becomes effective at `at`
        bool pending = false;
        std::uint64_t at = 0;
        std::uint32_t target = 0;    // 0 = cancel
    };

    struct State {
        std::string symbol;
        Quote bid, ask;
        std::int64_t inventory = 0;
        std::int64_t cash = 0;  // price units (dollars x 10^4) x shares
        std::uint64_t filled_shares = 0;
        std::int64_t max_abs_inventory = 0;
        std::uint64_t quote_updates = 0;
        std::vector<Fill> fills;
        std::size_t next_mark[3] = {0, 0, 0};
        std::uint32_t last_mid2 = 0;  // bid + ask (twice the mid), last seen during trading hours
    };

    MarketMaker(MmConfig cfg, std::vector<std::string> symbols)
        : cfg_(cfg), wanted_(symbols.begin(), symbols.end()) {}

    // --- observer interface (called by BookBuilder)

    void before(std::uint64_t ts, const StockBook& b) {
        State* s = state_for(b);
        if (!s) return;
        apply_due(ts, b, s->bid, 'B');
        apply_due(ts, b, s->ask, 'S');
        mark(ts, *s, b);
    }

    void on_add(std::uint64_t ts, const StockBook& b, const BookOrder& o) {
        last_seq_ = o.seq;
        after(ts, b);
    }

    void on_execute(std::uint64_t ts, const StockBook& b, const BookOrder& o, std::uint32_t shares, std::uint32_t,
                    std::uint32_t) {
        if (State* s = state_for(b)) {
            Quote& q = o.side == 'B' ? s->bid : s->ask;
            if (q.live) {
                const bool worse = o.side == 'B' ? o.price < q.price : o.price > q.price;
                if (o.price == q.price) {
                    if (o.seq <= q.join_seq || q.ahead > 0) {
                        // An order ahead of us traded. (An order behind us trading while shares are still
                        // ahead cannot happen under price time priority; if it does, assume conservatively
                        // that it used up queue ahead of us rather than reaching us.)
                        q.ahead -= std::min<std::uint64_t>(q.ahead, shares);
                    } else {  // nothing ahead and the aggressor reached an order behind us: it would have hit us
                        fill(ts, *s, q, o.side, std::min(shares, q.remaining));
                    }
                } else if (worse) {  // swept through our level
                    fill(ts, *s, q, o.side, q.remaining);
                }
            }
        }
        after(ts, b);
    }

    void on_reduce(std::uint64_t ts, const StockBook& b, const BookOrder& o, std::uint32_t shares, std::uint32_t) {
        if (State* s = state_for(b)) {
            Quote& q = o.side == 'B' ? s->bid : s->ask;
            if (q.live && o.price == q.price && o.seq <= q.join_seq) q.ahead -= std::min<std::uint64_t>(q.ahead, shares);
        }
        after(ts, b);
    }

    void on_hidden_trade(std::uint64_t, const StockBook&, char, std::uint32_t, std::uint32_t) {}

    // --- results

    const std::vector<State>& states() const { return states_; }

    struct Summary {
        std::string symbol;
        std::uint64_t fills, shares, updates;
        std::int64_t inventory, max_inventory;
        double spread_pnl, rebates, net_pnl;  // dollars, inventory marked at the last mid
        std::array<double, 3> markout;       // average dollars per share
    };

    Summary summary(const State& s) const {
        Summary r{s.symbol, s.fills.size(), s.filled_shares, s.quote_updates, s.inventory, s.max_abs_inventory,
                  0, 0, 0, {}};
        const double mark = s.last_mid2 / 2.0;
        r.spread_pnl = (static_cast<double>(s.cash) + static_cast<double>(s.inventory) * mark) / 1e4;
        r.rebates = cfg_.rebate_per_share * static_cast<double>(s.filled_shares);
        r.net_pnl = r.spread_pnl + r.rebates;
        for (int h = 0; h < 3; ++h) {
            double sum = 0, shares = 0;
            for (const Fill& f : s.fills)
                if (f.marked > h) {
                    sum += f.markout[static_cast<std::size_t>(h)] * f.shares;
                    shares += f.shares;
                }
            r.markout[static_cast<std::size_t>(h)] = shares > 0 ? sum / shares : 0;
        }
        return r;
    }

private:
    State* state_for(const StockBook& b) {
        if (b.locate >= index_.size()) index_.resize(static_cast<std::size_t>(b.locate) + 1, kUnseen);
        int& slot = index_[b.locate];
        if (slot == kUnseen) {
            if (b.symbol[0] == ' ') return nullptr;  // directory message not seen yet
            if (!wanted_.count(b.name())) {
                slot = kIgnored;
            } else {
                states_.push_back(State{});
                states_.back().symbol = b.name();
                slot = static_cast<int>(states_.size() - 1);
            }
        }
        return slot < 0 ? nullptr : &states_[static_cast<std::size_t>(slot)];
    }

    static constexpr int kUnseen = -2, kIgnored = -1;

    bool trading(std::uint64_t ts) const { return ts >= cfg_.start_ns && ts < cfg_.stop_ns; }

    // After the real book changed: follow the touch with each side's quote.
    void after(std::uint64_t ts, const StockBook& b) {
        State* s = state_for(b);
        if (!s) return;
        const std::uint32_t bb = b.best_bid(), ba = b.best_ask();
        if (bb && ba && ba > bb && trading(ts)) s->last_mid2 = bb + ba;
        steer(ts, *s, s->bid, 'B', bb, ba);
        steer(ts, *s, s->ask, 'S', bb, ba);
    }

    void steer(std::uint64_t ts, State& s, Quote& q, char side, std::uint32_t bb, std::uint32_t ba) {
        std::uint32_t want = 0;
        const bool book_ok = bb && ba && ba > bb && trading(ts);
        if (book_ok) {
            const bool room = side == 'B' ? s.inventory + static_cast<std::int64_t>(cfg_.lot) <= cfg_.max_inventory
                                          : s.inventory - static_cast<std::int64_t>(cfg_.lot) >= -cfg_.max_inventory;
            if (room) want = side == 'B' ? bb : ba;
        }
        const std::uint32_t have = q.pending ? q.target : (q.live ? q.price : 0);
        if (have == want) return;
        // A quote better than the touch means the level we joined emptied around us: step back to the real touch.
        q.pending = true;
        q.at = ts + cfg_.latency_ns;
        q.target = want;
        ++s.quote_updates;
    }

    // A change we decided on earlier reaches the exchange now. Uses the book as
    // it is just before the current message.
    void apply_due(std::uint64_t ts, const StockBook& b, Quote& q, char side) {
        if (!q.pending || q.at > ts) return;
        q.pending = false;
        q.live = false;
        if (q.target == 0) return;
        // A buy at or above the current ask would take liquidity: as a post only order it is rejected.
        const std::uint32_t bb = b.best_bid(), ba = b.best_ask();
        if (side == 'B' ? (ba && q.target >= ba) : (bb && q.target <= bb)) return;
        const PxLevel* lv = b.side(side).find(q.target);
        q.live = true;
        q.price = q.target;
        q.remaining = cfg_.lot;
        q.ahead = lv ? lv->total : 0;
        q.join_seq = last_seq_;
    }

    void fill(std::uint64_t ts, State& s, Quote& q, char resting_side, std::uint32_t shares) {
        if (shares == 0) return;
        const std::int64_t signed_shares = resting_side == 'B' ? shares : -static_cast<std::int64_t>(shares);
        s.inventory += signed_shares;
        s.cash -= signed_shares * static_cast<std::int64_t>(q.price);
        s.filled_shares += shares;
        s.max_abs_inventory = std::max<std::int64_t>(s.max_abs_inventory, s.inventory < 0 ? -s.inventory : s.inventory);
        s.fills.push_back(Fill{ts, resting_side, q.price, shares});
        q.remaining -= shares;
        if (q.remaining == 0) q.live = false;  // the next book update re-quotes
    }

    void mark(std::uint64_t ts, State& s, const StockBook& b) {
        const std::uint32_t bb = b.best_bid(), ba = b.best_ask();
        if (!(bb && ba && ba > bb)) return;
        const double mid = (bb + ba) / 2.0;
        for (std::size_t h = 0; h < 3; ++h) {
            while (s.next_mark[h] < s.fills.size() && s.fills[s.next_mark[h]].ts + kHorizons[h] <= ts) {
                Fill& f = s.fills[s.next_mark[h]++];
                const double sign = f.side == 'B' ? 1.0 : -1.0;
                f.markout[h] = sign * (mid - f.price) / 1e4;
                f.marked = static_cast<int>(h) + 1 > f.marked ? static_cast<int>(h) + 1 : f.marked;
            }
        }
    }

    MmConfig cfg_;
    std::unordered_set<std::string> wanted_;
    std::vector<State> states_;
    std::vector<int> index_;  // by stock locate: state index, kUnseen or kIgnored
    std::uint64_t last_seq_ = 0;
};

}  // namespace me::itch
