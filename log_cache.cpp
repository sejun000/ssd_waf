#include "log_cache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <list>
#include <set>

extern uint64_t interval;
extern double g_segment_blocks;
extern uint64_t g_threshold;
extern uint64_t g_timestamp;

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
             double valid_rate_max
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
      compaction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      compaction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_util_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_cache(cache_block_count * 0.1)
{
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
        valid_rate_period_blocks_ = static_cast<uint64_t>(valid_rate_period_gb_ * 1e9 / blk_sz);
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
                if (2.88 * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value()) > compaction_ratio.value()) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + 0.1);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.1);
                }
            }
        }
    }

    /* ── Periodic valid rate sweep ─────────────────────── */
    if (valid_rate_period_blocks_ > 0 &&
        log_cache_timestamp >= next_valid_rate_change_ts_) {
        std::uniform_real_distribution<double> dist(valid_rate_min_, valid_rate_max_);
        double old_rate = target_valid_blk_rate;
        target_valid_blk_rate = dist(valid_rate_rng_);
        next_valid_rate_change_ts_ += valid_rate_period_blocks_;
        printf("periodic_valid_rate: ts=%lu old=%.4f new=%.4f next_change=%lu\n",
               log_cache_timestamp, old_rate, target_valid_blk_rate,
               next_valid_rate_change_ts_);
    }
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
                printf("[CycleWrap] dummy_fill stream %d, seg=%p, write_ptr=%zu/%zu, valid_cnt=%ld, free_pool=%zu\n",
                       victim_id, (void*)vit->second, vit->second->write_ptr, vit->second->blocks.size(),
                       vit->second->valid_cnt, free_pool.size());
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
            }
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
            printf("[GC] event=RESET_EMPTY valid_cnt=0 free_pool=%zu global_valid=%lu ts=%lu\n",
                   free_pool.size(), global_valid_blocks, log_cache_timestamp);
            reset_segment(victim);
        }
        else if (compact == false) {
            printf("[GC] event=EVICT_SEG valid_cnt=%ld seg_age=%lu free_pool=%zu global_valid=%lu ts=%lu class=%d\n",
                   victim->valid_cnt, log_cache_timestamp - victim->create_timestamp,
                   free_pool.size(), global_valid_blocks, log_cache_timestamp, victim->get_class_num());
            evicted_segment_age = victim->get_create_time();
            evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
            evict_segment(victim);
            printf("[GC] event=EVICT_SEG_DONE free_pool=%zu global_valid=%lu\n",
                   free_pool.size(), global_valid_blocks);
        }
        else if (compact == true) {
            if (victim->valid_cnt > 0.95 * segment_size_blocks) {
                compact = false;
                compactor->add(victim, log_cache_timestamp);
                victim = (LogCacheSegment *)evictor->choose_segment();
                printf("[GC] event=COMPACT_TOO_FULL->EVICT_SEG valid_cnt=%ld seg_age=%lu free_pool=%zu global_valid=%lu ts=%lu class=%d\n",
                       victim->valid_cnt, log_cache_timestamp - victim->create_timestamp,
                       free_pool.size(), global_valid_blocks, log_cache_timestamp, victim->get_class_num());
                evicted_segment_age = victim->get_create_time();
                evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
                evict_segment(victim);
                printf("[GC] event=COMPACT_TOO_FULL->EVICT_SEG_DONE free_pool=%zu global_valid=%lu\n",
                       free_pool.size(), global_valid_blocks);
            }
            // Ghost compaction cost estimation
            else {
                /*if(is_ghost_cache && compactor) {
                double u_step = ghost_util_ratio.has_value() ? ghost_util_ratio.value() : 0.9;
                if (u_step < 0.01) u_step = 0.01; // 0 나누기 방지
                double extra_blocks = UTIL_STEP * total_cache_block_count;
                double m_segments = extra_blocks / (u_step * segment_size_blocks);

                auto segments = compactor->peek_top_segments((int)total_segments);
                double freed = 0;
                size_t k = 0;
                uint64_t current_valid = 0;
                for (auto* seg : segments) {
                    double u = (double)seg->valid_cnt / segment_size_blocks;
                    freed += (1.0 - u);
                    current_valid = seg->valid_cnt;
                    k++;
                    if (freed >= m_segments) break;
                }
                ghost_compacted_blocks += current_valid;
            }*/

                int stream_id = victim->get_class_num();
                printf("[GC] event=COMPACT valid_cnt=%ld seg_age=%lu threshold=%lu free_pool=%zu global_valid=%lu ts=%lu class=%d\n",
                       victim->valid_cnt, log_cache_timestamp - victim->create_timestamp,
                       threshold, free_pool.size(), global_valid_blocks, log_cache_timestamp, stream_id);
                compacted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
                last_target_seg = (LogCacheSegment *)evict_and_compaction(victim, threshold, stream_id);
                printf("[GC] event=COMPACT_DONE free_pool=%zu global_valid=%lu\n",
                       free_pool.size(), global_valid_blocks);
                if (last_target_seg) {
                    segment_list.push_back(last_target_seg);
                }
            }
        }
        else {
            printf("[GC] event=EVICT_SEG_FALLBACK valid_cnt=%ld seg_age=%lu free_pool=%zu global_valid=%lu ts=%lu class=%d\n",
                   victim->valid_cnt, log_cache_timestamp - victim->create_timestamp,
                   free_pool.size(), global_valid_blocks, log_cache_timestamp, victim->get_class_num());
            evicted_segment_age = victim->get_create_time();
            evicted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
            evict_segment(victim);
            printf("[GC] event=EVICT_SEG_FALLBACK_DONE free_pool=%zu global_valid=%lu\n",
                   free_pool.size(), global_valid_blocks);
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
        printf("[GC] event=DUMMY_FILL valid_cnt=%ld write_ptr=%zu/%zu free_pool=%zu global_valid=%lu class=%d\n",
               s->valid_cnt, s->write_ptr, s->blocks.size(), free_pool.size(), global_valid_blocks, s->get_class_num());
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
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu evicted_in_ghost: %zu ghost_compacted_blocks: %lu gc_victim_avg_valid_ratio: %.6f gc_victim_count: %lu\n",
                prefix_cstr, invalidate_blocks, compacted_blocks, global_valid_blocks, write_size_to_cache, evicted_blocks, write_hit_size, total_capacity_bytes, reinsert_blocks, read_blocks_in_partial_write, ghost_cache.evictCount(), ghost_compacted_blocks, avg_victim_valid_ratio, gc_victim_count);
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
