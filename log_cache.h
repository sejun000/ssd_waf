#pragma once
#include "icache.h"              // 기존 프로젝트 공용 인터페이스
#include "log_cache_segment.h"
#include "evict_policy.h"
#include "evict_policy_greedy.h"
#include "istream.h"
#include "histogram.h"
#include "emwa_ratio.h"
#include "ghost_cache.h"

#include <unordered_map>
#include <deque>
#include <list>
#include <memory>
#include <map>
#include <string>
#include <random>

/**
 * Multi‑stream append‑only Log Cache (세그먼트 단위)
 */
struct Config
{
    //std::size_t segment_bytes  = 32ull * 1024 * 1024; ///< default 32 MB
    std::size_t segment_bytes  = 6ull * 1024 * 1024 * 1024; ///< default 6 GB

    double      free_ratio_low = 0.04;                ///< 25% (~50 segments for 512 total)
    int         evicted_blk_size = 1;    // 4k eviction
    uint64_t         print_stats_interval = 10 * 1024ull * 1024 * 1024; // 10 GB
};

#define GHOST_CACHE 1

class LogCache final : public ICache
{
public:
    LogCache(uint64_t              cold_capacity,
             uint64_t              cache_block_count,
             int                   cache_block_size,
             bool                  cache_trace,
             const std::string&    trace_file,
             const std::string&    cold_trace,
             std::string&    waf_log_file,
             std::unique_ptr<EvictPolicy> ev =
                 std::make_unique<GreedyEvictPolicy>(),
             const Config*         cfg           = nullptr,
             IStream *input_stream_policy = nullptr,
             double target_valid_blk_rate = 0.0,
             std::unique_ptr<EvictPolicy> compactor = nullptr,
             double max_age_ratio_by_gc = 0.0,
             bool input_ghost_cache = false,
             std::string stat_log_file = "",
             double valid_rate_period_gb = 0.0,
             double valid_rate_min = 0.0,
             double valid_rate_max = 0.0,
             double periodic_ratio = 2.88
            );

    ~LogCache();

    /* ICache overrides */
    bool        exists(long key) override;
    void        touch(long, OP_TYPE) override {}              // no‑op
    std::size_t size() override { return mapping.size(); }

    /* 새로운 API – stream id 포함 */
    void batch_insert(int stream_id, const std::map<long,int>& newBlocks,
                      OP_TYPE                   op_type = OP_TYPE::WRITE);
    int get_block_size() override;
    void evict_one_block() override;
    void evict(LogCacheSegment::Block &blk);
    bool is_cache_filled() override;
    virtual void print_stats() override;
    void print_objects(std::string prefix, uint64_t value);
    void invalidate(long key, int lba_sz);
    void reset_segment(LogCacheSegment *seg);
    void dummy_fill_segment(LogCacheSegment* s);
    //void do_evict_and_compaction_with_same_policy();
    
    


private:
    /* configuration ******************************************************/
    const int         cache_block_size;
    Config            cfg_;

    /* segment pools ******************************************************/
    std::deque<LogCacheSegment*>                free_pool;
    std::list<std::unique_ptr<LogCacheSegment>> all_segments; // owner
    std::unordered_map<int, LogCacheSegment*>   active_seg;   // stream→seg
    std::unordered_map<int, LogCacheSegment*>   gc_active_seg;   // stream→seg

    /* page lookup ********************************************************/
    struct Loc { LogCacheSegment* seg; std::size_t idx;};
    std::unordered_map<long, Loc>                mapping;
    std::unordered_map<long, uint64_t>                evicted_timestamp; // for GC

    /* helpers ************************************************************/
    std::unique_ptr<EvictPolicy> evictor;

    LogCacheSegment* alloc_segment(bool shrink = true);
    void             check_and_evict_if_needed(int max_victims = 0);
    void             evict_segment(LogCacheSegment* s);
    Segment*         evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id = 0);

    void             evict_policy_add(LogCacheSegment *s);
    void             evict_policy_remove(LogCacheSegment *s);
    void             evict_policy_update(LogCacheSegment *s);
    LogCacheSegment* get_segment_to_active_stream(bool gc, int stream, bool check_only = false);
    LogCacheSegment* get_segment_with_stream_policy(bool gc, uint64_t key, bool check_only = false);
    void periodic();

    /* trace(optional) *****************************************************/
    bool  cache_trace_;
    FILE* trace_fp_      = nullptr;
    FILE* cold_trace_fp_ = nullptr;
    std::size_t segment_size_blocks;
    std::size_t total_segments;
    uint64_t total_cache_block_count = 0;

    uint64_t total_capacity_bytes = 0;
    uint64_t log_cache_timestamp = 0; // per 4kB block
    IStream *stream_policy = nullptr;
    uint64_t global_valid_blocks = 0;
    uint64_t compacted_blocks = 0;
    uint64_t invalidate_blocks = 0;
    uint64_t reinsert_blocks = 0;
    uint64_t ghost_cache_evicted_blocks = 0;
    uint64_t read_blocks_in_partial_write = 0;

    uint64_t evicted_segment_age = 0;
    uint64_t gc_victim_count = 0;
    double gc_victim_valid_ratio_sum = 0.0;
    uint64_t dummy_fill_segment_count = 0;

    double target_valid_blk_rate = 0.0; // ratio of write to QLC
    double valid_blk_rate_hard_limit = 0.0;
    std::unique_ptr<EvictPolicy> compactor;
    double additional_free_blks_ratio_by_gc;
    
    std::unique_ptr<Histogram> evicted_ages_histogram;
    std::unique_ptr<Histogram> evicted_blocks_histogram;
    std::unique_ptr<Histogram> compacted_blocks_histogram;
    std::unique_ptr<Histogram> evicted_ages_with_segment_histogram;
    std::unique_ptr<Histogram> compacted_ages_with_segment_histogram;
    std::unique_ptr<Histogram> evicted_cache_blocks_per_evict;
    std::unordered_map<long, uint64_t> compacted_at_;
    std::unique_ptr<Histogram> compacted_lifetime_histogram_;
    static const int HISTOGRAM_BUCKETS = 20;
    static const uint64_t DEFAULT_HALF_LIFE_IN_BLOCKS = (262144 * 6) * 4;
    static constexpr double TCO_EVICTION_WEIGHT = 2.8;
    static const std::size_t TCO_HISTORY_SIZE = 4;
    bool is_ghost_cache = false;
    uint64_t bypass_blocks_threshold = 128; // 128* 4k bytes = 512K bytes
    EwmaRatio compaction_ratio;
    EwmaRatio eviction_ratio;
    EwmaRatio eviction_ratio_in_ghost_cache;
    EwmaRatio compaction_ratio_in_ghost_cache;
    double periodic_ratio_ = 2.88;
    EwmaRatio ghost_util_ratio;  // ghost miss rate = U(util_step)
    GhostCache ghost_cache;
    uint64_t ghost_compacted_blocks = 0;
    uint64_t ghost_access_total = 0;
    uint64_t ghost_miss_total = 0;
    static constexpr double UTIL_STEP = 0.02;
    std::deque<double> tco_history;
    bool tco_policy_higher = true;

    /* ── Lifetime histogram (entire trace) ────────────────── */
    bool lifetime_tracking_active_ = false;
    static constexpr uint64_t LIFETIME_BUCKET_WIDTH = 262144; // 1 GB in 4KB blocks
    std::map<uint64_t, uint64_t> lifetime_hist_invalidate_;   // bucket → count
    std::map<uint64_t, uint64_t> lifetime_hist_evict_;        // bucket → count

    void record_lifetime(uint64_t lifetime, bool is_host_invalidate);
    void print_lifetime_results();

    /* ── Rewrite interval tracking (no-cache baseline) ──── */
    std::unordered_map<long, uint64_t> rewrite_last_ts_;  // LBA → last write ts
    std::map<uint64_t, uint64_t> rewrite_hist_;            // bucket → count

    void record_rewrite(long key);
    void print_rewrite_results();

    /* ── Segment utilization distribution (LFS-style) ──── */
    void print_utilization_distribution();

    /* ── Per-segment age scatter data ─────────────────── */
    void print_segment_age_scatter();

    /* ── Invalidation time snapshot ──────────────────── */
    static constexpr long long INV_SNAPSHOT_THRESHOLD = 10LL * 1024 * 1024 * 1024 * 1024; // 10TB
    bool inv_snapshot_taken_ = false;
    uint64_t inv_snapshot_ts_ = 0;
    struct InvSnapSegInfo {
        uint64_t seg_age;
        double utilization;
        uint64_t valid_count;
        double age_mean;
        double age_stddev;
        int class_num;
    };
    std::vector<InvSnapSegInfo> inv_snap_segs_;
    std::unordered_map<long, size_t> inv_snap_block_seg_idx_;
    std::vector<std::vector<uint64_t>> inv_snap_inv_times_;
    void take_inv_snapshot();
    void record_inv_time(long key);
    void print_inv_time_scatter();

    /* ── Periodic valid rate sweep ─────────────────────── */
    double valid_rate_period_gb_ = 0.0;
    double valid_rate_min_ = 0.0;
    double valid_rate_max_ = 0.0;
    uint64_t valid_rate_period_blocks_ = 0;
    uint64_t next_valid_rate_change_ts_ = 0;
    std::mt19937 valid_rate_rng_{42};

    /* ── A/B feedback: net free segs vs compaction cost ── */
    uint64_t gc_active_alloc_count_ = 0;   // cumulative gc active segments allocated
    uint64_t cumulative_B_ = 0;            // cumulative valid pages from get_mth_score_valid_pages
    EwmaRatio net_free_seg_ratio_;         // EWMA of A (gc_victim_count - gc_active_alloc_count_)
    EwmaRatio gc_valid_pages_ratio_;       // EWMA of B (cumulative valid pages)
};
