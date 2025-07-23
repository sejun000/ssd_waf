#include "log_cache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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
             IStream *input_stream_policy
             )
    : ICache(cold_capacity, waf_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : Config{}),
      evictor(std::move(ev)),
      cache_trace_(cache_trace)
{
    segment_size_blocks = cfg_.segment_bytes / blk_sz;
    total_segments = cache_block_count * blk_sz / cfg_.segment_bytes;
    assert(segment_size_blocks > 0 && "segment_bytes too small");
    assert(total_segments      > 0 && "device_bytes too small");
    log_cache_timestamp = 0;
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
            seg = alloc_segment();           // 필요 시 eviction
            seg->create_timestamp = log_cache_timestamp; // 초기화
            active_seg.emplace(stream_id, seg);
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
            int input_stream_id = stream_policy->Classify(key, false, log_cache_timestamp);
            auto it_stream = active_seg.find(input_stream_id);
            if (it_stream == active_seg.end())
            {
                seg = alloc_segment();           // 필요 시 eviction
                seg->create_timestamp = log_cache_timestamp; // 초기화
                active_seg.emplace(input_stream_id, seg);
            }
            else
            {
                seg = it_stream->second;
            }
            stream_id = input_stream_id;
        }
        /* overwrite 시 invalidate */
        if (exists(key))
        {
            auto loc = mapping[key];
            if (loc.seg->blocks[loc.idx].valid)
            {
                loc.seg->blocks[loc.idx].valid = false;
                --loc.seg->valid_cnt;
                global_valid_blocks -= 1;
                evictor->update(seg);
            }
            mapping.erase(key);
        }
        else
        {
            _invalidate_cold_block(key * cache_block_size,
                                   lba_sz,
                                   OP_TYPE::TRIM);
        }

        if (seg->full())                 // segment 소진 -> 새 seg
        {
            // add previous segment count 
            evictor->add(seg);
            seg = alloc_segment();
            seg->create_timestamp = log_cache_timestamp; // 초기화
            active_seg[stream_id] = seg;
        }

        seg->blocks[seg->write_ptr] = { key, true };
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
LogCacheSegment* LogCache::alloc_segment()
{
    check_and_evict_if_needed();     // proactive

    if (free_pool.empty())
        throw std::runtime_error("LogCache: no free segment");

    LogCacheSegment* s = free_pool.front();
    free_pool.pop_front();
    s->reset();
    return s;
}

void LogCache::check_and_evict_if_needed()
{
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));

    while (free_pool.size() < low_water)
    {
        /* eviction 후보 수집 */
        
        LogCacheSegment* victim = (LogCacheSegment *)evictor->choose_segment();
        if (!victim) break;          
        if (stream_policy) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }
        evict_segment(victim);
    }
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

void LogCache::evict_segment(LogCacheSegment* s)
{
    /* 모든 valid page flush */
    //printf("%d\n", s->valid_cnt);
    //int cnt = 0;
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;

        _evict_one_block(blk.key * cache_block_size,
                         cache_block_size,
                         OP_TYPE::WRITE);
        evicted_blocks += 1;
      //  cnt += 1;

        mapping.erase(blk.key);
        blk.valid = false;
    }
    //printf ("%d \n", cnt);

    s->valid_cnt = 0;
    s->write_ptr = 0;

    /* active_seg 맵에서 제거 */
    
    for (auto it = active_seg.begin(); it != active_seg.end();)
    {
        if (it->second == s) {
            it = active_seg.erase(it);
            assert(false);
        }
        else                 ++it;
    }
    free_pool.push_back(s);
    evictor->remove(s);
}
