#include "multi_hot_cold.h"
#include <cfloat>
#include <cstring>

int g_stream_cycles[IStream::MAX_STREAMS] = {0};
uint64_t g_cycle_length = 0;

MultiHotCold::MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append, bool classfy_for_gc_append, int num_host_streams){
    mMaxGcStreams = max_gc_streams;
    mTimestampGranularity = timestamp_granularity;
    mCheckCreatedTimestampOnly = check_created_timestamp_only;
    mClassifyForHostAppend = classify_for_host_append;
    mClassifyForGcAppend = classfy_for_gc_append;
    mNumHostStreams = num_host_streams;
    mAvgLifespan = DBL_MAX;
    mLba2Fifo = new FIFO();
    mMetadata = new Metadata();
    std::memset(mStreamCycles, -1, sizeof(mStreamCycles));
    std::memset(g_stream_cycles, 0, sizeof(g_stream_cycles));
    g_cycle_length = (uint64_t)mTimestampGranularity * mMaxGcStreams;
}

extern uint64_t g_threshold;

int MultiHotCold::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
    // Sync granularity with current g_threshold each call: interval = g_threshold
    // / kMultiHotColdStreams (segment-aligned).  Pre-warmup falls back to
    // set_stream_interval's saved cache_block_count.
    uint64_t fresh_interval = compute_stream_interval(0);
    if (fresh_interval > 0 && static_cast<uint64_t>(mTimestampGranularity) != fresh_interval) {
        mTimestampGranularity = static_cast<int>(fresh_interval);
        g_cycle_length        = static_cast<uint64_t>(mTimestampGranularity) * mMaxGcStreams;
    }
    uint64_t time_diff = global_timestamp - created_timestamp;
    if (!isGcAppend) {
        uint64_t lifespan = time_diff;
        if (lifespan != 0 && lifespan < mAvgLifespan) {
            return 0;
        }
        else {
            return 1;
        }
       return 0;
    }
    if (mCheckCreatedTimestampOnly) {
        time_diff = created_timestamp;
    }
    int raw_id = time_diff / mTimestampGranularity;
    int stream_id;
    if (mCheckCreatedTimestampOnly) {
        int cycle = raw_id / mMaxGcStreams;
        stream_id = raw_id % mMaxGcStreams;

        // Detect per-stream cycle wrap (silenced — was a hot printf).
        if (mStreamCycles[stream_id] >= 0 && cycle > mStreamCycles[stream_id]) {
            // This stream is about to be reused in a new cycle → queue for dummy fill
            mPendingVictimStreams.push_back(stream_id);
            mStreamCycles[stream_id] = cycle;
            g_stream_cycles[stream_id] = cycle;
        }
        if (mStreamCycles[stream_id] < 0) {
            mStreamCycles[stream_id] = cycle;
            g_stream_cycles[stream_id] = cycle;
        }
    } else {
        stream_id = raw_id;
        if (stream_id >= mMaxGcStreams) {
            stream_id = mMaxGcStreams - 1; // Limit to max GC streams
        }
    }
    return stream_id + Segment::GC_STREAM_START;
}

int MultiHotCold::ClassifyReadOnly(uint64_t /*blockAddr*/, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) const {
    // Pure computation twin of Classify(): returns the same stream id WITHOUT
    // mutating any state. Specifically it never writes mTimestampGranularity,
    // g_cycle_length, mStreamCycles, g_stream_cycles or mPendingVictimStreams —
    // so it is safe to call repeatedly inside a what-if GC simulation.
    uint64_t gran = static_cast<uint64_t>(mTimestampGranularity);
    uint64_t fresh_interval = compute_stream_interval(0);
    if (fresh_interval > 0) gran = fresh_interval;   // local copy only, not stored
    if (gran == 0) gran = 1;                          // div-by-zero guard

    uint64_t time_diff = global_timestamp - created_timestamp;
    if (!isGcAppend) {
        uint64_t lifespan = time_diff;
        return (lifespan != 0 && lifespan < mAvgLifespan) ? 0 : 1;
    }
    if (mCheckCreatedTimestampOnly) {
        time_diff = created_timestamp;
    }
    int raw_id = static_cast<int>(time_diff / gran);
    int stream_id;
    if (mCheckCreatedTimestampOnly) {
        stream_id = raw_id % mMaxGcStreams;           // no cycle-wrap bookkeeping
    } else {
        stream_id = raw_id;
        if (stream_id >= mMaxGcStreams) stream_id = mMaxGcStreams - 1;
    }
    return stream_id + Segment::GC_STREAM_START;
}

int MultiHotCold::GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) {
    if (!mCheckCreatedTimestampOnly) return -1;

    // Return pending cycle-wrap victims first
    if (!mPendingVictimStreams.empty()) {
        int id = mPendingVictimStreams.back();
        mPendingVictimStreams.pop_back();
        return id + Segment::GC_STREAM_START;
    }

    return -1;
}

void MultiHotCold::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
  /*  uint64_t valid_blocks = reinterpret_cast<uint64_t>(arg);
    global_timestamp += 1;
    mLba2Fifo->Update(blockAddr, mAvgLifespan, valid_blocks);
    mMetadata->Update(blockAddr, global_timestamp);*/
}

void MultiHotCold::CollectSegment(Segment *segment, uint64_t global_timestamp) {
  static uint64_t totLifespan = 0;
  static int nCollects = 0;
  if (segment->get_class_num() == 0) {
    //printf("CollectSegment: %lu, class_num: %d\n mAvgLifespan %f\n", segment->get_create_time(), segment->get_class_num(), mAvgLifespan);
    totLifespan += global_timestamp - segment->get_create_time();
    nCollects += 1;
  }
  if (nCollects == 16) {
    mAvgLifespan = 1.0 * totLifespan / nCollects;
    nCollects = 0;
    totLifespan = 0;
    //std::cout << "AvgLifespan: " << mAvgLifespan << std::endl;
  }
// mClassNumOfLastCollectedSegment = segment->get_class_num();
}
