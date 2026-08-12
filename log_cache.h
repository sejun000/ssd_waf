#pragma once
#include "icache.h"              // 기존 프로젝트 공용 인터페이스
#include "log_cache_segment.h"
#include "evict_policy.h"
#include "evict_policy_greedy.h"
#include "istream.h"
#include "histogram.h"
#include "emwa_ratio.h"
#include "ghost_cache.h"
#include "age_ghost_cache.h"
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
    GhostDelta_GC_SUM_Victim, // GS_FINAL 과 동일하되 LHS 를 ghost 추정 Gud 대신
                              // 실제 compaction victim 의 최근 비용 v/(1−v) EWMA 로 대체.
                              // (ghost Gud 는 p25 1.07~p90 12.11 로 널뛰는데 실제 victim
                              //  비용은 1.8 근처로 안정적 → 결정 노이즈 제거 + 과대평가 해소)
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

    // ── What-if GC net-free simulation ───────────────────────────────────
    // Walk the live compactor victim list in score order, relocating each
    // victim's valid blocks into per-stream GC active segments (seeded from the
    // current gc_active_seg write pointers, routed via stream_policy's
    // mutation-free ClassifyReadOnly), until one NET segment is freed:
    //   net_free = victims_consumed - gc_active_segs_allocated == 1.
    // Reports the boundary victim's WT (create_timestamp) and the resulting
    // active-seg WT under the GC "oldest-WT wins" rule. Pure read-only — mutates
    // NO live state (LogCache, compactor heap, or stream_policy cycle state).
    struct GcNetFreeSim {
        bool     reached            = false;        // net_free==1 actually reached
        int      victims            = 0;            // m: victims consumed
        int      new_allocs         = 0;            // new gc active segs allocated
        uint64_t cum_valid          = 0;            // total valid blocks routed to target (= true valid copied)
        uint64_t boundary_victim_wt = 0;            // m-th victim seg create_timestamp
        uint64_t boundary_seg_wt    = UINT64_MAX;   // min WT of active seg(s) the m-th victim landed in
        uint64_t min_victim_wt      = UINT64_MAX;   // min create_timestamp over consumed victims
        uint64_t active_seg_min_wt  = UINT64_MAX;   // min WT across all touched active segs
    };
    GcNetFreeSim simulate_gc_net_free() const;

    // Σ invalidate_rate over every evictor (WT-ordered) segment with
    // create_timestamp <= wt_hi. O(rank) scan with early-stop (evictor is WT
    // ascending). Pair with get_victim_wt_span_for_free_segments by passing
    // span.max_wt → prefix invalidate-rate mass up to and including victim v.
    double sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const;

    // invalidate-rate signal validation: snapshot the last decision's WT≤v cohort
    // (per-seg cumulative invalidate count + that seg's predicted rate), then next
    // decision measure the ACTUAL invalidations on the SURVIVING cohort and log
    // predicted-vs-actual. Keyed per-segment (and wt-verified to dodge pointer
    // reuse) because cohort membership churns — same reasoning as the independent
    // per-seg EWMA: summing cumulative counts over a drifting WT band is corrupted
    // by departures, so we diff per survivor instead.
    struct InvPredSnap { uint64_t count; uint64_t wt; double rate; };
    std::unordered_map<Segment*, InvPredSnap> invpred_snap_;
    uint64_t invpred_snap_ts_    = 0;   // host time at snapshot
    uint64_t invpred_snap_maxwt_ = 0;   // vspan.max_wt at snapshot (cohort upper bound)
    double   invpred_rate_sum_   = 0.0; // invrate_sum at snapshot (full-cohort rate)

    // raw (non-EWMA) per-tick Gud vs compact: log the SAME quantities the two
    // EWMAs smooth, but as direct first-differences between consecutive decision
    // ticks, to confirm the back-computed raw ratio (~1.0) was not an artifact.
    //   raw_gud     = d(ghost_compacted_blocks_sum_) / d(log_cache_timestamp)
    //   raw_compact = d(compacted_blocks) / d(net_free_pages)
    //   net_free_pages = d(compact_event_count_)·seg − d(compacted_blocks)
    uint64_t rawval_prev_ts_       = 0;
    double   rawval_prev_ghostsum_ = 0.0;
    uint64_t rawval_prev_comp_     = 0;
    uint64_t rawval_prev_compevt_  = 0;

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

    /* DOGI (FAST'26) standalone-Manager emulation knobs (read_cache port) ***/
    bool dogi_gc_trigger_ = false;         // invalid-ratio (GP) based GC trigger
    bool dogi_keep_seg_timestamp_ = false; // segment keeps its own create ts; Classify sees segment age
    bool dogi_share_active_segments_ = false; // host/gc writes share logical-group open segments
    double dogi_gc_threshold_ = 0.12;
    uint64_t dogi_total_sealed_blocks_ = 0;
    uint64_t dogi_total_sealed_invalid_blocks_ = 0;
public:
    void setDogiGcMode(bool trigger, double threshold = 0.12) {
        dogi_gc_trigger_ = trigger;
        dogi_gc_threshold_ = threshold;
    }
    void setDogiKeepSegTimestamp(bool on) { dogi_keep_seg_timestamp_ = on; }
    void setDogiShareActiveSegments(bool on) { dogi_share_active_segments_ = on; }
private:
    uint64_t global_valid_blocks = 0;
    uint64_t compacted_blocks = 0;
    uint64_t invalidate_blocks = 0;
    uint64_t reinsert_blocks = 0;
    uint64_t ghost_cache_evicted_blocks = 0;
    uint64_t read_blocks_in_partial_write = 0;

    uint64_t evicted_segment_age = 0;
    uint64_t gc_victim_count = 0;
    double gc_victim_valid_ratio_sum = 0.0;
    // GhostDelta_GC_SUM_Victim 의 LHS: 실제 compaction victim 의 GC 복사 비용
    //   v/(1−v) (= 1페이지 확보당 복사 페이지 수, ghost Gud 와 같은 단위) 을
    //   per-seg invalidate_rate 와 같은 고정-α EWMA (α=0.1) 로 평활.
    Ewma compaction_victim_cost_ewma_{Segment::INVAL_RATE_ALPHA};
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
    int consec_lower_ = 0;     // anti-stuck cap: consecutive LOWER run → force 1 RAISE after 15 (mid LOWER-stuck)
    int    gs_decision_period_segs_ = 8;  // GS hill-climb decision period (in segments)
    uint64_t fifo_after_warmup_pages_ = 0;  // > 0 → force target=0 once host write reaches this many pages
    EwmaRatio compaction_ratio;
    EwmaRatio eviction_ratio;
    EwmaRatio eviction_ratio_in_ghost_cache;
    EwmaRatio compaction_ratio_in_ghost_cache;
    // Per-flush-event average valid evict (pages/event). Updated every (seg/4)
    // tick: updateFromCumulative(flush_event_count_, evicted_blocks).
    EwmaRatio flush_avg_ratio;
    uint64_t  flush_event_count_ = 0;
    // Full-invalid victims (valid_cnt==0) freed directly via reset_segment —
    // counted in neither compact_event_count_ nor flush_event_count_. They free a
    // whole segment at zero cost. Conservation:
    //   host = comp_netfree + flush_netfree + (full_invalid_reset_count_ · seg)
    uint64_t  full_invalid_reset_count_ = 0;
    // Per-class-num victim count from most recent ghost scan
    // (update_ghost_compacted_blocks_sum_cum). Logged per gsdec row.
    double last_ghost_m_by_class_[16] = {0};
    // Σ invalidate_rate over picked victims (most recent ghost scan).
    // 평균 = last_ghost_sum_inv_rate_ / Σ last_ghost_m_by_class_.
    double last_ghost_sum_inv_rate_ = 0.0;
    // Σ raw last-fold inv_rate (no EWMA) over picked victims.
    double last_ghost_sum_inv_rate_lf_ = 0.0;
    // REAL-victim inv_rate tracking — accumulated for each segment that actually
    // gets compacted (post-K_VAL choice in check_and_evict_if_needed). Compare
    // against last_ghost_sum_inv_rate_/_lf_ (predicted) to expose drift between
    // ghost scan-time vs execution-time victim selection.
    double   real_victim_inv_rate_cum_     = 0.0;  // Σ invalidate_rate() (EWMA)
    double   real_victim_inv_rate_lf_cum_  = 0.0;  // Σ raw last-fold rate
    uint64_t real_victim_compact_count_    = 0;    // # of real compactions
    // Prev snapshots for per-gsdec-tick diff (analogous to rawval_prev_*):
    double   rawval_prev_real_inv_rate_    = 0.0;
    double   rawval_prev_real_inv_rate_lf_ = 0.0;
    uint64_t rawval_prev_real_compact_     = 0;
    // Real-victim age/u distribution — to test whether real picks include
    // recently-sealed segments (age < 1seg) with low-u. If many fresh victims
    // appear, ghost's predict-time view (taken before they existed) misses
    // them → cum_valid over-prediction.
    uint64_t real_victim_age_sum_       = 0;   // Σ age (host pages)
    double   real_victim_u_sum_         = 0.0; // Σ u (= valid_cnt/seg_blocks)
    uint64_t real_victim_fresh_count_   = 0;   // # with age < seg_blocks
    double   real_victim_fresh_u_sum_   = 0.0; // Σ u over fresh victims
    uint64_t rawval_prev_real_age_sum_     = 0;
    double   rawval_prev_real_u_sum_       = 0.0;
    uint64_t rawval_prev_real_fresh_count_ = 0;
    double   rawval_prev_real_fresh_u_sum_ = 0.0;
    // create_timestamp of first 4 ghost-picked victims (from latest ghost scan)
    // and first 4 real-compacted victims (since last gsdec print). −1 = unused.
    // Reset on each gsdec print of real-side. Lets us see if ghost set ⊂ real
    // set or vice versa.
    int64_t last_ghost_picked_ts_[4] = { -1, -1, -1, -1 };
    int64_t real_picked_ts_[4]       = { -1, -1, -1, -1 };
    int     real_picked_idx_         = 0;
    // u at scan time (ghost) / execute time (real) for the same 4 picks each.
    // Direct measurement — no derivation from raw_gud/m. NaN/0 if pick absent.
    double  last_ghost_picked_u_[4]  = { 0.0, 0.0, 0.0, 0.0 };
    double  real_picked_u_[4]        = { 0.0, 0.0, 0.0, 0.0 };
    // Fractional m from get_ghost_sum_for_free_segments (= s.m, includes
    // fractional last victim). Different from integer g_vid count (= ceil(m)).
    double  last_ghost_m_frac_       = 0.0;
    // Per-compaction-event average copied valid (pages/event). Mirrors
    // flush_avg_ratio: updateFromCumulative(compact_event_count_·seg_blocks,
    // compacted_blocks) → EWMA value = avg valid fraction per victim.
    EwmaRatio compact_avg_ratio;
    uint64_t  compact_event_count_ = 0;
    EwmaRatio flush_pred_ratio;
    double    ghost_flush_valid_sum_ = 0.0;
    // F_ghost: cumulative sum of age_ghost_cache.totalValidCount() snapshots
    // sampled at every (seg/4) tick, EWMA per host write.  Represents "what
    // would still be cached if extended by D segs".
    EwmaRatio flush_ghost_ratio;
    double    ghost_seg_valid_sum_ = 0.0;
    // 2-step lookahead candidate costs (GhostDelta_GC_SUM_Final). All three free
    // the SAME 2δN segments of space; pick the cheapest plan, act on its first
    // step. GG = GC×2 (ghost_sum(2δN)); FF = flush×2 (get_mth(2δN)); GF's GC leg
    // (δN) reuses Gud, only its flush leg (get_mth(δN)) is tracked here. Each is
    // a cumulative valid-page sum sampled every (seg/4) tick → EWMA per host
    // write, exactly like flush_ghost_ratio.
    EwmaRatio gg_ratio;          // ghost_sum(2δN).cum_valid
    double    gg_ghost_sum_ = 0.0;
    EwmaRatio ff_ratio;          // get_mth(2δN)
    double    ff_flush_sum_ = 0.0;
    EwmaRatio gf_flush_ratio;    // get_mth(δN)  (GF's flush leg; GC leg = Gud)
    double    gf_flush_sum_ = 0.0;
    // λ: device blocks invalidated per host-write page (EWMA over moving_avg
    //    window = 1 host-seg = 한 host_writes).
    //    GS_FINAL λ-rule: GC (RAISE) if Gud < λ·r, else flush.
    EwmaRatio lambda_ratio;
    double periodic_ratio_ = 2.88;
    EwmaRatio ghost_util_ratio;  // ghost miss rate = U(util_step)
    GhostCache ghost_cache;
    AgeGhostCache age_ghost_cache;
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

    /* ── Per-block update_interval (for invrate predictor) ──── */
    // LBA → previous-block lifetime (= invalidate-time − create-time of the
    // block that just got invalidated). On the NEXT write of that LBA, the
    // value is copied into block.update_interval. Cleared by the invalidate
    // path. Evicted LBAs (no invalidate) are NOT recorded → reinsert is
    // treated as 1st-write (update_interval = 0).
    std::unordered_map<long, uint64_t> lba_prev_lifetime_;
    // Histogram over 1st-write blocks (update_interval == 0): bin =
    // min(N-1, lifetime / age_prior_bin_width_). On invalidate of a 1st-write
    // block, hist[bin]++. Lookup later gives age-conditional invalidation
    // distribution → used by predictor step 4.
    static constexpr size_t AGE_PRIOR_BINS = 256;
    std::vector<uint64_t> age_prior_inv_count_;       // hist of 1st-write lifetimes
    uint64_t              age_prior_bin_width_ = 0;   // ticks per bin (= seg_size)
    uint64_t              age_prior_total_first_writes_ = 0;
    void                  init_age_prior(uint64_t bin_width);
    void                  print_age_prior();

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
             m == PeriodicMode::GhostDelta_GC_SUM_Final ||
             m == PeriodicMode::GhostDelta_GC_SUM_Victim) &&
            total_cache_block_count > 0 && segment_size_blocks > 0) {
            util_step_ = static_cast<double>(segment_size_blocks * gs_decision_period_segs_)
                       / static_cast<double>(total_cache_block_count);
            // Resize the LRU/FIFO shadow so it represents exactly the θ-tail
            // the GS hill-climb reasons about. Shrinking happens immediately
            // (front-evicted with evict_count_ accounted); growing is lazy.
            ghost_cache.setCapacity(
                static_cast<std::size_t>(
                    static_cast<double>(total_cache_block_count) * util_step_));
            // age_ghost_cache is segment-granular: capacity = D segments.
            age_ghost_cache.setCapacity(
                static_cast<std::size_t>(gs_decision_period_segs_));
        }
    }
    void setTdeltaStep(double s) { tdelta_step_ = s; }
    void setGhostReanchorStep(double s) { ghost_reanchor_step_ = s; }
    void setUtilStep(double s) { util_step_ = s; }  // post-ctor; ghost_cache size already fixed
    double getUtilStep() const { return util_step_; }
    void setFifoAfterWarmupPages(uint64_t p) { fifo_after_warmup_pages_ = p; }
    // Replace all moving-average ratios with the given (type, window_blocks).
    // Must be called BEFORE first periodic() update for samples to be consistent.
    // AUTO-mode hooks (no-ops unless GhostDelta_GC_AUTO is selected).
    void setGpTuner(std::unique_ptr<auto_tune::GpTuner> t) { gp_tuner_ = std::move(t); }
    void setAutotuneCsv(const std::string& path) { autotune_csv_path_ = path; }

    void setMovingAverage(const std::string& type, double window_blocks) {
        // Fallback half-life scales with the actual segment size (4 segment-lifetimes).
        // At the default 6 GB segment this == DEFAULT_HALF_LIFE_IN_BLOCKS = (262144*6)*4.
        if (window_blocks <= 0.0) window_blocks = static_cast<double>(segment_size_blocks) * 4.0;
        moving_avg_type_ = type;
        moving_avg_window_ = window_blocks;
        compaction_ratio                = MovingAverageRatio::Make(type, window_blocks);
        eviction_ratio                  = MovingAverageRatio::Make(type, window_blocks);
        eviction_ratio_in_ghost_cache   = MovingAverageRatio::Make(type, window_blocks);
        compaction_ratio_in_ghost_cache = MovingAverageRatio::Make(type, window_blocks);
        ghost_util_ratio                = MovingAverageRatio::Make(type, window_blocks);
        net_free_seg_ratio_             = MovingAverageRatio::Make(type, window_blocks);
        gc_valid_pages_ratio_           = MovingAverageRatio::Make(type, window_blocks);
        flush_avg_ratio                 = MovingAverageRatio::Make(type, window_blocks);
        compact_avg_ratio               = MovingAverageRatio::Make(type, window_blocks);
        flush_pred_ratio                = MovingAverageRatio::Make(type, window_blocks);
        flush_ghost_ratio               = MovingAverageRatio::Make(type, window_blocks);
        gg_ratio                        = MovingAverageRatio::Make(type, window_blocks);
        ff_ratio                        = MovingAverageRatio::Make(type, window_blocks);
        gf_flush_ratio                  = MovingAverageRatio::Make(type, window_blocks);
        lambda_ratio                    = MovingAverageRatio::Make(type, window_blocks);
    }
};
