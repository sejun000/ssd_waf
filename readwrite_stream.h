#pragma once
#include <cstdint>
#include <cfloat>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "istream.h"
#include "segment.h"

/**
 * ReadWriteStream policy:
 *   Host: hot(stream 0) / cold(stream 1) based on avg lifespan
 *   GC:
 *     - readwrite_hotcold:      single GC stream (was_read ignored)
 *     - readwrite_hotcold_read: GC write stream + GC read stream
 *     - circular mode:          write cold / read cold each split into
 *                                N sub-streams by created_timestamp cycle
 */
class ReadWriteStream : public IStream {
public:
    ReadWriteStream(bool separate_read, int max_gc_streams, int timestamp_granularity,
                    bool circular = false, uint64_t coldest_age_threshold = 0);
    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp,
                  uint64_t created_timestamp, bool is_read = false) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr) override;
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;
    int  GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) override;
    int  getNumHostStreams() const override { return 2; }

private:
    bool separate_read_;
    int max_gc_streams_;
    int timestamp_granularity_;
    bool circular_;
    double avg_lifespan_ = DBL_MAX;
    std::unordered_map<uint64_t, int> prev_class_;

    // circular mode state
    int stream_cycles_[IStream::MAX_STREAMS];
    std::vector<int> pending_victim_streams_;

    uint64_t coldest_age_threshold_;  // age >= this → coldest stream
    int circular_streams_;            // actual circular stream count (max_gc_streams or max_gc_streams-1)

    int classifyCircular(uint64_t created_timestamp, uint64_t global_timestamp, bool is_read);
};
