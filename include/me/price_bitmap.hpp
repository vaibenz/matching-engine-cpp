#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace me {

// One bit per price, set when that level has resting orders, plus a summary
// word with one bit per 64 bit word. Finding the next non empty level above
// or below a price is then a couple of ctz/clz instructions per 4,096 ticks
// instead of a tick by tick scan, so a sparse book costs the same as a
// dense one.
class PriceBitmap {
public:
    static constexpr std::int64_t kNone = -1;

    explicit PriceBitmap(std::int64_t max_price)
        : words_(static_cast<std::size_t>(max_price / 64 + 1), 0),
          summary_(words_.size() / 64 + 1, 0) {}

    void set(std::int64_t p) {
        auto w = static_cast<std::size_t>(p >> 6);
        if (words_[w] == 0) summary_[w >> 6] |= 1ULL << (w & 63);
        words_[w] |= 1ULL << (p & 63);
    }

    void clear(std::int64_t p) {
        auto w = static_cast<std::size_t>(p >> 6);
        words_[w] &= ~(1ULL << (p & 63));
        if (words_[w] == 0) summary_[w >> 6] &= ~(1ULL << (w & 63));
    }

    bool test(std::int64_t p) const { return (words_[static_cast<std::size_t>(p >> 6)] >> (p & 63)) & 1; }

    // lowest set price >= p, or kNone
    std::int64_t next_at_or_after(std::int64_t p) const {
        auto w = static_cast<std::size_t>(p >> 6);
        if (w >= words_.size()) return kNone;
        if (std::uint64_t bits = words_[w] & (~0ULL << (p & 63))) return static_cast<std::int64_t>((w << 6) + std::countr_zero(bits));
        std::size_t ws = w + 1;
        for (std::size_t s = ws >> 6; s < summary_.size(); ++s) {
            std::uint64_t sb = summary_[s];
            if (s == (ws >> 6)) sb &= (ws & 63) ? ~0ULL << (ws & 63) : ~0ULL;
            if (sb) {
                std::size_t wi = (s << 6) + std::countr_zero(sb);
                return static_cast<std::int64_t>((wi << 6) + std::countr_zero(words_[wi]));
            }
        }
        return kNone;
    }

    // highest set price <= p, or kNone
    std::int64_t prev_at_or_before(std::int64_t p) const {
        if (p < 0) return kNone;
        auto w = static_cast<std::size_t>(p >> 6);
        std::uint64_t low = (p & 63) == 63 ? ~0ULL : (1ULL << ((p & 63) + 1)) - 1;
        if (std::uint64_t bits = words_[w] & low) return static_cast<std::int64_t>((w << 6) + 63 - std::countl_zero(bits));
        if (w == 0) return kNone;
        std::size_t ws = w - 1;
        for (std::size_t s = (ws >> 6) + 1; s-- > 0;) {
            std::uint64_t sb = summary_[s];
            if (s == (ws >> 6)) sb &= (ws & 63) == 63 ? ~0ULL : (1ULL << ((ws & 63) + 1)) - 1;
            if (sb) {
                std::size_t wi = (s << 6) + 63 - std::countl_zero(sb);
                return static_cast<std::int64_t>((wi << 6) + 63 - std::countl_zero(words_[wi]));
            }
        }
        return kNone;
    }

private:
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace me
