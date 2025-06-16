#pragma once
#include "icache.h"
#include <cstdint>

class NoCache : public ICache {
public:
    NoCache(uint64_t cold_capacity, int cache_block_size) : ICache(cold_capacity), cache_block_size(cache_block_size) {
        // NoCache는 캐시를 사용하지 않으므로 cold_capacity는 무시됩니다.
    }
    ~NoCache(){}
    bool exists(long key) override { return false; }
    void touch(long key, OP_TYPE op_type) override { /* No operation */ }
    void batch_insert(const std::map<long, int> &newBlocks, OP_TYPE op_type) override {
        for (const auto& iter : newBlocks) {
            long block = iter.first;
            int lba_size = iter.second;
            // NoCache는 블록을 캐시에 저장하지 않으므로 아무 작업도 하지 않습니다.
            _evict_one_block(block * cache_block_size, lba_size, op_type); // 블록 크기 단위로 evict
            evicted_blocks += 1; // evicted_blocks 카운트 증가
            write_size_to_cache += lba_size; // 캐시에 쓰기 크기 업데이트
        }
    }
    bool is_cache_filled() override { return false; }
    int get_block_size() override { return cache_block_size; }
    void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type) override { /* No operation */ }
    void evict_one_block() override { /* No operation */ }
    bool is_no_cache() override { /* NoCache는 항상 true */ return true; }
    size_t size() override { return 1; }
    int cache_block_size; // 기본 블록 크기 (4K)
};