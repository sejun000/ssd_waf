#pragma once

#include "icache.h"
#include "log_cache_segment.h"
#include "histogram.h"

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

struct FairyWrenConfig {
    std::size_t segment_bytes = 32ull * 1024 * 1024; // 32MB
    double      fwlog_ratio   = 0.05;                // 5% of all segments
    double      hot_ratio_of_remaining = 0.5;        // 50% of remaining after fwlog
};

class FairyWrenCache final : public ICache {
public:
    FairyWrenCache(uint64_t           cold_capacity,
                   uint64_t           cache_block_count,
                   int                cache_block_size,
                   bool               cache_trace,
                   const std::string& trace_file,
                   const std::string& cold_trace_file,
                   std::string&       waf_log_file,
                   const FairyWrenConfig& cfg = FairyWrenConfig(),
                   std::string        stat_log_file = "");
    ~FairyWrenCache() override;

    bool        exists(long key) override;
    void        touch(long, OP_TYPE) override {}
    std::size_t size() override { return mapping_.size(); }
    void        batch_insert(int stream_id,
                             const std::map<long, int>& newBlocks,
                             OP_TYPE op_type = OP_TYPE::WRITE) override;
    bool        is_cache_filled() override;
    int         get_block_size() override { return cache_block_size_; }
    void        evict_one_block() override;
    void        print_stats() override;

private:
    enum class RegionKind { FWLOG = 0, HOT = 1, COLD = 2, COUNT };

    struct RegionState {
        std::string                 name;
        std::size_t                 total_segments = 0;
        std::deque<LogCacheSegment*> free_segments;
        std::deque<LogCacheSegment*> fifo_segments;
        LogCacheSegment*            active = nullptr;
    };

    struct Loc {
        LogCacheSegment* seg = nullptr;
        std::size_t      idx = 0;
    };

    const int             cache_block_size_;
    const FairyWrenConfig cfg_;
    std::size_t           segment_size_blocks_ = 0;
    std::size_t           total_segments_      = 0;
    uint64_t              log_cache_timestamp_ = 0;
    uint64_t              total_cache_block_count_ = 0;
    uint64_t              total_capacity_bytes_     = 0;

    std::list<std::unique_ptr<LogCacheSegment>> all_segments_;
    std::array<RegionState, static_cast<std::size_t>(RegionKind::COUNT)> regions_;
    std::unordered_map<LogCacheSegment*, RegionKind> segment_region_;
    std::unordered_map<long, Loc> mapping_;
    std::array<uint64_t, static_cast<std::size_t>(RegionKind::COUNT)> region_valid_blocks_ = {0, 0, 0};

    void initialize_regions(uint64_t cache_block_count);
    RegionState& region(RegionKind kind);
    const RegionState& region(RegionKind kind) const;
    std::size_t region_index(RegionKind kind) const;

    LogCacheSegment* ensure_active_segment(RegionKind kind, bool allow_force = true);
    void             close_active_segment(RegionKind kind);
    void             push_to_fifo(RegionKind kind, LogCacheSegment* seg);
    LogCacheSegment* pop_victim(RegionKind kind);
    LogCacheSegment* take_free_segment(RegionKind kind);
    void             release_segment_to_free(RegionKind kind, LogCacheSegment* seg);
    bool             steal_free_segment(RegionKind donor, RegionKind receiver);
    void             move_segment_region(LogCacheSegment* seg, RegionKind new_kind);
    RegionKind       current_region(LogCacheSegment* seg) const;
    void             adjust_region_valid(LogCacheSegment* seg, int64_t delta);
    void             adjust_region_valid(RegionKind kind, int64_t delta);

    void append_host_block(long key, int lba_sz);
    void append_block(LogCacheSegment* seg, long key, uint64_t timestamp);
    bool migrate_block(LogCacheSegment* src, std::size_t idx, RegionKind dest);
    bool copy_valid_blocks(LogCacheSegment* src, RegionKind dest, uint64_t* moved_blocks = nullptr);
    void drop_segment(LogCacheSegment* seg);
    void invalidate(long key, int lba_sz);

    void maybe_run_gc();
    bool needs_gc(RegionKind kind, double ratio) const;
    bool reclaim_from_fwlog();
    bool reclaim_from_hot();
    bool reclaim_from_cold();
    bool force_reclaim(RegionKind kind);
    
    uint64_t migrated_blocks_       = 0;
    uint64_t invalidate_blocks_     = 0;
    uint64_t next_stats_print_bytes_ = 0;
    static constexpr uint64_t STATS_PRINT_INTERVAL = 10ull * 1024ull * 1024ull * 1024ull; // 10 GB
    static constexpr int HISTOGRAM_BUCKETS = 20;
    std::unique_ptr<Histogram> evicted_ages_histogram_;
    std::unique_ptr<Histogram> evicted_blocks_histogram_;
    std::unique_ptr<Histogram> migrated_blocks_histogram_;
    std::unique_ptr<Histogram> evicted_ages_with_segment_histogram_;
    std::unique_ptr<Histogram> migrated_ages_with_segment_histogram_;
    std::unique_ptr<Histogram> migrated_ages_histogram_;
};
