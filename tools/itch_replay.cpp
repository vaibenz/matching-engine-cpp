// Replays a Nasdaq TotalView-ITCH 5.0 day file and rebuilds every stock's
// book, order by order.
//
//   itch_replay <file>[.gz] [--top N] [--parse-only]
//   itch_replay --synthetic <messages> <out file>   write a synthetic session
//
// Nasdaq publishes sample days at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
// (for example 01302019.NASDAQ_ITCH50.gz). Files are read in large chunks and
// gzip is decompressed on the fly with zlib, so the whole day never has to fit
// in memory.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "me/itch/book_builder.hpp"
#include "me/itch/engine_feed.hpp"
#include "me/itch/file_reader.hpp"
#include "me/itch/messages.hpp"

using namespace me::itch;
using Clock = std::chrono::steady_clock;

namespace {

// Reads raw or gzip files through zlib (gzread passes plain files through).
std::string money(std::uint64_t price) {  // four implied decimals
    char s[32];
    std::snprintf(s, sizeof s, "%llu.%04llu", static_cast<unsigned long long>(price / 10000),
                  static_cast<unsigned long long>(price % 10000));
    return s;
}

int synthetic(std::uint64_t target, const std::string& out) {
    SyntheticSession session(1000, 7);
    session.set_mean_gap_ns(36'000'000'000'000ULL / std::max<std::uint64_t>(target, 1));  // about 6 hours of trading
    std::ofstream f(out, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + out);
    std::uint64_t written = 0;
    auto flush = [&] {
        auto& b = session.writer().buffer();
        std::uint64_t n = 0;
        Framer::each(b.data(), b.size(), [&](const Message&) { ++n; });
        written += n;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        session.writer().clear();
    };
    flush();
    while (written < target) {
        for (int i = 0; i < 10'000; ++i) session.step();
        flush();
    }
    f.close();
    if (!f) throw std::runtime_error("write error in " + out);  // a full disk must not look like success
    std::printf("wrote %llu messages to %s\n", static_cast<unsigned long long>(written), out.c_str());
    return 0;
}

const char* kUsage =
    "usage: itch_replay <file>[.gz] [--top N] [--parse-only] [--no-prefetch]\n"
    "       itch_replay --synthetic <messages> <out file>\n";

// A whole number in [lo, hi], nothing else: std::stoi threw out of main on "--top x",
// and std::stoull read "--synthetic -1" as 2^64 - 1 messages.
bool parse_uint(const std::string& v, std::uint64_t lo, std::uint64_t hi, std::uint64_t& out) {
    const char* end = v.data() + v.size();
    auto [p, ec] = std::from_chars(v.data(), end, out);
    return ec == std::errc() && p == end && !v.empty() && out >= lo && out <= hi;
}

int usage_error(const std::string& why) {
    std::fprintf(stderr, "itch_replay: %s\n%s", why.c_str(), kUsage);
    return 2;
}

}  // namespace

// Options are checked before anything is read: an unknown one ("--parse_only") used to be
// skipped, so a typo cost a full replay of the day with the wrong settings.
int main(int argc, char** argv) try {
    if (argc >= 2 && std::string(argv[1]) == "--synthetic") {
        std::uint64_t target = 0;
        if (argc != 4) return usage_error("--synthetic wants a message count and an output file");
        if (!parse_uint(argv[2], 1, std::uint64_t{1} << 40, target))
            return usage_error(std::string("--synthetic wants a positive number of messages, not ") + argv[2]);
        return synthetic(target, argv[3]);
    }
    if (argc < 2 || argv[1][0] == '\0' || argv[1][0] == '-') {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }
    std::string path = argv[1];
    int top = 10;
    bool parse_only = false, no_prefetch = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--top") {
            std::uint64_t n = 0;
            if (i + 1 >= argc) return usage_error("--top needs a value");
            if (!parse_uint(argv[++i], 0, 1'000'000, n))
                return usage_error(std::string("--top wants a whole number of stocks, not ") + argv[i]);
            top = static_cast<int>(n);
        } else if (a == "--parse-only") {
            parse_only = true;
        } else if (a == "--no-prefetch") {
            no_prefetch = true;
        } else {
            return usage_error("unknown option " + a);
        }
    }

    std::uint64_t bytes = 0;
    if (parse_only) {
        std::uint64_t checksum = 0;
        auto t0 = Clock::now();
        std::uint64_t n = stream_file(path, [&](const Message& m) { checksum += m.timestamp(); }, &bytes);
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("%llu messages, %.1f MB, %.2f s: %.1f M msgs/s (framing and parsing only, checksum %llu)\n",
                    static_cast<unsigned long long>(n), bytes / 1e6, s, n / s / 1e6,
                    static_cast<unsigned long long>(checksum));
        return 0;
    }

    BookBuilder<> builder({}, 1 << 22);
    auto t0 = Clock::now();
    std::uint64_t n = no_prefetch ? stream_file(path, [&](const Message& m) { builder.on(m); }, &bytes)
                                  : stream_batches(path, [&](const Message* m, std::size_t k) { builder.on_batch(m, k); },
                                                   &bytes);
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    const BuilderStats& st = builder.stats();
    std::printf("%llu messages (%.1f MB) in %.2f s: %.1f M msgs/s, %.0f ns per message, books rebuilt order by order\n",
                static_cast<unsigned long long>(n), bytes / 1e6, secs, n / secs / 1e6, secs * 1e9 / static_cast<double>(n));
    std::printf("peak live orders %llu, live at end %zu\n", static_cast<unsigned long long>(st.peak_orders),
                builder.live_orders());
    std::printf("feed anomalies: %llu unknown references, %llu overfills, %llu duplicate adds, %llu adds with a bad side, %llu adds for zero shares, %llu adds at a price of 0\n",
                static_cast<unsigned long long>(st.unknown_ref), static_cast<unsigned long long>(st.overfill),
                static_cast<unsigned long long>(st.duplicate_ref), static_cast<unsigned long long>(st.bad_side),
                static_cast<unsigned long long>(st.zero_shares), static_cast<unsigned long long>(st.zero_price));
    std::printf("by type:");
    for (int t = 0; t < 128; ++t)
        if (st.by_type[t]) std::printf(" %c=%llu", t, static_cast<unsigned long long>(st.by_type[t]));
    std::printf("\n");

    std::vector<const StockBook*> active;
    for (const StockBook& b : builder.books())
        if (b.messages > 0) active.push_back(&b);
    std::size_t crossed = 0;
    for (const StockBook* b : active)
        if (b->best_bid() && b->best_ask() && b->best_bid() >= b->best_ask()) ++crossed;
    std::printf("%zu stocks with order activity, %zu locked or crossed at the end of the file\n", active.size(), crossed);

    std::sort(active.begin(), active.end(), [](auto a, auto b) { return a->messages > b->messages; });
    std::printf("\n%-8s %12s %14s %12s %12s %12s %8s\n", "stock", "messages", "shares traded", "vwap", "bid", "ask",
                "levels");
    for (int i = 0; i < top && i < static_cast<int>(active.size()); ++i) {
        const StockBook& b = *active[static_cast<std::size_t>(i)];
        std::uint64_t vwap = b.executed_shares ? b.executed_notional / b.executed_shares : 0;
        std::printf("%-8s %12llu %14llu %12s %12s %12s %8zu\n", b.name().c_str(),
                    static_cast<unsigned long long>(b.messages), static_cast<unsigned long long>(b.executed_shares),
                    money(vwap).c_str(), money(b.best_bid()).c_str(), money(b.best_ask()).c_str(),
                    b.bids.size() + b.asks.size());
    }
    return 0;
} catch (const std::exception& e) {
    // A missing, unreadable or truncated day file throws from the reader. Report it and exit 1
    // instead of letting std::terminate abort with a core dump.
    std::fprintf(stderr, "itch_replay: %s\n", e.what());
    return 1;
}
