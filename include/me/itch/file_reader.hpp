#pragma once

// Streams an ITCH day file (raw, or gzip decompressed on the fly with zlib) in
// large chunks, carrying any message split across two reads over to the next.

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "messages.hpp"

namespace me::itch {

// Calls on_batch(const Message*, n) with every complete message in each chunk read.
template <class F>
inline std::uint64_t stream_batches(const std::string& path, F&& on_batch, std::uint64_t* bytes) {
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
inline std::uint64_t stream_file(const std::string& path, F&& on_message, std::uint64_t* bytes) {
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

}  // namespace me::itch
