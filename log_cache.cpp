#include "log_cache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <list>

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
             double input_additional_free_blks_ratio_by_gc
             )
    : ICache(cold_capacity, waf_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : Config{}),
      evictor(std::move(ev)),
      cache_trace_(cache_trace),
      target_valid_blk_rate(input_target_valid_blk_rate),
      compactor(std::move(cp)),
      additional_free_blks_ratio_by_gc(input_additional_free_blks_ratio_by_gc)
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

void LogCache::batch_insert(int stream_id,
                            const std::map<long,int>& newBlocks,
                            OP_TYPE                   op_type)
{
    if (op_type == OP_TYPE::READ || newBlocks.empty())
        return;                          // 요구사항 ③ – read 무시
    
    /* 1) 스트림별 active segment 확보 */
    LogCacheSegment* seg = nullptr;
    if (!stream_policy) {
        auto it_stream = active_seg.find(stream_id);
        if (it_stream == active_seg.end())
        {
            seg = alloc_segment_to_active_stream(false, stream_id, true);
        }
        else
        {
            seg = it_stream->second;
        }
    }

    /* 2) 블록 단위 append */
    for (auto [key, lba_sz] : newBlocks)
    {
        if (stream_policy) {
            
            uint64_t previous_blk_create_timestamp = UINT64_MAX;
            if (exists(key))
            {
                auto loc = mapping[key];
                if (loc.seg->blocks[loc.idx].valid)
                {
                    previous_blk_create_timestamp = loc.seg->blocks[loc.idx].create_timestamp;
                }
            }
            int input_stream_id = stream_policy->Classify(key, false, log_cache_timestamp, previous_blk_create_timestamp);
            auto it_stream = active_seg.find(input_stream_id);
            if (it_stream == active_seg.end())
            {
                seg = alloc_segment_to_active_stream(false, input_stream_id, true);
            }
            else
            {
                seg = it_stream->second;
            }
            stream_id = input_stream_id;
        }
        
        if (seg->full())                 // segment 소진 -> 새 seg
        {
            // add previous segment count 
            
            evict_policy_add(seg);
            seg = alloc_segment_to_active_stream(false, stream_id, true);
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

    /* 3) free pool 부족 시 세그먼트 eviction */
    check_and_evict_if_needed();
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
LogCacheSegment* LogCache::alloc_segment(bool shrink)
{
    //printf("alloc_segment: free_pool size %ld, total_segments %ld bool %d \n", free_pool.size(), total_segments, shrink);
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


LogCacheSegment* LogCache::alloc_segment_to_active_stream(bool gc, int stream_id, bool shrink)
{

    LogCacheSegment *seg = alloc_segment(shrink);           // 필요 시 eviction
    std::unordered_map<int, LogCacheSegment*>*  active_table = &active_seg;
    int assigned_stream_id = stream_id;
    if (gc) {
        active_table = &gc_active_seg;
        assigned_stream_id += Segment::GC_STREAM_START;
    }
    seg->class_num = assigned_stream_id; // stream id로 class num 설정
    seg->create_timestamp = log_cache_timestamp; // 초기화
    (*active_table)[stream_id] = seg;
    return seg;
}

void LogCache::check_and_evict_if_needed()
{
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));
    LogCacheSegment* last_target_seg = nullptr;
    //bool first_compact = true;
    uint64_t threshold = 0;
    std::list<Segment *> segment_list;
    bool oddeven = false;
    while (free_pool.size() < low_water)
    {
        /* eviction 후보 수집 */
        bool compact = false;
        if (target_valid_blk_rate >= 0.01 || additional_free_blks_ratio_by_gc >= 0.01) {
            threshold = (cfg_.segment_bytes / cache_block_size) *static_cast<std::size_t>(std::ceil(total_segments *
                                           (1 - cfg_.free_ratio_low) * (1+additional_free_blks_ratio_by_gc)));
            if (compactor && (double)target_valid_blk_rate * total_cache_block_count  > global_valid_blocks) {
                compact = true;
            }
            /*if (compactor && log_cache_timestamp - evicted_segment_age  < (cfg_.segment_bytes / cache_block_size) *static_cast<std::size_t>(std::ceil(total_segments *
                                           (1 - cfg_.free_ratio_low) * (1+additional_free_blks_ratio_by_gc)))) {
                if (compactor && (double)target_valid_blk_rate * total_cache_block_count  > global_valid_blocks) {
                    compact = true;
                }
            }*/
        }

        LogCacheSegment* victim = nullptr; 
        if (compact == true){
            victim = (LogCacheSegment *)compactor->choose_segment();
        }
        else {
            victim = (LogCacheSegment *)evictor->choose_segment();
        }

        //printf("compact %d\n", compact);
        assert (victim != nullptr);
        if (victim->valid_cnt == 0) {
            reset_segment(victim);
        }
        else if (compact == true) {
            int stream_id = victim->get_class_num();
            if (stream_id >= Segment::GC_STREAM_START) {
                stream_id -= Segment::GC_STREAM_START;
            }
            last_target_seg = (LogCacheSegment *)evict_and_compaction(victim, threshold, stream_id);
            if (last_target_seg) {
                segment_list.push_back(last_target_seg);
            }
        }
        else {
            evicted_segment_age = victim->get_create_time();
            evict_segment(victim);
        }

        if (stream_policy) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }
//        first_compact = false;
    }
    /*
    for (auto iter : segment_list) {
        if (iter->full()) {
            evict_policy_add((LogCacheSegment *)iter);
        }
    }*/
    //dummy_fill_segment(last_target_seg);
}

void LogCache::evict_one_block() {
    return;
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
        s->write_ptr = s->blocks.size();
        evict_policy_add(s);
    }
}



Segment* LogCache::evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id)
{
    LogCacheSegment* target_seg = nullptr;
    if (threshold == 0) {
        return compaction(s, gc_stream_id);
    }
    auto it_stream = gc_active_seg.find(gc_stream_id);
    
    if (it_stream == gc_active_seg.end())
    {
        target_seg = alloc_segment_to_active_stream(true, gc_stream_id, false);
        
    }
    else
    {
        target_seg = it_stream->second;
    }
    if (target_seg->full()) {
        // add previous segment count 
        //printf("target seg streamid %d \n", target_seg->get_class_num());
        evict_policy_add((LogCacheSegment *)target_seg);
        
        target_seg = alloc_segment_to_active_stream(true, gc_stream_id, false);
    }
    assert(target_seg != s);
    //printf("stream_id %d\n", gc_stream_id);
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (log_cache_timestamp - blk.create_timestamp >= 0.9 * threshold) { 
            _evict_one_block(blk.key * cache_block_size,
                         cache_block_size,
                         OP_TYPE::WRITE);
            print_objects("evict", log_cache_timestamp - blk.create_timestamp);
            evicted_blocks += 1;
            global_valid_blocks -= 1;
            //  cnt += 1;
            evicted_timestamp[blk.key] = log_cache_timestamp;
            mapping.erase(blk.key);
            blk.valid = false;
            continue;
        }
        if (target_seg->full()) {
            // add previous segment count 
            //printf("target seg streamid %d \n", target_seg->get_class_num());
            evict_policy_add((LogCacheSegment *)target_seg);
            
            target_seg = alloc_segment_to_active_stream(true, gc_stream_id, false);
      //      printf("new target_seg : %p \n", target_seg);
        }
        assert(target_seg != s);
        // get p2L index
        if (target_seg->create_timestamp > blk.create_timestamp) {
            target_seg->create_timestamp = blk.create_timestamp;
        }

        print_objects("compact", log_cache_timestamp - blk.create_timestamp);
        target_seg->blocks[target_seg->write_ptr] = blk; // copy valid block
        mapping[blk.key] = { target_seg, target_seg->write_ptr };
     //   printf("%d\n", target_seg->write_ptr);
        ++target_seg->write_ptr;
        ++target_seg->valid_cnt;
        ++compacted_blocks;

        blk.valid = false;
    }
   printf("Compaction and Evict: %lu blocks moved from segment %p to segment %p, free_pool_size %ld, valid ratio %.4f age %lu target_seg_write_ptr %lu target_create_timestamp %lu threshold %lu stream_id %d\n", 
       s->valid_cnt, s, target_seg, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, target_seg->write_ptr, s->create_timestamp, threshold, gc_stream_id);
    reset_segment(s);
    return target_seg;
}

Segment* LogCache::compaction(LogCacheSegment* s, int gc_stream_id)
{
    LogCacheSegment* target_seg = nullptr;
    auto it_stream = gc_active_seg.find(gc_stream_id);
    
    if (it_stream == gc_active_seg.end())
    {
        target_seg = alloc_segment_to_active_stream(true, gc_stream_id, false);
        
    }
    else
    {
        target_seg = it_stream->second;
    }
    assert(target_seg != s);
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;

        if (target_seg->full()) {
            // add previous segment count 
            evict_policy_add((LogCacheSegment *)target_seg);
            target_seg = alloc_segment_to_active_stream(true, gc_stream_id, false);
      //      printf("new target_seg : %p \n", target_seg);
        }
        assert(target_seg != s);
        // get p2L index
        print_objects("compact", log_cache_timestamp - blk.create_timestamp);
        target_seg->blocks[target_seg->write_ptr] = blk; // copy valid block
        mapping[blk.key] = { target_seg, target_seg->write_ptr };
        ++target_seg->write_ptr;
        ++target_seg->valid_cnt;
        ++compacted_blocks;

        blk.valid = false;
    }
    printf("Compaction: %lu blocks moved from segment %p to segment %p, free_pool_size %ld, valid ratio %.4f age %lu target_seg_write_ptr %lu target_create_timestamp %lu\n", 
       s->valid_cnt, s, target_seg, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, target_seg->write_ptr, s->create_timestamp);
    reset_segment(s);
    return target_seg;
}


void LogCache::evict_segment(LogCacheSegment* s)
{
    /* 모든 valid page flush */
    if (s->valid_cnt == 0) {
        printf("%ld\n", s->valid_cnt);
        printf("No valid blocks to evict.\n");
    }
    
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;

        _evict_one_block(blk.key * cache_block_size,
                         cache_block_size,
                         OP_TYPE::WRITE);
        print_objects("evict", log_cache_timestamp - blk.create_timestamp);
        evicted_blocks += 1;
        global_valid_blocks -= 1;
        //  cnt += 1;
        evicted_timestamp[blk.key] = log_cache_timestamp;
        mapping.erase(blk.key);
        blk.valid = false;
        
    }
    //printf ("%d \n", cnt);
    printf("Evict: %lu blocks free_pool_size %ld, valid ratio %.4f age %lu, create_time %lu \n", 
     s->valid_cnt, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, s->create_timestamp);


   reset_segment(s);
}

void LogCache::print_objects(std::string prefix, uint64_t value) {
    fprintf(fp_object, "%s: %lu\n", prefix.c_str(), value);
}

void LogCache::print_stats() {
    fprintf (fp_stats, "invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu\n",
             invalidate_blocks, compacted_blocks, global_valid_blocks, write_size_to_cache, evicted_blocks, write_hit_size, total_capacity_bytes * cache_block_size, reinsert_blocks);
    fflush(fp_stats);

}