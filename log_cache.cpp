#include "log_cache.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <list>
#include <set>

extern uint64_t interval;
extern double g_segment_blocks;
extern uint64_t g_threshold;
extern uint64_t g_timestamp;

// GS decision-trace logging (env-controlled, opt-in via GS_DECISION_LOG=path).
// Counters track exception paths inside check_and_evict_if_needed so the
// dump tells us whether decision-firing actually translated to compaction.
namespace {
    uint64_t g_ex_low_target_count        = 0;  // target_valid_blk_rate < 0.1
    uint64_t g_ex_target_satisfied_count  = 0;  // target*total <= valid_blocks
    uint64_t g_ex_force_flush_count       = 0;  // free_pool <= 4 override
    uint64_t g_ex_high_valid_victim_count = 0;  // victim valid_cnt > 0.95*seg
    FILE*    g_gs_dec_fp                  = nullptr;
    bool     g_gs_dec_init                = false;
}

/* ------------------------------------------------------------------ */
/* ctor / dtor                                                        */
/* ------------------------------------------------------------------ */
LogCache::LogCache(uint64_t              cold_capacity,
             uint64_t              cache_block_count,
             int                   blk_sz,
             bool                  cache_trace,
             const std::string&    trace_file,
             const std::string&    cold_trace,
             std::string&    waf_log_file,
             std::unique_ptr<EvictPolicy> ev,
             const Config*         cfg, 
             IStream *input_stream_policy,
             double input_target_valid_blk_rate,
             std::unique_ptr<EvictPolicy> cp,
             double input_additional_free_blks_ratio_by_gc,
             bool input_ghost_cache,
             std::string stat_log_file,
             double valid_rate_period_gb,
             double valid_rate_min,
             double valid_rate_max,
             double periodic_ratio,
             double input_util_step
             )
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : Config{}),
      evictor(std::move(ev)),
      cache_trace_(cache_trace),
      target_valid_blk_rate(input_target_valid_blk_rate),
      valid_blk_rate_hard_limit(0.90),
      compactor(std::move(cp)),
      additional_free_blks_ratio_by_gc(input_additional_free_blks_ratio_by_gc),
      evicted_ages_histogram(std::make_unique<Histogram>("evicted_ages", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_blocks_histogram(std::make_unique<Histogram>("evicted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      compacted_blocks_histogram(std::make_unique<Histogram>("compacted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      evicted_ages_with_segment_histogram(std::make_unique<Histogram>("evicted_ages_with_segment", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      compacted_ages_with_segment_histogram(std::make_unique<Histogram>("compacted_ages_with_segment", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_cache_blocks_per_evict(std::make_unique<Histogram>("evicted_cache_blocks_per_evict", 1, 100, fp_stats)),
      compacted_lifetime_histogram_(std::make_unique<Histogram>("compacted_lifetime", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      is_ghost_cache(input_ghost_cache),
      util_step_(input_util_step),
      compaction_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio_in_ghost_cache(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      compaction_ratio_in_ghost_cache(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_util_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_cache(cache_block_count * util_step_),
      net_free_seg_ratio_(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gc_valid_pages_ratio_(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
{
    periodic_ratio_ = periodic_ratio;
    segment_size_blocks = cfg_.segment_bytes / blk_sz;
    g_segment_blocks = static_cast<double>(segment_size_blocks);
    total_segments = cache_block_count * blk_sz / cfg_.segment_bytes;
    total_cache_block_count = total_segments * segment_size_blocks;
    total_capacity_bytes = cache_block_count * blk_sz;
    assert(segment_size_blocks > 0 && "segment_bytes too small");
    assert(total_segments      > 0 && "device_bytes too small");
    log_cache_timestamp = 0;

    evictor->init(&log_cache_timestamp, cfg_.segment_bytes / blk_sz, total_segments);


    if (compactor) {
        compactor->init(&log_cache_timestamp, cfg_.segment_bytes / blk_sz, total_segments);
    }

    stream_policy = input_stream_policy;
    global_valid_blocks = 0;

    /* ── Periodic valid rate sweep init ─────────────────── */
    valid_rate_period_gb_ = valid_rate_period_gb;
    valid_rate_min_ = valid_rate_min;
    valid_rate_max_ = valid_rate_max;
    if (valid_rate_period_gb_ > 0.0) {
        valid_rate_period_blocks_ = segment_size_blocks * total_segments;
        next_valid_rate_change_ts_ = valid_rate_period_blocks_;
    }

    // score_warm_first / score_cold_first 가 heap add 시점에
    // g_threshold=0 fallback(-create_timestamp) 으로 음수 cached score 를 갖지 않도록 초기화
    g_threshold = cache_block_count * 2;
    g_timestamp = 1;  // > 0 이면 됨, log_cache_timestamp 가 아직 0 이라 1 로 설정
    /* 세그먼트 전부 미리 생성 → free_pool */
    for (std::size_t i = 0; i < total_segments; ++i)
    {
        all_segments.push_back(
            std::make_unique<LogCacheSegment>(segment_size_blocks, log_cache_timestamp));
        free_pool.push_back(all_segments.back().get());
    }

    if (cache_trace_)
    {
        if (!trace_file.empty())
            trace_fp_ = fopen(trace_file.c_str(), "w");
        if (!cold_trace.empty())
            cold_trace_fp_ = fopen(cold_trace.c_str(), "w");
    }
}

LogCache::~LogCache()
{
    print_lifetime_results();
    print_rewrite_results();
    print_utilization_distribution();
    print_segment_age_scatter();
    print_inv_time_scatter();
    if (trace_fp_)      std::fclose(trace_fp_);
    if (cold_trace_fp_) std::fclose(cold_trace_fp_);
    if (autotune_csv_)  std::fclose(autotune_csv_);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
bool LogCache::exists(long key)
{
    return mapping.find(key) != mapping.end();
}


void LogCache::invalidate(long key, int lba_sz) {
    if (exists(key))
    {
        auto loc = mapping[key];
        if (loc.seg->blocks[loc.idx].valid)
        {
            print_objects("invalidate", log_cache_timestamp - loc.seg->blocks[loc.idx].create_timestamp);
            record_lifetime(log_cache_timestamp - loc.seg->blocks[loc.idx].create_timestamp, true);
            {
                auto cit = compacted_at_.find(key);
                if (cit != compacted_at_.end()) {
                    compacted_lifetime_histogram_->inc(log_cache_timestamp - cit->second);
                    compacted_at_.erase(cit);
                }
            }
            invalidate_blocks += 1;
            loc.seg->blocks[loc.idx].valid = false;
            --loc.seg->valid_cnt;
            global_valid_blocks -= 1;
            record_inv_time(key);
            if (loc.seg->full()){
                evict_policy_update(loc.seg);
            }
        }
        else{
                assert(false);
        }
        mapping.erase(key);
    }
    else
    {
        if (evicted_timestamp.find(key) != evicted_timestamp.end()) {
            reinsert_blocks++;
            print_objects("reinsert", log_cache_timestamp - evicted_timestamp[key]);
            evicted_timestamp.erase(key);
        }
        _invalidate_cold_block(key * cache_block_size,
                                lba_sz,
                                OP_TYPE::TRIM);
    }
}

void LogCache::evict_policy_add(LogCacheSegment *s) {
    evictor->add(s, log_cache_timestamp);
    if (compactor) {
        compactor->add(s, log_cache_timestamp);
    }
}

void LogCache::evict_policy_remove(LogCacheSegment *s) {
    evictor->remove(s);
    if (compactor) {
        compactor->remove(s);
    }
}

void LogCache::evict_policy_update(LogCacheSegment *s) {
    evictor->update(s);
    if (compactor) {
        compactor->update(s);
    }
}

void LogCache::periodic() {
    static uint64_t t = 0;
    t++;
    if (t >= 188743680) { // 1200 MB/s * 10 minutes (t is 4k page unit)
        t = 0;
        uint64_t host_delta = ftl.GetHostWritePages() - lc_last_ftl_host_pages;
        uint64_t nand_delta = ftl.GetNandWritePages() - lc_last_ftl_nand_pages;
        current_waf = host_delta > 0 ? static_cast<double>(nand_delta) / host_delta : 0.0;
        lc_last_ftl_host_pages += host_delta;
        lc_last_ftl_nand_pages += nand_delta;
    }
    if (periodic_mode_ == PeriodicMode::TimeDelta) {
        periodic_t_delta();
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC) {
        periodic_ghost_delta_gc();
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC_NAND) {
        periodic_ghost_delta_gc_nand();
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC_AUTO) {
        periodic_ghost_delta_gc_auto();
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC_SUM) {
        periodic_ghost_delta_gc_sum();
        periodic_gs_predict_track();    // Phase 1: passive candidate tracking
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC_SUM_Replay) {
        periodic_ghost_delta_gc_sum_replay();
        periodic_gs_predict_track();    // keep tracker active so stats columns stay populated
    } else if (periodic_mode_ == PeriodicMode::GhostDelta_GC_SUM_Final) {
        periodic_ghost_delta_gc_sum_final();
    } else {
        periodic_ghost_delta();
    }
}


void LogCache::periodic_ghost_delta_gc() {
    if (is_ghost_cache){
        if (log_cache_timestamp % (segment_size_blocks/4) == 0) {
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            compaction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
        }
        if (log_cache_timestamp % (segment_size_blocks * 8) == 0) {
            if (compaction_ratio.has_value() &&
                compaction_ratio_in_ghost_cache.has_value() &&
                eviction_ratio.has_value() &&
                eviction_ratio_in_ghost_cache.has_value()){
                if (periodic_ratio_ * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value())
                        > compaction_ratio_in_ghost_cache.value() - compaction_ratio.value()) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + util_step_);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - util_step_);
                }
            }
        }
    }
}

void LogCache::update_ghost_compacted_blocks(LogCacheSegment* victim) {
    // GhostDelta_GC / GC_NAND / GC_AUTO ghost signal: at each real compaction
    // event, take the m-th best segment in the compactor's CB heap (where
    // m = θ·N segments) as a single-point estimate of "what GC would have cost
    // had we run at u+θ", scaled by free-space equivalence (1-u_cur)/(1-u_m).
    if (!compactor) return;
    double m_segments    = util_step_ * total_segments;
    auto   valid_pages   = compactor->get_kth_segment_valid_cnt_for_free_segments(m_segments);
    double u_cur         = (double)victim->valid_cnt / (double)segment_size_blocks;
    double u_m           = (double)valid_pages       / (double)segment_size_blocks;
    if (u_m < 1.0) {
        double scale = (1.0 - u_cur) / (1.0 - u_m);
        ghost_compacted_blocks += static_cast<uint64_t>(valid_pages * scale);
    } else {
        ghost_compacted_blocks += valid_pages;
    }
}

void LogCache::update_ghost_compacted_blocks_sum() {
    // GhostDelta_GC_SUM ghost signal: at each real compaction event, advance a
    // synthetic counter ghost_compacted_blocks_sum_ by (dt × u_avg/(1-u_avg))
    // where u_avg comes from the CB-sorted cumulative scan up to θ·N·seg
    // invalid pages (so it estimates the average valid/invalid ratio in the
    // would-be θ-regime victims).
    //
    // Invalidation-aware target: host writes invalidate the freshly freed
    // region at rate i; only the fraction landing there (≈ θ uniform) reduces
    // net free fill, so target_free_segs = θ·N·seg / (1 - θ·i).
    if (!compactor) return;
    double inv_corr = 1.0;
    if (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_) {
        const uint64_t win_writes = log_cache_timestamp - last_ghost_sum_ts_;
        const uint64_t win_inv    = (invalidate_blocks > last_invalidate_at_comp_)
                                  ? (invalidate_blocks - last_invalidate_at_comp_)
                                  : 0;
        const double i_rate = std::min(0.95,
            static_cast<double>(win_inv) / static_cast<double>(win_writes));
        const double theta_i = util_step_ * i_rate;
        inv_corr = 1.0 / (1.0 - std::min(0.95, theta_i));
    }
    const double target_free_segs = util_step_ *
        static_cast<double>(total_segments) * inv_corr;
    auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
    if (s.cum_invalid > 0.0) {
        const double rate = s.cum_valid / s.cum_invalid;
        const uint64_t dt = (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_)
                          ? (log_cache_timestamp - last_ghost_sum_ts_)
                          : 0;
        // Reassign form (per-compact): snapshot compacted_blocks + dt × rate.
        ghost_compacted_blocks_sum_ = static_cast<double>(compacted_blocks)
                                    + static_cast<double>(dt) * rate;
        ghost_sum_initialized_       = true;
        last_ghost_sum_ts_           = log_cache_timestamp;
        last_invalidate_at_comp_     = invalidate_blocks;
    }
}

void LogCache::update_ghost_compacted_blocks_sum_cum() {
    // Final form: at each tick, accumulate s.cum_valid — the realized δ-marginal
    // cost of making δ·N·seg free pages from the current victim list. No dt or
    // rate extrapolation; monotone; EWMA cumulative per host write yields G(u+δ).
    if (!compactor) return;
    double inv_corr = 1.0;
    if (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_) {
        const uint64_t win_writes = log_cache_timestamp - last_ghost_sum_ts_;
        const uint64_t win_inv    = (invalidate_blocks > last_invalidate_at_comp_)
                                  ? (invalidate_blocks - last_invalidate_at_comp_)
                                  : 0;
        const double i_rate = std::min(0.95,
            static_cast<double>(win_inv) / static_cast<double>(win_writes));
        const double theta_i = util_step_ * i_rate;
        inv_corr = 1.0 / (1.0 - std::min(0.95, theta_i));
    }
    const double target_free_segs = util_step_ *
        static_cast<double>(total_segments) * inv_corr;
    auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
    if (s.cum_invalid > 0.0) {
        ghost_compacted_blocks_sum_ += s.cum_valid;
        ghost_sum_initialized_       = true;
        last_ghost_sum_ts_           = log_cache_timestamp;
        last_invalidate_at_comp_     = invalidate_blocks;
    }
}

void LogCache::periodic_gs_predict_track() {
    // Phase 1: emit G(u+δ_k) for δ_k ∈ {(segs-1), segs, (segs+1)} every
    // segment/4 tick, buffer with timestamp.  When an entry has aged δ_k
    // worth of host writes, compare its G_pred against the currently-published
    // EWMA G(u) and accumulate relative + squared error.  Read-only w.r.t.
    // the live policy — `gs_decision_period_segs_` is not modified here.
    if (!is_ghost_cache || !compactor) return;
    if (log_cache_timestamp == 0) return;
    if (log_cache_timestamp % (segment_size_blocks / 4) != 0) return;

    // Lazy init around the configured segs.
    if (!gs_cand_initialized_) {
        for (int k = 0; k < 3; ++k) {
            int s = std::max(1, gs_decision_period_segs_ + (k - 1));
            gs_cand_[k].segs       = s;
            gs_cand_[k].util_step  = static_cast<double>(s) *
                static_cast<double>(segment_size_blocks) /
                static_cast<double>(total_cache_block_count);
        }
        gs_cand_initialized_ = true;
    }

    // inv_corr shared with the live ghost-sum advance (same i_rate window).
    double inv_corr = 1.0;
    if (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_) {
        const uint64_t win_writes = log_cache_timestamp - last_ghost_sum_ts_;
        const uint64_t win_inv    = (invalidate_blocks > last_invalidate_at_comp_)
                                  ? (invalidate_blocks - last_invalidate_at_comp_)
                                  : 0;
        const double i_rate  = std::min(0.95,
            static_cast<double>(win_inv) / static_cast<double>(win_writes));
        const double theta_i = util_step_ * i_rate;
        inv_corr = 1.0 / (1.0 - std::min(0.95, theta_i));
    }

    const double G_now            = compaction_ratio.has_value()
                                  ? compaction_ratio.value() : 0.0;
    const double F_now            = eviction_ratio.has_value()
                                  ? eviction_ratio.value() : 0.0;
    const double F_ghost_marginal = eviction_ratio_in_ghost_cache.has_value()
                                  ? eviction_ratio_in_ghost_cache.value() : 0.0;

    for (int k = 0; k < 3; ++k) {
        auto& c = gs_cand_[k];
        const uint64_t delta_ts = static_cast<uint64_t>(c.segs) *
            static_cast<uint64_t>(segment_size_blocks);

        // --- G(u+δ_k) emit + drain ---
        const double target_free_segs = c.util_step *
            static_cast<double>(total_segments) * inv_corr;
        auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
        if (s.cum_invalid > 0.0) {
            const double rate   = s.cum_valid / s.cum_invalid;
            const double g_pred = G_now + rate;       // v4-style: G(u) + ghost_marginal
            c.g_pred_buf.emplace_back(log_cache_timestamp, g_pred);
        }
        while (!c.g_pred_buf.empty() &&
               log_cache_timestamp - c.g_pred_buf.front().first >= delta_ts) {
            const double pred = c.g_pred_buf.front().second;
            const double real = G_now;
            const double diff = real - pred;
            c.g_sq_err_sum += diff * diff;
            if (real > 1e-12) c.g_err_sum += std::fabs(diff) / real;
            c.g_err_n      += 1;
            c.g_pred_sum_raw += pred;
            c.g_real_sum_raw += real;

            // Smoothed (SWMA 64 segs) err: feed paired (pred, real) into sliding
            // windows of size kSmoothWin and compare their averages instead of
            // the raw spikes that swamp the small-δ RMSE.
            c.g_pred_win.push_back(pred);
            c.g_real_win.push_back(real);
            c.g_pred_win_sum += pred;
            c.g_real_win_sum += real;
            if (c.g_pred_win.size() > kSmoothWin) {
                c.g_pred_win_sum -= c.g_pred_win.front();
                c.g_real_win_sum -= c.g_real_win.front();
                c.g_pred_win.pop_front();
                c.g_real_win.pop_front();
            }
            if (c.g_pred_win.size() == kSmoothWin) {
                const double avg_p = c.g_pred_win_sum / kSmoothWin;
                const double avg_r = c.g_real_win_sum / kSmoothWin;
                const double sdiff = avg_r - avg_p;
                c.g_smooth_sq_err_sum += sdiff * sdiff;
                if (avg_r > 1e-12) c.g_smooth_err_sum += std::fabs(sdiff) / avg_r;
                c.g_smooth_err_n      += 1;
            }
            c.g_pred_buf.pop_front();
        }

        // --- F(u+δ_k) emit + drain ---
        // Linear scale: F_pred_k = F(u) - (δ_k / δ_live) × F_ghost_marginal.
        // F_ghost_marginal (= eviction_ratio_in_ghost_cache) estimates how much
        // flush the LIVE ghost δ saves; we scale by candidate-to-live ratio.
        const double scale_k = (util_step_ > 0)
                             ? (c.util_step / util_step_) : 1.0;
        const double f_pred  = std::max(0.0, F_now - scale_k * F_ghost_marginal);
        c.f_pred_buf.emplace_back(log_cache_timestamp, f_pred);
        while (!c.f_pred_buf.empty() &&
               log_cache_timestamp - c.f_pred_buf.front().first >= delta_ts) {
            const double pred = c.f_pred_buf.front().second;
            const double real = F_now;
            const double diff = real - pred;
            c.f_sq_err_sum += diff * diff;
            if (real > 1e-12) c.f_err_sum += std::fabs(diff) / real;
            c.f_err_n      += 1;
            c.f_pred_sum_raw += pred;
            c.f_real_sum_raw += real;

            c.f_pred_win.push_back(pred);
            c.f_real_win.push_back(real);
            c.f_pred_win_sum += pred;
            c.f_real_win_sum += real;
            if (c.f_pred_win.size() > kSmoothWin) {
                c.f_pred_win_sum -= c.f_pred_win.front();
                c.f_real_win_sum -= c.f_real_win.front();
                c.f_pred_win.pop_front();
                c.f_real_win.pop_front();
            }
            if (c.f_pred_win.size() == kSmoothWin) {
                const double avg_p = c.f_pred_win_sum / kSmoothWin;
                const double avg_r = c.f_real_win_sum / kSmoothWin;
                const double sdiff = avg_r - avg_p;
                c.f_smooth_sq_err_sum += sdiff * sdiff;
                if (avg_r > 1e-12) c.f_smooth_err_sum += std::fabs(sdiff) / avg_r;
                c.f_smooth_err_n      += 1;
            }
            c.f_pred_buf.pop_front();
        }
    }
}

void LogCache::periodic_ghost_delta_gc_sum() {
    // GhostDelta_GC variant: estimate G(u+θ) via cumulative CB-sorted scan.
    //   m = min { k : Σ_{i<k} (seg - v_i) ≥ θ · N · seg }
    //   G(u+θ) cost rate per host write ≈ Σ v_i / Σ (seg-v_i) = u_avg/(1-u_avg)
    // Advance a synthetic ghost_compacted_blocks_sum_ counter at that rate so
    // compaction_ratio_in_ghost_cache stays comparable to compaction_ratio.
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
    }
    // Decision fires every 1 segment (independent of gs_decision_period_segs_,
    // which only controls util_step (= delta) via setPeriodicMode auto-derive).
    if (log_cache_timestamp % segment_size_blocks == 0) {
        if (compaction_ratio.has_value() &&
            compaction_ratio_in_ghost_cache.has_value() &&
            eviction_ratio.has_value() &&
            eviction_ratio_in_ghost_cache.has_value()){
            // Weight flush side by observed cold-tier WAF. Falls back to 1.0
            // before the first 10-minute window closes (current_waf==0).
            const double waf_w = (current_waf > 0.0) ? current_waf : 1.0;
            const double Gu  = compaction_ratio.value();
            const double Fu  = eviction_ratio.value();
            const double Gud = compaction_ratio_in_ghost_cache.value();
            const double Fud = eviction_ratio_in_ghost_cache.value();
            const double lhs = periodic_ratio_ * waf_w * (Fu - Fud);
            const double rhs = Gud;
            const bool   raise = (lhs > rhs);
            const double cur_util = (total_cache_block_count > 0)
                                  ? (double)global_valid_blocks / total_cache_block_count : 0.0;
            const double prev_target = target_valid_blk_rate;
            const double raw_target  = raise ? (cur_util + util_step_) : (cur_util - util_step_);
            const bool   hard_hit    = raise && (raw_target > valid_blk_rate_hard_limit);
            const bool   low_hit     = !raise && (raw_target < 0.0);

            if (raise) {
                target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, raw_target);
            } else {
                target_valid_blk_rate = std::max(0.0, raw_target);
            }

            if (!g_gs_dec_init) {
                g_gs_dec_init = true;
                const char* path = std::getenv("GS_DECISION_LOG");
                if (path && *path) {
                    g_gs_dec_fp = std::fopen(path, "w");
                    if (g_gs_dec_fp) {
                        std::fprintf(g_gs_dec_fp,
                            "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision "
                            "cur_util tgt_before tgt_after hard_limit low_floor "
                            "comp_cum evict_cum ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid\n");
                    }
                }
            }
            if (g_gs_dec_fp) {
                std::fprintf(g_gs_dec_fp,
                    "%lu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s "
                    "%.6f %.6f %.6f %d %d %lu %lu %lu %lu %lu %lu\n",
                    log_cache_timestamp, gs_decision_period_segs_,
                    periodic_ratio_, waf_w, Gu, Fu, Gud, Fud, lhs, rhs,
                    raise ? "RAISE" : "LOWER",
                    cur_util, prev_target, target_valid_blk_rate,
                    hard_hit ? 1 : 0, low_hit ? 1 : 0,
                    compacted_blocks, evicted_blocks,
                    g_ex_low_target_count, g_ex_target_satisfied_count,
                    g_ex_force_flush_count, g_ex_high_valid_victim_count);
                std::fflush(g_gs_dec_fp);
            }
        }
    }
}

void LogCache::periodic_ghost_delta_gc_sum_final() {
    // Final variant: ghost_sum += cum_valid (tick), LHS = r·waf·δ, RHS = Gud − Gu.
    //   * No dt × rate extrapolation (drops sustained-rate assumption).
    //   * LHS uses δ directly: utilization δ 증가 = host write 당 flush 안 한 valid pages 비율.
    //   * Gud = G(u+δ) (full rate per host write), so subtract Gu for δ-marginal cost.
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        update_ghost_compacted_blocks_sum_cum();
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
    }
    if (log_cache_timestamp % segment_size_blocks == 0) {
        if (compaction_ratio.has_value() &&
            compaction_ratio_in_ghost_cache.has_value() &&
            eviction_ratio.has_value() &&
            eviction_ratio_in_ghost_cache.has_value()){
            const double waf_w = (current_waf > 0.0) ? current_waf : 1.0;
            const double Gu  = compaction_ratio.value();
            const double Fu  = eviction_ratio.value();
            const double Gud = compaction_ratio_in_ghost_cache.value();
            const double Fud = eviction_ratio_in_ghost_cache.value();
            // LHS: per-host-write flush savings, expressed in valid pages.
            //   Δvalid pages per decision interval (= 1 seg)  = δ · N · seg_blocks
            //   per host write                                = δ · N (= D)
            // RHS (Gud) = EWMA<cum_valid/Δts> = pages/pages ratio, same units.
            const double lhs = periodic_ratio_ * waf_w
                             * util_step_ * static_cast<double>(total_segments);
            // cum_valid accumulator is prediction-only (no cb mixed in), so Gud
            // is already the δ-marginal GC rate per host write. Don't subtract Gu.
            const double rhs = Gud;
            const bool   raise = (lhs > rhs);
            const double cur_util = (total_cache_block_count > 0)
                                  ? (double)global_valid_blocks / total_cache_block_count : 0.0;
            const double prev_target = target_valid_blk_rate;
            const double raw_target  = raise ? (cur_util + util_step_) : (cur_util - util_step_);
            const bool   hard_hit    = raise && (raw_target > valid_blk_rate_hard_limit);
            const bool   low_hit     = !raise && (raw_target < 0.0);

            if (raise) {
                target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, raw_target);
            } else {
                target_valid_blk_rate = std::max(0.0, raw_target);
            }

            if (!g_gs_dec_init) {
                g_gs_dec_init = true;
                const char* path = std::getenv("GS_DECISION_LOG");
                if (path && *path) {
                    g_gs_dec_fp = std::fopen(path, "w");
                    if (g_gs_dec_fp) {
                        std::fprintf(g_gs_dec_fp,
                            "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision "
                            "cur_util tgt_before tgt_after hard_limit low_floor "
                            "comp_cum evict_cum ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid\n");
                    }
                }
            }
            if (g_gs_dec_fp) {
                std::fprintf(g_gs_dec_fp,
                    "%lu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s "
                    "%.6f %.6f %.6f %d %d %lu %lu %lu %lu %lu %lu\n",
                    log_cache_timestamp, gs_decision_period_segs_,
                    periodic_ratio_, waf_w, Gu, Fu, Gud, Fud, lhs, rhs,
                    raise ? "RAISE" : "LOWER",
                    cur_util, prev_target, target_valid_blk_rate,
                    hard_hit ? 1 : 0, low_hit ? 1 : 0,
                    compacted_blocks, evicted_blocks,
                    g_ex_low_target_count, g_ex_target_satisfied_count,
                    g_ex_force_flush_count, g_ex_high_valid_victim_count);
                std::fflush(g_gs_dec_fp);
            }
        }
    }
}

void LogCache::load_replay_log_if_needed() {
    if (replay_loaded_ || replay_failed_) return;
    const char* path = std::getenv("GS_REPLAY_LOG");
    if (!path || !*path) {
        std::fprintf(stderr, "[ReplayGS] GS_REPLAY_LOG env not set — replay disabled\n");
        replay_failed_ = true;
        return;
    }
    FILE* fp = std::fopen(path, "r");
    if (!fp) {
        std::fprintf(stderr, "[ReplayGS] cannot open %s — replay disabled\n", path);
        replay_failed_ = true;
        return;
    }
    char  buf[2048];
    bool  header_skipped = false;
    while (std::fgets(buf, sizeof(buf), fp)) {
        if (!header_skipped) {            // first line is column names
            header_skipped = true;
            if (!std::isdigit((unsigned char)buf[0])) continue;
        }
        // ts segs r waf G_u F_u G_ud F_ud LHS RHS decision cur_util tgt_before tgt_after ...
        unsigned long long ts = 0;
        int segs = 0;
        double r=0, waf=0, Gu=0, Fu=0, Gud=0, Fud=0, lhs=0, rhs=0;
        char dec[16] = {0};
        double cur_util = 0;
        if (std::sscanf(buf, "%llu %d %lf %lf %lf %lf %lf %lf %lf %lf %15s %lf",
                        &ts, &segs, &r, &waf, &Gu, &Fu, &Gud, &Fud, &lhs, &rhs,
                        dec, &cur_util) >= 12) {
            replay_log_.emplace_back(static_cast<uint64_t>(ts), cur_util);
        }
    }
    std::fclose(fp);
    if (replay_log_.empty()) {
        std::fprintf(stderr, "[ReplayGS] %s contained no usable rows\n", path);
        replay_failed_ = true;
        return;
    }
    replay_loaded_ = true;
    std::fprintf(stderr, "[ReplayGS] loaded %zu rows from %s (ts %llu..%llu, util_step_=%.6f)\n",
                 replay_log_.size(), path,
                 (unsigned long long)replay_log_.front().first,
                 (unsigned long long)replay_log_.back().first,
                 util_step_);
}

void LogCache::periodic_ghost_delta_gc_sum_replay() {
    // Mirror gc_sum's ratio bookkeeping so G_u/F_u/G_ud/F_ud stats stay populated.
    if (!is_ghost_cache) return;
    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        update_ghost_compacted_blocks_sum();
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
    }
    if (log_cache_timestamp % (segment_size_blocks * gs_decision_period_segs_) != 0) return;

    load_replay_log_if_needed();
    if (!replay_loaded_) return;

    // Advance cursor to smallest idx where replay_log_[idx].ts >= log_cache_timestamp.
    while (replay_idx_ < replay_log_.size() &&
           replay_log_[replay_idx_].first < log_cache_timestamp) {
        ++replay_idx_;
    }
    std::size_t use_idx = (replay_idx_ < replay_log_.size())
                          ? replay_idx_
                          : (replay_log_.size() - 1);  // past tail → hold last
    const double log_cur_util = replay_log_[use_idx].second;
    const double raw_target   = log_cur_util + util_step_;
    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit,
                                     std::max(0.0, raw_target));
}

void LogCache::periodic_ghost_delta_gc_nand() {
    if (is_ghost_cache){
        if (log_cache_timestamp % (segment_size_blocks/4) == 0) {
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            compaction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
        }
        if (log_cache_timestamp % (segment_size_blocks * 8) == 0) {
            if (compaction_ratio.has_value() &&
                compaction_ratio_in_ghost_cache.has_value() &&
                eviction_ratio.has_value() &&
                eviction_ratio_in_ghost_cache.has_value()){
                // NAND-aware: weight eviction-side benefit by observed cold-tier WAF.
                if (periodic_ratio_ * current_waf * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value())
                        > compaction_ratio_in_ghost_cache.value() - compaction_ratio.value()) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + util_step_);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - util_step_);
                }
            }
        }
    }
}

void LogCache::periodic_ghost_delta_gc_auto() {
    // FIXME: AUTO body disabled — auto_tune::Arm refactored to {signed_step}
    // only, but this function still references the prior {dir, half_life_blk,
    // window_segs, step_pct} schema and missing members (auto_dir_,
    // auto_compare_segs_). Re-port against arm_grid.h's signed_step grid
    // before re-enabling.
    return;
#if 0
    if (!is_ghost_cache) return;

    // 1) Feed cumulative counters; on round close apply the next arm.
    if (gp_tuner_) {
        gp_tuner_->observe(static_cast<uint64_t>(write_size_to_cache),
                           static_cast<uint64_t>(evicted_blocks),
                           static_cast<uint64_t>(compacted_blocks));
        if (gp_tuner_->round_closed_since_last_call()) {
            // Snapshot the round that just closed (using prev arm and the
            // reward/mu/sigma the GP just produced for the *next* arm).
            const auto closed_arm = auto_prev_arm_;
            const auto arm        = gp_tuner_->close_round_and_select();
            // setMovingAverage rebuilds the seven ratio objects, so only call
            // it when the half-life actually changes (avoids EWMA churn).
            if (arm.half_life_blk != auto_prev_arm_.half_life_blk) {
                setMovingAverage(moving_avg_type_, arm.half_life_blk);
            }
            auto_dir_           = (arm.dir >= 0) ? +1 : -1;
            auto_compare_segs_  = arm.window_segs;
            util_step_          = arm.step_pct;
            auto_prev_arm_      = arm;

            // Append one row per round. round_index in the tuner is the
            // number of rounds *closed* so far (1-based after the first
            // close), so the row records: round N closed with closed_arm
            // → reward → next arm chosen.
            if (!autotune_csv_path_.empty()) {
                if (!autotune_csv_) {
                    autotune_csv_ = fopen(autotune_csv_path_.c_str(), "w");
                    if (autotune_csv_) {
                        fprintf(autotune_csv_,
                                "round,host_TiB,closed_dir,closed_hl,closed_win,closed_step,"
                                "flush_rate,comp_rate,f,reward,n_train,"
                                "next_dir,next_hl,next_win,next_step,next_mu,next_sigma\n");
                    }
                }
                if (autotune_csv_) {
                    const double host_TiB =
                        static_cast<double>(write_size_to_cache) /
                        static_cast<double>(1ull << 40);
                    const double flush = gp_tuner_->last_flush_rate();
                    const double comp  = gp_tuner_->last_comp_rate();
                    const double f     = -gp_tuner_->last_reward();
                    fprintf(autotune_csv_,
                            "%llu,%.4f,%d,%.0f,%.3f,%.5f,"
                            "%.6f,%.6f,%.6f,%.6f,%zu,"
                            "%d,%.0f,%.3f,%.5f,%.6f,%.6f\n",
                            (unsigned long long)gp_tuner_->round_index(),
                            host_TiB,
                            closed_arm.dir, closed_arm.half_life_blk,
                            closed_arm.window_segs, closed_arm.step_pct,
                            flush, comp, f, gp_tuner_->last_reward(),
                            gp_tuner_->train_size(),
                            arm.dir, arm.half_life_blk,
                            arm.window_segs, arm.step_pct,
                            gp_tuner_->last_mu(), gp_tuner_->last_sigma());
                    fflush(autotune_csv_);
                }
            }
        }
    }

    // 2) Stock GhostDelta_GC sampling/decision loop with autotuned knobs.
    const uint64_t sample_period = segment_size_blocks / 4;
    const uint64_t decide_period = static_cast<uint64_t>(
        static_cast<double>(segment_size_blocks) * auto_compare_segs_);
    if (sample_period > 0 && log_cache_timestamp % sample_period == 0) {
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        const uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
    }
    if (decide_period == 0 || log_cache_timestamp % decide_period != 0) return;
    if (!compaction_ratio.has_value() || !compaction_ratio_in_ghost_cache.has_value() ||
        !eviction_ratio.has_value()   || !eviction_ratio_in_ghost_cache.has_value()) return;

    // GC's asymmetric comparison gives raise/lower evidence; auto_dir_ acts
    // as a polarity multiplier (dir=-1 inverts the evidence — exposed so the
    // GP can confirm dir=+1 is the right call).
    const bool raise_evidence =
        periodic_ratio_ * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value())
            > compaction_ratio_in_ghost_cache.value() - compaction_ratio.value();
    const int  applied = auto_dir_ * (raise_evidence ? +1 : -1);
    const double base  = static_cast<double>(global_valid_blocks) / total_cache_block_count;
    if (applied >= 0) {
        target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, base + util_step_);
    } else {
        target_valid_blk_rate = std::max(0.0, base - util_step_);
    }
#endif
}

void LogCache::periodic_ghost_delta() {
#if 1
    if (is_ghost_cache){
        if (log_cache_timestamp % (segment_size_blocks/4) == 0) {
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
        }
        if (log_cache_timestamp % (segment_size_blocks * 8) == 0) {
            if (compaction_ratio.has_value() &&
                eviction_ratio.has_value() &&
                eviction_ratio_in_ghost_cache.has_value()){
                if (periodic_ratio_ * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value()) > compaction_ratio.value() * 1.00) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + ghost_reanchor_step_);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - ghost_reanchor_step_);
                }
            }
        }
    }
#elif 0
    /* ── A/B feedback: net free segs vs compaction cost ── */
    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        uint64_t A = gc_victim_count - gc_active_alloc_count_;
        net_free_seg_ratio_.updateFromCumulative(log_cache_timestamp, A * segment_size_blocks);
        if (net_free_seg_ratio_.has_value()) {
            double a = net_free_seg_ratio_.value();
            cumulative_B_ += evictor->get_mth_score_valid_pages(a);
        }
        gc_valid_pages_ratio_.updateFromCumulative(log_cache_timestamp, cumulative_B_);
    }

    if (valid_rate_period_blocks_ > 0 &&
        log_cache_timestamp >= next_valid_rate_change_ts_) {
        next_valid_rate_change_ts_ += valid_rate_period_blocks_;
        if (net_free_seg_ratio_.has_value() && gc_valid_pages_ratio_.has_value()) {
            double a = net_free_seg_ratio_.value();
            double b = gc_valid_pages_ratio_.value();
            static constexpr double r = 2.88;
            double old_rate = target_valid_blk_rate;
            if (b * r > a) {
                target_valid_blk_rate = std::min(valid_blk_rate_hard_limit,
                    (double)global_valid_blocks / total_cache_block_count + 0.1);
            } else {
                target_valid_blk_rate = std::max(0.0,
                    (double)global_valid_blocks / total_cache_block_count - 0.1);
            }
            printf("periodic_ab: ts=%lu a=%.6f b=%.6f b*r=%.6f old=%.4f new=%.4f\n",
                   log_cache_timestamp, a, b, b * r, old_rate, target_valid_blk_rate);
        }
    }
#endif
}

void LogCache::periodic_t_delta() {
    /* Hill-climb on smoothed f = compaction_rate + r * eviction_rate.
     * MovingAverage(EWMA or SMA) 를 통해 누적값으로부터 평활화된 rate 를 계산하고,
     * 이전 window 의 smoothed f 와 비교해 방향을 결정. */
    if (!is_ghost_cache) return;
    if (log_cache_timestamp == 0) return;
    if (valid_blk_rate_hard_limit <= 0.0) return;

    if (log_cache_timestamp % (segment_size_blocks/4) == 0) {
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
    }
    if (log_cache_timestamp % (segment_size_blocks * 8) != 0) return;

    if (compaction_ratio.has_value() && eviction_ratio.has_value()) {
        double f_curr = compaction_ratio.value() + periodic_ratio_ * eviction_ratio.value();
        if (tdelta_have_prev_f_ && f_curr > tdelta_prev_f_) {
            tdelta_last_dir_ = -tdelta_last_dir_;
        }
        tdelta_prev_f_      = f_curr;
        tdelta_have_prev_f_ = true;
    }

    double new_rate = target_valid_blk_rate + tdelta_last_dir_ * tdelta_step_;
    if (new_rate < 0.0) new_rate = 0.0;
    if (new_rate > valid_blk_rate_hard_limit) new_rate = valid_blk_rate_hard_limit;
    target_valid_blk_rate = new_rate;
}
/*
void LogCache::periodic() {
    if (is_ghost_cache){
        if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
#ifdef GHOST_CACHE
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
            compaction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
            ghost_util_ratio.updateFromCumulative(ghost_access_total, ghost_miss_total);
#else
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
#endif
        }
        if (log_cache_timestamp % (segment_size_blocks * 4) == 0) {
#ifdef GHOST_CACHE
            if (compaction_ratio.has_value() &&
                eviction_ratio.has_value() &&
                compaction_ratio_in_ghost_cache.has_value() &&
                eviction_ratio_in_ghost_cache.has_value()) {
                double current_tco = compaction_ratio.value() + TCO_EVICTION_WEIGHT * eviction_ratio.value();
                double ghost_tco = compaction_ratio_in_ghost_cache.value() + TCO_EVICTION_WEIGHT * eviction_ratio_in_ghost_cache.value();
                // compaction_ratio > 1.3이면 무조건 LOWER (full random workload → eviction-heavy)
                if (compaction_ratio.value() > 1.3) {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    printf("periodic: LOWER (compact>1.3) target=%.4f, evict_val=%.6f, compact_val=%.6f current_tco=%.6f ghost_tco=%.6f\n",
                           target_valid_blk_rate, TCO_EVICTION_WEIGHT * eviction_ratio.value(), compaction_ratio.value(), current_tco, ghost_tco);
                }
                else if (current_tco > ghost_tco) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + 0.02);
                    printf("periodic: RISE target=%.4f, evict_val=%.6f, compact_val=%.6f, current_tco=%.6f ghost_tco=%.6f\n",
                           target_valid_blk_rate, TCO_EVICTION_WEIGHT * eviction_ratio.value(), compaction_ratio.value(), current_tco, ghost_tco);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    printf("periodic: LOWER target=%.4f, evict_val=%.6f, compact_val=%.6f, current_tco=%.6f ghost_tco=%.6f\n",
                           target_valid_blk_rate, TCO_EVICTION_WEIGHT * eviction_ratio.value(), compaction_ratio.value(), current_tco, ghost_tco);
                }
            }
#else
            if (compaction_ratio.has_value() && eviction_ratio.has_value()) {
                double current_tco = compaction_ratio.value() + TCO_EVICTION_WEIGHT * eviction_ratio.value();
                double old_tco = tco_history.size() >= TCO_HISTORY_SIZE ? tco_history.front() : 0.0;

                if (compaction_ratio.value() > 1.3) {
                    tco_policy_higher = false;
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    printf("periodic: LOWER (compact>1.3) target=%.4f, evict_val=%.6f, compact_val=%.6f, current_tco=%.6f old_tco=%.6f\n",
                           target_valid_blk_rate, TCO_EVICTION_WEIGHT * eviction_ratio.value(), compaction_ratio.value(), current_tco, old_tco);
                } else if (old_tco > 0.0) {
                    if (current_tco >= old_tco) {
                        tco_policy_higher = !tco_policy_higher;
                    }

                    if (tco_policy_higher) {
                        target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double)global_valid_blocks / total_cache_block_count + 0.02);
                    } else {
                        target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    }
                    printf("periodic: %s target=%.4f, evict_val=%.6f, compact_val=%.6f, current_tco=%.6f old_tco=%.6f (-%zu cycles)\n",
                           tco_policy_higher ? "HIGHER" : "LOWER",
                           target_valid_blk_rate, TCO_EVICTION_WEIGHT * eviction_ratio.value(), compaction_ratio.value(), current_tco, old_tco, tco_history.size());
                }

                tco_history.push_back(current_tco);
                if (tco_history.size() > TCO_HISTORY_SIZE) {
                    tco_history.pop_front();
                }
            }
#endif
        }
    }
}
*/

void LogCache::batch_insert(int stream_id,
                            const std::map<long,int>& newBlocks,
                            OP_TYPE                   op_type)
{
    if (op_type == OP_TYPE::READ || newBlocks.empty())
        return;                          // 요구사항 ③ – read 무시
    
    /* 1) 스트림별 active segment 확보 */
    LogCacheSegment* seg = nullptr;
    if (!stream_policy) {
        seg = get_segment_to_active_stream(false, stream_id);
    }
    
    /* 2) 블록 단위 append */
    for (auto [key, lba_sz] : newBlocks)
    {
        // every 32 MB, call some code.
        periodic();
        bool ghost_hit = ghost_cache.access(key);
        ++ghost_access_total;
        if (!ghost_hit) ++ghost_miss_total;
        if (stream_policy) {
            seg = get_segment_with_stream_policy(false, key);
        }
        
        if (seg->full())                 // segment 소진 -> 새 seg
        {
            // add previous segment count 
            
            evict_policy_add(seg);
            int assigned_class_num = seg->get_class_num();
            active_seg.erase(seg->get_class_num());
            seg = get_segment_to_active_stream(false, assigned_class_num);
        }
        
        record_rewrite(key);
        invalidate(key, lba_sz);

        seg->blocks[seg->write_ptr] = { key, true, log_cache_timestamp };
        mapping[key]                = { seg, seg->write_ptr };

        ++seg->write_ptr;
        ++seg->valid_cnt;
        ++global_valid_blocks;
        ++log_cache_timestamp; // increment timestamp for each block
        if (stream_policy) {
            stream_policy->Append(key, log_cache_timestamp, reinterpret_cast<void*>(seg->valid_cnt));
        }
        write_size_to_cache += lba_sz;
        if (!inv_snapshot_taken_ && write_size_to_cache >= INV_SNAPSHOT_THRESHOLD) {
            take_inv_snapshot();
        }

        /* proactive GC: 1 segment per batch, like async impl */
        if (log_cache_timestamp % (segment_size_blocks / 2) == 0 && free_pool.size() <= 10) {
            check_and_evict_if_needed(1);
        }
    }
    /* async-like: max 1 segment per batch_insert call, not burst */
    /*if (free_pool.size() <= 10) {
        check_and_evict_if_needed(1);
    }*/
    /* 3) free pool 부족 시 세그먼트 eviction */
    
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
LogCacheSegment* LogCache::alloc_segment(bool shrink)
{
    
    if (shrink == true) {
      //  if (free_pool.size() <= 3)
            check_and_evict_if_needed(0);        // emergency burst
      /*  else if (free_pool.size() <= 10)
            check_and_evict_if_needed(1);       // proactive: 1 segment*/
    }

    if (free_pool.empty()) {
        printf("[GC] FATAL: free_pool empty after evict! global_valid=%lu ts=%lu\n",
               global_valid_blocks, log_cache_timestamp);
        throw std::runtime_error("LogCache: no free segment");
    }

    LogCacheSegment* s = free_pool.front();
    free_pool.pop_front();
    s->reset();
    return s;
}


LogCacheSegment* LogCache::get_segment_with_stream_policy(bool gc, uint64_t key, bool check_only)
{
    LogCacheSegment *seg = nullptr;
    uint64_t previous_blk_create_timestamp = log_cache_timestamp;
    
    if (exists(key))
    {
        auto loc = mapping[key];
        assert(loc.seg != nullptr);
        assert(loc.idx < loc.seg->blocks.size());
        if (loc.seg->blocks[loc.idx].valid)
        {
            previous_blk_create_timestamp = loc.seg->blocks[loc.idx].create_timestamp;
        }
    }
    int stream_id = stream_policy->Classify(key, gc, log_cache_timestamp, previous_blk_create_timestamp);
    assert (!gc || (gc && stream_id >= Segment::GC_STREAM_START));

    // Cycle wrap detected → dummy fill old active GC segments before reuse
    if (gc) {
        int victim_id = stream_policy->GetVictimStreamId(log_cache_timestamp, 0);
        while (victim_id >= Segment::GC_STREAM_START) {
            auto vit = gc_active_seg.find(victim_id);
            if (vit != gc_active_seg.end()) {
                dummy_fill_segment(vit->second);
                gc_active_seg.erase(vit);
            }
            victim_id = stream_policy->GetVictimStreamId(log_cache_timestamp, 0);
        }
    }

    std::unordered_map<int, LogCacheSegment*>*  active_table = &active_seg;
    if (gc) {
        active_table = &gc_active_seg;
    }
    auto it_stream = active_table->find(stream_id);
    if (it_stream == active_table->end())
    {
        if(check_only) {
            return nullptr;
        }
        seg = alloc_segment(!gc);
        seg->class_num = stream_id; // stream id로 class num 설정
        seg->create_timestamp = log_cache_timestamp; // 초기화
        (*active_table)[stream_id] = seg;
        if (gc) ++gc_active_alloc_count_;
    }
    else
    {
        seg = it_stream->second;
        assert (!gc || (gc && stream_id >= Segment::GC_STREAM_START));
        assert (!gc || (gc && seg->get_class_num() >= Segment::GC_STREAM_START));
    }
    return seg;
}

LogCacheSegment* LogCache::get_segment_to_active_stream(bool gc, int stream_id, bool check_only)
{
    LogCacheSegment *seg = nullptr;
    std::unordered_map<int, LogCacheSegment*>*  active_table = &active_seg;
    if (gc) {
        active_table = &gc_active_seg;
        if (stream_id < Segment::GC_STREAM_START) {
            stream_id += Segment::GC_STREAM_START; 
        }
    }
    auto it_stream = active_table->find(stream_id);
    if (it_stream == active_table->end())
    {
        if (check_only) {
            return nullptr;
        }
        seg = alloc_segment(!gc);
        seg->class_num = stream_id; // stream id로 class num 설정
        seg->create_timestamp = log_cache_timestamp; // 초기화
        (*active_table)[stream_id] = seg;
        if (gc) ++gc_active_alloc_count_;
    }
    else
    {
        seg = it_stream->second;
    }
    return seg;
}

extern uint64_t g_threshold;
extern uint64_t g_timestamp;
void LogCache::check_and_evict_if_needed(int max_victims)
{
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));

    /* activate lifetime tracking once cache is full */
    if (!lifetime_tracking_active_ && free_pool.size() <= 3) {
        lifetime_tracking_active_ = true;
        printf("[Lifetime] tracking activated at timestamp %lu\n", log_cache_timestamp);
    }

    LogCacheSegment* last_target_seg = nullptr;
    //bool first_compact = true;
    std::list<Segment *> segment_list;
    g_timestamp = log_cache_timestamp;
    static uint64_t threshold = (cfg_.segment_bytes / cache_block_size) *static_cast<std::size_t>(std::ceil(total_segments *
                                           (1 - cfg_.free_ratio_low) * (1 + additional_free_blks_ratio_by_gc)));
    g_threshold = threshold + cfg_.segment_bytes / cache_block_size;
   // printf("%d\n", low_water);
    int processed = 0;
    while (free_pool.size() <= 3 ||
           (max_victims > 0 && processed < max_victims && free_pool.size() <= 10))
    {
        /* eviction 후보 수집 */
        bool compact = false;
        if (target_valid_blk_rate >= 0.1) {
            if (compactor && (double)target_valid_blk_rate * total_cache_block_count  > global_valid_blocks) {
                compact = true;
            } else {
                g_ex_target_satisfied_count++;   // target high enough but valid_blocks already low
            }
        } else {
            g_ex_low_target_count++;             // target < 0.1 → flush path
        }
        if (free_pool.size() <= 4 && compact) {
            g_ex_force_flush_count++;            // would-be compaction overridden by force-flush
            compact = false;
        }

        LogCacheSegment* victim = nullptr; 
        if (compact == true){
            victim = (LogCacheSegment *)evictor->choose_segment();
            threshold = log_cache_timestamp - victim->create_timestamp + 1;
            g_threshold = threshold;
            evictor->add(victim, log_cache_timestamp);
            victim = (LogCacheSegment *)compactor->choose_segment();
        }
        else {
            victim = (LogCacheSegment *)evictor->choose_segment();
            threshold = log_cache_timestamp - victim->create_timestamp;
            g_threshold = threshold;    
        }

        //printf("compact %d\n", compact);
        assert (victim != nullptr);
        gc_victim_count++;
        gc_victim_valid_ratio_sum += (double)victim->valid_cnt / victim->blocks.size();
        if (victim->valid_cnt == 0) {
            reset_segment(victim);
        }
        else if (compact == false) {
            evicted_segment_age = victim->get_create_time();
            evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
            evict_segment(victim);
        }
        else if (compact == true) {
            if (victim->valid_cnt > 0.95 * segment_size_blocks) {
                g_ex_high_valid_victim_count++;
                compact = false;
                compactor->add(victim, log_cache_timestamp);
                victim = (LogCacheSegment *)evictor->choose_segment();
                evicted_segment_age = victim->get_create_time();
                evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
                evict_segment(victim);
            }
            // Ghost compaction cost estimation
            else {
                if (is_ghost_cache && compactor) {
                    update_ghost_compacted_blocks(victim);       // GhostDelta_GC variants
                    // GS_SUM uses per-compact reassign; GS_SUM_Final uses tick-based
                    // cum_valid in periodic_ghost_delta_gc_sum_final — don't overwrite.
                    if (periodic_mode_ != PeriodicMode::GhostDelta_GC_SUM_Final) {
                        update_ghost_compacted_blocks_sum();      // GhostDelta_GC_SUM (reassign)
                    }
                }

                int stream_id = victim->get_class_num();
                compacted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
                last_target_seg = (LogCacheSegment *)evict_and_compaction(victim, threshold, stream_id);
                if (last_target_seg) {
                    segment_list.push_back(last_target_seg);
                }
            }
        }
        else {
            evicted_segment_age = victim->get_create_time();
            evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
            evict_segment(victim);
        }

        if (stream_policy && compact == true) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }
        if (stream_policy) {
            int victim_stream_id = stream_policy->GetVictimStreamId(log_cache_timestamp, threshold);
            while (victim_stream_id >= Segment::GC_STREAM_START) {
                auto vit = gc_active_seg.find(victim_stream_id);
                if (vit != gc_active_seg.end()) {
                    dummy_fill_segment(vit->second);
                    gc_active_seg.erase(vit);
                }
                victim_stream_id = stream_policy->GetVictimStreamId(log_cache_timestamp, threshold);
            }
        }
        ++processed;
    }
}

int LogCache::get_block_size()
{
    return cache_block_size;
}

bool LogCache::is_cache_filled() {
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));
    return free_pool.size() < low_water;
}


void LogCache::reset_segment(LogCacheSegment* s)
{
       // erase old segment
    s->valid_cnt = 0;
    s->write_ptr = 0;
    free_pool.push_back(s);
    evict_policy_remove(s);
}

void LogCache::dummy_fill_segment(LogCacheSegment* s)
{
    if (s) {
        ++dummy_fill_segment_count;
        for (std::size_t i = s->write_ptr; i < s->blocks.size(); ++i)
        {
            s->blocks[i] = { 0, false, UINT64_MAX };
        }
        s->write_ptr = s->blocks.size();
        evict_policy_add(s);
    }
}



Segment* LogCache::evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id)
{
    LogCacheSegment* target_seg = nullptr;
    int evicted_blocks_for_victim = 0, compacted_blocks_for_victim = 0;
    if (!stream_policy) {
        target_seg = get_segment_to_active_stream(true, gc_stream_id);
    }
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (threshold > 0 && log_cache_timestamp - blk.create_timestamp >= threshold) { 
            if (is_ghost_cache) {
                ghost_cache.push(blk.key);
            }
            print_objects("evict", log_cache_timestamp - blk.create_timestamp);
            evicted_blocks += cfg_.evicted_blk_size;
            evicted_ages_histogram->inc(log_cache_timestamp - blk.create_timestamp);
            {
                auto cit = compacted_at_.find(blk.key);
                if (cit != compacted_at_.end()) {
                    compacted_lifetime_histogram_->inc(log_cache_timestamp - cit->second);
                    compacted_at_.erase(cit);
                }
            }
            evicted_timestamp[blk.key] = log_cache_timestamp;
            evicted_blocks_for_victim += 1;
            // map erase and blk valid false is done in this function
            evict(blk);
            continue;
        }
        if (stream_policy) {
            target_seg = get_segment_with_stream_policy(true, blk.key);
        }
        assert(target_seg->class_num >= Segment::GC_STREAM_START || !stream_policy);
        if (target_seg->full())                 // segment 소진 -> 새 seg
        {
            // add previous segment count 
            evict_policy_add(target_seg);
            int assigned_class_num = target_seg->get_class_num();
            assert(assigned_class_num >= 0);
            gc_active_seg.erase(target_seg->get_class_num());
            target_seg = get_segment_to_active_stream(true, assigned_class_num);
        }
        assert(target_seg != s);
        // get p2L index
        if (target_seg->create_timestamp > blk.create_timestamp) {
            target_seg->create_timestamp = blk.create_timestamp;
        }

        print_objects("compact", log_cache_timestamp - blk.create_timestamp);
        target_seg->blocks[target_seg->write_ptr] = blk; // copy valid block
        mapping[blk.key] = { target_seg, target_seg->write_ptr };
        ++target_seg->write_ptr;
        ++target_seg->valid_cnt;
        ++compacted_blocks;
        compacted_blocks_for_victim += 1;
        {
            auto cit = compacted_at_.find(blk.key);
            if (cit != compacted_at_.end()) {
                compacted_lifetime_histogram_->inc(log_cache_timestamp - cit->second);
            }
            compacted_at_[blk.key] = log_cache_timestamp;
        }

        blk.valid = false;
    }
    assert((target_seg == nullptr && compacted_blocks_for_victim == 0) || target_seg);
    /*if (target_seg) {
        printf("Compaction and Evict: %lu blocks moved from segment %p to segment %p, free_pool_size %ld, valid ratio %.4f age %lu target_seg_write_ptr %lu target_create_timestamp %lu threshold %lu stream_id %d\n", 
        s->valid_cnt, s, target_seg, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, target_seg->write_ptr, s->create_timestamp, threshold, target_seg->get_class_num());
    }
    else {
        printf("Evict: %lu blocks free_pool_size %ld, valid ratio %.4f age %lu, create_time %lu \n", 
        s->valid_cnt, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, s->create_timestamp);
    }*/
    reset_segment(s);
    evicted_blocks_histogram->inc(evicted_blocks_for_victim);
    compacted_blocks_histogram->inc(compacted_blocks_for_victim);
    return target_seg;
}


void LogCache::evict_segment(LogCacheSegment* s)
{
    int evicted_blocks_for_victim = 0;
    /* 모든 valid page flush */
    if (s->valid_cnt == 0) {
    }
    
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (is_ghost_cache) {
            ghost_cache.push(blk.key);
        }
        print_objects("evict", log_cache_timestamp - blk.create_timestamp);
        evicted_ages_histogram->inc(log_cache_timestamp - blk.create_timestamp);
        {
            auto cit = compacted_at_.find(blk.key);
            if (cit != compacted_at_.end()) {
                compacted_lifetime_histogram_->inc(log_cache_timestamp - cit->second);
                compacted_at_.erase(cit);
            }
        }
        evicted_blocks += cfg_.evicted_blk_size;
        evicted_blocks_for_victim += 1;
        evicted_timestamp[blk.key] = log_cache_timestamp;

        // map erase and blk valid false is done in this function
        evict(blk);
        
    }
  //  printf("Evict: %lu blocks free_pool_size %ld, valid ratio %.4f age %lu, create_time %lu \n",
  //      s->valid_cnt, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, s->create_timestamp);
    reset_segment(s);
    evicted_blocks_histogram->inc(evicted_blocks_for_victim);
}


void LogCache::evict_one_block() {
}

void LogCache::evict(LogCacheSegment::Block &blk) {
    
    //const uint64_t DUMMY_VALUE = 0;
    uint64_t old_key = blk.key;
    int EVICTED_BLOCK_SIZE = cfg_.evicted_blk_size; // 16 blocks, 64k
    int evicted_blocks_per_evict = 0;

    uint64_t start_index_64k = old_key / EVICTED_BLOCK_SIZE * EVICTED_BLOCK_SIZE;
    for (uint64_t index_64k = start_index_64k; index_64k < start_index_64k + EVICTED_BLOCK_SIZE; index_64k++) {
        if (index_64k == old_key) {
            continue;
        }
        auto it = mapping.find(index_64k);

        if (it != mapping.end()) {
            record_lifetime(log_cache_timestamp - it->second.seg->blocks[it->second.idx].create_timestamp, false);
            record_inv_time(index_64k);
            auto &other_blk = it->second.seg->blocks[it->second.idx];
            other_blk.valid = false;
            mapping.erase(index_64k);
            global_valid_blocks -= 1;
            evicted_blocks_per_evict += 1;
        }
        else {
            read_blocks_in_partial_write += 1;
        }
    }
    evicted_cache_blocks_per_evict->inc(evicted_blocks_per_evict);
    _evict_one_block(start_index_64k  * cache_block_size /* 64k aligend */, cache_block_size * EVICTED_BLOCK_SIZE /* 64k */, OP_TYPE::WRITE);
    record_inv_time(blk.key);
    record_lifetime(log_cache_timestamp - blk.create_timestamp, false);
    mapping.erase(blk.key);
    blk.valid = false;
    global_valid_blocks -= 1;
}

void LogCache::print_objects(std::string prefix, uint64_t value) {
    //fprintf(fp_object, "%s: %lu\n", prefix.c_str(), value);
}

/* ── Lifetime histogram (entire trace) ─────────────────── */

void LogCache::record_lifetime(uint64_t lifetime, bool is_host_invalidate)
{
    if (!lifetime_tracking_active_) return;
    uint64_t bucket = lifetime / LIFETIME_BUCKET_WIDTH;
    if (is_host_invalidate)
        lifetime_hist_invalidate_[bucket]++;
    else
        lifetime_hist_evict_[bucket]++;
}

void LogCache::print_lifetime_results()
{
    if (!lifetime_tracking_active_) return;

    /* merge all bucket keys */
    std::set<uint64_t> all_buckets;
    for (auto& [b, _] : lifetime_hist_invalidate_) all_buckets.insert(b);
    for (auto& [b, _] : lifetime_hist_evict_)      all_buckets.insert(b);

    if (all_buckets.empty()) return;

    FILE* f = fopen("lifetime_histogram.csv", "w");
    if (!f) return;

    fprintf(f, "lifetime_start,lifetime_end,count_invalidate,count_evict,count_all\n");

    for (uint64_t b : all_buckets) {
        uint64_t start = b * LIFETIME_BUCKET_WIDTH;
        uint64_t end   = start + LIFETIME_BUCKET_WIDTH;
        uint64_t ci = lifetime_hist_invalidate_.count(b) ? lifetime_hist_invalidate_[b] : 0;
        uint64_t ce = lifetime_hist_evict_.count(b)      ? lifetime_hist_evict_[b]      : 0;
        fprintf(f, "%lu,%lu,%lu,%lu,%lu\n", start, end, ci, ce, ci + ce);
    }
    fclose(f);
    printf("[Lifetime] histogram written to lifetime_histogram.csv (%zu buckets)\n",
           all_buckets.size());
}

/* ── Rewrite interval tracking (no-cache baseline) ──────── */

void LogCache::record_rewrite(long key)
{
    if (!lifetime_tracking_active_) return;
    auto it = rewrite_last_ts_.find(key);
    if (it != rewrite_last_ts_.end()) {
        uint64_t interval = log_cache_timestamp - it->second;
        uint64_t bucket   = interval / LIFETIME_BUCKET_WIDTH;
        rewrite_hist_[bucket]++;
        it->second = log_cache_timestamp;
    } else {
        rewrite_last_ts_[key] = log_cache_timestamp;
    }
}

void LogCache::print_rewrite_results()
{
    if (rewrite_hist_.empty()) return;

    FILE* f = fopen("rewrite_histogram.csv", "w");
    if (!f) return;

    fprintf(f, "interval_start,interval_end,count\n");
    for (auto& [b, cnt] : rewrite_hist_) {
        uint64_t start = b * LIFETIME_BUCKET_WIDTH;
        uint64_t end   = start + LIFETIME_BUCKET_WIDTH;
        fprintf(f, "%lu,%lu,%lu\n", start, end, cnt);
    }
    fclose(f);
    printf("[Rewrite] histogram written to rewrite_histogram.csv (%zu buckets, %zu unique LBAs tracked)\n",
           rewrite_hist_.size(), rewrite_last_ts_.size());
}

void LogCache::print_utilization_distribution()
{
    // 2% bins: [0,2), [2,4), ... [98,100]
    static constexpr int NUM_BINS = 50;
    static constexpr double BIN_WIDTH = 2.0; // percent
    uint64_t bins[NUM_BINS] = {};
    uint64_t total_sealed = 0;

    for (auto& seg_ptr : all_segments) {
        LogCacheSegment* seg = seg_ptr.get();
        // skip active segments (not yet sealed) and free pool
        if (seg->write_ptr == 0) continue;

        double util_pct = 100.0 * seg->valid_cnt / seg->blocks.size();
        int bin = static_cast<int>(util_pct / BIN_WIDTH);
        if (bin >= NUM_BINS) bin = NUM_BINS - 1;
        bins[bin]++;
        total_sealed++;
    }

    if (total_sealed == 0) {
        printf("[UtilDist] no sealed segments to report\n");
        return;
    }

    // generate timestamped filename (use start timestamp + policy prefix)
    char fname[256];
    {
        const std::string& sts = start_ts();
        const std::string& pfx = stats_prefix();
        if (sts.empty()) {
            char ts_buf[64];
            time_t now = time(nullptr); struct tm tm;
            localtime_r(&now, &tm);
            strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
            snprintf(fname, sizeof(fname), "utilization_distribution.%s.csv", ts_buf);
        } else if (pfx.empty()) {
            snprintf(fname, sizeof(fname), "utilization_distribution.%s.csv", sts.c_str());
        } else {
            snprintf(fname, sizeof(fname), "%s.utilization_distribution.%s.csv", pfx.c_str(), sts.c_str());
        }
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;

    fprintf(f, "util_low,util_high,count,fraction\n");
    for (int i = 0; i < NUM_BINS; i++) {
        double lo = i * BIN_WIDTH;
        double hi = lo + BIN_WIDTH;
        double frac = static_cast<double>(bins[i]) / total_sealed;
        fprintf(f, "%.0f,%.0f,%lu,%.6f\n", lo, hi, bins[i], frac);
    }
    fclose(f);
    printf("[UtilDist] distribution written to %s (%lu segments)\n", fname, total_sealed);
}

void LogCache::print_segment_age_scatter()
{
    const std::string& ts = start_ts();
    const std::string& prefix = stats_prefix();
    char fname[256];
    if (ts.empty()) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts_buf[64];
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        snprintf(fname, sizeof(fname), "segment_age_scatter.%s.csv", ts_buf);
    } else if (prefix.empty()) {
        snprintf(fname, sizeof(fname), "segment_age_scatter.%s.csv", ts.c_str());
    } else {
        snprintf(fname, sizeof(fname), "%s.segment_age_scatter.%s.csv", prefix.c_str(), ts.c_str());
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;

    fprintf(f, "seg_age,utilization,valid_count,block_age_mean,block_age_stddev\n");
    uint64_t count = 0;

    for (auto& seg_ptr : all_segments) {
        LogCacheSegment* seg = seg_ptr.get();
        if (seg->write_ptr == 0) continue;

        uint64_t seg_age = log_cache_timestamp - seg->create_timestamp;
        double util = static_cast<double>(seg->valid_cnt) / seg->blocks.size();

        // compute mean and stddev of valid block ages
        double sum = 0.0;
        double sum_sq = 0.0;
        uint64_t n = 0;
        for (size_t i = 0; i < seg->write_ptr; i++) {
            if (!seg->blocks[i].valid) continue;
            double age = static_cast<double>(log_cache_timestamp - seg->blocks[i].create_timestamp);
            sum += age;
            sum_sq += age * age;
            n++;
        }

        double mean = 0.0, stddev = 0.0;
        if (n > 0) {
            mean = sum / n;
            if (n > 1) {
                double var = (sum_sq - sum * sum / n) / (n - 1);
                stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }

        fprintf(f, "%lu,%.6f,%lu,%.1f,%.1f\n", seg_age, util, n, mean, stddev);
        count++;
    }

    fclose(f);
    printf("[AgeScatter] written to %s (%lu segments)\n", fname, count);
}

void LogCache::print_stats() {
    static uint64_t written_window_bytes = cfg_.print_stats_interval;
    static uint64_t next_written_bytes = cfg_.segment_bytes;
    if ((uint64_t)write_size_to_cache >= next_written_bytes) {
        const std::string& prefix = stats_prefix();
        const char* prefix_cstr = prefix.empty() ? "LOG_CACHE" : prefix.c_str();
        double avg_victim_valid_ratio = (gc_victim_count > 0) ? gc_victim_valid_ratio_sum / gc_victim_count : 0.0;
        double F_u   = eviction_ratio.has_value()                ? eviction_ratio.value()                : 0.0;
        double F_ud  = eviction_ratio_in_ghost_cache.has_value() ? eviction_ratio_in_ghost_cache.value() : 0.0;
        double G_u   = compaction_ratio.has_value()              ? compaction_ratio.value()              : 0.0;
        double G_ud  = compaction_ratio_in_ghost_cache.has_value()? compaction_ratio_in_ghost_cache.value(): 0.0;
        // Phase-1 candidate tracker columns:
        //   candK_segs
        //   candK_g_n  candK_g_mre  candK_g_rmse  candK_g_smre  candK_g_srmse
        //   candK_f_n  candK_f_mre  candK_f_rmse  candK_f_smre  candK_f_srmse
        //   (K = 0,1,2 = segs-1, segs, segs+1)
        // mre/rmse  = raw drain pair stats
        // smre/srmse = SMA(64 segs) window of paired (pred, real) before err calc
        double g_mre[3] = {0,0,0}, g_rmse[3] = {0,0,0};
        double f_mre[3] = {0,0,0}, f_rmse[3] = {0,0,0};
        double g_smre[3] = {0,0,0}, g_srmse[3] = {0,0,0};
        double f_smre[3] = {0,0,0}, f_srmse[3] = {0,0,0};
        double g_pmean[3] = {0,0,0}, g_rmean[3] = {0,0,0};
        double f_pmean[3] = {0,0,0}, f_rmean[3] = {0,0,0};
        int    cand_segs[3] = {0,0,0};
        uint64_t g_n[3] = {0,0,0}, f_n[3] = {0,0,0};
        uint64_t g_sn[3] = {0,0,0}, f_sn[3] = {0,0,0};
        for (int k = 0; k < 3; ++k) {
            const auto& c = gs_cand_[k];
            cand_segs[k] = c.segs;
            g_n[k]  = c.g_err_n;
            f_n[k]  = c.f_err_n;
            g_sn[k] = c.g_smooth_err_n;
            f_sn[k] = c.f_smooth_err_n;
            if (c.g_err_n > 0) {
                const double n_d = static_cast<double>(c.g_err_n);
                g_mre[k]  = c.g_err_sum    / n_d;
                g_rmse[k] = std::sqrt(c.g_sq_err_sum / n_d);
                g_pmean[k] = c.g_pred_sum_raw / n_d;
                g_rmean[k] = c.g_real_sum_raw / n_d;
            }
            if (c.f_err_n > 0) {
                const double n_d = static_cast<double>(c.f_err_n);
                f_mre[k]  = c.f_err_sum    / n_d;
                f_rmse[k] = std::sqrt(c.f_sq_err_sum / n_d);
                f_pmean[k] = c.f_pred_sum_raw / n_d;
                f_rmean[k] = c.f_real_sum_raw / n_d;
            }
            if (c.g_smooth_err_n > 0) {
                g_smre[k]  = c.g_smooth_err_sum    / static_cast<double>(c.g_smooth_err_n);
                g_srmse[k] = std::sqrt(c.g_smooth_sq_err_sum / static_cast<double>(c.g_smooth_err_n));
            }
            if (c.f_smooth_err_n > 0) {
                f_smre[k]  = c.f_smooth_err_sum    / static_cast<double>(c.f_smooth_err_n);
                f_srmse[k] = std::sqrt(c.f_smooth_sq_err_sum / static_cast<double>(c.f_smooth_err_n));
            }
        }
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu evicted_in_ghost: %zu ghost_compacted_blocks: %lu gc_victim_avg_valid_ratio: %.6f gc_victim_count: %lu dummy_fill_segments: %lu ftl_host_pages: %lu ftl_nand_pages: %lu F_u: %.6f F_u_delta: %.6f G_u: %.6f G_u_delta: %.6f target_valid_rate: %.6f util_step: %.6f cand0_segs: %d cand0_g_n: %lu cand0_g_mre: %.6f cand0_g_rmse: %.6f cand0_g_sn: %lu cand0_g_smre: %.6f cand0_g_srmse: %.6f cand0_g_pmean: %.6f cand0_g_rmean: %.6f cand0_f_n: %lu cand0_f_mre: %.6f cand0_f_rmse: %.6f cand0_f_sn: %lu cand0_f_smre: %.6f cand0_f_srmse: %.6f cand0_f_pmean: %.6f cand0_f_rmean: %.6f cand1_segs: %d cand1_g_n: %lu cand1_g_mre: %.6f cand1_g_rmse: %.6f cand1_g_sn: %lu cand1_g_smre: %.6f cand1_g_srmse: %.6f cand1_g_pmean: %.6f cand1_g_rmean: %.6f cand1_f_n: %lu cand1_f_mre: %.6f cand1_f_rmse: %.6f cand1_f_sn: %lu cand1_f_smre: %.6f cand1_f_srmse: %.6f cand1_f_pmean: %.6f cand1_f_rmean: %.6f cand2_segs: %d cand2_g_n: %lu cand2_g_mre: %.6f cand2_g_rmse: %.6f cand2_g_sn: %lu cand2_g_smre: %.6f cand2_g_srmse: %.6f cand2_g_pmean: %.6f cand2_g_rmean: %.6f cand2_f_n: %lu cand2_f_mre: %.6f cand2_f_rmse: %.6f cand2_f_sn: %lu cand2_f_smre: %.6f cand2_f_srmse: %.6f cand2_f_pmean: %.6f cand2_f_rmean: %.6f\n",
                prefix_cstr, invalidate_blocks, compacted_blocks, global_valid_blocks, write_size_to_cache, evicted_blocks, write_hit_size, total_capacity_bytes, reinsert_blocks, read_blocks_in_partial_write, ghost_cache.evictCount(), ghost_compacted_blocks, avg_victim_valid_ratio, gc_victim_count, dummy_fill_segment_count, (uint64_t)ftl.GetHostWritePages(), (uint64_t)ftl.GetNandWritePages(), F_u, F_ud, G_u, G_ud, target_valid_blk_rate, util_step_,
                cand_segs[0], g_n[0], g_mre[0], g_rmse[0], g_sn[0], g_smre[0], g_srmse[0], g_pmean[0], g_rmean[0], f_n[0], f_mre[0], f_rmse[0], f_sn[0], f_smre[0], f_srmse[0], f_pmean[0], f_rmean[0],
                cand_segs[1], g_n[1], g_mre[1], g_rmse[1], g_sn[1], g_smre[1], g_srmse[1], g_pmean[1], g_rmean[1], f_n[1], f_mre[1], f_rmse[1], f_sn[1], f_smre[1], f_srmse[1], f_pmean[1], f_rmean[1],
                cand_segs[2], g_n[2], g_mre[2], g_rmse[2], g_sn[2], g_smre[2], g_srmse[2], g_pmean[2], g_rmean[2], f_n[2], f_mre[2], f_rmse[2], f_sn[2], f_smre[2], f_srmse[2], f_pmean[2], f_rmean[2]);
        fflush(fp_stats);
        next_written_bytes += written_window_bytes;
    }
}

void LogCache::take_inv_snapshot()
{
    inv_snapshot_taken_ = true;
    inv_snapshot_ts_ = log_cache_timestamp;

    size_t seg_idx = 0;
    for (auto& seg_ptr : all_segments) {
        LogCacheSegment* seg = seg_ptr.get();
        if (seg->write_ptr == 0) continue;
        if (seg->valid_cnt == 0) continue;

        uint64_t seg_age = log_cache_timestamp - seg->create_timestamp;
        double util = (double)seg->valid_cnt / seg->blocks.size();

        double sum = 0.0, sum_sq = 0.0;
        uint64_t n = 0;
        for (size_t i = 0; i < seg->write_ptr; i++) {
            if (!seg->blocks[i].valid) continue;
            double age = (double)(log_cache_timestamp - seg->blocks[i].create_timestamp);
            sum += age;
            sum_sq += age * age;
            n++;
        }
        double mean = 0.0, stddev = 0.0;
        if (n > 0) {
            mean = sum / n;
            if (n > 1) {
                double var = (sum_sq - sum * sum / n) / (n - 1);
                stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }

        inv_snap_segs_.push_back({seg_age, util, n, mean, stddev, seg->get_class_num()});
        inv_snap_inv_times_.push_back({});

        for (size_t i = 0; i < seg->write_ptr; i++) {
            if (!seg->blocks[i].valid) continue;
            inv_snap_block_seg_idx_[seg->blocks[i].key] = seg_idx;
        }
        seg_idx++;
    }

    printf("[InvSnapshot] taken at ts=%lu, write=%.1f TB, %zu segments, %zu blocks tracked\n",
           log_cache_timestamp, (double)write_size_to_cache / (1024.0*1024*1024*1024),
           inv_snap_segs_.size(), inv_snap_block_seg_idx_.size());
}

void LogCache::record_inv_time(long key)
{
    if (!inv_snapshot_taken_) return;
    auto it = inv_snap_block_seg_idx_.find(key);
    if (it != inv_snap_block_seg_idx_.end()) {
        uint64_t inv_time = log_cache_timestamp - inv_snapshot_ts_;
        inv_snap_inv_times_[it->second].push_back(inv_time);
        inv_snap_block_seg_idx_.erase(it);
    }
}

void LogCache::print_inv_time_scatter()
{
    if (!inv_snapshot_taken_ || inv_snap_segs_.empty()) return;

    char fname[256];
    const std::string& ts = start_ts();
    const std::string& prefix = stats_prefix();
    if (ts.empty()) {
        char ts_buf[64];
        time_t now = time(nullptr); struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        snprintf(fname, sizeof(fname), "inv_time_scatter.%s.csv", ts_buf);
    } else if (prefix.empty()) {
        snprintf(fname, sizeof(fname), "inv_time_scatter.%s.csv", ts.c_str());
    } else {
        snprintf(fname, sizeof(fname), "%s.inv_time_scatter.%s.csv", prefix.c_str(), ts.c_str());
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;

    fprintf(f, "seg_age,utilization,valid_count,age_mean,age_stddev,"
               "inv_time_mean,inv_time_stddev,inv_count,survived_count,class_num\n");

    uint64_t total_survived = 0;
    for (size_t i = 0; i < inv_snap_segs_.size(); i++) {
        auto& info = inv_snap_segs_[i];
        auto& times = inv_snap_inv_times_[i];

        uint64_t survived = info.valid_count - times.size();
        total_survived += survived;

        double inv_mean = 0.0, inv_stddev = 0.0;
        uint64_t n = times.size();
        if (n > 0) {
            double sum = 0.0, sum_sq = 0.0;
            for (auto t : times) {
                sum += (double)t;
                sum_sq += (double)t * t;
            }
            inv_mean = sum / n;
            if (n > 1) {
                double var = (sum_sq - sum * sum / n) / (n - 1);
                inv_stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }

        fprintf(f, "%lu,%.6f,%lu,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%d\n",
                info.seg_age, info.utilization, info.valid_count,
                info.age_mean, info.age_stddev,
                inv_mean, inv_stddev, n, survived, info.class_num);
    }

    fclose(f);
    printf("[InvTimeScatter] written to %s (%zu segments, %lu blocks survived)\n",
           fname, inv_snap_segs_.size(), total_survived);
}
