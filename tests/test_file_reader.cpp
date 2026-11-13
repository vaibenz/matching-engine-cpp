// The ITCH file reader on real files: raw and gzip, messages split across the
// 16 MB read chunks, and truncated input, which must be an error rather than a
// silently shorter day.
#include <zlib.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "me/itch/file_reader.hpp"
#include "me/itch/writer.hpp"

using namespace me::itch;

namespace {

std::string temp_path(const char* name) { return std::string(P_tmpdir) + "/me_test_" + name; }

void write_raw(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

std::vector<std::uint8_t> gzip(const std::vector<std::uint8_t>& bytes, const std::string& path) {
    gzFile g = gzopen(path.c_str(), "wb");
    gzwrite(g, bytes.data(), static_cast<unsigned>(bytes.size()));
    gzclose(g);
    std::vector<std::uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    for (int c; (c = std::fgetc(f)) != EOF;) out.push_back(static_cast<std::uint8_t>(c));
    std::fclose(f);
    return out;
}

// Enough adds to span more than one 16 MB read, so the chunk carry over is exercised too.
std::vector<std::uint8_t> session(std::uint64_t n) {
    Writer w;
    w.system_event(0, 'O');
    for (std::uint64_t i = 0; i < n; ++i) w.add(i + 1, 1, i + 1, 'B', 100, "X", 10'000 + static_cast<std::uint32_t>(i % 50));
    return w.buffer();
}

struct Read {
    std::uint64_t messages = 0, refs = 0, batched = 0;
    bool threw = false;
};

Read read_both_ways(const std::string& path) {
    Read r;
    std::uint64_t bytes = 0;
    try {
        r.messages = stream_file(path, [&](const Message& m) { r.refs += m.type() == 'A' ? m.add_ref() : 0; }, &bytes);
    } catch (const std::runtime_error&) {
        r.threw = true;
    }
    bool batch_threw = false;
    try {
        r.batched = stream_batches(path, [&](const Message*, std::size_t) {}, &bytes);
    } catch (const std::runtime_error&) {
        batch_threw = true;
    }
    CHECK_EQ(r.threw, batch_threw);  // both readers agree on what is a complete file
    return r;
}

}  // namespace

TEST(file_reader_reads_raw_and_gzip_across_chunks) {
    const std::uint64_t n = 500'000;  // 19 MB: more than one 16 MB read
    auto bytes = session(n);
    const std::uint64_t ref_sum = n * (n + 1) / 2;
    std::string raw = temp_path("full.itch"), gz = temp_path("full.itch.gz");
    write_raw(raw, bytes);
    gzip(bytes, gz);
    for (const std::string& path : {raw, gz}) {
        Read r = read_both_ways(path);
        CHECK(!r.threw);
        CHECK_EQ(r.messages, n + 1);
        CHECK_EQ(r.batched, n + 1);
        CHECK_EQ(r.refs, ref_sum);
    }
    std::remove(raw.c_str());
    std::remove(gz.c_str());
}

TEST(file_reader_rejects_a_truncated_gzip_stream) {
    auto bytes = session(20'000);
    std::string gz = temp_path("cut.itch.gz");
    auto compressed = gzip(bytes, gz);
    // Cut the compressed file at a few points, as an interrupted download would.
    for (std::size_t keep : {compressed.size() / 3, compressed.size() / 2, compressed.size() - 9}) {
        write_raw(gz, std::vector<std::uint8_t>(compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(keep)));
        CHECK(read_both_ways(gz).threw);
    }
    std::remove(gz.c_str());
}

TEST(file_reader_rejects_a_partial_last_message) {
    auto bytes = session(1'000);
    bytes.resize(bytes.size() - 5);  // the last add is cut off
    std::string raw = temp_path("partial.itch");
    write_raw(raw, bytes);
    CHECK(read_both_ways(raw).threw);
    std::remove(raw.c_str());
}
