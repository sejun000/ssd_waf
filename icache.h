#pragma once
#include <list>
#include <unordered_map>
#include <map>
#include <string>
#include <cassert>
#include <tuple>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <iostream>
#include "ftl.h"
#include "common.h"

struct CacheEntry {
    std::list<long>::iterator iter;
    size_t allocated_id;
};

/**
 * Log-append style LBA remapper.
 *
 * Used by cache_sim when --lba_remap is given. Treats the cache device as a
 * single storage of `pool_blocks` slots. The first time an upstream LBA is
 * seen, it is mapped to the next free slot (monotonically incrementing,
 * log-append). Subsequent appearances of the same LBA reuse the mapping. When
 * the slot pool is exhausted, the original LBA is passed through unchanged.
 *
 * Operates on byte-addressed offsets so callers do not need to know the
 * block size; intra-block byte offsets are preserved.
 */
class LbaRemapper {
public:
    LbaRemapper(uint64_t pool_blocks, int block_size)
        : pool_blocks_(pool_blocks), block_size_(block_size) {}

    long long remap(long long lba_offset) {
        long long blk = lba_offset / block_size_;
        long long off_in_blk = lba_offset - blk * block_size_;
        auto it = map_.find(blk);
        long long mapped;
        if (it == map_.end()) {
            if (next_alloc_ >= static_cast<long long>(pool_blocks_)) {
                // pool exhausted: pass through untouched
                ++passthrough_count_;
                return lba_offset;
            }
            mapped = next_alloc_++;
            map_[blk] = mapped;
        } else {
            mapped = it->second;
        }
        return mapped * static_cast<long long>(block_size_) + off_in_blk;
    }

    uint64_t mapped_count() const { return static_cast<uint64_t>(next_alloc_); }
    uint64_t passthrough_count() const { return passthrough_count_; }
    uint64_t pool_blocks() const { return pool_blocks_; }

private:
    std::unordered_map<long long, long long> map_;
    long long next_alloc_ = 0;
    uint64_t pool_blocks_;
    int block_size_;
    uint64_t passthrough_count_ = 0;
};

class ICache {
public:
    // 생성자: capacity는 블록 단위 최대 개수
    virtual ~ICache(){
        write_size_to_cache = 0;
        evicted_blocks = 0;
        write_hit_size = 0;
    }
    ICache(uint64_t cold_capacity, const std::string& waf_log_file, const std::string& stat_log_file = "");
    std::string get_timestamp() {
        auto     now   = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);

        // ② tm 구조체로 변환 (스레드 안전 함수 사용 – POSIX: localtime_r, Windows: localtime_s)
        std::tm tm {};
        localtime_r(&tt, &tm);        // Linux/Unix
        // localtime_s(&tm, &tt);     // Windows라면 이 줄로 교체

        // ③ 원하는 형식으로 문자열화
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");   // 예: 20250728_113014
        std::string timestamp = oss.str();
        return timestamp;
    }
    virtual bool exists(long key) = 0;
    virtual void touch(long key, OP_TYPE op_type) = 0;
    virtual void batch_insert(int stream_id, const std::map<long, int> &newBlocks, OP_TYPE op_type) = 0;
    virtual bool is_cache_filled() = 0;
    virtual int get_block_size() = 0;
    virtual void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type){};
    void _evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type);
    void _invalidate_cold_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type);
    virtual void print_stats() {}
    virtual void evict_one_block() = 0;
    virtual size_t size() = 0;
    virtual bool is_no_cache() { return false; }
    std::tuple<long long, long long, long long> get_status();
    void set_stats_prefix(const std::string& prefix);
    const std::string& stats_prefix() const;
    void set_start_ts(const std::string& ts) { start_ts_ = ts; }
    const std::string& start_ts() const { return start_ts_; }
    void rename_stat_log(const std::string& new_name);
    void rename_compare_log(const std::string& new_name);
    PageMappingFTL ftl;
    long long write_size_to_cache;
    long long evicted_blocks;
    long long write_hit_size;
    long long next_write_size_to_cache;
    FILE *fp;
    FILE *fp_stats = nullptr;
    FILE *fp_compare = nullptr;
    FILE *fp_object = nullptr;
protected:
    std::string stats_prefix_;
    std::string start_ts_;
};

extern uint64_t g_dogi_hot_bir;  // DOGI hot group BIR, updated by DogiStream

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, double valid_rate_threshold = 0.0, std::string stat_log_file = "", double periodic_ratio = 2.88);
