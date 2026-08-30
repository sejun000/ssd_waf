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
#include "oracle_lifetime.h"
#include "common.h"

struct CacheEntry {
    std::list<long>::iterator iter;
    size_t allocated_id;
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
    // CSAL-like mode: the cache layer does NOT send trim/deallocate to the
    // backend FTL; stale cold copies stay valid until re-evicted (overwrite).
    void set_cold_trim_enabled(bool on) { cold_trim_enabled_ = on; }
    // Oracle death-time placement (experiment A): when set, cold writes are
    // steered to one of `streams` backend streams by the block's remaining
    // lifetime (= host pages until the next write to it, which is exactly when
    // its cold copy gets trimmed). Off → every cold write keeps using stream 0.
    // `reserve` is the synthetic seq-inject region [0, reserve): those blocks
    // are rewritten round-robin with a fixed, uniformly long period, so they go
    // to the coldest stream (which is also where they belong).
    void set_cold_oracle(const OracleLifetime* o, int streams, uint64_t reserve,
                         uint64_t block_size) {
        cold_oracle_ = o; cold_oracle_streams_ = streams;
        cold_oracle_reserve_ = reserve; cold_oracle_block_ = block_size;
    }
    // Host-write clock in trace pages (inject excluded) — same unit the oracle
    // index is built in. Advanced by the replay loop.
    void set_trace_page_clock(uint64_t p) { trace_page_clock_ = p; }
    PageMappingFTL ftl;
    long long write_size_to_cache;
    long long evicted_blocks;
    long long write_hit_size;
    long long next_write_size_to_cache;
    /* Periodic snapshot of FTL counters (refreshed every 10 GB host write
     * inside _evict_one_block). Concrete caches (e.g. LogCache) read these
     * to drive periodic decisions without poking the FTL each tick. */
    uint64_t last_ftl_host_write_pages = 0;
    uint64_t last_ftl_nand_write_pages = 0;
    FILE *fp;
    FILE *fp_stats = nullptr;
    FILE *fp_object = nullptr;
protected:
    // Bin the block's remaining lifetime into [0, streams) — 0 = dies soonest.
    // Log-spaced so the bins stay meaningful across the 4-orders-of-magnitude
    // lifetime spread; blocks with no future write land in the coldest bin.
    int cold_oracle_stream_for(uint64_t lba_offset) const;
    std::string stats_prefix_;
    std::string start_ts_;
    bool cold_trim_enabled_ = true;
    const OracleLifetime* cold_oracle_ = nullptr;
    int      cold_oracle_streams_ = 0;
    uint64_t cold_oracle_reserve_ = 0;
    uint64_t cold_oracle_block_   = 4096;
    uint64_t trace_page_clock_    = 0;
};

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, double valid_rate_threshold = 0.0, std::string stat_log_file = "", double periodic_ratio = 2.88, double util_step = 0.02, const std::string& moving_avg_type = "ewma", double moving_avg_window = 0.0, int gs_decision_period_segs = 8, uint64_t segment_bytes = 0);
