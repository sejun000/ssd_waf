#include "oracle_lifetime.h"
#include "trace_parser.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace {
// Flat original-block → dense-id table. The trace's LBA space is a couple of TB,
// so a direct array (4 B per 4K block) beats a hash map by a wide margin on the
// billions of lookups this build performs.
constexpr uint32_t kUnseen = UINT32_MAX;
}

bool OracleLifetime::Build(const std::string& trace_file,
                           const std::string& trace_format,
                           uint64_t block_size, bool loop_trace) {
    enabled_ = false;
    loop_    = loop_trace;

    std::unique_ptr<ITraceParser> parser(createTraceParser(trace_format));
    if (!parser) return false;

    // ── pass 1: assign dense ids in first-touch order (same order as --remap_lba)
    //            and count how many times each block is written.
    std::vector<uint32_t> dense;          // original block → dense id
    std::vector<uint32_t> counts;         // dense id → write count
    uint64_t pages = 0;
    {
        std::ifstream in(trace_file);
        if (!in) { fprintf(stderr, "[oracle] cannot open %s\n", trace_file.c_str()); return false; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ParsedRow r = parser->parseTrace(line);
            if (r.op_type != "W" || r.lba_size <= 0) continue;
            const uint64_t b0 = static_cast<uint64_t>(r.lba_offset) / block_size;
            const uint64_t b1 = (static_cast<uint64_t>(r.lba_offset) + r.lba_size - 1) / block_size;
            if (b1 >= dense.size()) dense.resize(b1 + 1, kUnseen);
            for (uint64_t b = b0; b <= b1; ++b, ++pages) {
                if (dense[b] == kUnseen) {
                    dense[b] = static_cast<uint32_t>(counts.size());
                    counts.push_back(0);
                }
                ++counts[dense[b]];
            }
        }
    }
    if (counts.empty()) return false;
    loop_pages_ = pages;
    if (pages > UINT32_MAX) {
        fprintf(stderr, "[oracle] trace has %lu write pages (> uint32) — disable oracle\n",
                (unsigned long)pages);
        return false;
    }

    // ── pass 2: fill per-block write times (ascending by construction).
    offsets_.assign(counts.size() + 1, 0);
    for (size_t i = 0; i < counts.size(); ++i) offsets_[i + 1] = offsets_[i] + counts[i];
    times_.assign(offsets_.back(), 0);
    {
        std::vector<uint64_t> fill(offsets_.begin(), offsets_.end() - 1);
        std::ifstream in(trace_file);
        std::string line;
        uint64_t t = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ParsedRow r = parser->parseTrace(line);
            if (r.op_type != "W" || r.lba_size <= 0) continue;
            const uint64_t b0 = static_cast<uint64_t>(r.lba_offset) / block_size;
            const uint64_t b1 = (static_cast<uint64_t>(r.lba_offset) + r.lba_size - 1) / block_size;
            for (uint64_t b = b0; b <= b1; ++b, ++t) {
                times_[fill[dense[b]]++] = static_cast<uint32_t>(t);
            }
        }
    }

    enabled_ = true;
    printf("[oracle] blocks=%zu write_pages=%lu (%.2f TB) loop=%s\n",
           counts.size(), (unsigned long)loop_pages_,
           loop_pages_ * 4096.0 / 1e12, loop_ ? "yes" : "no");
    return true;
}

uint64_t OracleLifetime::NextWriteTime(uint64_t id, uint64_t now_pages) const {
    if (!enabled_ || id + 1 >= offsets_.size()) return kNever;
    const uint64_t lo = offsets_[id], hi = offsets_[id + 1];
    if (lo >= hi) return kNever;
    // Position inside the current pass; the index is one pass long.
    const uint64_t pass  = loop_pages_ ? (now_pages / loop_pages_) : 0;
    const uint64_t phase = loop_pages_ ? (now_pages % loop_pages_) : now_pages;
    const auto beg = times_.begin() + lo, end = times_.begin() + hi;
    const auto it  = std::upper_bound(beg, end, static_cast<uint32_t>(phase));
    if (it != end) return pass * loop_pages_ + *it;
    if (!loop_) return kNever;                       // single pass: never again
    return (pass + 1) * loop_pages_ + *beg;          // wraps into the next loop
}
