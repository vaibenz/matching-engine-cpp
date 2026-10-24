#include <random>
#include <unordered_map>

#include "check.hpp"
#include "me/id_map.hpp"

using me::IdMap;

TEST(id_map_basic) {
    IdMap m(4);
    m.insert(10, 1);
    m.insert(20, 2);
    CHECK_EQ(m.find(10), 1u);
    CHECK_EQ(m.find(30), me::kNil);
    CHECK(m.erase(10));
    CHECK(!m.erase(10));
    CHECK_EQ(m.find(20), 2u);
    CHECK_EQ(m.size(), 1u);
}

// Random inserts and erases against std::unordered_map, with a tiny table
// so it grows several times and clusters wrap around the end.
TEST(id_map_matches_unordered_map) {
    std::mt19937_64 rng(7);
    IdMap m(2);
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    for (int i = 0; i < 200'000; ++i) {
        std::uint64_t k = rng() % 5000;
        if (rng() % 3 == 0) {
            CHECK_EQ(m.erase(k), ref.erase(k) == 1);
        } else if (!ref.count(k)) {
            auto v = static_cast<std::uint32_t>(rng() % 1'000'000);
            m.insert(k, v);
            ref[k] = v;
        }
        if (i % 1000 == 0) {
            for (auto [key, val] : ref) CHECK_EQ(m.find(key), val);
            CHECK_EQ(m.size(), ref.size());
        }
    }
}
