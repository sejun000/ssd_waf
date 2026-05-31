#pragma once
#include <cstdint>
#include "segment.h"
#include <string>
class IStream {
public:
    virtual int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) = 0;
    // Mutation-free twin of Classify() for what-if GC simulation. Returns the
    // SAME stream id Classify() would, but must NOT touch any internal cycle /
    // granularity bookkeeping (mStreamCycles, pending-victim queues, …). Default
    // -1 = unsupported → caller should fall back to a single GC stream.
    virtual int  ClassifyReadOnly(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) const { return -1; }
    virtual void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) = 0;
    virtual void GcAppend(uint64_t blockAddr) = 0;
    virtual void CollectSegment(Segment *segment, uint64_t global_timestamp) = 0;
    virtual int GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold){
        return -1;
    }
    virtual int getNumHostStreams() const { return 2; }  // default: hot/cold
static const int MAX_STREAMS = 40;
};

IStream* createIstreamPolicy(std::string policy_type);
void set_stream_interval(uint64_t cache_block_count, uint64_t segment_size_blocks = 0);
// Re-evaluate interval against current g_threshold (called from Classify so the
// stream granularity tracks g_threshold drift).  fallback_cache_blocks is used
// only when g_threshold == 0 (pre-warmup).
uint64_t compute_stream_interval(uint64_t fallback_cache_blocks = 0);
extern uint64_t interval;  // = granularity (timestamp units per GC stream)
