#include "icache.h"
#include "lru_cache.h"
#include "fifo_cache.h"
#include "log_fifo_cache.h"
#include "no_cache.h"
#include "log_cache.h"
#include "evict_policy_fifo.h"
#include "evict_policy_fifo_zero.h"
#include "evict_policy_greedy.h"
#include "evict_policy_cost_benefit.h"
#include "evict_policy_lambda.h"
#include "evict_policy_selective_fifo.h"
#include "evict_policy_k_cost_benefit.h"
#include "evict_policy_multiqueue.h"
#include "istream.h"
#include <cassert>
#include <string>
#include <algorithm> 

#define TEN_GB (10 * 1024ULL * 1024ULL * 1024ULL)

double score_hot_and_greedy(Segment *seg) {
    return -seg->valid_cnt;
}


double score_age_evict(Segment *seg) {
    return -seg->create_timestamp;
}

double score_age(Segment *seg) {
    if (seg->valid_cnt > 8190) {
        return -UINT64_MAX;
    }
    return seg->create_timestamp;
}

uint64_t g_threshold = 0;
uint64_t g_timestamp = 0;
double score_hot_first(Segment *seg) {
    assert(g_threshold > 0 && g_timestamp > 0);
    double u = seg->valid_cnt / 8192.0;
    return (g_threshold - (g_timestamp - seg->create_timestamp)) * (1 - u) / (u);
}


double score_cold_first(Segment *seg) {
    assert(g_threshold > 0 && g_timestamp > 0);
    double u = seg->valid_cnt / 8192.0;
    return (g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}

double score_warm_first(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
        assert(g_threshold > 0 && g_timestamp > 0);
    }
    double u = seg->valid_cnt / 8192.0;
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}

double score_warm_first_hot_last(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
        assert(g_threshold > 0 && g_timestamp > 0);
    }
    double u = seg->valid_cnt / 8192.0;
    if (seg->hot) {
        return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
    }
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}


int g_numerator = 30;
int g_denominator = 90;

std::size_t ranking(std::size_t N) {
    
    std::size_t target_rank = (g_numerator * N) / g_denominator;
    
    //printf ("rank idx %ld\n", target_rank);
    //printf("rank N %d g_numerator %d g_denominator %d target_ranking_score %d ret_rank %d \n", N, g_numerator, g_denominator, (g_numerator * N) / g_denominator, ret_rank);
    return target_rank;
}

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file) {
    if (cache_type == "LRU") {
        return new LRUCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "FIFO") {
        return new FIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "LOG_FIFO") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>());
    }
    else if (cache_type == "LOG_FIFO_ZERO") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoZeroEvictPolicy>());
    }
    else if (cache_type == "NO_CACHE") {
        return new NoCache(cold_capacity, cache_block_size, waf_log_file);
    }
    else if (cache_type == "LOG_GREEDY") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "LOG_COST_BENEFIT") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>());
    }
    else if (cache_type == "LOG_LAMBDA") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<LambdaEvictPolicy>());
    }
    else if (cache_type == "LOG_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_GREEDY_SEPBIT"){
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_COST_BENEFIT_SEPBIT") { 
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_FIFO_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_GREEDY_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_COST_BENEFIT_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy);
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO_0_7") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.7, std::make_unique<GreedyEvictPolicy>());
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.85, std::make_unique<GreedyEvictPolicy>());
    }
    else if (cache_type == "LOG_MULTI_QUEUE") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<MultiQueueEvictPolicy>(), nullptr, nullptr);
    }
    else if (cache_type == "LOG_GREEDY_FIFO") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.93, std::make_unique<GreedyEvictPolicy>(), 1.2);
    }
    else if (cache_type == "LOG_HOT_FIRST_SELECTIVE_FIFO_0_6_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_hot_and_greedy));
    }
    else if (cache_type == "LOG_1TH_COST_BENEFIT") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 0.7);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<GreedyEvictPolicy>(), 0.7);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_2") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<GreedyEvictPolicy>(), 0.5, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_3") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4_4") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_cold_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_5") {
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_6") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_2");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_7") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_9") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true);
    }
    else if (cache_type == "LOG_GREEDY_FIFO_2") {
        g_numerator = 10;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.8, std::make_unique<GreedyEvictPolicy>(), 0);
    }
    else if (cache_type == "LOG_LAST_COST_BENEFIT") {
        g_numerator = 90;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0);
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
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(true, false, std::move(v_ptr2)), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5);
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
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5);
    }
    else if (cache_type == "LOG_5TH_COST_BENEFIT") {
        g_numerator = 50;
        g_denominator = 90;        
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.2);
    }
    else if (cache_type == "LOG_8TH_COST_BENEFIT") {
        g_numerator = 80;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
            
        return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0);
    }
    else {
        assert(false);
    }
    return nullptr;
}

ICache::ICache(uint64_t cold_capacity, const std::string& waf_log_file):ftl(cold_capacity, new GreedyEvictPolicy()) {
    write_size_to_cache = 0;
    evicted_blocks = 0;
    write_hit_size = 0;
    next_write_size_to_cache = TEN_GB;
    fp = fopen(waf_log_file.c_str(), "w");
    
    std::string stat_log_file = "stat.log.";
    stat_log_file += get_timestamp();
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