#pragma once

#include <cstdint>

namespace me {

// Prices are integer ticks and quantities integer lots, so matching never
// touches floating point.
using OrderId = std::uint64_t;
using Price = std::int64_t;
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,     // matches up to its price, remainder rests
    Market,    // matches at any price, remainder cancelled
    IOC,       // matches up to its price, remainder cancelled
    FOK,       // fills completely up to its price or does nothing
    PostOnly,  // rests, rejected if it would take liquidity
};

enum class RejectReason : std::uint8_t {
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    UnknownOrderId,
    PostOnlyWouldTake,
};

enum class CancelReason : std::uint8_t {
    UserRequested,
    UnfilledRemainder,  // market / IOC leftover
    FillOrKillFailed,
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr bool has_price(OrderType t) noexcept { return t != OrderType::Market; }
constexpr bool can_rest(OrderType t) noexcept { return t == OrderType::Limit || t == OrderType::PostOnly; }

struct Trade {
    OrderId taker;
    OrderId maker;
    Side taker_side;
    Price price;  // always the resting (maker) price
    Qty qty;
};

// Default listener: every callback is an empty inline function, so with
// the listener as a template parameter the event path compiles to nothing.
struct NullListener {
    void on_accepted(OrderId, Side, OrderType, Price, Qty) {}
    void on_trade(const Trade&) {}
    void on_rested(OrderId, Side, Price, Qty) {}
    void on_cancelled(OrderId, Qty, CancelReason) {}
    void on_modified(OrderId, Price, Qty) {}
    void on_rejected(OrderId, RejectReason) {}
};

}  // namespace me
