#pragma once
// Bit for bit port of java.util.Random (48 bit LCG), so the C++ benchmark
// generates exactly the same order flow as the Java engine's benchmark for
// the same seed, and the two can be compared on identical input.
#include <cstdint>

namespace me::bench {

class JavaRandom {
public:
    explicit JavaRandom(std::int64_t seed) : seed_((static_cast<std::uint64_t>(seed) ^ kMult) & kMask) {}

    std::int32_t next_int(std::int32_t bound) {
        std::int32_t r = next(31);
        const std::int32_t m = bound - 1;
        if ((bound & m) == 0) {
            return static_cast<std::int32_t>((static_cast<std::int64_t>(bound) * r) >> 31);
        }
        // Java's rejection loop relies on int overflow in `u - r + m < 0`
        for (std::int32_t u = r; static_cast<std::int64_t>(u) - (r = u % bound) + m > INT32_MAX; u = next(31)) {
        }
        return r;
    }

    bool next_bool() { return next(1) != 0; }

private:
    static constexpr std::uint64_t kMult = 0x5DEECE66DULL;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;

    std::int32_t next(int bits) {
        seed_ = (seed_ * kMult + 0xBULL) & kMask;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(seed_ >> (48 - bits)));
    }

    std::uint64_t seed_;
};

}  // namespace me::bench
