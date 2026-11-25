#pragma once

// Streams an ITCH day file (raw, or gzip decompressed on the fly with zlib) in
// large chunks, carrying any message split across two reads over to the next.
//
// A truncated file is an error, not a shorter day: a gzip stream that ends
// early (an interrupted download of a multi GB file) or bytes left over that
// do not form a whole message both throw once everything before them has been
// delivered, so a replay or backtest never reports results for half a day as
// if it were the whole of it.

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "messages.hpp"

namespace me::itch {

namespace detail {

struct GzClose {
    void operator()(gzFile_s* f) const { gzclose(f); }
};
using GzFile = std::unique_ptr<gzFile_s, GzClose>;  // closed even when a callback throws

inline GzFile open_gz(const std::string& path) {
    GzFile f(gzopen(path.c_str(), "rb"));
    if (!f) throw std::runtime_error("cannot open " + path);
    gzbuffer(f.get(), 1 << 20);
    return f;
}

// After the last read: zlib reports Z_BUF_ERROR when the input ended in the
// middle of a gzip stream, and any carried over bytes are a cut off message.
// A file with no bytes at all (a download that never started, or a gzip of
// nothing) is not a quiet day either: a real day file always has messages.
inline void check_complete(gzFile f, const std::string& path, std::size_t leftover, std::uint64_t read) {
    if (read == 0) throw std::runtime_error(path + ": the file is empty, there is no day to replay");
    int err = Z_OK;
    gzerror(f, &err);
    if (err == Z_BUF_ERROR) throw std::runtime_error(path + ": gzip stream ends early, the file is truncated");
    if (leftover != 0)
        throw std::runtime_error(path + ": " + std::to_string(leftover) +
                                 " trailing bytes do not form a complete message, the file is truncated");
}

}  // namespace detail

// Calls on_batch(const Message*, n) with every complete message in each chunk read.
template <class F>
inline std::uint64_t stream_batches(const std::string& path, F&& on_batch, std::uint64_t* bytes) {
    detail::GzFile file = detail::open_gz(path);
    gzFile f = file.get();
    std::vector<std::uint8_t> buf(16 << 20);
    std::vector<Message> batch;
    batch.reserve(1 << 20);
    std::size_t have = 0;
    std::uint64_t messages = 0, read = 0;
    for (;;) {
        int n = gzread(f, buf.data() + have, static_cast<unsigned>(buf.size() - have));
        if (n < 0) throw std::runtime_error("read error in " + path);
        if (n == 0) break;
        *bytes += static_cast<std::uint64_t>(n);
        read += static_cast<std::uint64_t>(n);
        have += static_cast<std::size_t>(n);
        batch.clear();
        std::size_t used = 0;
        try {
            used = Framer::each(buf.data(), have, [&](const Message& m) { batch.push_back(m); });
        } catch (...) {
            on_batch(batch.data(), batch.size());  // the messages before a corrupt one, as stream_file delivers them
            throw;
        }
        messages += batch.size();
        on_batch(batch.data(), batch.size());
        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;
    }
    detail::check_complete(f, path, have, read);
    return messages;
}

template <class F>
inline std::uint64_t stream_file(const std::string& path, F&& on_message, std::uint64_t* bytes) {
    detail::GzFile file = detail::open_gz(path);
    gzFile f = file.get();
    std::vector<std::uint8_t> buf(16 << 20);
    std::size_t have = 0;
    std::uint64_t messages = 0, read = 0;
    for (;;) {
        int n = gzread(f, buf.data() + have, static_cast<unsigned>(buf.size() - have));
        if (n < 0) throw std::runtime_error("read error in " + path);
        if (n == 0) break;
        *bytes += static_cast<std::uint64_t>(n);
        read += static_cast<std::uint64_t>(n);
        have += static_cast<std::size_t>(n);
        std::size_t used = Framer::each(buf.data(), have, [&](const Message& m) {
            ++messages;
            on_message(m);
        });
        std::memmove(buf.data(), buf.data() + used, have - used);  // keep a partial message for the next read
        have -= used;
    }
    detail::check_complete(f, path, have, read);
    return messages;
}

}  // namespace me::itch
