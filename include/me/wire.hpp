#pragma once
// Fixed 32 byte binary command, the format the gateway thread writes into
// the ring. Plain little endian struct, no varints, so decode is a copy.
#include <cstdint>
#include <cstring>

#include "types.hpp"

namespace me::wire {

enum class Msg : std::uint8_t { Submit = 1, Cancel = 2, Modify = 3 };

struct Command {
    Msg msg;
    Side side;
    OrderType type;
    std::uint8_t pad;
    std::int32_t qty;
    OrderId id;
    Price price;
    std::uint64_t sent_tsc;  // gateway timestamp, for one way latency
};
static_assert(sizeof(Command) == 32);

// Applies a command to the book. Returns false, leaving the book untouched,
// for a command whose message, side or order type byte is out of range: the
// book tests sides as "== Buy or else Sell" and types by what they allow, so
// a stray byte would otherwise be applied as some other, valid command.
template <class Book>
inline bool apply(Book& book, const Command& c) {
    switch (c.msg) {
        case Msg::Submit:
            if (c.side != Side::Buy && c.side != Side::Sell) return false;
            if (static_cast<std::uint8_t>(c.type) > static_cast<std::uint8_t>(OrderType::PostOnly)) return false;
            book.submit(c.id, c.side, c.type, c.price, c.qty);
            return true;
        case Msg::Cancel: book.cancel(c.id); return true;
        case Msg::Modify: book.modify(c.id, c.price, c.qty); return true;
    }
    return false;
}

}  // namespace me::wire
