#pragma once
#include "icache.h"              // 기존 프로젝트 공용 인터페이스
#include "log_cache_segment.h"
#include "evict_policy.h"
#include "evict_policy_greedy.h"
#include "istream.h"

#include <unordered_map>
#include <deque>
#include <list>
#include <memory>
#include <map>
#include <string>

/**
 * Multi‑stream append‑only Log Cache (세그먼트 단위)
 */
class LogCache final : public ICache
{
public:
    struct Config
    {
        std::size_t segment_bytes  = 32ull * 1024 * 1024; ///< default 32 MB
        double      free_ratio_low = 0.10;                ///< 10 %
    };


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
             IStream *input_stream_policy = nullptr
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
    bool is_cache_filled() override;


private:
    /* configuration ******************************************************/
    const int         cache_block_size;
    Config            cfg_;

    /* segment pools ******************************************************/
    std::deque<LogCacheSegment*>                free_pool;
    std::list<std::unique_ptr<LogCacheSegment>> all_segments; // owner
    std::unordered_map<int, LogCacheSegment*>   active_seg;   // stream→seg

    /* page lookup ********************************************************/
    struct Loc { LogCacheSegment* seg; std::size_t idx; };
    std::unordered_map<long, Loc>                mapping;

    /* helpers ************************************************************/
    std::unique_ptr<EvictPolicy> evictor;

    LogCacheSegment* alloc_segment();
    void             check_and_evict_if_needed();
    void             evict_segment(LogCacheSegment* s);

    /* trace(optional) *****************************************************/
    bool  cache_trace_;
    FILE* trace_fp_      = nullptr;
    FILE* cold_trace_fp_ = nullptr;
    std::size_t segment_size_blocks;
    std::size_t total_segments;
    uint64_t log_cache_timestamp = 0; // per 4kB block
    IStream *stream_policy = nullptr;
    uint64_t global_valid_blocks = 0;
};
