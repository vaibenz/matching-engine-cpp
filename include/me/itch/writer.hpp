#pragma once

// Encodes ITCH 5.0 messages, each with the two byte length prefix used in
// Nasdaq's files. Used to turn the matching engine's events into a feed whose
// correct book is known exactly, for testing and benchmarking the builder.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "messages.hpp"

namespace me::itch {

class Writer {
public:
    std::vector<std::uint8_t>& buffer() { return buf_; }
    void clear() { buf_.clear(); }

    void system_event(std::uint64_t ts, char code) {
        std::uint8_t* p = start('S', 0, ts);
        p[11] = static_cast<std::uint8_t>(code);
    }

    void stock_directory(std::uint64_t ts, std::uint16_t locate, std::string_view symbol) {
        std::uint8_t* p = start('R', locate, ts);
        std::memset(p + 11, ' ', 8);
        std::memcpy(p + 11, symbol.data(), std::min<std::size_t>(symbol.size(), 8));
        p[19] = 'Q';  // market category: Nasdaq Global Select
        p[20] = 'N';  // financial status: normal
        put32(p + 21, 100);  // round lot size
        p[25] = 'N';
    }

    void add(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
             std::string_view symbol, std::uint32_t price, bool attributed = false) {
        std::uint8_t* p = start(attributed ? 'F' : 'A', locate, ts);
        put64(p + 11, ref);
        p[19] = static_cast<std::uint8_t>(side);
        put32(p + 20, shares);
        std::memset(p + 24, ' ', 8);
        std::memcpy(p + 24, symbol.data(), std::min<std::size_t>(symbol.size(), 8));
        put32(p + 32, price);
        if (attributed) std::memcpy(p + 36, "MPID", 4);
    }

    void executed(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                  std::uint64_t match) {
        std::uint8_t* p = start('E', locate, ts);
        put64(p + 11, ref);
        put32(p + 19, shares);
        put64(p + 23, match);
    }

    void executed_with_price(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                             std::uint64_t match, std::uint32_t price) {
        std::uint8_t* p = start('C', locate, ts);
        put64(p + 11, ref);
        put32(p + 19, shares);
        put64(p + 23, match);
        p[31] = 'Y';
        put32(p + 32, price);
    }

    void cancel(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
        std::uint8_t* p = start('X', locate, ts);
        put64(p + 11, ref);
        put32(p + 19, shares);
    }

    void remove(std::uint64_t ts, std::uint16_t locate, std::uint64_t ref) {
        std::uint8_t* p = start('D', locate, ts);
        put64(p + 11, ref);
    }

    void replace(std::uint64_t ts, std::uint16_t locate, std::uint64_t old_ref, std::uint64_t new_ref,
                 std::uint32_t shares, std::uint32_t price) {
        std::uint8_t* p = start('U', locate, ts);
        put64(p + 11, old_ref);
        put64(p + 19, new_ref);
        put32(p + 27, shares);
        put32(p + 31, price);
    }

    void hidden_trade(std::uint64_t ts, std::uint16_t locate, char side, std::uint32_t shares,
                      std::string_view symbol, std::uint32_t price, std::uint64_t match) {
        std::uint8_t* p = start('P', locate, ts);
        put64(p + 11, 0);
        p[19] = static_cast<std::uint8_t>(side);
        put32(p + 20, shares);
        std::memset(p + 24, ' ', 8);
        std::memcpy(p + 24, symbol.data(), std::min<std::size_t>(symbol.size(), 8));
        put32(p + 32, price);
        put64(p + 36, match);
    }

private:
    std::uint8_t* start(char type, std::uint16_t locate, std::uint64_t ts) {
        const std::uint16_t len = kLength[static_cast<std::uint8_t>(type)];
        const std::size_t at = buf_.size();
        buf_.resize(at + 2 + len, 0);
        std::uint8_t* p = buf_.data() + at;
        put16(p, len);
        p += 2;
        p[0] = static_cast<std::uint8_t>(type);
        put16(p + 1, locate);
        put16(p + 3, 0);
        put48(p + 5, ts);
        return p;
    }

    std::vector<std::uint8_t> buf_;
};

}  // namespace me::itch
