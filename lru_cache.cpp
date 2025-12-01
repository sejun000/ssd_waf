#include "lru_cache.h"
#include "evict_policy_greedy.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
constexpr double kCacheOpRatio = 0.07;

long EffectiveCapacity(long base) {
    if (base <= 0) return 0;
    long op_blocks = static_cast<long>(std::ceil(base * kCacheOpRatio));
    if (op_blocks == 0) op_blocks = 1;
    long effective = base - op_blocks;
    return effective > 0 ? effective : 1;
}
}

LRUCache::LRUCache(uint64_t cold_capacity, long capacity, int _cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, const std::string &stat_log_file)
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      capacity_(EffectiveCapacity(capacity)),
      cache_filled(false),
      cache_trace_fp(nullptr),
      cold_trace_fp(nullptr),
      cache_block_size(_cache_block_size),
      cache_trace(_cache_trace),
      cache_ftl(static_cast<uint64_t>(capacity) * cache_block_size, new GreedyEvictPolicy()),
      cache_ftl_log_fp(nullptr),
      cache_ftl_log_interval_bytes(10ull * 1024 * 1024 * 1024),
      next_cache_ftl_log_bytes(10ull * 1024 * 1024 * 1024),
      allocator(static_cast<size_t>(capacity_) * _cache_block_size, _cache_block_size) {
    cache_trace_fp = nullptr;
    //printf("LRU cache created with capacity: %ld\n", capacity);
    if (cache_trace) {
        cache_trace_fp = fopen(trace_file.c_str(), "w");
    }
    if (cold_trace_file != "") {
        cold_trace_fp = fopen(cold_trace_file.c_str(), "w");
    }
}

LRUCache::~LRUCache() {
    if (cache_trace_fp) {
        fclose(cache_trace_fp);
    }
    if (cold_trace_fp) {
        fclose(cold_trace_fp);
    }
}

bool LRUCache::exists(long key) {
    return cacheMap.find(key) != cacheMap.end();
}

int LRUCache::get_block_size() {
    return cache_block_size;
}

void LRUCache::print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type) {
    if (cache_trace_fp) {
        long start_block = static_cast<long>(lba_offset / cache_block_size);
        long end_block = static_cast<long>((lba_offset + lba_size) / cache_block_size);
        // print trace as csv format
        for (long block = start_block; block <= end_block; block++) {
            auto it = cacheMap.find(block);
            if (it == cacheMap.end()) {
                continue;
            }
            CacheEntry &entry = it->second;
            const uint64_t DUMMY_VALUE = 0;
            std::string op_string = "W";
            if (op_type == OP_TYPE::READ) {
                op_string = "R";
            }
            long long block_start = static_cast<long long>(block) * cache_block_size;
            long long block_end = block_start + cache_block_size;
            long long req_start = lba_offset;
            long long req_end = lba_offset + lba_size;
            long long left_offset = std::max(block_start, req_start);
            long long right_offset = std::min(block_end, req_end);
            if (entry.iter != cacheList.end()) {
                size_t id = entry.allocated_id;
                
                size_t cache_lba_offset = id * cache_block_size + left_offset % cache_block_size;
                size_t cache_lba_size = right_offset - left_offset;
                if (cache_lba_size == 0) {
                    continue;
                }
                fprintf(cache_trace_fp, "%ld,%s,%ld,%ld,%ld\n", DUMMY_VALUE, op_string.c_str(), cache_lba_offset, cache_lba_size, DUMMY_VALUE);
            } else {
                if (right_offset > left_offset) {
                    assert(false);
                }
            }
        }
    }
    
}

void LRUCache::touch(long key, OP_TYPE op_type) {
    auto it = cacheMap.find(key);
    if (it != cacheMap.end()) {
        size_t id = it->second.allocated_id;
        cacheList.erase(it->second.iter);
        cacheList.push_back(key);
        CacheEntry cacheEntry = {
            .iter = std::prev(cacheList.end()),
            .allocated_id = id
        };
        cacheMap[key] = cacheEntry;
    }
}

void LRUCache::evict_one_block() {
    long oldest = cacheList.front();
    cacheList.pop_front();
    auto second = cacheMap[oldest];
    size_t id = second.allocated_id;
    cacheMap.erase(oldest);
    allocator.free(id);
    cache_ftl.Trim(id * cache_block_size, static_cast<uint64_t>(cache_block_size));
    cache_filled = true;
    if (cold_trace_fp) {
        const uint64_t DUMMY_VALUE = 0;
        fprintf(cold_trace_fp, "%ld,%s,%ld,%d,%ld\n",
                DUMMY_VALUE, "W", oldest * cache_block_size, cache_block_size, DUMMY_VALUE);
    }
    maybe_log_cache_ftl_stats();
    _evict_one_block(oldest * cache_block_size, cache_block_size, OP_TYPE::WRITE);
}

void LRUCache::batch_insert(int stream_id, const std::map<long, int> &newBlocks, OP_TYPE op_type) {
    for (auto iter : newBlocks) {
        long block = iter.first;
        int lba_size = iter.second;
        if (exists(block)) {
            touch(block, op_type);
            if (op_type == OP_TYPE::WRITE && lba_size > 0) {
                auto &entry = cacheMap[block];
                size_t alloc_id = entry.allocated_id;
                cache_ftl.Write(alloc_id * cache_block_size, static_cast<uint64_t>(lba_size), stream_id);
            }
        } else {
            while (cacheMap.size() >= static_cast<size_t>(capacity_)) {
                evict_one_block();
                evicted_blocks++;
            }
            cacheList.push_back(block);
            size_t alloc_id = allocator.alloc();
            CacheEntry cacheEntry = {
                .iter = std::prev(cacheList.end()),
                .allocated_id = alloc_id
            };
            cacheMap[block] = cacheEntry;
            if (op_type == OP_TYPE::WRITE && lba_size > 0) {
                cache_ftl.Write(alloc_id * cache_block_size, static_cast<uint64_t>(lba_size), stream_id);
            }
        }
        write_size_to_cache += lba_size;
    }
    if (op_type == OP_TYPE::WRITE) {
        maybe_log_cache_ftl_stats();
    }
}

bool LRUCache::is_cache_filled() {
    return cache_filled;
}

size_t LRUCache::size(){
    //printf("cacheMap.size() = %ld\n", cacheMap.size());
    return cacheMap.size();
}

void LRUCache::maybe_log_cache_ftl_stats() {
    if (cache_ftl_log_interval_bytes == 0) {
        return;
    }
    if (!fp_stats) {
        return;
    }
    const uint64_t host_bytes = cache_ftl.GetHostWritePages() * static_cast<uint64_t>(NAND_PAGE_SIZE);
    if (host_bytes < next_cache_ftl_log_bytes) {
        return;
    }
    const uint64_t nand_bytes = cache_ftl.GetNandWritePages() * static_cast<uint64_t>(NAND_PAGE_SIZE);

    const auto compacted_blocks_value = (cache_ftl.GetNandWritePages() - cache_ftl.GetHostWritePages())
        * static_cast<unsigned long long>(NAND_PAGE_SIZE)
        / static_cast<unsigned long long>(cache_block_size);
    const std::string& prefix = stats_prefix();
    const char* prefix_cstr = prefix.empty() ? "LRU" : prefix.c_str();
    fprintf(fp_stats,
            "%s invalidate_blocks: %lld compacted_blocks: %llu global_valid_blocks: %llu write_size_to_cache: %lld evicted_blocks: %lld write_hit_size: %lld total_cache_size: %llu reinsert_blocks: %d read_blocks_in_partial_write %d waf_host_bytes: %llu waf_nand_bytes: %llu\n",
            prefix_cstr,
            evicted_blocks,
            static_cast<unsigned long long>(compacted_blocks_value),
            static_cast<unsigned long long>(cacheMap.size()),
            write_size_to_cache,
            evicted_blocks,
            write_hit_size,
            static_cast<unsigned long long>(static_cast<uint64_t>(capacity_) * static_cast<uint64_t>(cache_block_size)),
            0,
            0,
            static_cast<unsigned long long>(host_bytes),
            static_cast<unsigned long long>(nand_bytes));
    fflush(fp_stats);
    next_cache_ftl_log_bytes += cache_ftl_log_interval_bytes;
}
