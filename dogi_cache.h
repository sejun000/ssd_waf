#pragma once

#include "icache.h"
#include <memory>
#include <vector>
#include <array>
#include <string>
#include <cstdint>

// Forward declarations to avoid pulling DOGI headers into wider compilation units.
class Manager;
class Selection;
class FreqFeatures;
class ModelTrainer;
class MlpInference;
class DogiHotClassifier;

/**
 * DogiCache: cache_sim ICache adapter that drives DOGI's in-memory log-structured
 * data placement (Hot Filter + ML-Alloc + Group Optimizer + Frozen Filter + ML-Reloc)
 * without any ZNS / RocksDB / ZenFS dependency.
 *
 * Threading model: single-threaded (cache_sim style). DOGI's worker thread + config
 * worker thread are inlined into batch_insert.
 */
class DogiCache final : public ICache {
public:
    DogiCache(uint64_t cold_capacity,
              uint64_t cache_block_count,
              int cache_block_size,
              bool cache_trace,
              const std::string& trace_file,
              const std::string& cold_trace_file,
              std::string& waf_log_file,
              const std::string& stat_log_file = "");

    ~DogiCache() override;

    /* ICache overrides */
    bool exists(long key) override;
    void touch(long, OP_TYPE) override {}
    void batch_insert(int stream_id,
                      const std::map<long, int>& newBlocks,
                      OP_TYPE op_type) override;
    int  get_block_size() override { return cache_block_size_; }
    bool is_cache_filled() override;
    void evict_one_block() override {}     // GC handled internally by DOGI
    std::size_t size() override;
    void print_stats() override;

private:
    /* helpers */
    void flush_buffer();
    void run_inline_gc();
    void try_run_config_step();

    int cache_block_size_;
    uint64_t cache_block_count_;
    uint64_t cold_capacity_;

    /* DOGI components */
    std::unique_ptr<Manager> manager_;
    std::unique_ptr<Selection> selection_;
    std::shared_ptr<FreqFeatures> freq_tracker_;
    std::unique_ptr<ModelTrainer> model_trainer_;
    std::unique_ptr<MlpInference> mlp_inference_;
    std::unique_ptr<DogiHotClassifier> hot_classifier_;

    /* batched write buffer (mirrors main.cc BufferSlot) */
    struct PendingWrite {
        uint32_t lba;
        uint64_t interval;
        bool isHot;
    };
    std::vector<PendingWrite> buffer_;
    std::size_t non_hot_count_ = 0;
    std::size_t buffer_bytes_ = 0;

    static constexpr std::size_t kBufferBytesLimit = 16ull * 1024 * 1024;
    static constexpr std::size_t kNonHotThreshold = 128;

    /* runtime state */
    uint64_t logical_time_ = 0;
    uint64_t feature_time_ = 0;
    uint64_t pass_time_blocks_ = 0;
    uint32_t prev_lba_ = 0;
    bool config_applied_ = false;

    std::string latest_model_name_;
    std::string latest_model_dir_;
    std::string gconf_base_;
};
