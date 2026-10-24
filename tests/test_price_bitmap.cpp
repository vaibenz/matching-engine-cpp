#include <random>
#include <set>

#include "check.hpp"
#include "me/price_bitmap.hpp"

using me::PriceBitmap;

// Compare against std::set on random sets, including the edges of words
// (63, 64) and of summary words (4095, 4096).
TEST(price_bitmap_matches_std_set) {
    constexpr std::int64_t kMax = 20'000;
    std::mt19937_64 rng(3);
    for (int round = 0; round < 30; ++round) {
        PriceBitmap bm(kMax);
        std::set<std::int64_t> ref;
        int fill = 1 + static_cast<int>(rng() % 60);
        for (int i = 0; i < fill; ++i) {
            std::int64_t p = rng() % 4 == 0 ? std::int64_t{4095 + static_cast<std::int64_t>(rng() % 3)}
                                            : static_cast<std::int64_t>(rng() % (kMax + 1));
            bm.set(p);
            ref.insert(p);
        }
        for (int i = 0; i < fill / 3; ++i) {
            auto it = std::next(ref.begin(), static_cast<long>(rng() % ref.size()));
            bm.clear(*it);
            ref.erase(it);
        }
        for (std::int64_t p : {std::int64_t{0}, std::int64_t{63}, std::int64_t{64}, std::int64_t{4095},
                               std::int64_t{4096}, kMax, static_cast<std::int64_t>(rng() % kMax)}) {
            auto up = ref.lower_bound(p);
            CHECK_EQ(bm.next_at_or_after(p), up == ref.end() ? PriceBitmap::kNone : *up);
            auto dn = ref.upper_bound(p);
            CHECK_EQ(bm.prev_at_or_before(p), dn == ref.begin() ? PriceBitmap::kNone : *std::prev(dn));
        }
    }
}
