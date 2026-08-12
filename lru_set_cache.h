#pragma once
#include <list>
#include <unordered_map>
#include <string>
#include <vector>
#include "allocator.h"
#include "icache.h"

// Set-associative LRU 캐시 (OpenCAS 식): 블록 키를 modulo 로 set 에 매핑,
// set 당 ways 개 엔트리, eviction 은 해당 set 안에서만 LRU 로 수행.
// LRUCache(fully associative) 는 그대로 두고 복사본으로 구현.
class LRUSetCache : public ICache {
public:
    LRUSetCache(uint64_t cold_capacity, long capacity, int _cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, const std::string &stat_log_file = "", int ways = 32);
    ~LRUSetCache();

    void evict_one_block();               // ICache 순수가상: 비어있지 않은 set 을 라운드로빈으로 골라 그 set 의 LRU 를 evict
    void evict_one_from_set(size_t set_idx);
    bool exists(long key);
    void touch(long key, OP_TYPE op_type);
    void batch_insert(int stream_id, const std::map<long, int> &newBlocks, OP_TYPE op_type);
    bool is_cache_filled();
    int get_block_size();
    void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type);
    size_t size();

private:
    size_t set_of(long key) const {
        return static_cast<size_t>(static_cast<unsigned long>(key) % num_sets_);
    }
    long capacity_;                       // num_sets_ * ways_ (7% OP 차감 후)
    int ways_;
    size_t num_sets_;
    size_t evict_cursor_ = 0;
    std::vector<std::list<long>> sets_;   // set 별 LRU 리스트: front = 가장 오래된
    std::unordered_map<long, CacheEntry> cacheMap;
    bool cache_filled;
    FILE *cache_trace_fp;
    FILE *cold_trace_fp;
    int cache_block_size;
    bool cache_trace;
    PageMappingFTL cache_ftl;
    FILE *cache_ftl_log_fp = nullptr;
    uint64_t cache_ftl_log_interval_bytes = 0;
    uint64_t next_cache_ftl_log_bytes = 0;
    void maybe_log_cache_ftl_stats();
    DummyAllocator allocator;
};
