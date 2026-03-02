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
             std::string stat_log_file = ""
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
    EwmaRatio ghost_util_ratio;  // ghost miss rate = U(util_step)
    GhostCache ghost_cache;
    uint64_t ghost_compacted_blocks = 0;
    uint64_t ghost_access_total = 0;
    uint64_t ghost_miss_total = 0;
    static constexpr double UTIL_STEP = 0.02;
    std::deque<double> tco_history;
    bool tco_policy_higher = true;

    /* ── Survival analysis: age → time-to-death ───────────── */
    bool survival_snapshot_taken_ = false;
    uint64_t survival_snapshot_ts_ = 0;

    struct SurvivalEntry {
        uint64_t age_at_snapshot;
    };
    std::unordered_map<long, SurvivalEntry> survival_tracker_;

    static const int SURVIVAL_BUCKETS = 100;
    uint64_t survival_bucket_width_ = 1;

    struct SurvivalResult {
        uint64_t sum_ttd_invalidate = 0;   // host overwrite
        uint64_t count_invalidate = 0;
        uint64_t sum_ttd_evict = 0;        // flush / eviction
        uint64_t count_evict = 0;
    };
    std::vector<SurvivalResult> survival_results_;

    void take_survival_snapshot();
    void record_survival_death(long key, bool is_host_invalidate);
    void print_survival_results();

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
};
