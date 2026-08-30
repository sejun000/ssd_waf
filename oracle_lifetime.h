#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Oracle death-time index for cold-tier placement studies (experiment "A").
//
// A cold copy of block X dies exactly when the host next writes X — that write
// lands in the cache and trims the cold copy (log_cache.cpp: _invalidate_cold_
// block(TRIM)). So the death time depends only on the trace, not on the cache
// policy, and one pass over the trace yields it for every block.
//
// The index is built in REMAPPED block space: the preprocessing pass replays the
// trace in the same order as the simulator and allocates 4K blocks on first
// touch exactly like --remap_lba does, so dense id == (mapped_lba − reserve)/4K.
// Times are counted in trace write pages (inject writes excluded, matching the
// clock the simulator feeds to next_write_time()).
//
// Off by default: nothing is built unless --cold_oracle_streams K (K>0).
class OracleLifetime {
public:
    // Returns false (and leaves the object disabled) if the trace can't be read.
    bool Build(const std::string& trace_file, const std::string& trace_format,
               uint64_t block_size, bool loop_trace);

    bool enabled() const { return enabled_; }
    uint64_t loop_pages() const { return loop_pages_; }

    // Next write time (in trace pages) for dense block `id` strictly after
    // `now_pages`. Wraps into the next loop iteration when looping; returns
    // kNever when the block is never written again.
    static constexpr uint64_t kNever = UINT64_MAX;
    uint64_t NextWriteTime(uint64_t id, uint64_t now_pages) const;

private:
    bool     enabled_    = false;
    bool     loop_       = false;
    uint64_t loop_pages_ = 0;                 // trace write pages per full pass
    std::vector<uint64_t> offsets_;           // CSR: per-block slice into times_
    std::vector<uint32_t> times_;             // write times, ascending per block
};
