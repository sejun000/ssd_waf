#include "icache.h"
#include "lru_cache.h"
#include "fifo_cache.h"
#include "log_fifo_cache.h"
#include "no_cache.h"
#include "log_cache.h"
#include "midas_cache.h"
#include "fairywren_cache.h"
#include "evict_policy_fifo.h"
#include "evict_policy_fifo_zero.h"
#include "evict_policy_greedy.h"
#include "evict_policy_cost_benefit.h"
#include "evict_policy_lambda.h"
#include "evict_policy_selective_fifo.h"
#include "evict_policy_k_cost_benefit.h"
#include "evict_policy_multiqueue.h"
#include "evict_policy_midas.h"
#include "istream.h"
#include <cassert>
#include <string>
#include <algorithm> 
#include <cmath>

#define TEN_GB (10 * 1024ULL * 1024ULL * 1024ULL)

double score_hot_and_greedy(Segment *seg) {
    return -seg->valid_cnt;
}


double score_age_evict(Segment *seg) {
    return -seg->create_timestamp;
}
double g_segment_blocks = 262144.0 * 6;  // default 1GB/4KB, set by cache init
double score_age(Segment *seg) {
    /*if (seg->valid_cnt > g_segment_blocks - 2) {
        return -UINT64_MAX;
    }*/
    return seg->create_timestamp;
}

uint64_t g_threshold = 0;
uint64_t g_timestamp = 0;

// Score functions for CbEvictPolicy (same as icache.cpp)
/*static double score_age_evict(Segment *seg) {
    return -static_cast<double>(seg->create_timestamp);
}*/

static double score_greedy_first(Segment *seg) {
    return -static_cast<double>(seg->valid_cnt);
}

// Global variables for score_warm_first (set by LogCache during GC)
extern uint64_t g_threshold;
extern uint64_t g_timestamp;

extern uint64_t g_cycle_length;
extern int g_stream_cycles[IStream::MAX_STREAMS];

// Check if segment is from an old cycle for its stream → protect from compaction
// Uses seg->create_timestamp (= oldest block's timestamp after compaction) instead of seg->cycle
static inline bool is_old_cycle_segment(Segment *seg) {
    if (g_cycle_length == 0 || interval == 0) return false;
    int seg_cycle = static_cast<int>(seg->create_timestamp / g_cycle_length);
    int idx = seg->class_num - Segment::GC_STREAM_START;
    if (idx < 0 || idx >= IStream::MAX_STREAMS) {
        // Host segment: estimate which GC stream its blocks would map to
        idx = static_cast<int>((seg->create_timestamp % g_cycle_length) / interval);
    }
    if (idx >= 0 && idx < IStream::MAX_STREAMS) {
        if (seg_cycle < g_stream_cycles[idx]) {
            return true;
        }
    }
    return false;
}

static double score_warm_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0;
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
    //printf("segment_size : %f\n",segment_size);
   // if (u > 0.8) return 0.0;  // Too full to compact efficiently
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    if (u < 0.0001) u = 0.0001;
    if (std::min(g_threshold - (g_timestamp - seg->create_timestamp),
                    g_timestamp - seg->create_timestamp) * (1 - u) / u < 0) {
        //print related variable
        printf("g_threshold: %lu, g_timestamp: %lu, seg->create_timestamp: %lu, u: %f\n",
               g_threshold, g_timestamp, seg->create_timestamp, u);
        assert(false);
    }
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp),
                    g_timestamp - seg->create_timestamp) * (1 - u) / u;
}

// Score function: prefer HOT segments (recently created) for compaction
static double score_hot_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0;
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
   // if (u > 0.8) return 0.0;
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    if (u < 0.0001) u = 0.0001;
    // Hot-first: higher score for segments with smaller age (recently created)
    return (g_threshold - (g_timestamp - seg->create_timestamp)) * (1 - u) / u;
}

// Score function: prefer COLD segments (old) for compaction
static double score_cold_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0;
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
  //  if (u > 0.8) return 0.0;
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    if (u < 0.0001) u = 0.0001;
    // Cold-first: higher score for segments with larger age (older)
    return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
}

static double score_sepbit_age(Segment *seg) {
    //if (is_old_cycle_segment(seg)) return 0.0;
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
//    if (u > 0.8) return 0.0;
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    if (u < 0.0001) u = 0.0001;
    return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
}


// double score_hot_first(Segment *seg) {
//     assert(g_threshold > 0 && g_timestamp > 0);
//     double u = seg->valid_cnt / g_segment_blocks;
//     return (g_threshold - (g_timestamp - seg->create_timestamp)) * (1 - u) / (u);
// }


// double score_cold_first(Segment *seg) {
//     assert(g_threshold > 0 && g_timestamp > 0);
//     double u = seg->valid_cnt / g_segment_blocks;
//     return (g_timestamp - seg->create_timestamp) * (1 - u) / (u);
// }

// double score_warm_first(Segment *seg) {
//     if (g_threshold <= 0 || g_timestamp <= 0) {
//         //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
//         assert(g_threshold > 0 && g_timestamp > 0);
//     }
//     double u = seg->valid_cnt / g_segment_blocks;
//     //printf("why %.4f\n", std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u));
//     return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
// }

// double score_warm_first_hot_last(Segment *seg) {
//     if (g_threshold <= 0 || g_timestamp <= 0) {
//         //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
//         assert(g_threshold > 0 && g_timestamp > 0);
//     }
//     double u = seg->valid_cnt / g_segment_blocks;
//     if (seg->hot) {
//         return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
//     }
//     return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
// }

// double score_sepbit_age(Segment *seg) {
    
//     assert(g_threshold > 0 && g_timestamp > 0);
//     double u = seg->valid_cnt / g_segment_blocks;
//     return sqrt(g_timestamp - seg->create_timestamp) * (1 - u) / (u);
// }


int g_numerator = 30;
int g_denominator = 90;

std::size_t ranking(std::size_t N) {
    
    std::size_t target_rank = (g_numerator * N) / g_denominator;
    
    //printf ("rank idx %ld\n", target_rank);
    //printf("rank N %d g_numerator %d g_denominator %d target_ranking_score %d ret_rank %d \n", N, g_numerator, g_denominator, (g_numerator * N) / g_denominator, ret_rank);
    return target_rank;
}

namespace {
template <typename T>
T* attach_prefix(T* cache, const std::string& prefix, const std::string& start_ts, bool keep_stat_log = false) {
    if (cache) {
        cache->set_stats_prefix(prefix);
        cache->set_start_ts(start_ts);
        if (!keep_stat_log) {
            std::string stat_name = prefix + ".stat.log." + start_ts;
            cache->rename_stat_log(stat_name);
        }
    }
    return cache;
}
}

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, double valid_rate_threshold, std::string stat_log_file, double periodic_ratio) {
    if (capacity <= 0) {
        capacity = 1;
    }
    // Generate start timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    std::string start_ts = oss.str();

    // Generate stat_log_file with cache_type prefix if not provided
    if (stat_log_file.empty()) {
        stat_log_file = cache_type + ".stat.log." + start_ts;
    }
    set_stream_interval(static_cast<uint64_t>(capacity), 262144ULL * 6);
    if (cache_type == "LRU") {
        return attach_prefix(new LRUCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, stat_log_file), cache_type, start_ts);
    }
    else if (cache_type == "FIFO") {
        return attach_prefix(new FIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file), cache_type, start_ts);
    }
    else if (cache_type == "LOG_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "LOG_FIFO_ZERO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoZeroEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "NO_CACHE") {
        return attach_prefix(new NoCache(cold_capacity, cache_block_size, waf_log_file), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file), cache_type, start_ts);
    }
    else if (cache_type == "LOG_COST_BENEFIT") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "FAIRYWREN") {
        FairyWrenConfig cfg;
        return attach_prefix(new FairyWrenCache(cold_capacity,
                                                capacity,
                                                cache_block_size,
                                                _cache_trace,
                                                trace_file,
                                                cold_trace_file,
                                                waf_log_file,
                                                cfg,
                                                stat_log_file),
                             cache_type, start_ts);
    }
    else if (cache_type == "LOG_MIDAS_DEFAULT") {
        IStream *input_stream_policy = createIstreamPolicy("midas_hotcold");
        auto cache = new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace,
                                  trace_file, cold_trace_file, waf_log_file,
                                  std::make_unique<MiDASGreedyEvictPolicy>(),
                                  nullptr, input_stream_policy);
        return attach_prefix(cache, cache_type, start_ts);
    }
    else if (cache_type == "LOG_LAMBDA") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<LambdaEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "LOG_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_SEPBIT"){
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_COST_BENEFIT_SEPBIT") { 
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_FIFO_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_COST_BENEFIT_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO_0_7") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.7, std::make_unique<GreedyEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.85, std::make_unique<GreedyEvictPolicy>()), cache_type, start_ts);
    }
    else if (cache_type == "LOG_MULTI_QUEUE") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<MultiQueueEvictPolicy>(), nullptr, nullptr), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.93, std::make_unique<GreedyEvictPolicy>(), 1.2), cache_type, start_ts);
    }
    else if (cache_type == "LOG_HOT_FIRST_SELECTIVE_FIFO_0_6_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_hot_and_greedy)), cache_type, start_ts);
    }
    else if (cache_type == "LOG_1TH_COST_BENEFIT") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.90, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 0.7), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<GreedyEvictPolicy>(), 0.7), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_2") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<GreedyEvictPolicy>(), 0.5, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_3") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.90, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4_4") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.90, std::make_unique<CbEvictPolicy>(score_cold_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_5") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_6") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_2");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_7") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_80") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.80, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_80_COLD") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.80, std::make_unique<CbEvictPolicy>(score_cold_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_80") {
        IStream *input_stream_policy = nullptr;
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.80, std::make_unique<CbEvictPolicy>(score_greedy_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_COLD_80") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.80, std::make_unique<CbEvictPolicy>(score_cold_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_70") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.70, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_60") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.60, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8_2") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_cold_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8_3") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_9") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_10") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict),
            nullptr, input_stream_policy, 0.6, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true, stat_log_file, 0, 0, 0, periodic_ratio), cache_type, start_ts, !stat_log_file.empty());
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_11") { // for getting optimized value from dynamic algorithm
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict),
            nullptr, input_stream_policy, valid_rate_threshold, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false, stat_log_file), cache_type, start_ts, !stat_log_file.empty());
    }
    else if (cache_type == "LOG_GREEDY_11") { // for getting optimized value from dynamic algorithm
        IStream *input_stream_policy = nullptr;
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict),
            nullptr, input_stream_policy, valid_rate_threshold, std::make_unique<CbEvictPolicy>(score_greedy_first), 0, false, stat_log_file), cache_type, start_ts, !stat_log_file.empty());
    }
    else if (cache_type == "LOG_FIFO_2") {
        Config cfg  ={
           // .segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .free_ratio_low = 0.01,                ///< 1 %
            .evicted_blk_size = 16,                 ///< 64k eviction
            .print_stats_interval = TEN_GB,
        };
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), &cfg), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_12") { // for 64k eviction
        Config cfg  ={
            //.segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .free_ratio_low = 0.01,                ///< 1 %
            .evicted_blk_size = 16,                 ///< 64k eviction
            .print_stats_interval = TEN_GB,
        };
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            &cfg, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_13") { // for 64k eviction
        Config cfg  ={
            //.segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .segment_bytes  = 1ull * 1024 * 1024 * 1024, ///< default 32 MB
            .free_ratio_low = 0.01,                ///< 1 %
            .evicted_blk_size = 16,                 ///< 64k eviction
            .print_stats_interval = TEN_GB,
        };
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            &cfg, input_stream_policy, 0.6, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true), cache_type, start_ts);
    }
    else if (cache_type == "LOG_SEPBIT_FIFO") {
        
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.80, std::make_unique<CbEvictPolicy>(score_sepbit_age), 0, false), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_FIFO_2") {
        g_numerator = 10;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.8, std::make_unique<GreedyEvictPolicy>(), 0), cache_type, start_ts);
    }
    else if (cache_type == "LOG_LAST_COST_BENEFIT") {
        g_numerator = 90;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0), cache_type, start_ts);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_2") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        std::vector<int> v;
        v.insert(v.end(), {0, 1, 2, 3});
        std::vector<int> v2;
        v2.insert(v2.end(), {3, 2, 1, 0});
        // v의 복사본을 가지는 unique_ptr를 생성하여 전달
        auto v_ptr = std::make_unique<std::vector<int>>(v);
        auto v_ptr2 = std::make_unique<std::vector<int>>(v2);
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(true, false, std::move(v_ptr2)), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5), cache_type, start_ts);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_3") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        std::vector<int> v;
        v.insert(v.end(), {0, 1, 2, 3});
        std::vector<int> v2;
        v2.insert(v2.end(), {3, 2, 1, 0});
        // v의 복사본을 가지는 unique_ptr를 생성하여 전달
        auto v_ptr = std::make_unique<std::vector<int>>(v);
        auto v_ptr2 = std::make_unique<std::vector<int>>(v2);
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5), cache_type, start_ts);
    }
    else if (cache_type == "LOG_5TH_COST_BENEFIT") {
        g_numerator = 50;
        g_denominator = 90;        
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.2), cache_type, start_ts);
    }
    else if (cache_type == "LOG_8TH_COST_BENEFIT") {
        g_numerator = 80;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
            
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0), cache_type, start_ts);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_PERIODIC") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict),
            nullptr, input_stream_policy, 0.70, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false, stat_log_file,
            600.0,   // period 600GB
            0.60,    // min
            0.88),   // max
            cache_type, start_ts, !stat_log_file.empty());
    }
    else if (cache_type == "MIDAS_CACHE") {
        MidasInitArgs midas_args;
        midas_args.workload = "midas_cache_adapter";
        midas_args.vs_policy = "cost-benefit";
        midas_args.dev_gb = static_cast<int>(capacity * cache_block_size / (1024ULL * 1024 * 1024));
        if (midas_args.dev_gb <= 0) midas_args.dev_gb = 1;
        midas_args.seg_mb = 6 * 1024; // default 32MB
        return attach_prefix(new MidasCache(cold_capacity, capacity, cache_block_size, _cache_trace,
                                            trace_file, cold_trace_file, waf_log_file,
                                            nullptr, nullptr, 0.8, stat_log_file, 5, midas_args),
                             cache_type, start_ts);
    }
    else {
        assert(false);
    }
    return nullptr;
}

ICache::ICache(uint64_t cold_capacity, const std::string& waf_log_file, const std::string& input_stat_log_file):ftl(cold_capacity, new GreedyEvictPolicy()) {
    write_size_to_cache = 0;
    evicted_blocks = 0;
    write_hit_size = 0;
    next_write_size_to_cache = TEN_GB;
    fp = fopen(waf_log_file.c_str(), "w");
    
    std::string stat_log_file = "stat.log.";
    if (!input_stat_log_file.empty()) {
        stat_log_file = input_stat_log_file;
    } else {
        stat_log_file += get_timestamp();
    }
    std::string object_log_file = "object.log.";
    object_log_file += get_timestamp();
    fp = fopen(waf_log_file.c_str(), "w");
    if (fp == NULL) {
        std::cerr << "Cannot open file: " << waf_log_file << std::endl;
        exit(1);
    }
    fp_stats = fopen(stat_log_file.c_str(), "w");
    if (fp_stats == NULL) {
        std::cerr << "Cannot open file: " << stat_log_file << std::endl << std::endl;
        exit(1);
    }
    fp_object = fopen(object_log_file.c_str(), "w");
    if (fp_object == NULL) {
        std::cerr << "Cannot open file: " << object_log_file << std::endl;
        exit(1);
    }
    
    printf("waf log : %s, stat log : %s, object log : %s\n", waf_log_file.c_str(), stat_log_file.c_str(), object_log_file.c_str());
    assert(fp != NULL);
    assert(fp_stats != NULL);
    assert(fp_object != NULL);
}

void ICache::set_stats_prefix(const std::string& prefix) {
    stats_prefix_ = prefix;
}

const std::string& ICache::stats_prefix() const {
    return stats_prefix_;
}

void ICache::rename_stat_log(const std::string& new_name) {
    if (fp_stats) {
        fclose(fp_stats);
        fp_stats = nullptr;
    }
    fp_stats = fopen(new_name.c_str(), "w");
    if (fp_stats) {
        printf("stat log renamed to: %s\n", new_name.c_str());
    }
}

void ICache::_evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type) {
    if (op_type == OP_TYPE::WRITE) { 
        //printf("Evicting block at offset: %lu, size: %d\n", lba_offset, lba_size);
        ftl.Write(lba_offset, lba_size, 0); // 0은 stream ID로 가정
    }
    if (write_size_to_cache > next_write_size_to_cache) {
        next_write_size_to_cache += TEN_GB;
        fprintf(fp, "%lld %lld %ld %ld\n", write_size_to_cache, evicted_blocks * get_block_size(), ftl.GetHostWritePages() * NAND_PAGE_SIZE, ftl.GetNandWritePages() * NAND_PAGE_SIZE);
        fflush(fp);
    }
}

void ICache::_invalidate_cold_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type) {
    if (op_type == OP_TYPE::TRIM) {
        ftl.Trim(lba_offset, lba_size);
    }
}

std::tuple<long long, long long, long long> ICache::get_status() {
    
    return std::tuple<long long, long long, long long>(write_size_to_cache, evicted_blocks, write_hit_size);
}
