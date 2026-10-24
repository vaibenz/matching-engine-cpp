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

template <class Book>
inline void apply(Book& book, const Command& c) {
    switch (c.msg) {
        case Msg::Submit: book.submit(c.id, c.side, c.type, c.price, c.qty); break;
        case Msg::Cancel: book.cancel(c.id); break;
        case Msg::Modify: book.modify(c.id, c.price, c.qty); break;
    }
}

}  // namespace me::wire
