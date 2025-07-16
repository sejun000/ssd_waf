#include "icache.h"
#include "lru_cache.h"
#include "fifo_cache.h"
#include "log_fifo_cache.h"
#include "no_cache.h"
#include "ftl.h"
#include <cassert>
#include <string>

#define TEN_GB (10 * 1024ULL * 1024ULL * 1024ULL)
ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file) {
    if (cache_type == "LRU") {
        return new LRUCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "FIFO") {
        return new FIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "LOG_FIFO") {
        return new LogFIFOCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file);
    }
    else if (cache_type == "NO_CACHE") {
        return new NoCache(cold_capacity, cache_block_size, waf_log_file);
    }
    else {
        assert(false);
    }
    return nullptr;
}

ICache::ICache(uint64_t cold_capacity, std::string& waf_log_file):ftl(cold_capacity, new GreedyGcPolicy(cold_capacity)) {
    write_size_to_cache = 0;
    evicted_blocks = 0;
    write_hit_size = 0;
    next_write_size_to_cache = TEN_GB;
    fp = fopen(waf_log_file.c_str(), "w");
    //printf("%s\n",waf_log_file.c_str());
    assert(fp != NULL);
}

void ICache::_evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type) {
    if (op_type == OP_TYPE::WRITE) { 
        //printf("Evicting block at offset: %lu, size: %d\n", lba_offset, lba_size);
        ftl.Write(lba_offset, lba_size, 0); // 0은 stream ID로 가정
    }
    if (write_size_to_cache > next_write_size_to_cache) {
        next_write_size_to_cache += TEN_GB;
        fprintf(fp, "%lld %lld %ld %ld\n", write_size_to_cache, evicted_blocks * get_block_size(), ftl.GetHostWritePages() * NAND_PAGE_SIZE, ftl.GetNandWritePages() * NAND_PAGE_SIZE);
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