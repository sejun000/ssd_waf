#pragma once
#include "icache.h"
#include "log_cache_segment.h"
#include "istream.h"
#include "histogram.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace midas {
struct SSD;
struct STATS;
class GROUP;
}

/**
 * MiDAS-style log cache.
 * 그룹별 active 유지, FIFO victim GC(compaction) + 외부 주입 eviction 정책.
 */
struct MidasConfig
{
    std::size_t segment_bytes  = 32ull * 1024 * 1024; ///< default 32 MB
    double      free_ratio_low = 0.01;                ///< 1 %
    int         evicted_blk_size = 1;                 // 4k eviction
    uint64_t    print_stats_interval = 10 * 1024ull * 1024 * 1024; // 10 GB
};

struct MidasInitArgs
{
    std::string workload;
    std::string vs_policy;
    int dev_gb = -1;
    int seg_mb = -1;
};

class MidasCache final : public ICache
{
public:
    MidasCache(uint64_t              cold_capacity,
               uint64_t              cache_block_count,
               int                   cache_block_size,
               bool                  cache_trace,
               const std::string&    trace_file,
               const std::string&    cold_trace,
               std::string&          waf_log_file,
               const MidasConfig*    cfg           = nullptr,
               IStream *input_stream_policy = nullptr,
               double target_valid_blk_rate = 0.0,
               std::string stat_log_file = "",
    int group_count = 5,
    const MidasInitArgs& midas_init_args = MidasInitArgs{}
              );

    ~MidasCache();

    /* ICache overrides */
    bool        exists(long key) override;
    void        touch(long, OP_TYPE) override {}              // no‑op
    std::size_t size() override { return global_valid_blocks; }

    /* 새로운 API – stream id 포함 */
    void batch_insert(int stream_id, const std::map<long,int>& newBlocks,
                      OP_TYPE                   op_type = OP_TYPE::WRITE);
    int get_block_size() override;
    void evict_one_block() override;
    void evict(LogCacheSegment::Block &blk);
    bool is_cache_filled() override;
    virtual void print_stats() override;
    void print_objects(std::string prefix, uint64_t value);
    int invalidate(long key, int lba_sz);
    void print_group_stats();
    void maybe_finish_epoch();


private:
    /* configuration ******************************************************/
    const int         cache_block_size;
    MidasConfig       cfg_;
    int               group_num;
    MidasInitArgs     midas_args_;
    bool              midas_initialized = false;
    std::string       workload_str_;
    std::string       vs_policy_str_;

    midas::SSD*       midas_ssd = nullptr;
    midas::STATS*     midas_stats = nullptr;
    std::array<midas::GROUP*, 20> midas_group{};

    /* helpers ************************************************************/
    double target_valid_blk_rate = 0.0; // ratio of write to QLC
    double valid_blk_rate_hard_limit = 0.0;

    void             maybe_run_gc_policy();
    void             evict_one_segment();
    void             initialize_midas();
    void             destroy_midas();
    int              purge_segment_valids(int victim_seg);
    uint64_t         segment_age_pages(int segment_idx) const;
    void             record_eviction_histograms(int removed_pages, uint64_t segment_age_pages);

    /* trace(optional) *****************************************************/
    bool  cache_trace_;
    FILE* trace_fp_      = nullptr;
    FILE* cold_trace_fp_ = nullptr;
    std::size_t segment_size_blocks;
    std::size_t total_segments;
    uint64_t total_cache_block_count = 0;

    uint64_t total_capacity_bytes = 0;
    uint64_t global_valid_blocks = 0;
    uint64_t invalidate_blocks = 0;
    uint64_t compacted_blocks = 0;
    uint64_t reinsert_blocks = 0;
    uint64_t read_blocks_in_partial_write = 0;

    static const int HISTOGRAM_BUCKETS = 20;
    static const uint64_t DEFAULT_HALF_LIFE_IN_BLOCKS = 262144 * 4;
    bool is_ghost_cache = false;
    uint64_t bypass_blocks_threshold = 128; // 128* 4k bytes = 512K bytes

    uint64_t epoch_written_pages = 0;
    uint64_t epoch_threshold_pages = 0;

    struct RemapTable {
        void init(std::size_t total_blocks);
        int  get_or_assign(long key);
        int  remove(long key);
        int  remove_by_mapped(int mapped); // remove by mapped index (for reverse lookup)

        bool ready = false;
        std::size_t total_blocks = 0;
        std::unordered_map<long, int> mapping;
        std::unordered_map<int, long> rev_mapping;
        std::vector<char> used;
        std::vector<int> free_list;

    private:
        int next_free();
    };
    RemapTable remap_;

    std::unique_ptr<Histogram> evicted_ages_histogram_;
    std::unique_ptr<Histogram> evicted_blocks_histogram_;
    std::unique_ptr<Histogram> evicted_ages_with_segment_histogram_;
};
