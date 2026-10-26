#pragma once

// Nasdaq TotalView-ITCH 5.0: the binary feed Nasdaq publishes for every order
// event on its book. Every message starts with a one byte type, a two byte
// stock locate code, a two byte tracking number and a six byte timestamp
// (nanoseconds since midnight). All integers are big endian. Prices are
// unsigned 32 bit with four implied decimals ($12.34 is 123400).
//
// In Nasdaq's daily files each message is preceded by a two byte big endian
// length. Reading is zero copy: a view points straight into the file buffer
// and decodes fields on access.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace me::itch {

inline std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

inline std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

inline std::uint64_t be48(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = v << 8 | p[i];
    return v;
}

inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 3; i >= 0; --i, v >>= 8) p[i] = static_cast<std::uint8_t>(v);
}

inline void put48(std::uint8_t* p, std::uint64_t v) {
    for (int i = 5; i >= 0; --i, v >>= 8) p[i] = static_cast<std::uint8_t>(v);
}

inline void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i, v >>= 8) p[i] = static_cast<std::uint8_t>(v);
}

// Message lengths from the ITCH 5.0 specification. 0 means unknown type.
constexpr std::array<std::uint16_t, 128> kLength = [] {
    std::array<std::uint16_t, 128> l{};
    l['S'] = 12;  // system event
    l['R'] = 39;  // stock directory
    l['H'] = 25;  // stock trading action
    l['Y'] = 20;  // Reg SHO restriction
    l['L'] = 26;  // market participant position
    l['V'] = 35;  // MWCB decline level
    l['W'] = 12;  // MWCB status
    l['K'] = 28;  // IPO quoting period update
    l['J'] = 35;  // LULD auction collar
    l['h'] = 21;  // operational halt
    l['A'] = 36;  // add order, no attribution
    l['F'] = 40;  // add order with MPID attribution
    l['E'] = 31;  // order executed
    l['C'] = 36;  // order executed with price
    l['X'] = 23;  // order cancel (partial)
    l['D'] = 19;  // order delete
    l['U'] = 35;  // order replace
    l['P'] = 44;  // trade (non cross, against hidden liquidity)
    l['Q'] = 40;  // cross trade
    l['B'] = 19;  // broken trade
    l['I'] = 50;  // NOII
    l['N'] = 20;  // retail price improvement indicator
    l['O'] = 48;  // direct listing with capital raise
    return l;
}();

// A view of one message. Field offsets are from the specification; accessors
// exist only for the fields the book builder and tools need.
struct Message {
    const std::uint8_t* p;
    std::uint16_t len;

    char type() const { return static_cast<char>(p[0]); }
    std::uint16_t locate() const { return be16(p + 1); }
    std::uint64_t timestamp() const { return be48(p + 5); }

    // A, F
    std::uint64_t add_ref() const { return be64(p + 11); }
    char add_side() const { return static_cast<char>(p[19]); }
    std::uint32_t add_shares() const { return be32(p + 20); }
    std::string_view add_stock() const { return {reinterpret_cast<const char*>(p + 24), 8}; }
    std::uint32_t add_price() const { return be32(p + 32); }

    // E, C, X, D: the order reference is always at offset 11
    std::uint64_t ref() const { return be64(p + 11); }
    std::uint32_t exec_shares() const { return be32(p + 19); }   // E, C
    std::uint64_t match_number() const { return be64(p + 23); }  // E, C
    char printable() const { return static_cast<char>(p[31]); }  // C
    std::uint32_t exec_price() const { return be32(p + 32); }    // C
    std::uint32_t cancel_shares() const { return be32(p + 19); } // X

    // U
    std::uint64_t replace_old() const { return be64(p + 11); }
    std::uint64_t replace_new() const { return be64(p + 19); }
    std::uint32_t replace_shares() const { return be32(p + 27); }
    std::uint32_t replace_price() const { return be32(p + 31); }

    // R
    std::string_view directory_stock() const { return {reinterpret_cast<const char*>(p + 11), 8}; }

    // S
    char event_code() const { return static_cast<char>(p[11]); }

    // P (trades against hidden orders: no effect on the visible book)
    std::uint32_t trade_shares() const { return be32(p + 20); }
    std::uint32_t trade_price() const { return be32(p + 32); }
};

// Splits a buffer of length prefixed messages. Stops cleanly at a partial
// message at the end of the buffer and reports how many bytes it used, so the
// caller can carry the remainder over to the next read.
class Framer {
public:
    // Calls f(Message) for each complete message. Returns bytes consumed, or
    // throws on a message whose length does not match its type.
    template <class F>
    static std::size_t each(const std::uint8_t* data, std::size_t size, F&& f) {
        std::size_t pos = 0;
        while (pos + 2 <= size) {
            const std::uint16_t len = be16(data + pos);
            if (pos + 2 + len > size) break;  // partial message: wait for more bytes
            const std::uint8_t* msg = data + pos + 2;
            if (len == 0) throw std::runtime_error("zero length ITCH message");
            const std::uint8_t t = msg[0];
            if (t < 128 && kLength[t] != 0 && kLength[t] != len)
                throw std::runtime_error(std::string("bad length ") + std::to_string(len) + " for ITCH type " +
                                         static_cast<char>(t));
            f(Message{msg, len});
            pos += 2 + len;
        }
        return pos;
    }
};

}  // namespace me::itch
