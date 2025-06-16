#pragma once
#include <list>
#include <unordered_map>
#include <map>
#include <string>
#include <cassert>
#include <tuple>
#include "ftl.h"
#include "common.h"

struct CacheEntry {
    std::list<long>::iterator iter;
    size_t allocated_id;
};

class ICache {
public:
    // 생성자: capacity는 블록 단위 최대 개수
    ~ICache(){
        write_size_to_cache = 0;
        evicted_blocks = 0;
        write_hit_size = 0;
    }
    ICache(uint64_t cold_capacity);
    
    virtual bool exists(long key) = 0;
    virtual void touch(long key, OP_TYPE op_type) = 0;
    virtual void batch_insert(const std::map<long, int> &newBlocks, OP_TYPE op_type) = 0;
    virtual bool is_cache_filled() = 0;
    virtual int get_block_size() = 0;
    virtual void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type) = 0;
    void _evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type);
    virtual void evict_one_block() = 0;
    virtual size_t size() = 0;
    virtual bool is_no_cache() { return false; }
    std::tuple<long long, long long, long long> get_status();
    PageMappingFTL ftl;
    long long write_size_to_cache;
    long long evicted_blocks;
    long long write_hit_size;
};

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file);