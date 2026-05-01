#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "istream.h"

class ITraceParser;

/**
 * NodapStream: Near-optimal Data Placement (NoDaP) oracle from
 * "DOGI: Data Placement with Oracle-Guided Insights" (FAST'26).
 *
 * Performs an offline pre-pass over the trace to build, for every LBA,
 * the sequence of future invalidation timestamps (in batch_insert
 * block-event units, matching log_cache_timestamp). At Classify time,
 * the block's BIR (= next_inv_time - current_time) is looked up in
 * a fixed group-config table and routed to the matching group.
 *
 * Default group config = paper Table 2 (YCSB-A on MySQL):
 *   BIR upper bounds: [200K, 9M, 17M, 27M, 42M, 51M, ∞]
 *   N = 7 groups
 *
 * Limitations of this initial port:
 *  - Assumes --no_fill (prefill phase not pre-passed).
 *  - --lba_remap and per-block remapping are NOT modelled in pre-pass;
 *    use direct LBA addresses only.
 *  - Per-group segment-count constraint from the paper is NOT enforced
 *    on the LogCache side; placement-only oracle.
 */
class NodapStream : public IStream {
public:
    explicit NodapStream(int num_groups = 7,
                         std::vector<uint64_t> bir_upper = {
                             200ULL,
                             9000ULL,
                             17000ULL,
                             27000ULL,
                             42000ULL,
                             51000ULL,
                             UINT64_MAX});

    void LoadOracle(const std::string& trace_file,
                    ITraceParser& parser,
                    int block_size,
                    int lba_scale = 1);

    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp,
                  uint64_t created_timestamp, bool is_read = false) override;
    void Append(uint64_t /*blockAddr*/, uint64_t /*global_timestamp*/, void* /*arg*/) override {}
    void GcAppend(uint64_t /*blockAddr*/) override {}
    void CollectSegment(Segment* /*segment*/, uint64_t /*global_timestamp*/) override {}
    int  GetVictimStreamId(uint64_t /*global_timestamp*/, uint64_t /*threshold*/) override { return -1; }
    int  getNumHostStreams() const override { return num_groups_; }

    const std::vector<uint64_t>& bir_upper() const { return bir_upper_; }
    int num_groups() const { return num_groups_; }
    bool oracle_loaded() const { return oracle_loaded_; }

private:
    int classify_bir(uint64_t bir) const;

    int num_groups_;
    std::vector<uint64_t> bir_upper_;
    std::unordered_map<uint64_t, std::deque<uint64_t>> next_inv_q_;
    std::unordered_map<uint64_t, uint64_t> current_inv_;
    bool oracle_loaded_ = false;

    uint64_t classify_calls_ = 0;
    uint64_t classify_misses_ = 0;
};

extern uint64_t g_nodap_bir_upper[IStream::MAX_STREAMS];
extern int      g_nodap_num_groups;
