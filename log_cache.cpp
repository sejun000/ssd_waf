#include "log_cache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <list>

extern int interval;
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
             std::string stat_log_file
             )
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : Config{}),
      evictor(std::move(ev)),
      cache_trace_(cache_trace),
      target_valid_blk_rate(input_target_valid_blk_rate),
      valid_blk_rate_hard_limit(0.93),
      compactor(std::move(cp)),
      additional_free_blks_ratio_by_gc(input_additional_free_blks_ratio_by_gc),
      evicted_ages_histogram(std::make_unique<Histogram>("evicted_ages", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_blocks_histogram(std::make_unique<Histogram>("evicted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      compacted_blocks_histogram(std::make_unique<Histogram>("compacted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      evicted_ages_with_segment_histogram(std::make_unique<Histogram>("evicted_ages_with_segment", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      compacted_ages_with_segment_histogram(std::make_unique<Histogram>("compacted_ages_with_segment", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_cache_blocks_per_evict(std::make_unique<Histogram>("evicted_cache_blocks_per_evict", 1, 100, fp_stats)),
      is_ghost_cache(input_ghost_cache),
      compaction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_cache(cache_block_count * 0.1)
{
    segment_size_blocks = cfg_.segment_bytes / blk_sz;
    total_segments = cache_block_count * blk_sz / cfg_.segment_bytes;
    total_cache_block_count = cache_block_count;
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
            invalidate_blocks += 1;
            loc.seg->blocks[loc.idx].valid = false;
            --loc.seg->valid_cnt;
            global_valid_blocks -= 1;
            if (loc.seg->full()){
                evict_policy_update(loc.seg);
            }
        }
       // else{
        //    assert(false);
       // }
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
        if (log_cache_timestamp % segment_size_blocks == 0) {
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
        }
        if (log_cache_timestamp % (segment_size_blocks * 64) == 0) {
            if (compaction_ratio.has_value() &&
                eviction_ratio.has_value() && 
                eviction_ratio_in_ghost_cache.has_value()){
                if (6.73 * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value()) > compaction_ratio.value()) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + 0.02);
                    /*printf("rise cache !!!!!!!! %.2f\n", target_valid_blk_rate);
                    printf ("eviction value : %.6f\n", 2.34 * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value()));
                    printf ("compaction value : %.6f\n", compaction_ratio.value());
                    printf ("eviction_ratio : %.6f\n", eviction_ratio.value());
                    printf ("eviction_ratio_in_ghost_cache : %.6f\n", eviction_ratio_in_ghost_cache.value());*/
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    /*printf("lower cache !!!!!!!! %.2f\n", target_valid_blk_rate);
                    printf ("eviction value : %.6f\n", 2.34 * (eviction_ratio.value() - eviction_ratio_in_ghost_cache.value()));
                    printf ("compaction value : %.6f\n", compaction_ratio.value());
                    printf ("eviction_ratio : %.6f\n", eviction_ratio.value());
                    printf ("eviction_ratio_in_ghost_cache : %.6f\n", eviction_ratio_in_ghost_cache.value());*/
                }
            }
        }
    }
}

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
        ghost_cache.access(key);
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
         
    }
    check_and_evict_if_needed();   
    /* 3) free pool 부족 시 세그먼트 eviction */
    
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
LogCacheSegment* LogCache::alloc_segment(bool shrink)
{
    
    if (shrink == true) {
        check_and_evict_if_needed();     // proactive
    }

    if (free_pool.empty())
        throw std::runtime_error("LogCache: no free segment");

    LogCacheSegment* s = free_pool.front();
    free_pool.pop_front();
    s->reset();
    return s;
}


LogCacheSegment* LogCache::get_segment_with_stream_policy(bool gc, uint64_t key, bool check_only)
{
    LogCacheSegment *seg = nullptr;
    uint64_t previous_blk_create_timestamp = UINT64_MAX;
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
void LogCache::check_and_evict_if_needed()
{
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));
    LogCacheSegment* last_target_seg = nullptr;
    //bool first_compact = true;
    std::list<Segment *> segment_list;
    g_timestamp = log_cache_timestamp;
    static uint64_t threshold = (cfg_.segment_bytes / cache_block_size) *static_cast<std::size_t>(std::ceil(total_segments *
                                           (1 - cfg_.free_ratio_low) * (1 + additional_free_blks_ratio_by_gc)));
    g_threshold = threshold;
    while (free_pool.size() < low_water)
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
            
            if (additional_free_blks_ratio_by_gc < 0.01 ||
                log_cache_timestamp - victim->create_timestamp < threshold) {
                evictor->add(victim, log_cache_timestamp);
                victim = (LogCacheSegment *)compactor->choose_segment();
            }
            else {
                // addtional_free_blks_ratio_by_gc is on 
                // and threshold < log_cache_timestamp - victim->create_timestamp
                compact = false; 
            }
        }
        else {
            victim = (LogCacheSegment *)evictor->choose_segment();
            threshold = log_cache_timestamp - victim->create_timestamp;
            g_threshold = threshold;    
        }

        //printf("compact %d\n", compact);
        assert (victim != nullptr);
        if (victim->valid_cnt == 0) {
            reset_segment(victim);
        }
        else if (compact == true) {
            int stream_id = victim->get_class_num();
            compacted_ages_with_segment_histogram->inc(log_cache_timestamp - victim->create_timestamp);
            last_target_seg = (LogCacheSegment *)evict_and_compaction(victim, threshold, stream_id);
            if (last_target_seg) {
                segment_list.push_back(last_target_seg);
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
            while (stream_policy && victim_stream_id >= Segment::GC_STREAM_START) {
                LogCacheSegment* seg = get_segment_to_active_stream(true, victim_stream_id, true);
                if (seg){
                   // printf("Evict GC stream %d, write_ptr %d\n", victim_stream_id, seg->write_ptr);
                    dummy_fill_segment(seg);
                    //evict_segment(seg);
                    gc_active_seg.erase(victim_stream_id);
                }
                else {
                    break;
                }
            }
        }
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
        printf("%ld\n", s->valid_cnt);
        printf("No valid blocks to evict.\n");
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
            
            mapping.erase(index_64k);
            auto &other_blk = it->second.seg->blocks[it->second.idx];
            other_blk.valid = false;
            global_valid_blocks -= 1;
            evicted_blocks_per_evict += 1;
        }
        else {
            read_blocks_in_partial_write += 1;
        }
    }
    evicted_cache_blocks_per_evict->inc(evicted_blocks_per_evict);
    _evict_one_block(start_index_64k  * cache_block_size /* 64k aligend */, cache_block_size * EVICTED_BLOCK_SIZE /* 64k */, OP_TYPE::WRITE);
    mapping.erase(blk.key);
    blk.valid = false;
    global_valid_blocks -= 1;
}

void LogCache::print_objects(std::string prefix, uint64_t value) {
    //fprintf(fp_object, "%s: %lu\n", prefix.c_str(), value);
}

void LogCache::print_stats() {
    static uint64_t written_window_bytes = cfg_.print_stats_interval;
    static uint64_t next_written_bytes = cfg_.segment_bytes;
    if ((uint64_t)write_size_to_cache >= next_written_bytes) {
        const std::string& prefix = stats_prefix();
        const char* prefix_cstr = prefix.empty() ? "LOG_CACHE" : prefix.c_str();
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu\n",
                prefix_cstr, invalidate_blocks, compacted_blocks, global_valid_blocks, write_size_to_cache, evicted_blocks, write_hit_size, total_capacity_bytes * cache_block_size, reinsert_blocks, read_blocks_in_partial_write);
        fflush(fp_stats);
        next_written_bytes += written_window_bytes;
    }
}
