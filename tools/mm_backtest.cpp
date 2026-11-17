// Runs the passive market making backtest over an ITCH day file.
//
//   mm_backtest <file>[.gz] --symbols AAPL,MSFT,... [--latency-us 50] [--lot 100]
//               [--max-inventory 500] [--rebate 0.0020]
//
// Every stock's book is rebuilt (the builder needs every message anyway), and
// the strategy quotes the listed symbols. See include/me/itch/backtest.hpp for
// the fill model and its assumptions.
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "me/itch/backtest.hpp"
#include "me/itch/book_builder.hpp"
#include "me/itch/file_reader.hpp"

using namespace me::itch;

namespace {

const char* kUsage =
    "usage: mm_backtest <file>[.gz] --symbols A,B,C [--latency-us N] [--lot N] "
    "[--max-inventory N] [--rebate dollars]\n";

// A whole number in [lo, hi], nothing else. std::stoull reads "-5" as 2^64 - 5, so
// "--latency-us -5" used to mean a latency of centuries; from_chars refuses a sign.
bool parse_uint(const std::string& v, std::uint64_t lo, std::uint64_t hi, std::uint64_t& out) {
    const char* end = v.data() + v.size();
    auto [p, ec] = std::from_chars(v.data(), end, out);
    return ec == std::errc() && p == end && !v.empty() && out >= lo && out <= hi;
}

int usage_error(const std::string& why) {
    std::fprintf(stderr, "mm_backtest: %s\n%s", why.c_str(), kUsage);
    return 2;
}

}  // namespace

// Options are checked before the replay starts: a typo used to be skipped silently and
// only show up as an empty report after reading the whole day.
int main(int argc, char** argv) try {
    if (argc < 3) {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }
    MmConfig cfg;
    std::vector<std::string> symbols;
    for (int i = 2; i < argc; i += 2) {
        std::string k = argv[i];
        if (i + 1 >= argc) return usage_error(k + " needs a value");
        std::string v = argv[i + 1];
        std::uint64_t n = 0;
        if (k == "--symbols") {
            std::stringstream ss(v);
            for (std::string s; std::getline(ss, s, ',');) {
                if (s.empty()) return usage_error("empty symbol in --symbols " + v);
                symbols.push_back(s);
            }
        } else if (k == "--latency-us") {
            if (!parse_uint(v, 0, std::numeric_limits<std::uint64_t>::max() / 1000, n))
                return usage_error("--latency-us wants a whole number of microseconds, not " + v);
            cfg.latency_ns = n * 1000;
        } else if (k == "--lot") {
            if (!parse_uint(v, 1, std::numeric_limits<std::uint32_t>::max(), n))
                return usage_error("--lot wants a positive whole number of shares, not " + v);
            cfg.lot = static_cast<std::uint32_t>(n);
        } else if (k == "--max-inventory") {
            if (!parse_uint(v, 0, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), n))
                return usage_error("--max-inventory wants a whole number of shares, not " + v);
            cfg.max_inventory = static_cast<std::int64_t>(n);
        } else if (k == "--rebate") {
            char* end = nullptr;
            cfg.rebate_per_share = std::strtod(v.c_str(), &end);
            if (v.empty() || *end != '\0' || !std::isfinite(cfg.rebate_per_share))
                return usage_error("--rebate wants dollars per share, not " + v);
        } else {
            return usage_error("unknown option " + k);
        }
    }
    if (symbols.empty()) return usage_error("--symbols is required");

    BookBuilder<MarketMaker> builder(MarketMaker(cfg, symbols), 1 << 22);
    std::uint64_t bytes = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t n = stream_batches(argv[1], [&](const Message* m, std::size_t k) { builder.on_batch(m, k); }, &bytes);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("%llu messages in %.1f s | lot %u, max inventory %lld, latency %llu us, rebate $%.4f/share\n\n",
                static_cast<unsigned long long>(n), secs, cfg.lot, static_cast<long long>(cfg.max_inventory),
                static_cast<unsigned long long>(cfg.latency_ns / 1000), cfg.rebate_per_share);
    std::printf("%-8s %7s %10s %11s %10s %11s %8s %8s  %s\n", "stock", "fills", "shares", "spread pnl", "rebates",
                "net pnl", "max inv", "updates", "markout c/share at 100ms / 1s / 10s");
    double total = 0, total_spread = 0;
    std::uint64_t total_shares = 0;
    const MarketMaker& mm = builder.observer();
    for (const auto& st : mm.states()) {
        auto r = mm.summary(st);
        total += r.net_pnl;
        total_spread += r.spread_pnl;
        total_shares += r.shares;
        std::printf("%-8s %7llu %10llu %11.2f %10.2f %11.2f %8lld %8llu  %+.2f / %+.2f / %+.2f\n", r.symbol.c_str(),
                    static_cast<unsigned long long>(r.fills), static_cast<unsigned long long>(r.shares), r.spread_pnl,
                    r.rebates, r.net_pnl, static_cast<long long>(r.max_inventory),
                    static_cast<unsigned long long>(r.updates), r.markout[0] * 100, r.markout[1] * 100,
                    r.markout[2] * 100);
    }
    std::printf("\ntotal: %llu shares, spread pnl $%.2f, net pnl $%.2f (inventory marked at the last mid)\n",
                static_cast<unsigned long long>(total_shares), total_spread, total);
    return 0;
} catch (const std::exception& e) {
    // A missing, unreadable or truncated day file throws from the reader. Report it and exit 1
    // instead of letting std::terminate abort with a core dump.
    std::fprintf(stderr, "mm_backtest: %s\n", e.what());
    return 1;
}
