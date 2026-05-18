#pragma once
#include "icache.h"              // 기존 프로젝트 공용 인터페이스
#include "log_cache_segment.h"
#include "evict_policy.h"
#include "evict_policy_greedy.h"
#include "istream.h"
#include "histogram.h"
#include "emwa_ratio.h"
#include "ghost_cache.h"
#include "auto_tune/gp_tuner.h"

#include <unordered_map>
#include <array>
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
    uint64_t         print_stats_interval = 6ull * 1024 * 1024 * 1024; // 6 GiB (= segment_bytes)
};

#define GHOST_CACHE 1

enum class PeriodicMode {
    GhostDelta,   // 기존 ghost-cache 기반 비대칭 비교 (eviction delta vs compaction amount)
    GhostDelta_GC, // GC 적용된 ghost-cache 기반 비대칭 비교 (G(u+δ)-G(u) 예측)
    GhostDelta_GC_NAND, // GC 비대칭 비교 + cold-tier WAF (current_waf) 가중
    TimeDelta,    // t-delta hill-climb: f = compacted + r*evicted 의 windowed Δ로 방향 결정
    GhostDelta_GC_AUTO, // GhostDelta_GC + GP/UCB autotuner over (dir, HL, window, step)
    GhostDelta_GC_SUM,  // G(u+θ) via cumulative CB-sorted scan: m s.t. Σ(seg-v_i)=θ·N·seg
    GhostDelta_GC_SUM_Replay, // Replay: load gsdec.log, force target = log.cur_util[i] + util_step_
    GhostDelta_GC_SUM_Final,  // cum_valid accumulator + LHS=r·waf·δ + RHS=Gud−Gu
};

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
             double periodic_ratio = 2.88,
             double input_util_step = 0.02
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
    void periodic_ghost_delta();
    void periodic_ghost_delta_gc();
    void periodic_t_delta();
    void periodic_ghost_delta_gc_nand();
    void periodic_ghost_delta_gc_auto();
    void periodic_ghost_delta_gc_sum();
    void periodic_ghost_delta_gc_sum_replay();   // Replay variant: target = log.cur_util[i] + util_step_
    void periodic_ghost_delta_gc_sum_final();    // cum_valid accumulator + LHS=r·waf·δ + RHS=Gud−Gu
    void periodic_gs_predict_track();   // Phase 1: passive candidate-segs tracking

    // Per-compaction ghost-signal updates.  Called from check_and_evict_if_needed
    // every time a real compaction event commits.  Separated by policy:
    //   - update_ghost_compacted_blocks: GhostDelta_GC / GC_NAND / GC_AUTO
    //     (single m-th segment estimate, scaled by (1-u_cur)/(1-u_m)).
    //   - update_ghost_compacted_blocks_sum: GhostDelta_GC_SUM
    //     (CB-sorted cumulative scan up to θ·N·seg, rate * dt accumulator).
    void update_ghost_compacted_blocks(LogCacheSegment* victim);
    void update_ghost_compacted_blocks_sum();
    void update_ghost_compacted_blocks_sum_cum();  // Final: tick-based cum_valid accumulator

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
    double util_step_ = 0.02;  // ghost cache size factor + GC anchor step (per-policy override)
    int    gs_decision_period_segs_ = 8;  // GS hill-climb decision period (in segments)
    EwmaRatio compaction_ratio;
    EwmaRatio eviction_ratio;
    EwmaRatio eviction_ratio_in_ghost_cache;
    EwmaRatio compaction_ratio_in_ghost_cache;
    double periodic_ratio_ = 2.88;
    EwmaRatio ghost_util_ratio;  // ghost miss rate = U(util_step)
    GhostCache ghost_cache;
    uint64_t ghost_compacted_blocks = 0;
    // GhostDelta_GC_SUM state: synthetic cumulative counter advanced at each
    // periodic tick by (timestamp_delta × u_avg/(1-u_avg)) where u_avg comes
    // from the cumulative CB scan up to θ·N·seg invalid pages.
    double   ghost_compacted_blocks_sum_ = 0.0;
    uint64_t last_ghost_sum_ts_ = 0;
    bool     ghost_sum_initialized_ = false;  // first comp event: skip dt accum, just anchor ts
    uint64_t last_invalidate_at_comp_ = 0;    // invalidate_blocks snapshot at prev real comp
    uint64_t last_compacted_at_ghost_sum_ = 0; // compacted_blocks snapshot at prev tick (for realized-correction)

    // Candidate-segs prediction tracker (Phase 1): for k ∈ {segs-1, segs, segs+1}
    // emit G(u+δ_k) at every segment/4 tick, buffer with timestamp, and on
    // entries older than δ_k compute |G_realized - G_predicted| / G_realized.
    // Used to compare prediction accuracy across neighbouring δ choices without
    // actually changing the live policy.
    // SMA window for smoothed RMSE: fixed 64 segments × 4 ticks/segment = 256.
    // All cands share the same absolute window (so noise-rate effects matter,
    // not WIN/δ ratio).
    static constexpr size_t kSmoothWin = 256;
    struct GsCandTracker {
        int      segs       = 0;
        double   util_step  = 0.0;
        // raw drain pred / real totals (for per-cand mean reporting)
        double   g_pred_sum_raw = 0.0;
        double   g_real_sum_raw = 0.0;
        double   f_pred_sum_raw = 0.0;
        double   f_real_sum_raw = 0.0;
        // G(u+δ) prediction tracking — raw err
        std::deque<std::pair<uint64_t, double>> g_pred_buf;  // (t_emit, G_pred)
        double   g_err_sum    = 0.0;   // Σ |Δ| / G_realized   (relative)
        double   g_sq_err_sum = 0.0;   // Σ (Δ)^2              (absolute)
        uint64_t g_err_n      = 0;
        // G smoothed err (SMA of pred / real over last kSmoothWin paired drains)
        std::deque<double> g_pred_win;
        std::deque<double> g_real_win;
        double   g_pred_win_sum = 0.0;
        double   g_real_win_sum = 0.0;
        double   g_smooth_err_sum    = 0.0;
        double   g_smooth_sq_err_sum = 0.0;
        uint64_t g_smooth_err_n      = 0;
        // F(u+δ) prediction tracking — raw err
        std::deque<std::pair<uint64_t, double>> f_pred_buf;
        double   f_err_sum    = 0.0;
        double   f_sq_err_sum = 0.0;
        uint64_t f_err_n      = 0;
        // F smoothed err
        std::deque<double> f_pred_win;
        std::deque<double> f_real_win;
        double   f_pred_win_sum = 0.0;
        double   f_real_win_sum = 0.0;
        double   f_smooth_err_sum    = 0.0;
        double   f_smooth_sq_err_sum = 0.0;
        uint64_t f_smooth_err_n      = 0;
    };
    std::array<GsCandTracker, 3> gs_cand_;   // index 0,1,2 = segs-1, segs, segs+1
    bool gs_cand_initialized_ = false;
    uint64_t ghost_access_total = 0;
    uint64_t ghost_miss_total = 0;
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

    /* ── Periodic mode selector + t-delta hill-climb state ─────────────── */
    PeriodicMode periodic_mode_ = PeriodicMode::GhostDelta;
    bool     tdelta_have_prev_snapshot_ = false;
    bool     tdelta_have_prev_f_        = false;
    uint64_t tdelta_prev_compacted_     = 0;
    uint64_t tdelta_prev_evicted_       = 0;
    double   tdelta_prev_f_             = 0.0;
    int      tdelta_last_dir_           = +1;
    double   tdelta_step_               = 0.01;

    /* GhostDelta reanchoring step (default ±0.1 for original behavior) */
    double   ghost_reanchor_step_       = 0.1;
    std::string moving_avg_type_        = "ewma";
    double   moving_avg_window_         = (double)DEFAULT_HALF_LIFE_IN_BLOCKS;
    uint64_t lc_last_ftl_host_pages = 0;
    uint64_t lc_last_ftl_nand_pages = 0;
    double   current_waf = 0.0;

    /* GhostDelta_GC_AUTO state — GP/UCB autotuner state. gp_tuner_ is null
     * unless the AUTO policy is wired up; the periodic function then falls
     * back to stock GC behavior. */
    std::unique_ptr<auto_tune::GpTuner> gp_tuner_;
    double         auto_signed_step_  = auto_tune::kDefaultSignedStep;  // arm value: target = clamp(base + this)
    auto_tune::Arm auto_prev_arm_     = auto_tune::DefaultArm();
    std::string    autotune_csv_path_;
    FILE*          autotune_csv_      = nullptr;

    /* GhostDelta_GC_SUM_Replay state. Loaded once on first periodic call.
     * Each row holds (ts in blocks, cur_util) from gsdec.log. */
    std::vector<std::pair<uint64_t,double>> replay_log_;
    std::size_t   replay_idx_     = 0;
    bool          replay_loaded_  = false;
    bool          replay_failed_  = false;
    void load_replay_log_if_needed();

public:
    // GS hill-climb decision period in segments. Call BEFORE setPeriodicMode
    // (it's the override trigger). util_step_ + ghost_cache capacity are
    // auto-derived from this so the per-decision step matches one window's
    // host-write coverage (N·seg host-writes / total_cache_blocks).
    void setGsDecisionPeriodSegs(int n) {
        if (n > 0) gs_decision_period_segs_ = n;
    }
    int  getGsDecisionPeriodSegs() const { return gs_decision_period_segs_; }

    void setPeriodicMode(PeriodicMode m) {
        periodic_mode_ = m;
        if ((m == PeriodicMode::GhostDelta_GC_SUM ||
             m == PeriodicMode::GhostDelta_GC_SUM_Replay ||
             m == PeriodicMode::GhostDelta_GC_SUM_Final) &&
            total_cache_block_count > 0 && segment_size_blocks > 0) {
            util_step_ = static_cast<double>(segment_size_blocks * gs_decision_period_segs_)
                       / static_cast<double>(total_cache_block_count);
            // Resize the LRU/FIFO shadow so it represents exactly the θ-tail
            // the GS hill-climb reasons about. Shrinking happens immediately
            // (front-evicted with evict_count_ accounted); growing is lazy.
            ghost_cache.setCapacity(
                static_cast<std::size_t>(
                    static_cast<double>(total_cache_block_count) * util_step_));
        }
    }
    void setTdeltaStep(double s) { tdelta_step_ = s; }
    void setGhostReanchorStep(double s) { ghost_reanchor_step_ = s; }
    void setUtilStep(double s) { util_step_ = s; }  // post-ctor; ghost_cache size already fixed
    double getUtilStep() const { return util_step_; }
    // Replace all moving-average ratios with the given (type, window_blocks).
    // Must be called BEFORE first periodic() update for samples to be consistent.
    // AUTO-mode hooks (no-ops unless GhostDelta_GC_AUTO is selected).
    void setGpTuner(std::unique_ptr<auto_tune::GpTuner> t) { gp_tuner_ = std::move(t); }
    void setAutotuneCsv(const std::string& path) { autotune_csv_path_ = path; }

    void setMovingAverage(const std::string& type, double window_blocks) {
        if (window_blocks <= 0.0) window_blocks = (double)DEFAULT_HALF_LIFE_IN_BLOCKS;
        moving_avg_type_ = type;
        moving_avg_window_ = window_blocks;
        compaction_ratio                = MovingAverageRatio::Make(type, window_blocks);
        eviction_ratio                  = MovingAverageRatio::Make(type, window_blocks);
        eviction_ratio_in_ghost_cache   = MovingAverageRatio::Make(type, window_blocks);
        compaction_ratio_in_ghost_cache = MovingAverageRatio::Make(type, window_blocks);
        ghost_util_ratio                = MovingAverageRatio::Make(type, window_blocks);
        net_free_seg_ratio_             = MovingAverageRatio::Make(type, window_blocks);
        gc_valid_pages_ratio_           = MovingAverageRatio::Make(type, window_blocks);
    }
};
