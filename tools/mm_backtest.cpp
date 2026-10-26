// Runs the passive market making backtest over an ITCH day file.
//
//   mm_backtest <file>[.gz] --symbols AAPL,MSFT,... [--latency-us 50] [--lot 100]
//               [--max-inventory 500] [--rebate 0.0020]
//
// Every stock's book is rebuilt (the builder needs every message anyway), and
// the strategy quotes the listed symbols. See include/me/itch/backtest.hpp for
// the fill model and its assumptions.
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "me/itch/backtest.hpp"
#include "me/itch/book_builder.hpp"
#include "me/itch/file_reader.hpp"

using namespace me::itch;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mm_backtest <file>[.gz] --symbols A,B,C [--latency-us N] [--lot N] "
                             "[--max-inventory N] [--rebate dollars]\n");
        return 2;
    }
    MmConfig cfg;
    std::vector<std::string> symbols;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--symbols") {
            std::stringstream ss(v);
            for (std::string s; std::getline(ss, s, ',');) symbols.push_back(s);
        } else if (k == "--latency-us") {
            cfg.latency_ns = std::stoull(v) * 1000;
        } else if (k == "--lot") {
            cfg.lot = static_cast<std::uint32_t>(std::stoul(v));
        } else if (k == "--max-inventory") {
            cfg.max_inventory = std::stoll(v);
        } else if (k == "--rebate") {
            cfg.rebate_per_share = std::stod(v);
        }
    }

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
}
