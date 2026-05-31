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
      age_ghost_cache(static_cast<std::size_t>(gs_decision_period_segs_)),
      net_free_seg_ratio_(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gc_valid_pages_ratio_(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      flush_avg_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      compact_avg_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      flush_pred_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      flush_ghost_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gg_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ff_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gf_flush_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      lambda_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
{
    periodic_ratio_ = periodic_ratio;
    segment_size_blocks = cfg_.segment_bytes / blk_sz;
    g_segment_blocks = static_cast<double>(segment_size_blocks);
    total_segments = cache_block_count * blk_sz / cfg_.segment_bytes;
    total_cache_block_count = total_segments * segment_size_blocks;
    init_age_prior(segment_size_blocks);   // 1 seg-life per bin
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
    print_age_prior();
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
            const auto& invblk = loc.seg->blocks[loc.idx];
            const uint64_t lifetime = log_cache_timestamp - invblk.create_timestamp;
            print_objects("invalidate", lifetime);
            record_lifetime(lifetime, true);
            // per-block invrate predictor (step 1-3):
            //   * record this lifetime under the LBA so the next write of the
            //     same LBA can stamp its block.update_interval with it.
            //   * if this block itself was a 1st-write (update_interval == 0),
            //     feed its observed lifetime into the global age-prior hist.
            lba_prev_lifetime_[key] = lifetime;
            if (invblk.update_interval == 0 && age_prior_bin_width_ > 0) {
                size_t bin = static_cast<size_t>(lifetime / age_prior_bin_width_);
                if (bin >= AGE_PRIOR_BINS) bin = AGE_PRIOR_BINS - 1;
                ++age_prior_inv_count_[bin];
                ++age_prior_total_first_writes_;
            }
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
            loc.seg->note_invalidation();   // per-seg cumulative inval count (++ only)
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
        static_cast<double>(total_segments);
    auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs,
                                                        log_cache_timestamp);
    // Capture per-class breakdown for gsdec logging (even when cum_invalid==0).
    for (int k = 0; k < 16; ++k) last_ghost_m_by_class_[k] = s.m_by_class[k];
    last_ghost_sum_inv_rate_    = s.sum_invalidate_rate;
    last_ghost_sum_inv_rate_lf_ = s.sum_inv_rate_last_fold;
    for (int k = 0; k < 4; ++k) {
        last_ghost_picked_ts_[k] = s.picked_create_ts[k];
        last_ghost_picked_u_[k]  = s.picked_u[k];
    }
    last_ghost_m_frac_ = s.m;
    last_ghost_resort_max_wt_ = s.max_picked_wt;   // 0 if RESORT off
    if (s.cum_invalid > 0.0) {
        ghost_compacted_blocks_sum_ += s.cum_valid;
        ghost_sum_initialized_       = true;
        last_ghost_sum_ts_           = log_cache_timestamp;
        last_invalidate_at_comp_     = invalidate_blocks;
    }
}

LogCache::GcNetFreeSim LogCache::simulate_gc_net_free() const
{
    GcNetFreeSim r;
    const std::size_t seg_blocks = segment_size_blocks;
    if (!compactor || seg_blocks == 0) return r;

    // Per-stream simulated GC active segment: remaining free slots + running WT.
    // Seeded from the live gc_active_seg so each one's current write_ptr counts
    // (a victim whose valid pages fit in the leftover space allocates nothing).
    struct SimSeg { std::size_t remaining; uint64_t wt; };
    std::unordered_map<int, SimSeg> sim;
    sim.reserve(gc_active_seg.size() * 2 + 8);
    for (const auto& kv : gc_active_seg) {
        LogCacheSegment* s   = kv.second;
        const std::size_t used = s->write_ptr;
        const std::size_t rem  = (seg_blocks > used) ? (seg_blocks - used) : 0;
        sim[kv.first] = SimSeg{ rem, s->create_timestamp };
    }

    int      victims       = 0;
    int      new_allocs    = 0;
    uint64_t min_victim_wt = UINT64_MAX;
    uint64_t active_min_wt = UINT64_MAX;

    compactor->for_each_victim_in_order([&](Segment* base) -> bool {
        LogCacheSegment* v = static_cast<LogCacheSegment*>(base);
        ++victims;
        if (v->create_timestamp < min_victim_wt) min_victim_wt = v->create_timestamp;

        // Relocate every valid block, routing by its own create_timestamp.
        uint64_t this_victim_seg_wt = UINT64_MAX;  // min WT of seg(s) THIS victim lands in
        for (std::size_t i = 0; i < v->blocks.size(); ++i) {
            const auto& blk = v->blocks[i];
            if (!blk.valid) continue;
            int sid = stream_policy
                ? stream_policy->ClassifyReadOnly(blk.key, /*isGcAppend=*/true,
                                                  log_cache_timestamp, blk.create_timestamp)
                : -1;
            if (sid < 0) sid = Segment::GC_STREAM_START;   // fallback: single GC stream

            auto it = sim.find(sid);
            if (it == sim.end()) {
                // No live active seg for this stream → first GC write allocates one.
                ++new_allocs;
                it = sim.emplace(sid, SimSeg{ seg_blocks, log_cache_timestamp }).first;
            } else if (it->second.remaining == 0) {
                // Active seg full → allocate a fresh one for this stream.
                ++new_allocs;
                it->second = SimSeg{ seg_blocks, log_cache_timestamp };
            }
            SimSeg& ss = it->second;
            --ss.remaining;
            ++r.cum_valid;  // exact valid blocks routed in this sim
            if (blk.create_timestamp < ss.wt) ss.wt = blk.create_timestamp;  // oldest-WT rule
            if (ss.wt < this_victim_seg_wt) this_victim_seg_wt = ss.wt;
            if (ss.wt < active_min_wt)       active_min_wt      = ss.wt;
        }

        // Victim segment is reclaimed (+1 free); allocations consumed free segs.
        // net_free rises by at most 1 per victim, so the first time it reaches 1
        // it is exactly 1 — that victim is the boundary.
        if (victims - new_allocs >= 1) {
            r.reached            = true;
            r.victims            = victims;
            r.new_allocs         = new_allocs;
            r.boundary_victim_wt = v->create_timestamp;
            r.boundary_seg_wt    = this_victim_seg_wt;
            r.min_victim_wt      = min_victim_wt;
            r.active_seg_min_wt  = active_min_wt;
            return false;                            // stop scanning
        }
        return true;
    });

    if (!r.reached) {            // heap exhausted before reaching net_free==1
        r.victims           = victims;
        r.new_allocs        = new_allocs;
        r.min_victim_wt     = min_victim_wt;
        r.active_seg_min_wt = active_min_wt;
    }
    return r;
}

double LogCache::sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const
{
    double sum = 0.0;
    if (!evictor) return sum;
    // evictor (score_age_evict = -create_timestamp, max-heap) hands out segments
    // in WT-ascending order → once we pass wt_hi every later seg is also out.
    // No lower bound: accumulate every resident seg up to victim v (wt_hi).
    evictor->for_each_victim_in_order([&](Segment* s) -> bool {
        if (s->get_create_time() > wt_hi) return false;   // ascending → done
        sum += s->invalidate_rate();
        return true;
    });
    return sum;
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
    // Final variant: ghost_sum += cum_valid (tick), LHS = r·waf·F(u)·δ·N, RHS = Gud.
    //   * No dt × rate extrapolation (drops sustained-rate assumption).
    //   * LHS uses F(u)·δ·N: D segments 살리면 그 안에 들어있던 자연 flush valid 양만
    //     cache 에 남는다 (= F(u) · D · seg_blocks pages, host write 당 = F(u)·D).
    //   * Gud = G(u+δ) per host write (cum_valid is prediction-only marginal cost).
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % segment_size_blocks == 0) {
        update_ghost_compacted_blocks_sum_cum();
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
        // Avg valid fraction per real flush event (D-symmetric with GC cum_valid).
        // Normalize: flush_event_count_ → pages by × seg_blocks so both numerator
        // and denominator are in pages → EWMA value = fraction (0~1).
        flush_avg_ratio.updateFromCumulative(
            flush_event_count_ * segment_size_blocks, evicted_blocks);
        // compact_avg = per-NET-FREE-segment valid copy. denom = victims·seg −
        //   compacted_blocks = (segs GC reclaimed − segs GC re-allocated)·seg =
        //   NET segments freed by GC ·seg. value = u/(1−u), unit-matched to the
        //   ghost_sum per-free prediction. (Was per-victim u, which ignored that
        //   GC re-writes valid into fresh segs → undercounted real free cost.)
        compact_avg_ratio.updateFromCumulative(
            compact_event_count_ * segment_size_blocks - compacted_blocks, compacted_blocks);
        // λ: device blocks invalidated per host-write page. invalidate_blocks =
        //   cumulative host-overwrite/trim invalidations; log_cache_timestamp =
        //   cumulative host-write pages. EWMA over the moving_avg window → recent
        //   per-page invalidation rate (×seg = blocks invalidated per host-seg).
        lambda_ratio.updateFromCumulative(log_cache_timestamp, invalidate_blocks);
        // F_inv: rate at which ghost-resident pages die, measured AT POP time.
        //   totalPush − totalPop = Σ_popped(push_valid − pop_valid) + resident
        //   push. push_valid−pop_valid = pages a segment lost while resident in
        //   the ghost window (= initial − pop-time valid), which is what we want
        //   (vs totalInitial−totalValid, a progress snapshot biased by just-
        //   pushed segments). Fed STRAIGHT into updateFromCumulative (a
        //   derivative) → per-host-write death rate. NOT pre-accumulated — that
        //   double-integrates and diverges (seen before).
        flush_pred_ratio.updateFromCumulative(
            log_cache_timestamp,
            age_ghost_cache.totalPushValidCount() -
            age_ghost_cache.totalPopValidCount());
        // F_ghost: sample age_ghost_cache.totalValidCount() every (seg/4) and
        // accumulate. EWMA(cum/ts) ≈ avg valid pages held in D resident segs
        // per host write.
        ghost_seg_valid_sum_ += static_cast<double>(age_ghost_cache.totalValidCount());
        flush_ghost_ratio.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_seg_valid_sum_));
        // 2-step lookahead candidates. δN = util_step_·N segments freed per step.
        //   GG: GC frees 2δN  → ghost_sum(2δN).cum_valid (valid copied).
        //   FF: flush frees 2δN → get_mth(2δN) (valid flushed = top-2δN victims).
        //   GF: GC frees δN (= Gud, already accumulated) + flush frees δN.
        // Each is a cumulative valid-page sum fed to updateFromCumulative → EWMA
        // per-host-write rate, same scale as Gud/flush_ghost_ratio.
        const double dN = util_step_ * static_cast<double>(total_segments);
        if (compactor) {
            gg_ghost_sum_ +=
                compactor->get_ghost_sum_for_free_segments(2.0 * dN).cum_valid;
            gg_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(gg_ghost_sum_));
        }
        if (evictor) {
            ff_flush_sum_ +=
                static_cast<double>(evictor->get_mth_score_valid_pages(2.0 * dN));
            ff_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(ff_flush_sum_));
            gf_flush_sum_ +=
                static_cast<double>(evictor->get_mth_score_valid_pages(dN));
            gf_flush_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(gf_flush_sum_));
        }
        // Per-segment invalidation rate: fold each resident (sealed) segment's
        // cumulative count at this regular cadence — same updateFromCumulative
        // pattern as the ratios above, so invrate_sum reads a recency-weighted
        // rate while the per-event path stays a bare ++. ~O(sealed segs) per tick.
        if (evictor) {
            evictor->for_each_victim_in_order([this](Segment* s) -> bool {
                s->fold_invalidate_rate(log_cache_timestamp);
                return true;
            });
        }
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
            // F_pred_rate now = per-host-write rate at which ghost-resident
            //   pages get invalidated (totalPush − totalPop derivative).
            const double F_pred_rate  = flush_pred_ratio.has_value()
                                      ? flush_pred_ratio.value() : 0.0;
            const double F_ghost_rate = flush_ghost_ratio.has_value()
                                      ? flush_ghost_ratio.value() : 0.0;
            (void)F_ghost_rate;  // kept for logging / easy −F_ghost restore
            (void)F_pred_rate;   // kept for logging (col f_frac_pred)
            // ── invrate-rule: GC if its copy cost beats the natural-death credit ──
            //   invrate_sum = Σ per-seg invalidate_rate over resident segs with
            //   WT ≤ v, where victim v = newest seg GC must touch to free
            //   util_step_·N. Each invalidate_rate ≈ invalidated pages per
            //   host-write page on that seg → the sum is the host-page death rate
            //   of the old cohort GC would reclaim. Routing that reclaim through
            //   cold-tier flush costs invrate_sum·r writes; GC instead copies Gud
            //   valid pages. → GC (RAISE) iff Gud < invrate_sum·r, else flush.
            //   Computed every tick (cheap heap scans); gcsim below stays log-only.
            const double vic_target_free =
                util_step_ * static_cast<double>(total_segments);
            EvictPolicy::VictimWtSpanResult vspan;
            double invrate_sum = 0.0;
            if (compactor) {
                vspan = compactor->get_victim_wt_span_for_free_segments(vic_target_free);
                // RESORT-aware: use the corrected-pick-set's max wt when active
                // (last_ghost_resort_max_wt_ > 0), else fall back to vspan.max_wt
                // computed from the original-score iteration.
                const uint64_t wt_hi = (last_ghost_resort_max_wt_ > 0)
                                       ? last_ghost_resort_max_wt_ : vspan.max_wt;
                invrate_sum = sum_invalidate_rate_in_wt_range(wt_hi);
                // Subtract the picked victims' own inv_rates: those segments
                // will be GC'd (cost already in G_ud); only the NON-picked segs
                // in the wt range forecast the LF-vs-GS eviction-saving.
                invrate_sum -= last_ghost_sum_inv_rate_;
                if (invrate_sum < 0.0) invrate_sum = 0.0;
            }
            const double lambda = lambda_ratio.has_value() ? lambda_ratio.value() : 0.0;
            (void)lambda;  // kept for the `lambda` log column only (no longer in the rule)
            const double lhs    = Gud;                           // GC copy cost / host page
            const double rhs    = invrate_sum * periodic_ratio_; // invrate·r / host page
            bool         raise  = (lhs < rhs);
            // Marginal cols kept for logging only (not used by the λ-rule).
            const double GG    = gg_ratio.has_value()       ? gg_ratio.value()       : 0.0;
            const double FFv   = ff_ratio.has_value()       ? ff_ratio.value()       : 0.0;
            const double Fv    = gf_flush_ratio.has_value() ? gf_flush_ratio.value() : 0.0;
            const double Gnext = GG  - Gud;
            const double Fnext = FFv - Fv;
            // (anti-stuck cap removed: decision is now purely Gud < invrate_sum·r,
            //  no forced flush after N consecutive RAISEs.)
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

            const double compact_avg = compact_avg_ratio.has_value()
                                     ? compact_avg_ratio.value() : 0.0;
            const double flush_avg   = flush_avg_ratio.has_value()
                                     ? flush_avg_ratio.value()   : 0.0;
            if (!g_gs_dec_init) {
                g_gs_dec_init = true;
                const char* path = std::getenv("GS_DECISION_LOG");
                if (path && *path) {
                    g_gs_dec_fp = std::fopen(path, "w");
                    if (g_gs_dec_fp) {
                        std::fprintf(g_gs_dec_fp,
                            "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision "
                            "cur_util tgt_before tgt_after hard_limit low_floor "
                            "comp_cum evict_cum ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid "
                            "compact_avg flush_avg compact_evt ghost_compact_sum f_frac_pred GGr FFr Fr Gnext Fnext lambda "
                            "gcsim_reached gcsim_m gcsim_alloc gcsim_vic_wt gcsim_seg_wt gcsim_minvic_wt gcsim_actmin_wt "
                            "vic_v_wt invrate_sum "
                            "invpred_pred_full invpred_pred_surv invpred_actual invpred_nseg invpred_nsurv "
                            "raw_gud raw_compact flush_evt full_inv_reset gcsim_cum_valid "
                            "gud_m_c0 gud_m_c1 gud_m_c10 gud_m_c11 gud_m_c12 gud_m_c13 gud_m_c14 "
                            "gud_sum_inv_rate gud_sum_inv_rate_lf "
                            "real_inv_rate real_inv_rate_lf "
                            "real_age_mean_segs real_u_mean real_fresh_frac real_fresh_u_mean "
                            "g_vid0 g_vid1 g_vid2 g_vid3 r_vid0 r_vid1 r_vid2 r_vid3 "
                            "g_u0 g_u1 g_u2 g_u3 r_u0 r_u1 r_u2 r_u3 "
                            "g_m_frac\n");
                    }
                }
            }
            if (g_gs_dec_fp) {
                // What-if GC net-free sim — run only when the decision log is on
                // (block-level victim scan is not free; keeps normal runs intact).
                const GcNetFreeSim gcsim = simulate_gc_net_free();
                auto wt_or_0 = [](uint64_t w) -> uint64_t {
                    return (w == UINT64_MAX) ? 0UL : w;
                };
                // ── invalidate-rate signal validation: predicted (last snapshot) vs
                //    actual invalidations measured over the SAME remembered cohort ──
                double invpred_pred_full = 0.0, invpred_pred_surv = 0.0, invpred_actual = 0.0;
                uint64_t invpred_nseg = 0, invpred_nsurv = 0;
                if (evictor) {
                    // (1) MEASURE: predicted vs actual on last snapshot's survivors.
                    if (!invpred_snap_.empty() && log_cache_timestamp > invpred_snap_ts_) {
                        const double dts = static_cast<double>(log_cache_timestamp - invpred_snap_ts_);
                        invpred_pred_full = invpred_rate_sum_ * dts;       // full prev cohort (incl. departed)
                        invpred_nseg      = invpred_snap_.size();
                        evictor->for_each_victim_in_order([&](Segment* s) -> bool {
                            if (s->get_create_time() > invpred_snap_maxwt_) return false; // WT asc → past cohort
                            auto it = invpred_snap_.find(s);
                            if (it != invpred_snap_.end() && it->second.wt == s->get_create_time()) {
                                invpred_pred_surv += it->second.rate * dts;
                                invpred_actual    += static_cast<double>(s->invalidate_count_ - it->second.count);
                                ++invpred_nsurv;
                            }
                            return true;
                        });
                    }
                    // (2) SNAPSHOT current cohort (WT ≤ vspan.max_wt) for the next tick.
                    invpred_snap_.clear();
                    evictor->for_each_victim_in_order([&](Segment* s) -> bool {
                        if (s->get_create_time() > vspan.max_wt) return false;            // WT asc → stop at v
                        invpred_snap_[s] = InvPredSnap{ s->invalidate_count_,
                                                        s->get_create_time(),
                                                        s->invalidate_rate() };
                        return true;
                    });
                    invpred_snap_ts_    = log_cache_timestamp;
                    invpred_snap_maxwt_ = vspan.max_wt;
                    invpred_rate_sum_   = invrate_sum;
                }
                // ── raw (non-EWMA) Gud vs compact: direct first-differences ──
                double raw_gud = 0.0, raw_compact = 0.0;
                if (rawval_prev_ts_ != 0 && log_cache_timestamp > rawval_prev_ts_) {
                    const double dts   = static_cast<double>(log_cache_timestamp - rawval_prev_ts_);
                    const double dgs   = ghost_compacted_blocks_sum_ - rawval_prev_ghostsum_;
                    const double dcomp = static_cast<double>(compacted_blocks - rawval_prev_comp_);
                    const double dnetfree =
                        static_cast<double>(compact_event_count_ - rawval_prev_compevt_)
                        * static_cast<double>(segment_size_blocks) - dcomp;
                    if (dts > 0.0)      raw_gud     = dgs / dts;             // valid-copy per host page (ghost)
                    if (dnetfree > 0.0) raw_compact = dcomp / dnetfree;      // valid-copy per net-freed page
                }
                rawval_prev_ts_       = log_cache_timestamp;
                rawval_prev_ghostsum_ = ghost_compacted_blocks_sum_;
                rawval_prev_comp_     = compacted_blocks;
                rawval_prev_compevt_  = compact_event_count_;

                // Mean inv_rate per REAL compacted victim during this gsdec period.
                // Comparable to last_ghost_sum_inv_rate_/m (ghost-picked mean).
                double real_inv_rate = 0.0, real_inv_rate_lf = 0.0;
                const uint64_t dcomp_real =
                    real_victim_compact_count_ - rawval_prev_real_compact_;
                if (dcomp_real > 0) {
                    real_inv_rate    = (real_victim_inv_rate_cum_    - rawval_prev_real_inv_rate_)    / static_cast<double>(dcomp_real);
                    real_inv_rate_lf = (real_victim_inv_rate_lf_cum_ - rawval_prev_real_inv_rate_lf_) / static_cast<double>(dcomp_real);
                }
                rawval_prev_real_inv_rate_    = real_victim_inv_rate_cum_;
                rawval_prev_real_inv_rate_lf_ = real_victim_inv_rate_lf_cum_;
                rawval_prev_real_compact_     = real_victim_compact_count_;

                // Real-victim age/u distribution (per-period means).
                //   real_age_mean_segs = mean age (segments) of real victims
                //   real_u_mean        = mean u of real victims
                //   real_fresh_frac    = fraction with age < 1 seg
                //   real_fresh_u_mean  = mean u of fresh victims (NaN→0 if none)
                double real_age_mean_segs = 0.0;
                double real_u_mean        = 0.0;
                double real_fresh_frac    = 0.0;
                double real_fresh_u_mean  = 0.0;
                if (dcomp_real > 0) {
                    const uint64_t dage    = real_victim_age_sum_     - rawval_prev_real_age_sum_;
                    const double   du      = real_victim_u_sum_       - rawval_prev_real_u_sum_;
                    const uint64_t dfresh  = real_victim_fresh_count_ - rawval_prev_real_fresh_count_;
                    const double   dfreshu = real_victim_fresh_u_sum_ - rawval_prev_real_fresh_u_sum_;
                    real_age_mean_segs = static_cast<double>(dage) /
                                         static_cast<double>(segment_size_blocks) /
                                         static_cast<double>(dcomp_real);
                    real_u_mean      = du / static_cast<double>(dcomp_real);
                    real_fresh_frac  = static_cast<double>(dfresh) / static_cast<double>(dcomp_real);
                    real_fresh_u_mean = (dfresh > 0) ? (dfreshu / static_cast<double>(dfresh)) : 0.0;
                }
                rawval_prev_real_age_sum_     = real_victim_age_sum_;
                rawval_prev_real_u_sum_       = real_victim_u_sum_;
                rawval_prev_real_fresh_count_ = real_victim_fresh_count_;
                rawval_prev_real_fresh_u_sum_ = real_victim_fresh_u_sum_;

                // Snapshot real_picked_ts_/u_ for this print, then reset for next period.
                int64_t real_pick_log[4] = {
                    real_picked_ts_[0], real_picked_ts_[1],
                    real_picked_ts_[2], real_picked_ts_[3] };
                double  real_pick_u_log[4] = {
                    real_picked_u_[0], real_picked_u_[1],
                    real_picked_u_[2], real_picked_u_[3] };
                real_picked_ts_[0] = real_picked_ts_[1] =
                real_picked_ts_[2] = real_picked_ts_[3] = -1;
                real_picked_u_[0] = real_picked_u_[1] =
                real_picked_u_[2] = real_picked_u_[3] = 0.0;
                real_picked_idx_ = 0;

                // vspan / invrate_sum already computed above (now drive the rule).
                std::fprintf(g_gs_dec_fp,
                    "%lu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s "
                    "%.6f %.6f %.6f %d %d %lu %lu %lu %lu %lu %lu "
                    "%.6f %.6f %lu %.0f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                    "%d %d %d %lu %lu %lu %lu "
                    "%lu %.6f %.6f %.6f %.6f %lu %lu "
                    "%.6f %.6f %lu %lu %lu "
                    "%.4f %.4f %.4f %.4f %.4f %.4f %.4f "
                    "%.6f %.6f "
                    "%.6f %.6f "
                    "%.4f %.6f %.4f %.6f "
                    "%ld %ld %ld %ld %ld %ld %ld %ld "
                    "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
                    "%.6f\n",
                    log_cache_timestamp, gs_decision_period_segs_,
                    periodic_ratio_, waf_w, Gu, Fu, Gud, Fud, lhs, rhs,
                    raise ? "RAISE" : "LOWER",
                    cur_util, prev_target, target_valid_blk_rate,
                    hard_hit ? 1 : 0, low_hit ? 1 : 0,
                    compacted_blocks, evicted_blocks,
                    g_ex_low_target_count, g_ex_target_satisfied_count,
                    g_ex_force_flush_count, g_ex_high_valid_victim_count,
                    compact_avg, flush_avg,
                    compact_event_count_, ghost_compacted_blocks_sum_,
                    F_pred_rate, GG, FFv, Fv, Gnext, Fnext, lambda,
                    gcsim.reached ? 1 : 0, gcsim.victims, gcsim.new_allocs,
                    gcsim.boundary_victim_wt, wt_or_0(gcsim.boundary_seg_wt),
                    wt_or_0(gcsim.min_victim_wt), wt_or_0(gcsim.active_seg_min_wt),
                    vspan.max_wt, invrate_sum,
                    invpred_pred_full, invpred_pred_surv, invpred_actual,
                    invpred_nseg, invpred_nsurv,
                    raw_gud, raw_compact, flush_event_count_, full_invalid_reset_count_,
                    gcsim.cum_valid,
                    last_ghost_m_by_class_[0],  last_ghost_m_by_class_[1],
                    last_ghost_m_by_class_[10], last_ghost_m_by_class_[11],
                    last_ghost_m_by_class_[12], last_ghost_m_by_class_[13],
                    last_ghost_m_by_class_[14],
                    last_ghost_sum_inv_rate_, last_ghost_sum_inv_rate_lf_,
                    real_inv_rate, real_inv_rate_lf,
                    real_age_mean_segs, real_u_mean, real_fresh_frac, real_fresh_u_mean,
                    (long)last_ghost_picked_ts_[0], (long)last_ghost_picked_ts_[1],
                    (long)last_ghost_picked_ts_[2], (long)last_ghost_picked_ts_[3],
                    (long)real_pick_log[0], (long)real_pick_log[1],
                    (long)real_pick_log[2], (long)real_pick_log[3],
                    last_ghost_picked_u_[0], last_ghost_picked_u_[1],
                    last_ghost_picked_u_[2], last_ghost_picked_u_[3],
                    real_pick_u_log[0], real_pick_u_log[1],
                    real_pick_u_log[2], real_pick_u_log[3],
                    last_ghost_m_frac_);
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
        if (is_ghost_cache) {
            age_ghost_cache.invalidate(key);
        }
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

        // Per-block update_interval (step 1-3): stamp the new block with the
        // lifetime of the previous version of this LBA, if recorded. 0 ⇒ 1st-
        // write (LBA never seen, or last seen as evict).  invalidate() above
        // is what populates lba_prev_lifetime_ — so we look up AFTER it.
        uint64_t upd_int = 0;
        {
            auto it = lba_prev_lifetime_.find(key);
            if (it != lba_prev_lifetime_.end()) {
                upd_int = it->second;
                lba_prev_lifetime_.erase(it);
            }
        }
        seg->blocks[seg->write_ptr] = { key, true, log_cache_timestamp, upd_int };
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
    // Proactive top-up: keep evicting until free_pool has grown by `max_victims`
    // NET free segments. Capture the baseline BEFORE the loop and exit once
    // free_pool reaches baseline + max_victims — not after `max_victims` victims,
    // because a compaction victim can net 0 free segments (it allocates a target
    // while freeing the victim). Every victim is freed synchronously via
    // reset_segment() (flush/reset → +1, compaction → 0/+1) and >0.95-util victims
    // are force-flushed, so the goal is reached; `processed < total_segments` is a
    // safety backstop against a non-terminating loop.
    const std::size_t free_goal =
        free_pool.size() + (max_victims > 0 ? static_cast<std::size_t>(max_victims) : 0);
    int processed = 0;
    while (free_pool.size() <= 3 ||
           (max_victims > 0 && free_pool.size() < free_goal &&
            processed < static_cast<int>(total_segments)))
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
            ++full_invalid_reset_count_;
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
                // Track REAL victim's inv_rate (EWMA + raw last-fold) for ghost
                // predictor validation. Mirrors what ghost reports in
                // last_ghost_sum_inv_rate_/_lf_ — but for segments that actually
                // get compacted (post-K_VAL choice). Diff in gsdec print gives
                // mean inv_rate per real victim during the period.
                {
                    real_victim_inv_rate_cum_ += victim->invalidate_rate();
                    // Raw rate over the JUST-CLOSED 1-seg window [seg_ago_ts,
                    // prev_ts]. This matches the ghost-side window exactly: at
                    // the boundary ghost reads [prev_ts, now] which is the
                    // same closed segment. real-side must NOT use partial
                    // [prev_ts, log_cache_timestamp] (variable 0~1 seg).
                    double inv_rate_lf = 0.0;
                    if (victim->invalidate_seg_ago_ts_ > 0 &&
                        victim->invalidate_prev_ts_ > victim->invalidate_seg_ago_ts_) {
                        const uint64_t dU = victim->invalidate_prev_count_ - victim->invalidate_seg_ago_count_;
                        const uint64_t dH = victim->invalidate_prev_ts_ - victim->invalidate_seg_ago_ts_;
                        inv_rate_lf = static_cast<double>(dU) / static_cast<double>(dH);
                    }
                    real_victim_inv_rate_lf_cum_ += inv_rate_lf;
                    ++real_victim_compact_count_;
                    // Age/u distribution — test fresh-victim hypothesis (user):
                    //   host active = 2 segs, so each 1-seg period seals ~1 new
                    //   segment. If real victims' age < 1 seg with low u, those
                    //   weren't visible to ghost at predict time → over-pred.
                    const uint64_t age =
                        log_cache_timestamp - victim->create_timestamp;
                    const double u = static_cast<double>(victim->valid_cnt) /
                                     static_cast<double>(segment_size_blocks);
                    real_victim_age_sum_ += age;
                    real_victim_u_sum_   += u;
                    if (age < segment_size_blocks) {
                        ++real_victim_fresh_count_;
                        real_victim_fresh_u_sum_ += u;
                    }
                    // Record create_timestamp of first 4 real victims this period
                    // (since last gsdec print). Compare against last_ghost_picked_ts_.
                    if (real_picked_idx_ < 4) {
                        real_picked_ts_[real_picked_idx_] =
                            static_cast<int64_t>(victim->create_timestamp);
                        real_picked_u_[real_picked_idx_] = u;   // directly measured
                        ++real_picked_idx_;
                    }
                }
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
                ++compact_event_count_;
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
    std::vector<uint64_t> flushed_keys;
    flushed_keys.reserve(s->blocks.size());
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (threshold > 0 && log_cache_timestamp - blk.create_timestamp >= threshold) {
            if (is_ghost_cache) {
                ghost_cache.push(blk.key);
                flushed_keys.push_back(blk.key);
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
    if (is_ghost_cache && !flushed_keys.empty()) {
        age_ghost_cache.pushSegment(flushed_keys);
    }
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
    std::vector<uint64_t> flushed_keys;
    flushed_keys.reserve(s->blocks.size());
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (is_ghost_cache) {
            ghost_cache.push(blk.key);
            flushed_keys.push_back(blk.key);
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
    if (is_ghost_cache && !flushed_keys.empty()) {
        age_ghost_cache.pushSegment(flushed_keys);
    }
    evicted_blocks_histogram->inc(evicted_blocks_for_victim);
    ++flush_event_count_;
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
            auto* other_seg = it->second.seg;
            auto &other_blk = other_seg->blocks[it->second.idx];
            other_blk.valid = false;
            --other_seg->valid_cnt;   // collateral seg 의 valid_cnt 도 일관성 위해 깎음.
            // note_invalidation() 은 호출 안 함 — cold-tier eviction 은 GC-driven 이라
            // host overwrite (invalidate_count_) 의미와 다름.
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

void LogCache::init_age_prior(uint64_t bin_width)
{
    age_prior_bin_width_ = bin_width > 0 ? bin_width : 1;
    age_prior_inv_count_.assign(AGE_PRIOR_BINS, 0);
    age_prior_total_first_writes_ = 0;
}

void LogCache::print_age_prior()
{
    if (age_prior_total_first_writes_ == 0) return;
    FILE* f = fopen("age_prior_histogram.csv", "w");
    if (!f) return;
    fprintf(f, "bin_idx,age_low,age_high,inv_count,frac\n");
    for (size_t b = 0; b < AGE_PRIOR_BINS; ++b) {
        const uint64_t lo = b * age_prior_bin_width_;
        const uint64_t hi = (b + 1) * age_prior_bin_width_;
        const double frac = static_cast<double>(age_prior_inv_count_[b])
                          / static_cast<double>(age_prior_total_first_writes_);
        fprintf(f, "%zu,%lu,%lu,%lu,%.6f\n",
                b, lo, hi, age_prior_inv_count_[b], frac);
    }
    fclose(f);
    printf("[AgePrior] %zu bins (width=%lu ticks/seg) total 1st-write invs=%lu → age_prior_histogram.csv\n",
           AGE_PRIOR_BINS, age_prior_bin_width_, age_prior_total_first_writes_);
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
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu evicted_in_ghost: %zu ghost_compacted_blocks: %lu gc_victim_avg_valid_ratio: %.6f gc_victim_count: %lu dummy_fill_segments: %lu ftl_host_pages: %lu ftl_nand_pages: %lu F_u: %.6f F_u_delta: %.6f G_u: %.6f G_u_delta: %.6f target_valid_rate: %.6f util_step: %.6f cand0_segs: %d cand0_g_n: %lu cand0_g_mre: %.6f cand0_g_rmse: %.6f cand0_g_sn: %lu cand0_g_smre: %.6f cand0_g_srmse: %.6f cand0_g_pmean: %.6f cand0_g_rmean: %.6f cand0_f_n: %lu cand0_f_mre: %.6f cand0_f_rmse: %.6f cand0_f_sn: %lu cand0_f_smre: %.6f cand0_f_srmse: %.6f cand0_f_pmean: %.6f cand0_f_rmean: %.6f cand1_segs: %d cand1_g_n: %lu cand1_g_mre: %.6f cand1_g_rmse: %.6f cand1_g_sn: %lu cand1_g_smre: %.6f cand1_g_srmse: %.6f cand1_g_pmean: %.6f cand1_g_rmean: %.6f cand1_f_n: %lu cand1_f_mre: %.6f cand1_f_rmse: %.6f cand1_f_sn: %lu cand1_f_smre: %.6f cand1_f_srmse: %.6f cand1_f_pmean: %.6f cand1_f_rmean: %.6f cand2_segs: %d cand2_g_n: %lu cand2_g_mre: %.6f cand2_g_rmse: %.6f cand2_g_sn: %lu cand2_g_smre: %.6f cand2_g_srmse: %.6f cand2_g_pmean: %.6f cand2_g_rmean: %.6f cand2_f_n: %lu cand2_f_mre: %.6f cand2_f_rmse: %.6f cand2_f_sn: %lu cand2_f_smre: %.6f cand2_f_srmse: %.6f cand2_f_pmean: %.6f cand2_f_rmean: %.6f ghost_compact_sum: %.0f\n",
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
