#include "icache.h"
#include "lru_cache.h"
#include "fifo_cache.h"
#include "log_fifo_cache.h"
#include "no_cache.h"
#include "ftl.h"
#include <cassert>

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file) {
    if (cache_type == "LRU") {
        return new LRUCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file);
    }
    else if (cache_type == "FIFO") {
        return new FIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file);
    }
    else if (cache_type == "LOG_FIFO") {
        return new LogFIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file);
    }
    else if (cache_type == "NO_CACHE") {
        return new NoCache(cold_capacity, cache_block_size);
    }
    else {
        assert(false);
    }
    return nullptr;
}

ICache::ICache(uint64_t cold_capacity):ftl(cold_capacity, new GreedyGcPolicy(), "waf_log.txt") {
    write_size_to_cache = 0;
    evicted_blocks = 0;
    write_hit_size = 0;
}

void ICache::_evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type) {
    if (op_type == OP_TYPE::WRITE) { 
        //printf("Evicting block at offset: %lu, size: %d\n", lba_offset, lba_size);
        ftl.Write(lba_offset, lba_size, 0); // 0은 stream ID로 가정
    }/*else if (op_type == OP_TYPE::READ) {
        ftl.Trim(lba_offset, lba_size);
    } else {
        assert(false); // 지원하지 않는 OP_TYPE
    }*/
}

std::tuple<long long, long long, long long> ICache::get_status() {
    
    return std::tuple<long long, long long, long long>(write_size_to_cache, evicted_blocks, write_hit_size);
}