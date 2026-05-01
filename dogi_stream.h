#pragma once
#include <cstdint>
#include <cfloat>
#include <unordered_map>
#include <vector>
#include "istream.h"
#include "segment.h"

/**
 * DogiStream: DOGI heuristic-only data placement as IStream.
 *
 * Implements DOGI's three heuristic components on top of LogCache:
 *   1. Hot Filter: dynamic threshold μ on latest invalidation time
 *      - hot blocks → stream 0
 *      - non-hot → stream 1
 *   2. GC relocation: age-based cascade (G_i → G_{i+1})
 *      + Frozen Filter: blocks never overwritten → frozen stream (coldest)
 *   3. Expired segment victim hint via GetVictimStreamId
 *
 * No ML, no PLog, no Group Optimizer. Pure heuristic baseline
 * matching DOGI APPLY_ML=0 behavior.
 */
class DogiStream : public IStream {
public:
    DogiStream(int num_gc_streams = 5, uint64_t hot_threshold_init = 2,  // quantized units (like DOGI original)
               uint64_t hot_step = 65000, uint64_t hot_window_blocks = 10000000,
               uint64_t frozen_clear_interval = 200000000);

    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp,
                  uint64_t created_timestamp, bool is_read = false) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr) override;
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;
    int  GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) override;
    int  getNumHostStreams() const override { return 2; }
    void setPassTimeBlocks(uint64_t ptb) { pass_time_blocks_ = ptb; }

private:
    // Hot Filter state
    int num_gc_streams_;
    uint64_t hot_threshold_;       // μ: upper bound of hot BIR (blocks)
    uint64_t hot_step_;            // d: step size for μ adjustment
    uint64_t hot_window_blocks_;   // n: window size for WAF measurement

    uint64_t total_writes_ = 0;        // total user writes for warmup tracking
    uint64_t pass_time_blocks_ = 0;    // warmup period (= device blocks, like DOGI)

    uint64_t window_start_ts_ = 0;
    uint64_t window_user_writes_ = 0;
    uint64_t window_gc_writes_ = 0;
    double prev_window_waf_ = DBL_MAX;

    // Per-LBA 8-bit saturating counter (matches DOGI HotIntervalTracker)
    static constexpr uint64_t kTrackerCapacity = 128ull * 1024ull * 1024ull;
    static constexpr uint64_t kIntervalUnit = 65536;
    std::unordered_map<uint64_t, uint8_t> intervals_;

    uint8_t saturatingAdd(uint8_t base, uint64_t deltaUnits) {
        uint64_t sum = static_cast<uint64_t>(base) + deltaUnits;
        return static_cast<uint8_t>(std::min<uint64_t>(sum, 255));
    }

    // Frozen Filter: 1-bit per LBA (has this LBA ever been overwritten?)
    std::unordered_map<uint64_t, bool> ever_overwritten_;
    uint64_t frozen_clear_interval_;
    uint64_t next_frozen_clear_ = 0;

    // GC stream tracking: which GC stream each block is in
    std::unordered_map<uint64_t, int> gc_stream_; // blockAddr → current gc stream id

    // Expired segment detection
    std::vector<int> pending_victim_streams_;

    bool tracksHotState(uint64_t blockAddr) const {
        return blockAddr < kTrackerCapacity;
    }
    uint8_t estimateInterval(uint64_t blockAddr, uint64_t segmentAge);
    void adjustHotThreshold(double current_waf);
};
