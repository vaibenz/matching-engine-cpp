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
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "me/itch/book_builder.hpp"
#include "me/itch/engine_feed.hpp"
#include "me/itch/messages.hpp"

using namespace me::itch;
using Clock = std::chrono::steady_clock;

namespace {

// Reads raw or gzip files through zlib (gzread passes plain files through).
// Calls on_batch(const Message*, n) with every complete message in each chunk read.
template <class F>
std::uint64_t stream_batches(const std::string& path, F&& on_batch, std::uint64_t* bytes) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    gzbuffer(f, 1 << 20);
    std::vector<std::uint8_t> buf(16 << 20);
    std::vector<Message> batch;
    batch.reserve(1 << 20);
    std::size_t have = 0;
    std::uint64_t messages = 0;
    for (;;) {
        int n = gzread(f, buf.data() + have, static_cast<unsigned>(buf.size() - have));
        if (n < 0) throw std::runtime_error("read error in " + path);
        if (n == 0) break;
        *bytes += static_cast<std::uint64_t>(n);
        have += static_cast<std::size_t>(n);
        batch.clear();
        std::size_t used = Framer::each(buf.data(), have, [&](const Message& m) { batch.push_back(m); });
        messages += batch.size();
        on_batch(batch.data(), batch.size());
        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;
    }
    gzclose(f);
    return messages;
}

template <class F>
std::uint64_t stream_file(const std::string& path, F&& on_message, std::uint64_t* bytes) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    gzbuffer(f, 1 << 20);
    std::vector<std::uint8_t> buf(16 << 20);
    std::size_t have = 0;
    std::uint64_t messages = 0;
    for (;;) {
        int n = gzread(f, buf.data() + have, static_cast<unsigned>(buf.size() - have));
        if (n < 0) throw std::runtime_error("read error in " + path);
        if (n == 0) break;
        *bytes += static_cast<std::uint64_t>(n);
        have += static_cast<std::size_t>(n);
        std::size_t used = Framer::each(buf.data(), have, [&](const Message& m) {
            ++messages;
            on_message(m);
        });
        std::memmove(buf.data(), buf.data() + used, have - used);  // keep a partial message for the next read
        have -= used;
    }
    gzclose(f);
    if (have != 0) std::fprintf(stderr, "warning: %zu trailing bytes do not form a complete message\n", have);
    return messages;
}

std::string money(std::uint64_t price) {  // four implied decimals
    char s[32];
    std::snprintf(s, sizeof s, "%llu.%04llu", static_cast<unsigned long long>(price / 10000),
                  static_cast<unsigned long long>(price % 10000));
    return s;
}

int synthetic(std::uint64_t target, const std::string& out) {
    SyntheticSession session(1000, 7);
    std::ofstream f(out, std::ios::binary);
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
    std::printf("wrote %llu messages to %s\n", static_cast<unsigned long long>(written), out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--synthetic") return synthetic(std::stoull(argv[2]), argv[3]);
    if (argc < 2) {
        std::fprintf(stderr, "usage: itch_replay <file>[.gz] [--top N] [--parse-only]\n"
                             "       itch_replay --synthetic <messages> <out file>\n");
        return 2;
    }
    std::string path = argv[1];
    int top = 10;
    bool parse_only = false, no_prefetch = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--top" && i + 1 < argc) top = std::stoi(argv[++i]);
        if (a == "--parse-only") parse_only = true;
        if (a == "--no-prefetch") no_prefetch = true;
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
    std::printf("feed anomalies: %llu unknown references, %llu overfills, %llu duplicate adds\n",
                static_cast<unsigned long long>(st.unknown_ref), static_cast<unsigned long long>(st.overfill),
                static_cast<unsigned long long>(st.duplicate_ref));
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
}
