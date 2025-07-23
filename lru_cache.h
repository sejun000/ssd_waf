#pragma once
#include <list>
#include <unordered_map>
#include <string>
#include <set>
#include "allocator.h"
#include "icache.h"

// LRU 캐시 클래스 선언
class LRUCache : public ICache {
public:
    // 생성자: capacity는 블록 단위 최대 개수
    LRUCache(uint64_t cold_capacity, long capacity, int _cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file);
    ~LRUCache();

    void evict_one_block();
    bool exists(long key);
    void touch(long key, OP_TYPE op_type);
    void batch_insert(int stream_id, const std::map<long, int> &newBlocks, OP_TYPE op_type);
    bool is_cache_filled();
    int get_block_size();
    void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type);
    size_t size();

private:
    long capacity_;
    std::list<long> cacheList; // LRU 순서: front = 가장 오래된, back = 최신
    std::unordered_map<long, CacheEntry> cacheMap;
    bool cache_filled;
    FILE *cache_trace_fp;
    FILE *cold_trace_fp;
    int cache_block_size;
    bool cache_trace;
    DummyAllocator allocator;
};